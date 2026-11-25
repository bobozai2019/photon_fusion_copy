#include "session/object_authority.hpp"

#include <algorithm>
#include <utility>

namespace netcode {
namespace {

constexpr std::uint16_t protocol_version = 1;
constexpr std::size_t max_models = (control_payload_limit - 21) / 6;

bool is_authority_message(std::uint8_t code)
{
    return (code >= static_cast<std::uint8_t>(GameMessage::AuthorityOffer) &&
            code <= static_cast<std::uint8_t>(GameMessage::AuthorityAck)) ||
           (code >= static_cast<std::uint8_t>(GameMessage::ClaimHello) &&
            code <= static_cast<std::uint8_t>(GameMessage::ClaimResult)) ||
           code == static_cast<std::uint8_t>(GameMessage::AuthorityStateWords);
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
    word_deltas_ = false;
    word_state_probe_.reset();
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
    grants_.erase(object);
    writable_words_.erase(object);
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
    return g ? g->writer : master_;
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
    restoring_writer_.erase(id);
    if (policies_.count(obj->schema)) last_transfer_[id] = now;
    notify(grants_.at(id));
    return true;
}

bool ObjectAuthority::submit(ObjectId id, std::uint32_t epoch, Tick step, Values values, Bytes recovery)
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
    g.sample = {g.sample.sequence + 1, step, std::move(values), std::move(recovery)};
    ++submissions_;
    return true;
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
    for (const auto field : model.fields) {
        Value value;
        if (!read_value(in, field, value)) return false;
        sample.values.push_back(value);
    }
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

void ObjectAuthority::send_ack(Peer peer, const ObjectGrant& g)
{
    auto ack = game_packet(GameMessage::AuthorityAck, room_, round_);
    ack.u32(g.object);
    ack.u32(g.epoch);
    ack.u32(g.confirmed);
    link_.send(peer, Lane::Game, ack.bytes());
}

void ObjectAuthority::receive_state(Peer peer, Reader& in, Tick now, bool delta)
{
    ObjectId id;
    std::uint32_t epoch;
    if (!in.u32(id) || !in.u32(epoch) || !grants_.count(id)) return;
    auto& g = grants_.at(id);
    const auto member = peers_.find(peer);
    const auto* obj = lookup_(id);
    if (!obj || member == peers_.end() || member->second.player != g.writer || g.epoch != epoch ||
        (known_ && !known_(peer, id))) return;
    auto& model = models_.at(g.schema);
    AuthoritySample next;
    if (delta) {
        if (!in.u32(next.sequence) || !next.sequence || !in.u32(next.source_step)) return;
        const bool newer = next.sequence > g.confirmed;
        Words words;
        if (newer) model.layout.pack(model.fields, g.sample.values, words);
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
        auto updated = obj->values;
        merge(model, next.values, updated);
        if (!commit_(id, updated, now)) return;
        g.confirmed = next.sequence;
        g.committed_at = now;
        g.baseline_revision = lookup_(id)->revision;
        g.sample = std::move(next);
        ++acceptances_;
    }
    send_ack(peer, g);
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
        if (type == GameMessage::AuthorityState || type == GameMessage::AuthorityStateWords)
            receive_state(event.peer, in, now, type == GameMessage::AuthorityStateWords);
        else if (type == GameMessage::AuthorityModels && peer->second.offered) {
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
        } else if (type == GameMessage::AuthorityAck) {
            ObjectId id;
            std::uint32_t epoch, sequence;
            if (!in.u32(id) || !in.u32(epoch) || !in.u32(sequence) || !in.done()) return true;
            auto g = grants_.find(id);
            if (g != grants_.end() && g->second.writer == local_ && g->second.epoch == epoch &&
                sequence <= g->second.sample.sequence)
                g->second.confirmed = std::max(g->second.confirmed, sequence);
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
    if (!round_ || !coordinator_ || local_ == master_ || word_deltas_ ||
        (word_state_probe_ && send_tick - *word_state_probe_ < 30)) return;
    const auto hello = game_packet(GameMessage::WordStateHello, room_, round_);
    if (link_.send(coordinator_, Lane::Game, hello.bytes())) word_state_probe_ = send_tick;
}

void ObjectAuthority::flush(Tick now, AuthorityState* replicas)
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
                if (g.writer == member.player) restoring_writer_.erase(id);
                if (!member.models.count(g.schema) || member.delivered[id] == g.epoch) continue;
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
    Writer delta;
    for (const auto& [id, g] : grants_) {
        if (g.writer != local_ || g.confirmed == g.sample.sequence || !lookup_(id)) continue;
        auto& state = writable_words_.at(id);
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
                auto packet = game_packet(GameMessage::AuthorityStateWords, room_, round_);
                packet.u32(id);
                packet.u32(g.epoch);
                packet.u32(g.sample.sequence);
                packet.u32(g.sample.source_step);
                write_word_varuint(packet, count);
                packet.append(delta.bytes());
                packet.u16(static_cast<std::uint16_t>(g.sample.recovery.size()));
                packet.append(g.sample.recovery);
                link_.send(coordinator_, Lane::Game, packet.bytes());
                continue;
            }
        }
        auto w = game_packet(GameMessage::AuthorityState, room_, round_);
        w.u32(id);
        w.u32(g.epoch);
        encode_sample(w, g.sample);
        link_.send(coordinator_, Lane::Game, w.bytes());
    }
}

}
