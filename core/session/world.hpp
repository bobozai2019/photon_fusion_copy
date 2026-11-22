#pragma once

#include "session/object_authority.hpp"
#include <functional>

namespace netcode {
class GameSession;

struct MapInstance {
    SceneId id;
    std::uint32_t generation;
    std::string asset;
};
struct ObjectIdentity {
    SceneId scene = 0;
    std::uint64_t key = 0; // A nonzero authoring key is unique within its scene, including scene zero.
    ObjectId parent = 0;
    std::optional<ClaimMode> mode;
    uint64_t reference = 0; // Within this round, high word identifies the allocated creation namespace.
    bool grant_creator = false;
    bool operator==(const ObjectIdentity &other) const {
        return scene == other.scene && key == other.key && parent == other.parent && mode == other.mode &&
               reference == other.reference;
    }
    bool operator!=(const ObjectIdentity &other) const { return !(*this == other); }
};

class SessionWorld {
  public:
    SessionWorld(GameSession &game, Transport &link) : game_(game), link_(link) {}
    bool load_map(SceneId id, std::string asset);
    bool unload_map(SceneId id);
    bool change_map(SceneId id, std::string asset);
    bool loaded(SceneId id, std::uint32_t generation);
    const auto &maps() const { return maps_; }
    ObjectId spawn(SchemaId schema, PlayerId owner, const Values &fields, ObjectIdentity identity = {});
    bool reparent(ObjectId child, ObjectId parent);
    bool destroy(ObjectId id);
    ObjectId find(SceneId scene, std::uint64_t key) const;
    const ObjectIdentity *identity(ObjectId id) const;
    // Only registered factories accept remote requests. The host provides initial values.
    void allow_spawn(SchemaId schema, std::function<Values(PlayerId, SceneId)> factory,
                     bool creator_writes = false);
    uint64_t request_reference(SchemaId schema, SceneId scene);
    ObjectId resolve(uint64_t reference) const;
    // Field handles are scoped to the current round. Unavailable objects resolve to zero.
    ObjectReference field_reference(ObjectId id) const;
    ObjectId resolve_field(ObjectReference reference) const;
    std::uint32_t request_spawn(SchemaId schema, SceneId scene);
    std::function<void(std::uint32_t, ObjectId)> on_spawn_result;
    std::function<void(const MapInstance &, bool added)> on_map;
    std::function<void(ObjectId)> on_identity;

  private:
    friend class CheckpointCodec;
    friend class GameSession;
    void attach(Peer peer);
    void detach(Peer peer);
    void reset();
    void remove_local(ObjectId id);
    void remove_map_objects(SceneId scene);
    bool receive(const Event &event);
    void flush();
    void publish(Peer peer);
    bool accept_maps(Reader &in);
    bool accept_objects(Reader &in, bool extended);
    GameSession &game_;
    Transport &link_;
    std::map<SceneId, MapInstance> maps_;
    std::map<ObjectId, ObjectIdentity> objects_;
    std::map<SchemaId, std::function<Values(PlayerId, SceneId)>> factories_;
    uint32_t issue_spawn(SchemaId schema, SceneId scene, bool referenced);
    std::set<SchemaId> creator_models_;
    uint32_t namespace_ = 0;
    std::uint32_t map_version_ = 0, next_request_ = 0;
    std::optional<Tick> hello_;
    bool supported_ = false;
    std::set<std::uint32_t> outstanding_;
    struct PeerWorld {
        bool supported = false, extended = false;
        std::uint32_t maps_sent = 0, last_request = 0;
        uint32_t creation_namespace = 0;
        bool namespace_sent = false;
        ObjectId last_result = 0;
        std::set<SceneId> ready;
        std::map<ObjectId, ObjectIdentity> seen;
    };
    std::map<Peer, PeerWorld> peers_;
};

} // namespace netcode
