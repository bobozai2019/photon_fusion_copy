#include "state/replication.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace netcode {
namespace {

constexpr std::size_t snapshot_header_size = 17 + 4 + 1;
constexpr std::size_t max_snapshot_records = 96;
constexpr std::uint8_t live = 1, removed = 2;

std::uint64_t full_mask(std::size_t count)
{
    return count == 64 ? UINT64_MAX : (std::uint64_t{1} << count) - 1;
}

std::uint32_t schema_fingerprint(const Schema& schema)
{
    std::uint32_t hash = 2166136261u;
    for (const auto& field : schema.fields) {
        hash = (hash ^ field.id) * 16777619u;
        hash = (hash ^ static_cast<std::uint32_t>(field.type)) * 16777619u;
    }
    return hash;
}

Tick progress_at(const std::map<ObjectId, Tick>& progress, ObjectId id)
{
    const auto it = progress.find(id);
    return it == progress.end() ? 0 : it->second;
}

bool valid_value(FieldType type, const Value& value)
{
    if (type == FieldType::Flag) return std::holds_alternative<bool>(value);
    const auto* number = std::get_if<float>(&value);
    return number && std::isfinite(*number);
}

void write_value(Writer& writer, const Value& value)
{
    if (const auto* number = std::get_if<float>(&value)) writer.f32(*number);
    else writer.u8(std::get<bool>(value) ? 1 : 0);
}

bool read_value(Reader& reader, FieldType type, Value& value)
{
    if (type == FieldType::Float) {
        float number;
        if (!reader.f32(number) || !std::isfinite(number)) return false;
        value = number;
        return true;
    }
    std::uint8_t flag;
    if (!reader.u8(flag) || flag > 1) return false;
    value = flag != 0;
    return true;
}

}

bool Schemas::add(Schema schema)
{
    if (!schema.id || !schema.version || schema.fields.empty() || schema.fields.size() > 64) return false;
    FieldId previous = 0;
    for (const auto& field : schema.fields) {
        if (field.id <= previous || (field.type != FieldType::Float && field.type != FieldType::Flag)) {
            return false;
        }
        previous = field.id;
    }
    return schemas_.emplace(schema.id, std::move(schema)).second;
}

const Schema* Schemas::get(SchemaId id) const
{
    const auto it = schemas_.find(id);
    return it == schemas_.end() ? nullptr : &it->second;
}

bool Schemas::valid(SchemaId id, const Values& values) const
{
    const auto* schema = get(id);
    return schema && schema->fields.size() == values.size() &&
        std::equal(schema->fields.begin(), schema->fields.end(), values.begin(), [](Field f, const Value& v) {
            return valid_value(f.type, v);
        });
}

void AuthorityState::clear()
{
    entries_.clear();
    peers_.clear();
    highest_id_ = 0;
    changes_ = 0;
}

bool AuthorityState::create(ObjectId id, SchemaId schema, PlayerId owner, const Values& values, Tick tick)
{
    if (id <= highest_id_ || !tick || !schemas_.valid(schema, values)) return false;
    highest_id_ = id;
    entries_.emplace(id, Entry{{id, schema, owner, tick, 1, values}, std::vector<Tick>(values.size(), 1)});
    return true;
}

bool AuthorityState::capture(ObjectId id, const Values& values, Tick tick)
{
    const auto it = entries_.find(id);
    if (it == entries_.end() || it->second.removed) return false;
    auto& entry = it->second;
    auto& object = entry.object;
    if (tick < object.sample_tick || !schemas_.valid(object.schema, values)) return false;
    if (object.values == values) return true;
    ++changes_;
    ++object.revision;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (object.values[i] != values[i]) entry.changes[i] = object.revision;
    }
    object.values = values;
    object.sample_tick = tick;
    return true;
}

bool AuthorityState::destroy(ObjectId id, Tick tick)
{
    const auto it = entries_.find(id);
    if (it == entries_.end() || it->second.removed || tick < it->second.object.sample_tick) return false;
    auto& entry = it->second;
    entry.removed = true;
    ++entry.object.revision;
    entry.object.sample_tick = tick;
    prune();
    return true;
}

const Object* AuthorityState::get(ObjectId id) const
{
    const auto it = entries_.find(id);
    return it == entries_.end() || it->second.removed ? nullptr : &it->second.object;
}

std::vector<ObjectId> AuthorityState::active() const
{
    std::vector<ObjectId> result;
    for (const auto& [id, entry] : entries_) {
        if (!entry.removed) result.push_back(id);
    }
    return result;
}

void AuthorityState::attach(Peer peer)
{
    if (!peer || peers_.count(peer)) return;
    auto& progress = peers_[peer];
    for (const auto& [id, entry] : entries_) {
        if (!entry.removed) progress.baseline[id] = entry.object.revision;
    }
}

void AuthorityState::detach(Peer peer)
{
    peers_.erase(peer);
    prune();
}

bool AuthorityState::baseline_complete(Peer peer) const
{
    const auto found = peers_.find(peer);
    if (found == peers_.end()) return false;
    const auto& progress = found->second;
    return std::all_of(progress.baseline.begin(), progress.baseline.end(), [&](const auto& record) {
        return progress_at(progress.ack, record.first) >= record.second;
    });
}

