#include "state/replication.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace netcode {
namespace {

constexpr std::size_t snapshot_header_size = 17 + 4 + 1;
constexpr std::size_t max_snapshot_records = 96;
constexpr std::uint8_t live = 1, removed = 2, hidden = 3, scoped = 4, word_delta = 5, scoped_word_delta = 6;

std::uint64_t full_mask(std::size_t count)
{
    return count == 64 ? UINT64_MAX : (std::uint64_t{1} << count) - 1;
}

Tick progress_at(const std::map<ObjectId, Tick>& progress, ObjectId id)
{
    const auto it = progress.find(id);
    return it == progress.end() ? 0 : it->second;
}

}

std::uint32_t schema_fingerprint(const Schema& schema)
{
    std::uint32_t hash = 2166136261u;
    for (const auto& field : schema.fields) {
        hash = (hash ^ field.id) * 16777619u;
        hash = (hash ^ static_cast<std::uint32_t>(field.type)) * 16777619u;
        if (field.limit) hash = (hash ^ field.limit) * 16777619u;
    }
    return hash;
}

bool Schemas::add(Schema schema)
{
    if (!schema.id || !schema.version || schema.fields.empty() || schema.fields.size() > 64) return false;
    FieldId previous = 0;
    std::size_t bytes = snapshot_header_size + 33;
    for (const auto& field : schema.fields) {
        const auto capacity = field_capacity(field);
        bytes += capacity;
        if (field.id <= previous || !capacity || bytes > game_payload_limit) return false;
        previous = field.id;
    }
    schema.layout.initialize(schema.fields);
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
            return valid_value(f, v);
        });
}

void AuthorityState::clear()
{
    entries_.clear();
    peers_.clear();
    highest_id_ = 0;
    changes_ = 0;
}

AuthorityState::Entry::Entry(Object initial, const Schema& schema, Distribution scope)
    : object(std::move(initial)), distribution(std::move(scope))
{
    state.initialize(schema.layout, schema.fields, object.values, object.revision);
}

bool AuthorityState::create(ObjectId id, SchemaId schema, PlayerId owner, const Values& values, Tick tick,
                            SceneId scene, const std::vector<ObjectId>& dependencies, Distribution distribution)
{
    if (id <= highest_id_ || !tick || !distribution.send_divisor || !schemas_.valid(schema, values)) return false;
    if (dependencies.size() > 16 || !std::is_sorted(dependencies.begin(), dependencies.end()) ||
        std::adjacent_find(dependencies.begin(), dependencies.end()) != dependencies.end()) return false;
    for (const auto dep : dependencies) if (!get(dep)) return false;
    if (scene || !dependencies.empty()) {
        std::size_t bytes = snapshot_header_size + 38 + 4 * dependencies.size();
        for (const auto f : schemas_.get(schema)->fields) bytes += field_capacity(f);
        if (bytes > game_payload_limit) return false;
    }
    highest_id_ = id;
    entries_.emplace(id, Entry{{id, schema, owner, tick, 1, values, scene, dependencies},
                              *schemas_.get(schema), std::move(distribution)});
    for (auto& [peer, progress] : peers_) {
        progress.baseline[id] = 1;
        refresh(progress);
    }
    return true;
}

