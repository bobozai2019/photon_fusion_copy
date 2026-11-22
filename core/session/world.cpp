#include "session/world.hpp"
#include "session/game_session.hpp"
#include <algorithm>

namespace netcode {
namespace {
constexpr std::size_t map_limit = 128, asset_limit = 255, records_per_message = 128;
bool asset_valid(const std::string &name) {
    return !name.empty() && name.size() <= asset_limit && name.find(char(0)) == std::string::npos;
}
} // namespace
void SessionWorld::reset() {
    peers_.clear();
    objects_.clear();
    maps_.clear();
    outstanding_.clear();
    namespace_ = map_version_ = next_request_ = 0;
    hello_.reset();
    supported_ = false;
}
void SessionWorld::attach(Peer peer) {
    if (!peers_.try_emplace(peer).second)
        return;
    for (const auto &[id, map] : maps_)
        game_.authority().gate_scene(peer, id, false);
}
void SessionWorld::detach(Peer peer) {
    auto p = peers_.find(peer);
    if (p == peers_.end())
        return;
    std::vector<ObjectId> retire;
    for (auto &[id, meta] : objects_)
        if (meta.grant_creator && game_.host()->member(peer) && game_.object(id) &&
            game_.object(id)->owner == game_.host()->member(peer)->id) {
            meta.grant_creator = false;
            if (game_.writers().mode(id, game_.object(id)->schema) == ClaimMode::PlayerAttached)
                retire.push_back(id);
        }
    peers_.erase(p);
    for (auto id : retire)
        destroy(id);
}
void SessionWorld::remove_local(ObjectId id) { objects_.erase(id); }
bool SessionWorld::load_map(SceneId id, std::string asset) {
    if (!game_.host() || !game_.round() || !id || maps_.count(id) || maps_.size() >= map_limit ||
        map_version_ >= UINT32_MAX - 1 || !asset_valid(asset))
        return false;
    maps_.emplace(id, MapInstance{id, ++map_version_, std::move(asset)});
    for (auto &entry : peers_)
        game_.authority().gate_scene(entry.first, id, false);
    if (on_map)
        on_map(maps_.at(id), true);
    return true;
}
bool SessionWorld::unload_map(SceneId id) {
    if (!game_.host() || !maps_.count(id) || map_version_ == UINT32_MAX)
        return false;
    auto old = maps_.at(id);
    remove_map_objects(id);
    maps_.erase(id);
    ++map_version_;
    for (auto &[peer, p] : peers_) {
        p.ready.erase(id);
        game_.authority().gate_scene(peer, id, true);
    }
    if (on_map)
        on_map(old, false);
    return true;
}
void SessionWorld::remove_map_objects(SceneId id) {
    for (auto object : game_.authority().active()) {
        const auto *live = game_.object(object);
        if (live && live->scene == id)
            destroy(object);
    }
}
bool SessionWorld::change_map(SceneId id, std::string asset) {
    if (!game_.host() || !game_.round() || !id || map_version_ >= UINT32_MAX - 1 || !asset_valid(asset))
        return false;
    auto previous = std::move(maps_);
    maps_.clear();
    MapInstance next{id, ++map_version_, std::move(asset)};
    maps_.emplace(id, next);
    for (const auto &[old, instance] : previous)
        remove_map_objects(old);
    for (auto &[peer, state] : peers_) {
        state.ready.clear();
        for (const auto &[old, instance] : previous)
            if (old != id)
                game_.authority().gate_scene(peer, old, true);
        game_.authority().gate_scene(peer, id, false);
    }
    if (on_map) {
        for (const auto &[old, instance] : previous)
            on_map(instance, false);
        on_map(next, true);
    }
    return true;
}
bool SessionWorld::loaded(SceneId id, std::uint32_t generation) {
    auto it = maps_.find(id);
    if (!game_.guest() || it == maps_.end() || generation != it->second.generation)
        return false;
    auto msg = game_packet(GameMessage::MapReady, game_.room(), game_.round());
    msg.u32(id);
    msg.u32(generation);
    if (!link_.send(game_.guest()->peer(), Lane::Control, msg.bytes()))
        return false;
    game_.scene_ready(id, true);
    return true;
}
ObjectId SessionWorld::find(SceneId scene, std::uint64_t key) const {
    if (key)
        for (const auto &[id, item] : objects_)
            if (item.key == key && item.scene == scene)
                return id;
    return 0;
}
ObjectId SessionWorld::resolve(uint64_t reference) const {
    if (reference)
        for (const auto &[id, meta] : objects_)
            if (meta.reference == reference)
                return id;
    return 0;
}
ObjectReference SessionWorld::field_reference(ObjectId id) const {
    if (!game_.object(id)) return {};
    const auto* meta = identity(id);
    return {meta && meta->reference > UINT32_MAX ? meta->reference : uint64_t(id)};
}
ObjectId SessionWorld::resolve_field(ObjectReference reference) const {
    if (!reference.value) return 0;
    const auto id = reference.value > UINT32_MAX ? resolve(reference.value) : ObjectId(reference.value);
    return id && game_.object(id) ? id : 0;
}
const ObjectIdentity *SessionWorld::identity(ObjectId id) const {
    auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second;
}
ObjectId SessionWorld::spawn(SchemaId schema, PlayerId owner, const Values &fields, ObjectIdentity identity) {
    if (!game_.host() || !game_.round() || !owner || (identity.scene && !maps_.count(identity.scene)) ||
        find(identity.scene, identity.key) || resolve(identity.reference))
        return 0;
    if (identity.mode && !game_.writers().supports_mode(schema, *identity.mode))
        return 0;
    const auto policy = identity.mode ? identity.mode : game_.writers().mode(schema);
    if (policy == ClaimMode::GameGlobal) {
        if ((!identity.key && identity.scene) || identity.parent)
            return 0;
        for (const auto &[id, meta] : objects_) {
            const auto* live = game_.object(id);
            if (live && (!identity.key || !meta.key) && live->schema == schema &&
                game_.writers().mode(id, schema) == ClaimMode::GameGlobal)
                return 0;
        }
    }
    if (policy && master_only(*policy))
        owner = game_.coordinator();
    if (identity.parent) {
        auto parent = objects_.find(identity.parent);
        if (parent == objects_.end() || !game_.object(identity.parent) ||
            (parent->second.scene && parent->second.scene != identity.scene))
            return 0;
    }
    auto id = game_.authority().next_id();
    if (!game_.authority().create(id, schema, owner, fields, game_.tick(), identity.scene))
        return 0;
    if (identity.parent)
        game_.authority().set_scope_parent(id, identity.parent);
    objects_.emplace(id, identity);
    if (on_identity)
        on_identity(id);
    return id;
}
bool SessionWorld::reparent(ObjectId child, ObjectId parent) {
    const auto *object = game_.object(child);
    if (!game_.host() || !object || !objects_.count(child) || (parent && !objects_.count(parent)))
        return false;
    if (parent && game_.writers().mode(child, object->schema) == ClaimMode::GameGlobal)
        return false;
    auto &original = objects_.at(child);
    if (parent && objects_.at(parent).scene && objects_.at(parent).scene != original.scene)
        return false;
    if (!game_.authority().set_scope_parent(child, parent))
        return false;
    original.parent = parent;
    if (on_identity)
        on_identity(child);
    return true;
}
bool SessionWorld::destroy(ObjectId id) {
    if (!game_.host() || !game_.object(id))
        return false;
    std::vector<ObjectId> children;
    for (const auto &entry : objects_)
        if (entry.second.parent == id)
            children.push_back(entry.first);
    for (auto child : children)
        if (!destroy(child))
            return false;
    if (!game_.authority().destroy(id, game_.tick()))
        return false;
    game_.writers().forget(id);
    remove_local(id);
    return true;
}
void SessionWorld::allow_spawn(SchemaId schema, std::function<Values(PlayerId, SceneId)> factory,
                               bool creator_writes) {
    if (game_.schemas().get(schema) && factory) {
        factories_[schema] = std::move(factory);
        if (creator_writes)
            creator_models_.insert(schema);
        else
            creator_models_.erase(schema);
    }
}
std::uint32_t SessionWorld::request_spawn(SchemaId schema, SceneId scene) {
    return issue_spawn(schema, scene, false);
}
uint64_t SessionWorld::request_reference(SchemaId schema, SceneId scene) {
    if (!namespace_)
        return 0;
    auto seq = issue_spawn(schema, scene, true);
    return seq ? (uint64_t(namespace_) << 32) | seq : 0;
}
uint32_t SessionWorld::issue_spawn(SchemaId schema, SceneId scene, bool referenced) {
    auto guest = game_.guest();
    if (!guest || guest->spectator() || !supported_ || !game_.round() || !game_.schemas().get(schema) ||
        (scene && !maps_.count(scene)) || next_request_ == UINT32_MAX || outstanding_.size() >= 64)
        return 0;
    auto request = ++next_request_;
    auto msg = game_packet(referenced ? GameMessage::SpawnReferenced : GameMessage::SpawnRequest,
                           game_.room(), game_.round());
    msg.u32(request);
    msg.u16(schema);
    msg.u32(scene);
    if (referenced)
        msg.u32(namespace_);
    if (!link_.send(guest->peer(), Lane::Control, msg.bytes()))
        return 0;
    outstanding_.insert(request);
    return request;
}

void SessionWorld::flush() {
    if (game_.host()) {
        std::vector<ObjectId> pending;
        for (const auto &[id, meta] : objects_)
            if (meta.grant_creator)
                pending.push_back(id);
        for (auto id : pending) {
            auto *obj = game_.object(id);
            if (obj && game_.writers().assign(id, obj->owner, game_.tick())) {
                auto meta = objects_.find(id);
                if (meta != objects_.end())
                    meta->second.grant_creator = false;
            }
        }
        for (auto &[peer, p] : peers_)
            if (p.supported)
                publish(peer);
    } else if (!supported_ && (!hello_ || game_.tick() - *hello_ >= 30)) {
        auto msg = game_packet(GameMessage::WorldHello, game_.room(), game_.round());
        link_.send(game_.guest()->peer(), Lane::Game, msg.bytes());
        auto enhanced = game_packet(GameMessage::WorldExtendedHello, game_.room(), game_.round());
        link_.send(game_.guest()->peer(), Lane::Control, enhanced.bytes());
        hello_ = game_.tick();
    }
}
void SessionWorld::publish(Peer peer) {
    auto &p = peers_.at(peer);
    if (p.extended && !p.namespace_sent) {
        if (!p.creation_namespace && namespace_ != UINT32_MAX)
            p.creation_namespace = ++namespace_;
        if (p.creation_namespace) {
            auto out = game_packet(GameMessage::WorldNamespace, game_.room(), game_.round());
            out.u32(p.creation_namespace);
            p.namespace_sent = link_.send(peer, Lane::Control, out.bytes());
        }
    }
    if (p.maps_sent != map_version_) {
        auto out = game_packet(GameMessage::WorldMaps, game_.room(), game_.round());
        out.u32(map_version_);
        out.u16(static_cast<std::uint16_t>(maps_.size()));
        for (const auto &[id, m] : maps_) {
            out.u32(id);
            out.u32(m.generation);
            out.u8(static_cast<std::uint8_t>(m.asset.size()));
            out.append(Bytes(m.asset.begin(), m.asset.end()));
        }
        if (!link_.send(peer, Lane::Control, out.bytes()))
            return;
        p.maps_sent = map_version_;
    }
    std::map<ObjectId, ObjectIdentity> visible;
    for (const auto &[id, meta] : objects_) {
        bool known = game_.authority().known(peer, id);
        for (auto parent = meta.parent; known && parent; parent = objects_.at(parent).parent)
            known = game_.authority().known(peer, parent);
        if (known)
            visible.emplace(id, meta);
    }
    std::vector<ObjectId> dirty;
    for (const auto &[id, item] : p.seen)
        if (!visible.count(id))
            dirty.push_back(id);
    for (const auto &[id, item] : visible)
        if (!p.seen.count(id) || item != p.seen.at(id))
            dirty.push_back(id);
    for (std::size_t index = 0; index < dirty.size();) {
        const auto n = std::min(records_per_message, dirty.size() - index);
        auto out = game_packet(p.extended ? GameMessage::WorldExtendedObjects : GameMessage::WorldObjects,
                               game_.room(), game_.round());
        out.u16(static_cast<std::uint16_t>(n));
        for (std::size_t i = index; i < index + n; ++i) {
            auto id = dirty[i];
            auto it = visible.find(id);
            out.u32(id);
            out.u8(it != visible.end());
            if (it != visible.end()) {
                out.u32(it->second.scene);
                out.u64(it->second.key);
                out.u32(it->second.parent);
                if (p.extended) {
                    const auto &meta = it->second;
                    out.u8((meta.mode ? uint8_t(*meta.mode) + 1 : 0) | (meta.reference ? 128 : 0));
                    if (meta.reference)
                        out.u64(meta.reference);
                }
            }
        }
        if (!link_.send(peer, Lane::Control, out.bytes()))
            return;
        for (std::size_t i = index; i < index + n; ++i) {
            auto id = dirty[i];
            if (visible.count(id))
                p.seen[id] = visible.at(id);
            else
                p.seen.erase(id);
        }
        index += n;
    }
}

bool SessionWorld::accept_maps(Reader &in) {
    std::uint32_t version;
    std::uint16_t count;
    if (!in.u32(version) || version <= map_version_ || !in.u16(count) || count > map_limit)
        return false;
    std::map<SceneId, MapInstance> next;
    for (unsigned i = 0; i < count; ++i) {
        MapInstance m{};
        Bytes name;
        std::uint8_t n;
        if (!in.u32(m.id) || !m.id || !in.u32(m.generation) || !m.generation || m.generation > version ||
            !in.u8(n) || !in.read(n, name))
            return false;
        m.asset.assign(name.begin(), name.end());
        if (!asset_valid(m.asset) || !next.emplace(m.id, m).second)
            return false;
    }
    if (!in.done())
        return false;
    auto previous = std::move(maps_);
    maps_ = std::move(next);
    map_version_ = version;
    for (const auto &[id, m] : previous)
        if (!maps_.count(id) || maps_.at(id).generation != m.generation) {
            game_.scene_ready(id, false);
            if (on_map)
                on_map(m, false);
        }
    for (const auto &[id, m] : maps_)
        if (!previous.count(id) || previous.at(id).generation != m.generation) {
            game_.scene_ready(id, false);
            if (on_map)
                on_map(m, true);
        }
    return true;
}
bool SessionWorld::accept_objects(Reader &in, bool extended) {
    std::uint16_t n;
    if (!in.u16(n) || n > records_per_message)
        return false;
    std::map<ObjectId, std::optional<ObjectIdentity>> changes;
    for (unsigned i = 0; i < n; ++i) {
        ObjectId id;
        ObjectIdentity identity;
        std::uint8_t live;
        if (!in.u32(id) || !id || changes.count(id) || !in.u8(live) || live > 1 ||
            (live && (!in.u32(identity.scene) || !in.u64(identity.key) || !in.u32(identity.parent))))
            return false;
        if (live && extended) {
            uint8_t mode;
            if (!in.u8(mode))
                return false;
            if ((mode & 128) && (!in.u64(identity.reference) || !identity.reference))
                return false;
            mode &= 127;
            if (mode > uint8_t(ClaimMode::GameGlobal) + 1)
                return false;
            if (mode)
                identity.mode = ClaimMode(mode - 1);
        }
        changes[id] = live ? std::optional{identity} : std::nullopt;
    }
    if (!in.done())
        return false;
    for (const auto &[id, meta] : changes) {
        if (meta)
            objects_[id] = *meta;
        else
            objects_.erase(id);
    }
    if (on_identity)
        for (const auto &entry : changes)
            on_identity(entry.first);
    return true;
}
bool SessionWorld::receive(const Event &event) {
    if (event.type != EventType::Message || event.payload.empty() ||
        ((event.payload[0] < static_cast<unsigned>(GameMessage::WorldHello) ||
          event.payload[0] > static_cast<unsigned>(GameMessage::SpawnResult)) &&
         event.payload[0] != uint8_t(GameMessage::WorldExtendedHello) &&
         event.payload[0] != uint8_t(GameMessage::WorldExtendedObjects) &&
         event.payload[0] != uint8_t(GameMessage::WorldNamespace) &&
         event.payload[0] != uint8_t(GameMessage::SpawnReferenced)))
        return false;
    Reader in(event.payload);
    GameMessage type;
    if (!read_game_header(in, type, game_.room(), game_.round()) ||
        event.lane != (type == GameMessage::WorldHello ? Lane::Game : Lane::Control))
        return true;
    if (auto host = game_.host()) {
        const auto *member = host->member(event.peer);
        auto peer = peers_.find(event.peer);
        if (!member || peer == peers_.end())
            return true;
        auto &p = peer->second;
        if (type == GameMessage::WorldExtendedHello && in.done() && !p.extended) {
            p.extended = true;
            p.seen.clear();
        }
        if (type == GameMessage::WorldHello && in.done()) {
            auto out = game_packet(GameMessage::WorldOffer, game_.room(), game_.round());
            if (link_.send(event.peer, Lane::Control, out.bytes())) {
                p.supported = true;
                game_.rpc().allow_cancel(event.peer);
            }
        } else if (p.supported && type == GameMessage::MapReady) {
            SceneId id;
            std::uint32_t generation;
            if (in.u32(id) && in.u32(generation) && in.done() && maps_.count(id) &&
                maps_.at(id).generation == generation) {
                p.ready.insert(id);
                game_.authority().gate_scene(event.peer, id, true);
            }
        } else if (p.supported &&
                   (type == GameMessage::SpawnRequest || type == GameMessage::SpawnReferenced)) {
            std::uint32_t request;
            SchemaId schema;
            SceneId scene;
            if (!in.u32(request) || !request || request < p.last_request || !in.u16(schema) || !in.u32(scene))
                return true;
            ObjectIdentity identity{scene};
            if (type == GameMessage::SpawnReferenced) {
                uint32_t ns;
                if (!in.u32(ns) || !ns || ns != p.creation_namespace || !p.namespace_sent)
                    return true;
                identity.reference = (uint64_t(ns) << 32) | request;
                identity.grant_creator = creator_models_.count(schema) != 0;
            }
            if (!in.done())
                return true;
            if (request != p.last_request) {
                p.last_request = request;
                auto f = factories_.find(schema);
                p.last_result =
                    !member->spectator && member->ready && (!scene || p.ready.count(scene)) &&
                            f != factories_.end() &&
                            (!identity.grant_creator ||
                             (game_.writers().supports(member->id, schema) &&
                              !master_only(game_.writers().mode(schema).value_or(ClaimMode::Dynamic))))
                        ? spawn(schema, member->id, f->second(member->id, scene), identity)
                        : 0;
            }
            auto out = game_packet(GameMessage::SpawnResult, game_.room(), game_.round());
            out.u32(request);
            out.u32(p.last_result);
            if (!link_.send(event.peer, Lane::Control, out.bytes()))
                link_.close(event.peer);
        }
    } else if (event.peer == game_.guest()->peer()) {
        if (type == GameMessage::WorldOffer && in.done()) {
            supported_ = true;
            game_.rpc().allow_cancel(event.peer);
        }
        if (type == GameMessage::WorldNamespace) {
            uint32_t ns;
            if (in.u32(ns) && ns && in.done() && !namespace_)
                namespace_ = ns;
        }
        if (supported_ && type == GameMessage::WorldMaps)
            accept_maps(in);
        if (supported_ && (type == GameMessage::WorldObjects || type == GameMessage::WorldExtendedObjects))
            accept_objects(in, type == GameMessage::WorldExtendedObjects);
        if (supported_ && type == GameMessage::SpawnResult) {
            std::uint32_t request;
            ObjectId id;
            if (in.u32(request) && in.u32(id) && in.done() && outstanding_.erase(request) && on_spawn_result)
                on_spawn_result(request, id);
        }
    }
    return true;
}
} // namespace netcode
