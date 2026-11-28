#include "session/object_authority.hpp"
#include <algorithm>
#include <array>
#include <utility>

namespace netcode {
namespace {

constexpr std::uint16_t protocol_version = 1;
constexpr std::size_t max_models = (control_payload_limit - 21) / 6;
constexpr std::uint16_t batch_word_flag = 0x8000;
constexpr std::size_t receipt_size = 12;
constexpr std::size_t game_header_size = 1 + sizeof(RoomId) + sizeof(RoundId);

bool is_authority_message(std::uint8_t code)
{
    return (code >= static_cast<std::uint8_t>(GameMessage::AuthorityOffer) &&
            code <= static_cast<std::uint8_t>(GameMessage::AuthorityAck)) ||
           (code >= static_cast<std::uint8_t>(GameMessage::ClaimHello) &&
            code <= static_cast<std::uint8_t>(GameMessage::ClaimResult)) ||
           (code >= static_cast<std::uint8_t>(GameMessage::AuthorityStateWords) &&
            code <= static_cast<std::uint8_t>(GameMessage::AuthorityAckBatch));
}

void write_receipt(Writer& out, const ObjectGrant& grant)
{
    out.u32(grant.object);
    out.u32(grant.epoch);
    out.u32(grant.confirmed);
}

bool read_batched_state(Reader& in, Reader& entry, bool& delta)
{
    std::uint16_t length;
    if (!in.u16(length)) return false;
    delta = (length & batch_word_flag) != 0;
    const auto size = length & ~batch_word_flag;
    return size >= 18 && in.slice(size, entry);
}

}

ObjectAuthority::ObjectAuthority(Transport& link, const Schemas& schemas, Lookup lookup,
                                 Commit commit, Changed changed, Known known, InstanceMode instance_mode)
    : link_(link), schemas_(schemas), lookup_(std::move(lookup)), commit_(std::move(commit)),
      changed_(std::move(changed)), known_(std::move(known)), instance_mode_(std::move(instance_mode)) {}

bool ObjectAuthority::register_model(SchemaId id, std::uint16_t version, const std::vector<FieldId>& fields,
                                     std::size_t max_recovery, Validate validate)
{
    const auto* schema = schemas_.get(id);
    if (round_ || !schema || !version || fields.empty() || fields.size() > schema->fields.size() ||
        models_.size() >= max_models || max_recovery > game_payload_limit - 40) return false;
    Model model{{}, {}, max_recovery, schema_fingerprint(*schema), std::move(validate)};
    auto mix = [&](std::uint32_t word) { model.signature = (model.signature ^ word) * 16777619u; };
    mix(schema->version);
    mix(version);
    mix(static_cast<std::uint32_t>(max_recovery));
    FieldId previous = 0;
    auto bytes = max_recovery + 40;
    for (const auto field : fields) {
        const auto it = std::find_if(schema->fields.begin(), schema->fields.end(),
                                     [field](Field f) { return f.id == field; });
        if (field <= previous || it == schema->fields.end()) return false;
        previous = field;
        model.slots.push_back(static_cast<std::size_t>(it - schema->fields.begin()));
        model.fields.push_back(*it);
        bytes += field_capacity(*it);
        if (bytes > game_payload_limit) return false;
        mix(field);
    }
    model.layout.initialize(model.fields);
    return models_.emplace(id, std::move(model)).second;
}

void ObjectAuthority::reset(RoomId room, RoundId round, PlayerId local, Peer coordinator, PlayerId master)
{
    master_=master;
    grants_.clear();
    writable_words_.clear();
    accepted_words_.clear();
    receive_buffers_ = {};
    word_deltas_ = false;
    word_state_probe_.reset();
    state_batches_ = false;
    state_batch_probe_.reset();
    awaiting_object_.clear();
    restoring_writer_.clear();
    removed_.clear();
    displays_.clear();
    peers_.clear();
    pending_.clear();
    outgoing_.clear();
    predicted_.clear();
    local_step_ = 0;
    claim_peers_.clear();
    last_transfer_.clear();
    last_claim_probe_.reset();
    request_sequence_ = 0;
    claim_supported_ = false;
    room_ = room;
    round_ = round;
    local_ = local;
    coordinator_ = coordinator;
    reply_pending_ = false;
}

void ObjectAuthority::attach(Peer peer, PlayerId player, bool writable)
{
    if (round_ && local_ == master_ && peer && player && player != master_ &&
        std::none_of(peers_.begin(), peers_.end(), [player](const auto& entry) {
            return entry.second.player == player;
        }))
        peers_.try_emplace(peer, Participant{player, false, {}, {}, 0, writable});
}

void ObjectAuthority::detach(Peer peer, Tick now)
{
    const auto it = peers_.find(peer);
    if (it == peers_.end()) return;
    const auto player = it->second.player;
    peers_.erase(it);
    claim_peers_.erase(peer);
    std::vector<ObjectId> owned;
    for (const auto& [id, grant] : grants_) if (grant.writer == player) owned.push_back(id);
    for (const auto id : owned) {
        auto current = grants_.find(id);
        if (current == grants_.end()) continue;
        const auto policy = policies_.find(current->second.schema);
        if (policy != policies_.end() && mode(id, current->second.schema) == ClaimMode::PlayerAttached) {
            policy->second.destroy(id, now);
            forget(id);
        } else assign(id, master_, now);
    }
    maintain_claims(now);
}

void ObjectAuthority::forget(ObjectId object)
{
    if (local_ != master_) removed_.insert(object);
    else for (auto& [peer, participant] : peers_) participant.receipts.erase(object);
    unobserve(object);
}

void ObjectAuthority::unobserve(ObjectId object)
{
    if (auto it = pending_.find(object); it != pending_.end()) {
        finish_claim(it->second.claim, ClaimStatus::Unavailable);
        pending_.erase(it);
    }
    resolve_prediction(object, false);
    auto cancelled = outgoing_.extract(object);
    if (!cancelled.empty() && result_) result_(cancelled.mapped(), ClaimStatus::Unavailable);
    restoring_writer_.erase(object);
    last_transfer_.erase(object);
    if (grants_.erase(object) && spatial_writer_changed_) spatial_writer_changed_(object, 0);
    writable_words_.erase(object);
    accepted_words_.erase(object);
    awaiting_object_.erase(object);
    displays_.erase(object);
    for (auto& [peer, member] : peers_) member.delivered.erase(object);
}

bool ObjectAuthority::supports(PlayerId player, SchemaId schema) const
{
    for (const auto& [peer, member] : peers_)
        if (member.player == player) return member.writable && member.models.count(schema) != 0;
    return false;
}

const ObjectGrant* ObjectAuthority::grant(ObjectId id) const
{
    const auto it = grants_.find(id);
    return it == grants_.end() ? nullptr : &it->second;
}

PlayerId ObjectAuthority::writer(ObjectId id) const
{
    const auto* g = grant(id);
    return g ? g->writer : default_writer();
}

void ObjectAuthority::notify(const ObjectGrant& current)
{
    if (changed_) changed_(current);
}

bool ObjectAuthority::assign(ObjectId id, PlayerId writer, Tick now, const Bytes& recovery, Tick source_step)
{
    if (!round_ || local_ != master_ || !writer) return false;
    const Object* obj = lookup_(id);
    if (!obj || !models_.count(obj->schema) || (writer != master_ && !supports(writer, obj->schema)))
        return false;
    const auto policy=mode(id, obj->schema);
    if (writer!=master_ && policy && master_only(*policy)) return false;
    if (writer != master_ && known_ && std::none_of(peers_.begin(), peers_.end(), [&](const auto& peer) {
        return peer.second.player == writer && known_(peer.first, id);
    })) return false;
    const auto& model = models_.at(obj->schema);
    const auto* prior = grant(id);
    if (writer == this->writer(id)) return true;
    if ((prior && prior->epoch == UINT32_MAX) || recovery.size() > model.max_recovery) return false;
    ObjectGrant next;
    next.object = id;
    next.schema = obj->schema;
    next.writer = writer;
    next.epoch = prior ? prior->epoch + 1 : 1;
    next.baseline_revision = obj->revision;
    next.committed_at = now;
    const bool from_client = prior && prior->writer != master_;
    next.sample.source_step = from_client ? prior->sample.source_step : source_step;
    next.sample.recovery = from_client ? prior->sample.recovery : recovery;
    for (const auto slot : model.slots) next.sample.values.push_back(obj->values.at(slot));
    grants_[id] = std::move(next);
    accepted_words_.erase(id);
    restoring_writer_.erase(id);
    if (policies_.count(obj->schema)) last_transfer_[id] = now;
    if (spatial_writer_changed_) spatial_writer_changed_(id, writer == master_ ? 0 : writer);
    notify(grants_.at(id));
    return true;
}

template<class Sample>
bool ObjectAuthority::submit_values(ObjectId id, std::uint32_t epoch, Tick step, Sample&& values, Bytes recovery)
{
    auto it = grants_.find(id);
    if (!round_ || local_ == master_ || it == grants_.end()) return false;
    auto& g = it->second;
    const auto* obj = lookup_(id);
    if (g.writer != local_ || g.epoch != epoch || !obj || obj->schema != g.schema ||
        g.sample.sequence == UINT32_MAX || (step && step < g.sample.source_step)) return false;
    const auto& model = models_.at(g.schema);
    if (recovery.size() > model.max_recovery || values.size() != model.fields.size()) return false;
    for (std::size_t i = 0; i < values.size(); ++i)
        if (!valid_value(model.fields[i], values[i])) return false;
    model.layout.pack(model.fields, values, sampled_words_);
    writable_words_.at(id).stage(sampled_words_);
    g.sample.values = std::forward<Sample>(values);
    g.sample.recovery = std::move(recovery);
    g.sample.source_step = step;
    ++g.sample.sequence;
    ++submissions_;
    return true;
}

bool ObjectAuthority::submit(ObjectId id, std::uint32_t epoch, Tick step, Values values, Bytes recovery)
{
    return submit_values(id, epoch, step, std::move(values), std::move(recovery));
}

bool ObjectAuthority::submit_scene_sample(ObjectId id, std::uint32_t epoch, const Values& values)
{
    return submit_values(id, epoch, 0, values, {});
}

void ObjectAuthority::merge(const Model& model, const Values& fields, Values& target)
{
    for (std::size_t i = 0; i < fields.size(); ++i) target.at(model.slots[i]) = fields[i];
}

bool ObjectAuthority::overlay(Object& view) const
{
    auto predicted = predicted_.find(view.id);
    if (predicted != predicted_.end() && predicted->second.pending) {
        merge(models_.at(view.schema), predicted->second.pending->values, view.values);
        return true;
    }
    const auto* g = grant(view.id);
    if (!g || local_ == master_ || view.schema != g->schema ||
        (g->writer != local_ && view.revision >= g->baseline_revision)) return false;
    merge(models_.at(g->schema), g->sample.values, view.values);
    return true;
}

Object ObjectAuthority::presentation(const Object& canonical)
{
    Object result = canonical;
    const auto* g = grant(result.id);
    if (local_ == master_ || (!g && !predicted_.count(result.id))) return result;
    overlay(result);
    const auto epoch = g ? g->epoch : 0;
    auto& display = displays_[result.id];
    if (display.epoch != epoch || display.values != result.values) {
        display.revision = std::max(display.revision, canonical.revision) + 1;
        display.epoch = epoch;
        display.values = result.values;
    }
    result.revision = display.revision;
    return result;
}

void ObjectAuthority::encode_sample(Writer& out, const AuthoritySample& sample)
{
    out.u32(sample.sequence);
    out.u32(sample.source_step);
    for (const auto& value : sample.values) write_value(out, value);
    out.u16(static_cast<std::uint16_t>(sample.recovery.size()));
    out.append(sample.recovery);
}

bool ObjectAuthority::decode_sample(Reader& in, const Model& model, AuthoritySample& sample)
{
    if (!in.u32(sample.sequence) || !in.u32(sample.source_step)) return false;
    sample.values.resize(model.fields.size());
    for (std::size_t i = 0; i < model.fields.size(); ++i)
        if (!read_value(in, model.fields[i], sample.values[i])) return false;
    std::uint16_t bytes;
    return in.u16(bytes) && bytes <= model.max_recovery && in.read(bytes, sample.recovery) && in.done();
}

void ObjectAuthority::receive_grant(Reader& in)
{
    ObjectGrant next;
    std::uint32_t signature;
    if (!in.u32(next.object) || !next.object || !in.u16(next.schema) || !in.u32(signature) ||
        !in.u32(next.writer) || !next.writer || !in.u32(next.epoch) || !next.epoch ||
        !in.u32(next.baseline_revision) || !models_.count(next.schema)) return;
    const auto* obj = lookup_(next.object);
    if (removed_.count(next.object) || (obj && obj->schema != next.schema)) return;
    const auto* old = grant(next.object);
    auto& model = models_.at(next.schema);
    if ((old && next.epoch <= old->epoch) || signature != model.signature ||
        !decode_sample(in, model, next.sample)) return;
    next.confirmed = next.sample.sequence;
    const auto id = next.object;
    if (next.writer == local_) {
        writable_words_[id].initialize(model.layout, model.fields, next.sample.values, next.sample.sequence);
    } else {
        writable_words_.erase(id);
    }
    grants_[id] = std::move(next);
    if (obj && policies_.count(obj->schema)) last_transfer_[id] = local_step_;
    const auto prediction = predicted_.find(id);
    if (prediction != predicted_.end() && grants_.at(id).epoch > prediction->second.epoch)
        resolve_prediction(id, grants_.at(id).writer == local_);
    if (obj) {
        awaiting_object_.erase(id);
        notify(grants_.at(id));
    } else awaiting_object_.insert(id);
}

void ObjectAuthority::receive_state(Peer peer, PlayerId player, Reader& in, Tick now, bool delta, Receipts* receipts)
{
    ObjectId id;
    std::uint32_t epoch;
    if (!in.u32(id) || !in.u32(epoch)) return;
    const auto grant = grants_.find(id);
    if (grant == grants_.end()) return;
    auto& g = grant->second;
    if (player != g.writer || g.epoch != epoch) return;
    const auto* obj = lookup_(id);
    if (!obj || (known_ && !known_(peer, id))) return;
    auto& model = models_.at(g.schema);
    // Keep this decode isolated when a validation or commit callback receives another sample.
    auto buffers = std::exchange(receive_buffers_, {});
    struct RecycleBuffers {
        ReceiveBuffers& spare;
        ReceiveBuffers& used;
        ~RecycleBuffers() { spare = std::move(used); }
    } recycle{receive_buffers_, buffers};
    auto& next = buffers.sample;
    auto& words = buffers.words;
    if (delta) {
        if (!in.u32(next.sequence) || !next.sequence || !in.u32(next.source_step)) return;
        const bool newer = next.sequence > g.confirmed;
        if (newer) {
            const auto cached = accepted_words_.find(id);
            if (cached == accepted_words_.end()) model.layout.pack(model.fields, g.sample.values, words);
            else words = cached->second;
        }
        std::uint64_t mask = 0;
        std::uint16_t bytes;
        if (!read_word_changes(in, model.layout, newer ? &words : nullptr, mask, true) ||
            !in.u16(bytes) || bytes > model.max_recovery || !in.read(bytes, next.recovery) || !in.done()) return;
        if (newer) {
            next.values = g.sample.values;
            for (std::size_t field = 0; field < model.fields.size(); ++field) {
                if ((mask & (std::uint64_t{1} << field)) &&
                    !unpack_words(model.fields[field], words.data() + model.layout.offsets[field], next.values[field]))
                    return;
            }
        }
    } else if (!decode_sample(in, model, next) || !next.sequence) return;
    if (next.sequence > g.confirmed) {
        if ((next.source_step && next.source_step < g.sample.source_step) ||
            (model.validate && !model.validate(*obj, g.sample, next, now))) {
            assign(id, master_, now);
            return;
        }
        if (model.slots.size() == obj->values.size()) {
            // Both layouts register fields in schema order. A complete model's
            // validated words can be used as the canonical snapshot sample.
            const bool committed = delta && commit_words_ ? commit_words_(id, next.values, now, words)
                                                         : commit_(id, next.values, now);
            if (!committed) return;
        } else {
            auto& updated = buffers.merged;
            updated = obj->values;
            merge(model, next.values, updated);
            if (!commit_(id, updated, now)) return;
        }
        g.confirmed = next.sequence;
        g.committed_at = now;
        g.baseline_revision = lookup_(id)->revision;
        std::swap(g.sample, next);
        if (delta) accepted_words_[id].swap(words);
        else accepted_words_.erase(id);
        ++acceptances_;
    }
    if (receipts) {
        if (!receipts->insert_or_assign(id, PendingReceipt{g.epoch, g.confirmed}).second)
            ++receipt_coalesces_;
    } else {
        Writer ack;
        ack.reserve(game_header_size + receipt_size);
        write_game_header(ack, GameMessage::AuthorityAck, room_, round_);
        write_receipt(ack, g);
        link_.send(peer, Lane::Game, ack.bytes());
    }
}

void ObjectAuthority::receive_state_batch(Peer peer, Participant& participant, Reader in, Tick now)
{
    // Check all boundaries before applying any record from the batch.
    Reader scan = in, entry = in;
    bool delta;
    while (!scan.done()) {
        if (!read_batched_state(scan, entry, delta)) return;
    }
    while (!in.done()) {
        if (!read_batched_state(in, entry, delta)) return;
        receive_state(peer, participant.player, entry, now, delta, &participant.receipts);
    }
}

std::size_t ObjectAuthority::initial_receipt_budget(Peer peer, std::size_t available)
{
    const auto found = peers_.find(peer);
    if (!available || found == peers_.end() || found->second.receipts.empty()) return 0;
    if (available > 1) return std::min(max_receipt_packets, available - 1);
    auto& first = found->second.receipt_first;
    const bool receipt_first = first;
    first = !first;
    return receipt_first ? 1 : 0;
}

std::size_t ObjectAuthority::flush_receipts(Peer peer, std::size_t budget)
{
    if (!budget) return 0;
    const auto found = peers_.find(peer);
    if (found == peers_.end()) return 0;
    auto& participant = found->second;
    auto& receipts = participant.receipts;
    std::size_t sent = 0;
    while (sent < budget && !receipts.empty()) {
        auto first = receipts.upper_bound(participant.receipt_cursor);
        if (first == receipts.end()) first = receipts.begin();
        auto next = first;
        ObjectId last = first->first;
        auto packet = game_packet(GameMessage::AuthorityAckBatch, room_, round_);
        while (next != receipts.end() && packet.bytes().size() + receipt_size <= game_payload_limit) {
            const auto& [object, receipt] = *next++;
            packet.u32(object);
            packet.u32(receipt.epoch);
            packet.u32(receipt.sequence);
            last = object;
        }
        if (!link_.send(peer, Lane::Game, packet.bytes())) break;
        receipts.erase(first, next);
        participant.receipt_cursor = last;
        ++sent;
    }
    return sent;
}

void ObjectAuthority::receive_ack(Reader& in)
{
    ObjectId id;
    std::uint32_t epoch, sequence;
    if (!in.u32(id) || !in.u32(epoch) || !in.u32(sequence)) return;
    const auto g = grants_.find(id);
    if (g != grants_.end() && g->second.writer == local_ && g->second.epoch == epoch &&
        sequence <= g->second.sample.sequence)
        g->second.confirmed = std::max(g->second.confirmed, sequence);
}

bool ObjectAuthority::receive(const Event& event, Tick now)
{
    if (event.type != EventType::Message || event.payload.empty() || !is_authority_message(event.payload[0]))
        return false;
    Reader in(event.payload);
    GameMessage type;
    if (!read_game_header(in, type, room_, round_)) return true;
    if (type >= GameMessage::ClaimHello && type <= GameMessage::ClaimResult) {
        const auto lane = type == GameMessage::ClaimHello ? Lane::Game : Lane::Control;
        if (event.lane == lane) receive_claim(event.peer, type, in, now);
        return true;
    }
    const bool control = type <= GameMessage::AuthorityGrant;
    if ((event.lane == Lane::Control) != control) return true;
    if (local_ == master_) {
        auto peer = peers_.find(event.peer);
        if (peer == peers_.end()) return true;
        if (type == GameMessage::AuthorityState || type == GameMessage::AuthorityStateWords) {
            receive_state(event.peer, peer->second.player, in, now, type == GameMessage::AuthorityStateWords,
                          peer->second.state_batches ? &peer->second.receipts : nullptr);
        } else if (type == GameMessage::AuthorityBatchHello && in.done() && link_.reliable_ordered(Lane::Game)) {
            peer->second.state_batches = true;
            const auto reply = game_packet(GameMessage::AuthorityBatchSupported, room_, round_);
            link_.send(event.peer, Lane::Game, reply.bytes());
        } else if (type == GameMessage::AuthorityStateBatch && peer->second.state_batches &&
                   event.payload.size() <= game_payload_limit) {
            receive_state_batch(event.peer, peer->second, in, now);
        } else if (type == GameMessage::AuthorityModels && peer->second.offered) {
            std::uint16_t version, count;
            if (!in.u16(version) || version != protocol_version || !in.u16(count) || in.remaining() != count * 6u)
                return true;
            std::map<SchemaId, std::uint32_t> compatible;
            for (unsigned i = 0; i < count; ++i) {
                SchemaId id;
                std::uint32_t signature;
                if (!in.u16(id) || !in.u32(signature)) return true;
                const auto model = models_.find(id);
                if (model != models_.end() && model->second.signature == signature)
                    compatible.emplace(id, signature);
            }
            peer->second.models = std::move(compatible);
        }
    } else if (event.peer == coordinator_) {
        if (type == GameMessage::AuthorityGrant) receive_grant(in);
        else if (type == GameMessage::AuthorityOffer) {
            std::uint16_t version;
            reply_pending_ = in.u16(version) && version == protocol_version && in.done();
        } else if (type == GameMessage::AuthorityBatchSupported && state_batch_probe_ && in.done()) {
            state_batches_ = true;
        } else if (type == GameMessage::AuthorityAck && in.remaining() == receipt_size) {
            receive_ack(in);
        } else if (type == GameMessage::AuthorityAckBatch && state_batches_ &&
                   event.payload.size() <= game_payload_limit && in.remaining() % receipt_size == 0) {
            while (!in.done()) receive_ack(in);
        }
    }
    return true;
}

bool ObjectAuthority::negotiate_word_state(AuthorityState& state, Peer peer, GameMessage type, Reader& payload)
{
    if (type != GameMessage::WordStateHello && type != GameMessage::WordStateSupported) return false;
    if (!round_ || !payload.done()) return true;
    if (local_ == master_ && type == GameMessage::WordStateHello && state.enable_word_deltas(peer)) {
        const auto reply = game_packet(GameMessage::WordStateSupported, room_, round_);
        link_.send(peer, Lane::Game, reply.bytes());
    } else if (local_ != master_ && peer == coordinator_ && type == GameMessage::WordStateSupported) {
        word_deltas_ = true;
    }
    return true;
}

void ObjectAuthority::probe_word_state(Tick send_tick)
{
    if (!round_ || !coordinator_ || local_ == master_) return;
    if (!word_deltas_ && (!word_state_probe_ || send_tick - *word_state_probe_ >= 30)) {
        const auto hello = game_packet(GameMessage::WordStateHello, room_, round_);
        if (link_.send(coordinator_, Lane::Game, hello.bytes())) word_state_probe_ = send_tick;
    }
    if (!state_batches_ && link_.reliable_ordered(Lane::Game) &&
        (!state_batch_probe_ || send_tick - *state_batch_probe_ >= 30)) {
        const auto hello = game_packet(GameMessage::AuthorityBatchHello, room_, round_);
        if (link_.send(coordinator_, Lane::Game, hello.bytes())) state_batch_probe_ = send_tick;
    }
}

GameMessage ObjectAuthority::encode_state(Writer& out, Writer& delta, const ObjectGrant& g)
{
    out.u32(g.object);
    out.u32(g.epoch);
    auto& state = writable_words_.at(g.object);
    state.scan(g.sample.sequence);
    if (word_deltas_) {
        delta.clear();
        std::uint32_t count = 0;
        std::size_t next_word = 0;
        for (std::size_t i = 0; i < state.words.size(); ++i) {
            if (state.revisions[i] <= g.confirmed) continue;
            write_word_change(delta, std::uint32_t(i - next_word), state.words[i]);
            ++count;
            next_word = i + 1;
        }
        const auto& model = models_.at(g.schema);
        std::size_t field_bytes = 0;
        for (std::size_t i = 0; i < model.fields.size(); ++i)
            field_bytes += value_size(model.fields[i], g.sample.values[i]);
        if (delta.bytes().size() + word_varuint_size(count) <= field_bytes) {
            out.u32(g.sample.sequence);
            out.u32(g.sample.source_step);
            write_word_varuint(out, count);
            out.append(delta.bytes());
            out.u16(static_cast<std::uint16_t>(g.sample.recovery.size()));
            out.append(g.sample.recovery);
            return GameMessage::AuthorityStateWords;
        }
    }
    encode_sample(out, g.sample);
    return GameMessage::AuthorityState;
}

void ObjectAuthority::flush(Tick now, AuthorityState* replicas, Tick retry_ticks)
{
    if (!room_ || !round_ || !local_) return;
    local_step_ = now;
    maintain_claims(now);
    if (local_ == master_) {
        for (auto& [peer, member] : peers_) {
            if (!member.offered) {
                auto offer = game_packet(GameMessage::AuthorityOffer, room_, round_);
                offer.u16(protocol_version);
                member.offered = link_.send(peer, Lane::Control, offer.bytes());
            }
            for (const auto& [id, g] : grants_) {
                if (known_ && !known_(peer, id)) {
                    member.delivered.erase(id);
                    if (g.writer == member.player && !restoring_writer_.count(id))
                        assign(id, master_, std::max(now, g.committed_at));
                    continue;
                }
                if (g.writer == member.player && !restoring_writer_.empty()) restoring_writer_.erase(id);
                const auto delivered = member.delivered.find(id);
                if (delivered != member.delivered.end() && delivered->second == g.epoch) continue;
                if (!member.models.count(g.schema)) continue;
                const auto* obj = lookup_(id);
                if (!obj) continue;
                auto w = game_packet(GameMessage::AuthorityGrant, room_, round_);
                w.u32(id);
                w.u16(g.schema);
                w.u32(models_.at(g.schema).signature);
                w.u32(g.writer);
                w.u32(g.epoch);
                w.u32(g.baseline_revision);
                encode_sample(w, g.sample);
                if (link_.send(peer, Lane::Control, w.bytes())) {
                    member.delivered[id] = g.epoch;
                    if (replicas) replicas->require_baseline(peer, id, g.baseline_revision);
                }
            }
        }
        return;
    }
    for (auto it = awaiting_object_.begin(); it != awaiting_object_.end();) {
        const auto* object = lookup_(*it);
        if (!object) { ++it; continue; }
        const auto id = *it;
        it = awaiting_object_.erase(it);
        if (object->schema == grants_.at(id).schema) notify(grants_.at(id));
        else forget(id);
    }
    if (reply_pending_) {
        auto reply = game_packet(GameMessage::AuthorityModels, room_, round_);
        reply.u16(protocol_version);
        reply.u16(static_cast<std::uint16_t>(models_.size()));
        for (const auto& [id, model] : models_) { reply.u16(id); reply.u32(model.signature); }
        reply_pending_ = !link_.send(coordinator_, Lane::Control, reply.bytes());
    }
    Writer delta, record, packet;
    std::array<ObjectGrant*, game_payload_limit / 18> offered;
    std::size_t offers = 0;
    const auto flush_batch = [&] {
        if (packet.bytes().empty()) return;
        if (link_.send(coordinator_, Lane::Game, packet.bytes())) {
            for (std::size_t i = 0; i < offers; ++i) {
                auto& grant = *offered[i];
                grant.offered_sequence = grant.sample.sequence;
                grant.offered_at = now;
            }
        }
        offers = 0;
        packet.clear();
    };
    for (auto& [id, g] : grants_) {
        if (g.writer != local_ || g.confirmed == g.sample.sequence || !lookup_(id)) continue;
        if (retry_ticks && g.offered_sequence == g.sample.sequence && now - g.offered_at < retry_ticks) continue;
        record.clear();
        const auto type = encode_state(record, delta, g);
        const auto length = record.bytes().size();
        const auto entry_size = length + sizeof(std::uint16_t);
        if (state_batches_ && game_header_size + entry_size <= game_payload_limit) {
            if (packet.bytes().size() + entry_size > game_payload_limit) flush_batch();
            if (packet.bytes().empty()) write_game_header(packet, GameMessage::AuthorityStateBatch, room_, round_);
            const auto flags = type == GameMessage::AuthorityStateWords ? batch_word_flag : 0;
            packet.u16(static_cast<std::uint16_t>(length | flags));
            packet.append(record.bytes());
            if (retry_ticks) offered[offers++] = &g;
        } else {
            flush_batch();
            write_game_header(packet, type, room_, round_);
            packet.append(record.bytes());
            if (retry_ticks) offered[offers++] = &g;
            flush_batch();
        }
    }
    flush_batch();
}

}