std::vector<Bytes> AuthorityState::packets(Peer peer, RoomId room, RoundId round, Tick send_tick)
{
    auto& progress = peers_.at(peer);
    std::vector<Bytes> result;
    Writer records;
    std::uint8_t count = 0;
    const auto flush = [&] {
        if (!count) return;
        auto packet = game_packet(GameMessage::Snapshot, room, round);
        packet.u32(send_tick);
        packet.u8(count);
        packet.append(records.take());
        result.push_back(packet.take());
        count = 0;
    };
    for (const auto& [id, entry] : entries_) {
        const auto& object = entry.object;
        const Tick ack = progress_at(progress.ack, id);
        if (ack >= object.revision ||
            (entry.removed && !progress.sent.count(id) && !progress.baseline.count(id))) continue;
        Writer record;
        record.u32(id);
        record.u32(object.revision);
        record.u32(object.sample_tick);
        record.u8(entry.removed ? removed : live);
        if (!entry.removed) {
            record.u16(object.schema);
            const auto& schema = *schemas_.get(object.schema);
            record.u16(schema.version);
            record.u32(schema_fingerprint(schema));
            record.u32(object.owner);
            std::uint64_t mask = 0;
            for (std::size_t i = 0; i < entry.changes.size(); ++i) {
                if (entry.changes[i] > ack) mask |= std::uint64_t{1} << i;
            }
            record.u64(mask);
            for (std::size_t i = 0; i < object.values.size(); ++i) {
                if (mask & (std::uint64_t{1} << i)) write_value(record, object.values[i]);
            }
        }
        if (snapshot_header_size + records.bytes().size() + record.bytes().size() > game_payload_limit ||
            count == max_snapshot_records) flush();
        ++count;
        records.append(record.bytes());
        progress.sent[id] = object.revision;
    }
    flush();
    return result;
}

bool AuthorityState::acknowledge(Peer peer, Reader& payload)
{
    const auto it = peers_.find(peer);
    if (it == peers_.end()) return false;
    auto& progress = it->second;
    std::uint8_t count;
    if (!payload.u8(count) || !count || count > max_snapshot_records) return false;
    std::vector<std::pair<ObjectId, Tick>> acks;
    for (std::uint8_t i = 0; i < count; ++i) {
        ObjectId id;
        Tick revision;
        if (!payload.u32(id) || !payload.u32(revision) || !id || !revision) return false;
        acks.emplace_back(id, revision);
    }
    if (!payload.done()) return false;
    for (const auto& [id, revision] : acks) {
        if (revision <= progress_at(progress.sent, id)) {
            progress.ack[id] = std::max(revision, progress_at(progress.ack, id));
        }
    }
    prune();
    return true;
}

void AuthorityState::prune()
{
    for (auto it = entries_.begin(); it != entries_.end();) {
        const auto id = it->first;
        const bool pending = it->second.removed && std::any_of(peers_.begin(), peers_.end(),
            [&](const auto& pair) {
                const auto& progress = pair.second;
                return (progress.sent.count(id) || progress.baseline.count(id)) &&
                       progress_at(progress.ack, id) < it->second.object.revision;
            });
        if (!it->second.removed || pending) {
            ++it;
            continue;
        }
        for (auto& [peer, progress] : peers_) {
            progress.baseline.erase(id);
            progress.sent.erase(id);
            progress.ack.erase(id);
        }
        it = entries_.erase(it);
    }
}

void ReplicaState::clear()
{
    objects_.clear();
    removed_.clear();
}

bool ReplicaState::apply(Reader& payload, RoomId room, RoundId round, AppliedSnapshot& result)
{
    Tick send_tick;
    std::uint8_t count;
    if (!payload.u32(send_tick) || !send_tick || !payload.u8(count) || !count ||
        count > max_snapshot_records) return false;
    struct Update { Object object; bool erase; bool changed; };
    std::vector<Update> updates;
    std::set<ObjectId> seen;
    for (std::uint8_t i = 0; i < count; ++i) {
        Object o;
        std::uint8_t kind;
        if (!payload.u32(o.id) || !payload.u32(o.revision) || !payload.u32(o.sample_tick) ||
            !payload.u8(kind) || !o.id || !o.revision || !o.sample_tick ||
            !seen.insert(o.id).second) return false;
        const auto current = objects_.find(o.id);
        const Tick tombstone = progress_at(removed_, o.id);
        const Tick known = current == objects_.end() ? tombstone : current->second.revision;
        const bool newer = o.revision > known;
        if (kind == removed) {
            updates.push_back({o, true, newer});
            continue;
        }
        std::uint16_t version;
        std::uint32_t fingerprint;
        std::uint64_t mask;
        if (kind != live || !payload.u16(o.schema) || !payload.u16(version) || !payload.u32(fingerprint) ||
            !payload.u32(o.owner) || !payload.u64(mask)) return false;
        const auto* schema = schemas_.get(o.schema);
        if (!schema || schema->version != version || schema_fingerprint(*schema) != fingerprint || !mask ||
            (mask & ~full_mask(schema->fields.size()))) return false;
        if (newer && tombstone) return false;
        if (current != objects_.end()) {
            if (o.schema != current->second.schema || o.owner != current->second.owner) return false;
            o.values = current->second.values;
        } else {
            if (newer && mask != full_mask(schema->fields.size())) return false;
            o.values.resize(schema->fields.size());
        }
        for (std::size_t index = 0; index < schema->fields.size(); ++index) {
            if ((mask & (std::uint64_t{1} << index)) &&
                !read_value(payload, schema->fields[index].type, o.values[index])) return false;
        }
        updates.push_back({std::move(o), false, newer});
    }
    if (!payload.done()) return false;
    result = {};
    result.send_tick = send_tick;
    auto ack = game_packet(GameMessage::StateAck, room, round);
    ack.u8(count);
    for (auto& update : updates) {
        const auto id = update.object.id;
        ack.u32(id);
        ack.u32(update.object.revision);
        if (!update.changed) continue;
        if (update.erase) {
            removed_[id] = update.object.revision;
            objects_.erase(id);
        } else {
            objects_[id] = std::move(update.object);
        }
        result.changed.push_back(id);
    }
    result.ack = ack.take();
    return true;
}

}