bool AuthorityState::capture(ObjectId id, const Values& values, Tick tick)
{
    const auto it = entries_.find(id);
    if (it == entries_.end() || it->second.removed) return false;
    auto& entry = it->second;
    auto& object = entry.object;
    if (tick < object.sample_tick || !schemas_.valid(object.schema, values)) return false;
    const auto* schema = schemas_.get(object.schema);
    schema->layout.pack(schema->fields, values, sampled_words_);
    if (!entry.state.stage(sampled_words_)) return true;
    ++changes_;
    ++object.revision;
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
    for (auto& [peer, progress] : peers_) refresh(progress);
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

void AuthorityState::attach(Peer peer, bool word_deltas)
{
    if (!peer || peers_.count(peer)) return;
    auto& progress = peers_[peer];
    progress.word_deltas = word_deltas;
    for (const auto& [id, entry] : entries_) {
        if (!entry.removed) progress.baseline[id] = entry.object.revision;
    }
    refresh(progress);
}

void AuthorityState::detach(Peer peer)
{
    peers_.erase(peer);
    prune();
}

bool AuthorityState::known(Peer peer, ObjectId id) const
{
    const auto it = peers_.find(peer);
    if (it == peers_.end() || !get(id) || it->second.hidden.count(id)) return false;
    return progress_at(it->second.ack, id) >= std::max<Tick>(1, progress_at(it->second.reentered, id));
}

std::uint16_t AuthorityState::send_divisor(const Entry& entry, const Progress& p) const
{
    const auto* source = &entry;
    while (source->distribution.kind == InterestKind::Inherit) {
        if (!source->scope_parent) return source->distribution.send_divisor;
        const auto parent = entries_.find(source->scope_parent);
        if (parent == entries_.end() || parent->second.removed) return 0;
        source = &parent->second;
    }
    const auto& group = source->distribution;
    if (group.kind == InterestKind::Global) return group.send_divisor;
    const auto& subscriptions = group.kind == InterestKind::Area ? p.areas : p.users;
    std::uint16_t rate = 0;
    auto match = [&](std::uint64_t key) {
        auto it = subscriptions.find(key);
        if (it != subscriptions.end() && (!rate || it->second < rate)) rate = it->second;
    };
    match(group.key);
    for (auto key : group.additional_keys) match(key);
    return rate ? std::max(rate, group.send_divisor) : 0;
}

void AuthorityState::refresh(Progress& p)
{
    // A newly attached parent can have a higher object ID than its child.
    // Resolve the whole visibility graph before emitting visibility transitions.
    std::map<ObjectId, size_t> waiting;
    std::map<ObjectId, std::vector<ObjectId>> dependents;
    std::vector<ObjectId> visible_objects;
    for (const auto& [id, entry] : entries_) {
        const auto& o = entry.object;
        if (entry.removed || !send_divisor(entry, p) || p.pending_scenes.count(o.scene) ||
            (p.scene && o.scene && p.scene != o.scene) || (p.selected && !p.selected->count(id))) continue;
        size_t required = o.dependencies.size() + (entry.scope_parent != 0);
        if (!required) visible_objects.push_back(id);
        else {
            waiting[id] = required;
            for (auto dep : o.dependencies) dependents[dep].push_back(id);
            if (entry.scope_parent) dependents[entry.scope_parent].push_back(id);
        }
    }
    for (size_t i = 0; i < visible_objects.size(); ++i) {
        auto children = dependents.find(visible_objects[i]);
        if (children != dependents.end()) for (auto child : children->second)
            if (!--waiting.at(child)) visible_objects.push_back(child);
    }
    const std::set<ObjectId> resolved(visible_objects.begin(), visible_objects.end());
    for (auto& [id, entry] : entries_) {
        auto& o = entry.object;
        if (entry.removed) continue;
        const bool visible = resolved.count(id) != 0;
        if (visible == !p.hidden.count(id)) continue;
        // Visibility transitions share the object's monotonic revision space.
        // Consequently an old datagram cannot undo a leave/re-enter transition.
        if (p.sent.count(id) || visible) ++o.revision;
        if (visible) {
            p.hidden.erase(id);
            p.reentered[id] = o.revision;
            p.baseline[id] = o.revision;
        } else {
            p.hidden[id] = o.revision;
            p.baseline.erase(id);
        }
    }
}

bool AuthorityState::set_scope_parent(ObjectId child, ObjectId parent)
{
    if (!get(child) || (parent && !get(parent))) return false;
    for (auto id = parent; id; id = entries_.at(id).scope_parent) {
        if (id == child || !get(id)) return false;
    }
    auto& current = entries_.at(child).scope_parent;
    if (current != parent) {
        current = parent;
        for (auto& entry : peers_) refresh(entry.second);
    }
    return true;
}

bool AuthorityState::set_interest(Peer peer, SceneId scene, std::optional<std::set<ObjectId>> selected)
{
    auto it = peers_.find(peer);
    if (it == peers_.end()) return false;
    it->second.scene = scene;
    it->second.selected = std::move(selected);
    refresh(it->second);
    return true;
}

bool AuthorityState::subscribe(Peer peer, InterestKind kind, std::set<std::uint64_t> keys)
{
    std::map<std::uint64_t, std::uint16_t> rates;
    for (auto key : keys) rates.emplace(key, 1);
    return subscribe_rates(peer, kind, std::move(rates));
}

bool AuthorityState::subscribe_rates(Peer peer, InterestKind kind, std::map<std::uint64_t, std::uint16_t> keys)
{
    for (auto& item : keys) if (!item.second) item.second = 1;
    auto it = peers_.find(peer);
    if (it == peers_.end() || (kind != InterestKind::Area && kind != InterestKind::User)) return false;
    auto& current = kind == InterestKind::Area ? it->second.areas : it->second.users;
    if (current != keys) {
        current = std::move(keys);
        refresh(it->second);
    }
    return true;
}

bool AuthorityState::set_distribution(ObjectId id, Distribution distribution)
{
    auto it = entries_.find(id);
    if (it == entries_.end() || it->second.removed || !distribution.send_divisor) return false;
    auto old = it->second.distribution;
    it->second.distribution = distribution;
    if (old.kind != distribution.kind || old.key != distribution.key || old.additional_keys != distribution.additional_keys)
        for (auto& [peer, progress] : peers_) refresh(progress);
    return true;
}

bool AuthorityState::set_quantum(SchemaId schema, FieldId id, float quantum)
{
    const auto* def=schemas_.get(schema);
    if (!def || !std::isfinite(quantum) || quantum < 0 || !entries_.empty()) return false;
    auto it=std::find_if(def->fields.begin(),def->fields.end(),[id](Field f){return f.id==id;});
    if (it==def->fields.end() || (it->type!=FieldType::Float && it->type!=FieldType::Vec2 && it->type!=FieldType::Vec3)) return false;
    if (quantum) quantization_[{schema,id}]=quantum; else quantization_.erase({schema,id});
    return true;
}
bool AuthorityState::has_quantization(SchemaId schema) const
{
    const auto it = quantization_.lower_bound({schema, 0});
    return it != quantization_.end() && it->first.first == schema;
}

void AuthorityState::enable_quantization(Peer peer, bool enabled)
{
    const auto it = peers_.find(peer);
    if (it == peers_.end() || it->second.quantize == enabled) return;
    auto& progress = it->second;
    progress.quantize = enabled;
    if (quantization_.empty()) return;
    for (auto& [id, entry] : entries_) {
        if (entry.removed || progress.hidden.count(id) || !progress.sent.count(id) ||
            !has_quantization(entry.object.schema)) continue;
        // An older projection can be in flight even if it has not been acknowledged yet.
        progress.required_baselines[id] = ++entry.object.revision;
    }
}

bool AuthorityState::enable_word_deltas(Peer peer)
{
    const auto it = peers_.find(peer);
    if (it == peers_.end()) return false;
    it->second.word_deltas = true;
    return true;
}

void AuthorityState::require_baseline(Peer peer, ObjectId id, Tick revision)
{
    const auto it = peers_.find(peer);
    const auto* object = get(id);
    if (it == peers_.end() || !object || revision > object->revision || revision <= progress_at(it->second.ack, id)) return;
    auto& required = it->second.required_baselines[id];
    required = std::max(required, revision);
}
bool AuthorityState::gate_scene(Peer peer, SceneId scene, bool ready)
{
    auto it = peers_.find(peer);
    if (!scene || it == peers_.end()) return false;
    auto& gate = it->second.pending_scenes;
    if (ready ? gate.erase(scene) != 0 : gate.insert(scene).second) refresh(it->second);
    return true;
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

std::vector<Bytes> AuthorityState::packets(Peer peer, RoomId room, RoundId round, Tick send_tick, Tick retry_ticks)
{
    auto& progress = peers_.at(peer);
    std::vector<Bytes> result;
    Writer records, record, data, delta;
    std::uint8_t count = 0;
    const auto flush = [&] {
        if (!count) return;
        auto packet = game_packet(GameMessage::Snapshot, room, round);
        packet.u32(send_tick);
        packet.u8(count);
        packet.append(records.bytes());
        result.push_back(packet.take());
        records.clear();
        count = 0;
    };
    for (auto& [id, entry] : entries_) {
        const auto& object = entry.object;
        if (!entry.removed) entry.state.scan(object.revision);
        const Tick ack = progress_at(progress.ack, id);
        const bool hide = !entry.removed && progress.hidden.count(id);
        const Tick revision = hide ? progress.hidden.at(id) : object.revision;
        if (ack >= revision || (hide && !progress.sent.count(id)) ||
            (entry.removed && !progress.sent.count(id) && !progress.baseline.count(id))) continue;
        if (retry_ticks && progress_at(progress.sent, id) == revision &&
            send_tick - progress_at(progress.last_sent_tick, id) < retry_ticks) continue;
        if (!entry.removed && !hide && !std::all_of(object.dependencies.begin(), object.dependencies.end(),
                [&](ObjectId dep) { return known(peer, dep); })) continue;
        const bool has_scope = object.scene || !object.dependencies.empty();
        auto kind = entry.removed ? removed : hide ? hidden : has_scope ? scoped : live;
        data.clear();
        if (!entry.removed && !hide) {
            const auto& schema = *schemas_.get(object.schema);
            const auto& offsets = schema.layout.offsets;
            std::uint64_t mask = full_mask(schema.fields.size());
            delta.clear();
            std::uint32_t dirty_count = 0;
            const auto required_baseline = std::max(progress_at(progress.reentered, id),
                                                     progress_at(progress.required_baselines, id));
            if (ack && ack >= required_baseline) {
                const auto rate = send_divisor(entry, progress);
                if (!rate || (rate > 1 && send_tick / rate == progress.last_sent_tick[id] / rate)) continue;
                const bool quantized = progress.quantize && has_quantization(object.schema);
                const bool use_words = progress.word_deltas && !quantized;
                std::size_t next_word = 0, field_bytes = 8;
                mask = 0;
                for (std::size_t field = 0; field < schema.fields.size(); ++field) {
                    bool dirty = false;
                    for (auto i = offsets[field]; i < offsets[field + 1]; ++i) {
                        if (entry.state.revisions[i] <= ack) continue;
                        dirty = true;
                        if (!use_words) break;
                        write_word_change(delta, std::uint32_t(i - next_word), entry.state.words[i]);
                        next_word = i + 1;
                        ++dirty_count;
                    }
                    if (!dirty) continue;
                    mask |= std::uint64_t{1} << field;
                    if (use_words) field_bytes += value_size(schema.fields[field], object.values[field]);
                }
                if (!mask) continue;
                if (use_words && delta.bytes().size() + word_varuint_size(dirty_count) <= field_bytes)
                    kind = has_scope ? scoped_word_delta : word_delta;
            }
            if (kind == word_delta || kind == scoped_word_delta) {
                write_word_varuint(data, dirty_count);
                data.append(delta.bytes());
            } else {
                data.u64(mask);
                for (std::size_t i = 0; i < object.values.size(); ++i) {
                    if (!(mask & (std::uint64_t{1} << i))) continue;
                    const auto quantum = progress.quantize ? quantization_.find({object.schema, schema.fields[i].id})
                                                            : quantization_.end();
                    if (quantum == quantization_.end()) {
                        write_value(data, object.values[i]);
                        continue;
                    }
                    auto value=object.values[i];
                    auto quantize=[q=double(quantum->second)](float& n) {
                        const auto rounded=std::round(double(n)/q)*q;
                        const auto result=static_cast<float>(rounded);
                        if (std::isfinite(result)) n=result;
                    };
                    if (auto* n=std::get_if<float>(&value)) quantize(*n);
                    if (auto* v=std::get_if<Vector2>(&value)) for(auto& n:*v) quantize(n);
                    if (auto* v=std::get_if<Vector3>(&value)) for(auto& n:*v) quantize(n);
                    write_value(data,value);
                }
            }
        }
        record.clear();
        record.u32(id);
        record.u32(revision);
        record.u32(object.sample_tick);
        record.u8(kind);
        if (!entry.removed && !hide) {
            if (has_scope) {
                record.u32(object.scene);
                record.u8(static_cast<std::uint8_t>(object.dependencies.size()));
                for (const auto dep : object.dependencies) record.u32(dep);
            }
            const auto& schema = *schemas_.get(object.schema);
            record.u16(object.schema);
            record.u16(schema.version);
            record.u32(schema_fingerprint(schema));
            record.u32(object.owner);
            record.append(data.bytes());
        }
        if (snapshot_header_size + records.bytes().size() + record.bytes().size() > game_payload_limit ||
            count == max_snapshot_records) flush();
        ++count;
        records.append(record.bytes());
        progress.sent[id] = revision;
        progress.last_sent_tick[id] = send_tick;
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
            const auto baseline = progress.required_baselines.find(id);
            if (baseline != progress.required_baselines.end() && revision >= baseline->second)
                progress.required_baselines.erase(baseline);
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
            progress.last_sent_tick.erase(id);
            progress.ack.erase(id);
            progress.hidden.erase(id);
            progress.reentered.erase(id);
            progress.required_baselines.erase(id);
        }
        it = entries_.erase(it);
    }
}

void ReplicaState::clear()
{
    objects_.clear();
    waiting_for_dependencies_.clear();
    words_.clear();
    loading_.clear();
    removed_.clear();
    hidden_.clear();
}

bool ReplicaState::apply(Reader& payload, RoomId room, RoundId round, AppliedSnapshot& result)
{
    Tick send_tick;
    std::uint8_t count;
    if (!payload.u32(send_tick) || !send_tick || !payload.u8(count) || !count ||
        count > max_snapshot_records) return false;
    struct Update { Object object; std::uint8_t kind; bool changed; Words words; };
    std::vector<Update> updates;
    std::set<ObjectId> seen;
    for (std::uint8_t i = 0; i < count; ++i) {
        Object o;
        std::uint8_t kind;
        if (!payload.u32(o.id) || !payload.u32(o.revision) || !payload.u32(o.sample_tick) ||
            !payload.u8(kind) || !o.id || !o.revision || !o.sample_tick ||
            !seen.insert(o.id).second) return false;
        const Object* prior = nullptr;
        if (const auto it = objects_.find(o.id); it != objects_.end()) prior = &it->second;
        else if (const auto it = waiting_for_dependencies_.find(o.id); it != waiting_for_dependencies_.end())
            prior = &it->second;
        const Tick tombstone = progress_at(removed_, o.id);
        const Tick known = prior ? prior->revision : std::max(tombstone, progress_at(hidden_, o.id));
        const bool newer = o.revision > known;
        if (kind == removed || kind == hidden) {
            updates.push_back({std::move(o), kind, newer, {}});
            continue;
        }
        const bool delta = kind == word_delta || kind == scoped_word_delta;
        if (kind == scoped || kind == scoped_word_delta) {
            std::uint8_t deps;
            if (!payload.u32(o.scene) || !payload.u8(deps) || deps > 16) return false;
            ObjectId previous = 0;
            for (unsigned j = 0; j < deps; ++j) {
                ObjectId dep;
                if (!payload.u32(dep) || dep <= previous || dep >= o.id) return false;
                o.dependencies.push_back(dep);
                previous = dep;
            }
        }
        std::uint16_t version;
        std::uint32_t fingerprint;
        if ((!delta && kind != live && kind != scoped) || !payload.u16(o.schema) || !payload.u16(version) ||
            !payload.u32(fingerprint) || !payload.u32(o.owner)) return false;
        const auto* schema = schemas_.get(o.schema);
        if (!schema || schema->version != version || schema_fingerprint(*schema) != fingerprint) return false;
        if (newer && tombstone) return false;
        if (prior) {
            if (o.schema != prior->schema || o.owner != prior->owner ||
                o.scene != prior->scene || o.dependencies != prior->dependencies) return false;
        }
        Words words;
        const auto& offsets = schema->layout.offsets;
        if (newer) {
            if (prior) {
                words = words_.at(o.id);
                o.values = prior->values;
            } else {
                words.resize(offsets.back());
                o.values.resize(schema->fields.size());
            }
        }
        std::uint64_t mask = 0;
        if (delta) {
            if ((newer && !prior) || !read_word_changes(payload, schema->layout, newer ? &words : nullptr, mask)) return false;
        } else {
            const auto full = full_mask(schema->fields.size());
            if (!payload.u64(mask) || !mask || (mask & ~full) || (newer && !prior && mask != full)) return false;
        }
        for (std::size_t index = 0; index < schema->fields.size(); ++index) {
            if (!(mask & (std::uint64_t{1} << index))) continue;
            const auto field = schema->fields[index];
            if (delta) {
                if (!unpack_words(field, words.data() + offsets[index], o.values[index])) return false;
            } else {
                Value ignored;
                auto& value = newer ? o.values[index] : ignored;
                if (!read_value(payload, field, value)) return false;
                if (newer) pack_words(field, value, words.data() + offsets[index]);
            }
        }
        updates.push_back({std::move(o), kind, newer, std::move(words)});
    }
    if (!payload.done()) return false;
    result = {};
    result.send_tick = send_tick;
    auto ack = game_packet(GameMessage::StateAck, room, round);
    ack.u8(count);
    std::set<ObjectId> fresh;
    for (auto& update : updates) {
        const auto id = update.object.id;
        ack.u32(id);
        ack.u32(update.object.revision);
        if (!update.changed) continue;
        if (update.kind == removed || update.kind == hidden) {
            if (update.kind == removed) {
                removed_[id] = update.object.revision;
                hidden_.erase(id);
                result.destroyed.push_back(id);
            } else {
                hidden_[id] = update.object.revision;
                result.hidden.push_back(id);
            }
            objects_.erase(id);
            waiting_for_dependencies_.erase(id);
            words_.erase(id);
        } else {
            if (!objects_.count(id)) fresh.insert(id);
            hidden_.erase(id);
            objects_[id] = std::move(update.object);
            waiting_for_dependencies_.erase(id);
            words_[id] = std::move(update.words);
        }
        result.changed.push_back(id);
    }
    result.ack = ack.take();
    resolve_dependencies(result, std::move(fresh));
    return true;
}

AppliedSnapshot ReplicaState::set_scene_ready(SceneId scene, bool loaded)
{
    AppliedSnapshot result;
    if (!scene) return result;
    const bool changed = loaded ? loading_.erase(scene) != 0 : loading_.insert(scene).second;
    if (changed) resolve_dependencies(result, {});
    return result;
}

void ReplicaState::resolve_dependencies(AppliedSnapshot& result, std::set<ObjectId> fresh)
{
    const auto ready = [&](const Object& o) {
        return !loading_.count(o.scene) && std::all_of(o.dependencies.begin(), o.dependencies.end(),
            [&](ObjectId dep) { return objects_.count(dep); });
    };
    for (auto it = objects_.begin(); it != objects_.end();) {
        if (ready(it->second)) { ++it; continue; }
        result.changed.push_back(it->first);
        waiting_for_dependencies_.insert(objects_.extract(it++));
    }
    for (auto it = waiting_for_dependencies_.begin(); it != waiting_for_dependencies_.end();) {
        if (!ready(it->second)) { ++it; continue; }
        fresh.insert(it->first);
        result.changed.push_back(it->first);
        objects_.insert(waiting_for_dependencies_.extract(it++));
    }
    for (const auto id : fresh) if (objects_.count(id)) result.ready.push_back(id);
    std::sort(result.changed.begin(), result.changed.end());
    result.changed.erase(std::unique(result.changed.begin(), result.changed.end()), result.changed.end());
}

}
