#include "session/checkpoint.hpp"
#include <algorithm>

namespace netcode {
namespace {
constexpr uint32_t magic = 0x314b434e;
struct SavedObject {
    Object object;
    Distribution distribution;
    std::optional<ObjectIdentity> identity;
    std::optional<ObjectGrant> writer;
    PlayerId input = 0;
    Tick input_epoch = 0, consumed = 0, simulated = 0;
};
void put_rates(Writer& out, const std::map<uint64_t, uint16_t>& keys) {
    out.u32(uint32_t(keys.size()));
    for (auto [key, rate] : keys) {
        out.u64(key);
        out.u16(rate);
    }
}
bool get_rates(Reader& in, std::map<uint64_t, uint16_t>& keys) {
    uint32_t n;
    if (!in.u32(n) || n > in.remaining() / 10)
        return false;
    for (unsigned i = 0; i < n; ++i) {
        uint64_t key;
        uint16_t rate;
        if (!in.u64(key) || !in.u16(rate) || !rate || !keys.emplace(key, rate).second)
            return false;
    }
    return true;
}
bool flag(Reader& in, bool& f) {
    uint8_t b;
    if (!in.u8(b) || b > 1)
        return false;
    f = b != 0;
    return true;
}
}
Bytes CheckpointCodec::capture(const GameSession& g) {
    if (!g.host_ || !g.round() || !g.term_)
        return {};
    const auto& h = *g.host_;
    Writer out;
    out.u32(magic + unsigned(h.options_.player_ttl_ms.has_value()) + (g.world_.namespace_ ? 2 : 0));
    out.u64(g.room());
    out.u64(g.round());
    out.u32(g.term_);
    out.u32(g.coordinator());
    out.u32(g.tick_);
    out.u32(uint32_t(g.options_.step.count()));
    out.u8(h.options_.capacity);
    out.u8(h.options_.late_join);
    out.u8(h.options_.spectators);
    out.u32(h.next_player_);
    out.u16(uint16_t(h.members_.size()));
    for (const auto& [id, m] : h.members_) {
        out.u32(id);
        out.u8(m.spectator);
        for (auto k : m.key)
            out.u32(k);
        GameSession::SavedInterest s;
        auto it = g.state_.peers_.find(m.peer);
        if (it != g.state_.peers_.end()) {
            s = {it->second.scene, it->second.selected, it->second.areas, it->second.users};
        } else if (auto old = g.resumed_interest_.find(id); old != g.resumed_interest_.end())
            s = old->second;
        out.u32(s.scene);
        out.u8(s.selected.has_value());
        if (s.selected) {
            out.u32(uint32_t(s.selected->size()));
            for (auto n : *s.selected)
                out.u32(n);
        }
        put_rates(out, s.areas);
        put_rates(out, s.users);
    }
    out.u32(g.world_.map_version_);
    out.u16(uint16_t(g.world_.maps_.size()));
    for (const auto& [id, m] : g.world_.maps_) {
        out.u32(id);
        out.u32(m.generation);
        out.u8(uint8_t(m.asset.size()));
        out.append(Bytes(m.asset.begin(), m.asset.end()));
    }
    auto ids = g.state_.active();
    out.u32(g.state_.highest_id_);
    out.u32(uint32_t(ids.size()));
    for (auto id : ids) {
        const auto& entry = g.state_.entries_.at(id);
        const auto& o = entry.object;
        const auto& dist = entry.distribution;
        out.u32(id);
        out.u16(o.schema);
        auto& def = *g.schemas_.get(o.schema);
        out.u16(def.version);
        out.u32(schema_fingerprint(def));
        out.u32(o.owner);
        out.u32(o.sample_tick);
        out.u32(o.revision);
        out.u32(o.scene);
        out.u8(uint8_t(o.dependencies.size()));
        for (auto dep : o.dependencies)
            out.u32(dep);
        for (const auto& v : o.values)
            write_value(out, v);
        out.u8(uint8_t(dist.kind));
        out.u64(dist.key);
        out.u16(dist.send_divisor);
        out.u32(uint32_t(dist.additional_keys.size()));
        for (auto key : dist.additional_keys)
            out.u64(key);
        const auto meta = g.world_.identity(id);
        out.u8(meta ? (meta->mode ? 2 : 1) | (meta->reference ? 4 : 0) : 0);
        if (meta) {
            out.u64(meta->key);
            out.u32(meta->parent);
            if (meta->mode) out.u8(uint8_t(*meta->mode));
            if (meta->reference) { out.u64(meta->reference); out.u8(meta->grant_creator); }
        }
        auto grant = g.writers_.grant(id);
        out.u8(grant != nullptr);
        if (grant) {
            out.u32(grant->writer);
            out.u32(grant->epoch);
            out.u32(grant->confirmed);
            out.u32(grant->sample.source_step);
            out.u32(g.writers_.models_.at(o.schema).signature);
            out.u16(uint16_t(grant->sample.recovery.size()));
            out.append(grant->sample.recovery);
        }
        auto pred = g.prediction_.bindings_.find(id);
        out.u8(pred != g.prediction_.bindings_.end());
        if (pred != g.prediction_.bindings_.end()) {
            const auto& b = pred->second;
            out.u32(b.player);
            out.u32(b.epoch);
            out.u32(b.consumed);
            out.u32(b.simulated);
            out.u32(g.prediction_.signature(o.schema));
        }
        if (out.bytes().size() > max_size)
            return {};
    }
    if (h.options_.player_ttl_ms) {
        out.u32(*h.options_.player_ttl_ms);
        const auto sampled = g.origin_ + g.options_.step * (g.tick_ - 1);
        for (const auto& [id, m] : h.members_) {
            const auto ttl = *h.options_.player_ttl_ms;
            const auto remaining = m.reserved_until ?
                std::chrono::duration_cast<std::chrono::milliseconds>(*m.reserved_until - sampled).count() : int64_t(ttl);
            out.u32(uint32_t(std::clamp<int64_t>(remaining, 0, ttl)));
        }
    }
    if (g.world_.namespace_) out.u32(g.world_.namespace_);
    return out.bytes().size() <= max_size ? out.take() : Bytes{};
}
bool CheckpointCodec::header(Reader& r, CheckpointHeader& h) {
    uint32_t m;
    if (!r.u32(m) || (m < magic || m > magic + 3)) return false;
    h.has_seat_ttl = (m - magic) & 1;
    h.has_namespace = (m - magic) & 2;
    return r.u64(h.room) && h.room && r.u64(h.round) && h.round && r.u32(h.term) &&
           h.term && r.u32(h.coordinator) && h.coordinator && r.u32(h.tick) && h.tick && r.u32(h.step_ms) &&
           h.step_ms && h.step_ms <= 60000;
}
bool CheckpointCodec::restore(GameSession& g, const Bytes& image, TimePoint now) {
    if (!g.host_ || g.round() || image.size() > max_size)
        return false;
    Reader in(image);
    CheckpointHeader h;
    if (!header(in, h) || h.room != g.room() || h.term == UINT32_MAX || g.coordinator() == h.coordinator)
        return false;
    RoomOptions admission;
    admission.coordinator = g.coordinator();
    PlayerId next;
    uint16_t count;
    if (!in.u8(admission.capacity) || !flag(in, admission.late_join) || !flag(in, admission.spectators) ||
        !in.u32(next) || !next || !in.u16(count) || count < 2 || count > admission.capacity)
        return false;
    std::map<PlayerId, Member> members;
    std::map<PlayerId, GameSession::SavedInterest> scopes;
    for (unsigned i = 0; i < count; ++i) {
        Member m{};
        if (!in.u32(m.id) || !m.id || m.id >= next || !flag(in, m.spectator))
            return false;
        for (auto& k : m.key)
            if (!in.u32(k))
                return false;
        if (!members.emplace(m.id, m).second)
            return false;
        auto& s = scopes[m.id];
        bool selected;
        if (!in.u32(s.scene) || !flag(in, selected))
            return false;
        if (selected) {
            uint32_t n;
            if (!in.u32(n) || n > in.remaining() / 4)
                return false;
            s.selected.emplace();
            for (unsigned j = 0; j < n; ++j) {
                ObjectId id;
                if (!in.u32(id) || !id)
                    return false;
                s.selected->insert(id);
            }
        }
        if (!get_rates(in, s.areas) || !get_rates(in, s.users))
            return false;
    }
    if (!members.count(admission.coordinator) || members.at(admission.coordinator).spectator ||
        !members.count(h.coordinator))
        return false;
    uint32_t version;
    uint16_t maps;
    if (!in.u32(version) || !in.u16(maps) || maps > 128)
        return false;
    std::map<SceneId, MapInstance> scenes;
    for (unsigned i = 0; i < maps; ++i) {
        MapInstance m{};
        uint8_t n;
        Bytes name;
        if (!in.u32(m.id) || !m.id || !in.u32(m.generation) || !m.generation || m.generation > version ||
            !in.u8(n) || !n || !in.read(n, name))
            return false;
        m.asset.assign(name.begin(), name.end());
        if (!scenes.emplace(m.id, m).second)
            return false;
    }
    uint32_t high, n;
    if (!in.u32(high) || !in.u32(n) || n > in.remaining() / 30)
        return false;
    std::map<ObjectId, SavedObject> objects;
    ObjectId previous = 0;
    for (unsigned i = 0; i < n; ++i) {
        SavedObject saved;
        auto& o = saved.object;
        uint16_t v;
        uint32_t fingerprint;
        uint8_t deps, kind;
        if (!in.u32(o.id) || o.id <= previous || o.id > high || !in.u16(o.schema) || !in.u16(v) ||
            !in.u32(fingerprint))
            return false;
        previous = o.id;
        auto def = g.schemas_.get(o.schema);
        if (!def || def->version != v || fingerprint != schema_fingerprint(*def) || !in.u32(o.owner) ||
            !o.owner || !in.u32(o.sample_tick) || !o.sample_tick || o.sample_tick > h.tick ||
            !in.u32(o.revision) || !o.revision || !in.u32(o.scene) || !in.u8(deps) || deps > 16)
            return false;
        for (unsigned j = 0; j < deps; ++j) {
            ObjectId id;
            if (!in.u32(id) || !objects.count(id) || (!o.dependencies.empty() && id <= o.dependencies.back()))
                return false;
            o.dependencies.push_back(id);
        }
        for (auto f : def->fields) {
            Value value;
            if (!read_value(in, f, value))
                return false;
            o.values.push_back(std::move(value));
        }
        auto& d = saved.distribution;
        uint32_t keys;
        if (!in.u8(kind) || kind > uint8_t(InterestKind::Inherit) || !in.u64(d.key) || !in.u16(d.send_divisor) || !d.send_divisor ||
            !in.u32(keys) || keys > in.remaining() / 8)
            return false;
        d.kind = InterestKind(kind);
        for (unsigned j = 0; j < keys; ++j) {
            uint64_t key;
            in.u64(key);
            d.additional_keys.insert(key);
        }
        bool has;
        uint8_t identity_kind;
        if (!in.u8(identity_kind) || (identity_kind != 0 && identity_kind != 1 && identity_kind != 2 &&
            identity_kind != 5 && identity_kind != 6)) return false;
        if (identity_kind) {
            ObjectIdentity meta{o.scene};
            if (!in.u64(meta.key) || !in.u32(meta.parent))
                return false;
            if ((identity_kind & 3) == 2) {
                uint8_t mode;
                if (!in.u8(mode) || !g.writers_.supports_mode(o.schema, ClaimMode(mode))) return false;
                meta.mode = ClaimMode(mode);
            }
            if ((identity_kind & 4) && (!in.u64(meta.reference) || !uint32_t(meta.reference) ||
                !uint32_t(meta.reference >> 32) || !flag(in,meta.grant_creator))) return false;
            saved.identity = meta;
        }
        if (!flag(in, has))
            return false;
        if (has) {
            ObjectGrant grant;
            grant.object = o.id;
            grant.schema = o.schema;
            uint32_t sig;
            uint16_t size;
            auto m = g.writers_.models_.find(o.schema);
            if (!in.u32(grant.writer) || !members.count(grant.writer) || !in.u32(grant.epoch) ||
                grant.epoch == UINT32_MAX || !in.u32(grant.confirmed) || !in.u32(grant.sample.source_step) ||
                !in.u32(sig) || m == g.writers_.models_.end() || m->second.signature != sig ||
                !in.u16(size) || size > m->second.max_recovery || !in.read(size, grant.sample.recovery))
                return false;
            for (auto slot : m->second.slots)
                grant.sample.values.push_back(o.values[slot]);
            grant.baseline_revision = o.revision;
            grant.committed_at = h.tick;
            grant.sample.sequence = grant.confirmed;
            saved.writer = std::move(grant);
        }
        if (!flag(in, has))
            return false;
        if (has) {
            uint32_t sig;
            if (!in.u32(saved.input) || !members.count(saved.input) || !in.u32(saved.input_epoch) ||
                saved.input_epoch == UINT32_MAX || !in.u32(saved.consumed) || !in.u32(saved.simulated) ||
                !in.u32(sig) || !g.prediction_.models_.count(o.schema) ||
                g.prediction_.signature(o.schema) != sig)
                return false;
        }
        objects.emplace(o.id, std::move(saved));
    }
    if (h.has_seat_ttl) {
        uint32_t ttl;
        if (!in.u32(ttl) || in.remaining() != members.size() * 4 + (h.has_namespace ? 4 : 0)) return false;
        admission.player_ttl_ms = ttl;
        for (auto& [id, m] : members) {
            uint32_t remaining;
            if (!in.u32(remaining) || remaining > ttl) return false;
            if (id != g.coordinator()) m.reserved_until = now + std::chrono::milliseconds(remaining);
        }
    }
    uint32_t creation_namespace = 0;
    if (h.has_namespace && (!in.u32(creation_namespace) || !creation_namespace)) return false;
    if (!in.done()) return false;
    std::set<uint64_t> references;
    for (const auto& [id, s] : objects)
        if (s.identity) {
            auto ref = s.identity->reference;
            if (ref && (uint32_t(ref >> 32) > creation_namespace || !references.insert(ref).second)) return false;
            auto parent = s.identity->parent;
            size_t depth = 0;
            while (parent) {
                auto it = objects.find(parent);
                if (it == objects.end() || !it->second.identity || ++depth > objects.size())
                    return false;
                parent = it->second.identity->parent;
            }
        }
    // Publish only after verifying the complete checkpoint and local schema signatures.
    auto& host = *g.host_;
    host.options_ = admission;
    g.options_.admission = admission;
    g.options_.step = std::chrono::milliseconds(h.step_ms);
    members.erase(h.coordinator);
    members.at(g.coordinator()).ready = true;
    host.members_ = std::move(members);
    host.round_ = h.round;
    host.next_player_ = next;
    host.phase_ = RoomPhase::Playing;
    g.term_ = h.term + 1;
    g.reset(now - g.options_.step * (h.tick - 1));
    g.tick_ = h.tick;
    g.clock_.update(now);
    g.resumed_interest_ = std::move(scopes);
    g.world_.namespace_ = creation_namespace;
    g.world_.map_version_ = version;
    g.world_.maps_ = std::move(scenes);
    std::vector<ObjectId> retire;
    for (auto& [id, s] : objects) {
        auto& o = s.object;
        const auto original_owner = o.owner;
        if (o.owner == h.coordinator)
            o.owner = g.coordinator();
        g.state_.entries_.emplace(id, AuthorityState::Entry{o, *g.schemas_.get(o.schema), s.distribution});
        if (s.identity) {
            g.world_.objects_[id] = *s.identity;
            g.state_.entries_.at(id).scope_parent = s.identity->parent;
        }
        const auto previous_writer = s.writer ? s.writer->writer :
            s.identity && s.identity->grant_creator ? original_owner : h.coordinator;
        if (previous_writer == h.coordinator && g.writers_.mode(id, o.schema) == ClaimMode::PlayerAttached)
            retire.push_back(id);
        if (s.writer) {
            auto grant = std::move(*s.writer);
            ++grant.epoch;
            if (grant.writer == h.coordinator)
                grant.writer = g.coordinator();
            if (grant.writer != g.coordinator())
                g.writers_.restoring_writer_.insert(id);
            g.writers_.grants_[id] = std::move(grant);
            g.writers_.last_transfer_[id] = h.tick;
        }
        if (s.input) {
            auto& b = g.prediction_.bindings_[id];
            b.schema = o.schema;
            b.player = s.input == h.coordinator ? g.coordinator() : s.input;
            b.epoch = s.input_epoch + 1;
            b.consumed = s.consumed;
            b.simulated = s.simulated;
        }
    }
    g.state_.highest_id_ = high;
    for (auto id : retire)
        if (g.object(id))
            g.world_.destroy(id);
    return true;
}
void CheckpointCodec::save_interest(GameSession& g, Peer peer, PlayerId id) {
    auto it = g.state_.peers_.find(peer);
    if (it != g.state_.peers_.end()) {
        const auto& scope = it->second;
        g.resumed_interest_[id] = {scope.scene,scope.selected,scope.areas,scope.users};
    }
}
void CheckpointCodec::reattach(GameSession& g, Peer peer, PlayerId id) {
    auto it = g.resumed_interest_.find(id);
    if (it == g.resumed_interest_.end())
        return;
    auto& s = it->second;
    g.state_.set_interest(peer, s.scene, s.selected);
    g.state_.subscribe_rates(peer, InterestKind::Area, s.areas);
    g.state_.subscribe_rates(peer, InterestKind::User, s.users);
}
}
