#pragma once

#include "session/game_wire.hpp"
#include "state/value.hpp"

#include <map>
#include <set>

namespace netcode {

using ObjectId = std::uint32_t;
using SchemaId = std::uint16_t;
using Tick = std::uint32_t;
using SceneId = std::uint32_t;
enum class InterestKind { Global, Area, User, Inherit };
struct Distribution {
    InterestKind kind = InterestKind::Global;
    std::uint64_t key = 0;
    std::uint16_t send_divisor = 1;
    std::set<std::uint64_t> additional_keys;
};

struct Schema {
    SchemaId id;
    std::uint16_t version;
    std::vector<Field> fields;
};

std::uint32_t schema_fingerprint(const Schema& schema);

class Schemas {
public:
    bool add(Schema schema);
    const Schema* get(SchemaId id) const;
    bool valid(SchemaId id, const Values& values) const;
    const auto& all() const { return schemas_; }

private:
    std::map<SchemaId, Schema> schemas_;
};

struct Object {
    ObjectId id = 0;
    SchemaId schema = 0;
    PlayerId owner = 0;
    Tick sample_tick = 0;
    Tick revision = 0;
    Values values{};
    SceneId scene = 0;
    std::vector<ObjectId> dependencies;
};

class AuthorityState {
public:
    explicit AuthorityState(const Schemas& schemas) : schemas_(schemas) {}
    void clear();
    bool create(ObjectId id, SchemaId schema, PlayerId owner, const Values& values, Tick tick,
                SceneId scene = 0, const std::vector<ObjectId>& dependencies = {}, Distribution distribution = {});
    bool capture(ObjectId id, const Values& values, Tick tick);
    bool destroy(ObjectId id, Tick tick);
    const Object* get(ObjectId id) const;
    std::vector<ObjectId> active() const;
    std::uint64_t changes() const { return changes_; }

    void attach(Peer peer);
    void detach(Peer peer);
    // Scene zero subscribes to all scenes. A missing selection includes all
    // matching objects; an empty selection includes none.
    bool set_interest(Peer peer, SceneId scene, std::optional<std::set<ObjectId>> selected = {});
    bool subscribe(Peer peer, InterestKind kind, std::set<std::uint64_t> keys);
    bool subscribe_rates(Peer peer, InterestKind kind, std::map<std::uint64_t, std::uint16_t> keys);
    bool set_distribution(ObjectId object, Distribution distribution);
    bool set_quantum(SchemaId schema, FieldId field, float quantum);
    void enable_quantization(Peer peer, bool enabled);
    bool set_scope_parent(ObjectId child, ObjectId parent);
    bool gate_scene(Peer peer, SceneId scene, bool ready);
    ObjectId next_id() const { return highest_id_ == UINT32_MAX ? 0 : highest_id_ + 1; }
    bool baseline_complete(Peer peer) const;
    bool known(Peer peer, ObjectId id) const;
    std::vector<Bytes> packets(Peer peer, RoomId room, RoundId round, Tick send_tick);
    bool acknowledge(Peer peer, Reader& payload);

private:
    friend class CheckpointCodec;
    struct Entry {
        Object object;
        std::vector<Tick> changes;
        bool removed = false;
        Distribution distribution{};
        ObjectId scope_parent = 0;
    };
    struct Progress {
        std::map<ObjectId, Tick> baseline, sent, ack;
        SceneId scene = 0;
        bool quantize = false;
        std::optional<std::set<ObjectId>> selected;
        std::set<SceneId> pending_scenes;
        std::map<std::uint64_t, std::uint16_t> areas, users;
        std::map<ObjectId, Tick> last_sent_tick;
        std::map<ObjectId, Tick> hidden, reentered;
    };
    std::uint16_t send_divisor(const Entry& entry, const Progress& progress) const;
    void refresh(Progress& progress);
    void prune();
    const Schemas& schemas_;
    ObjectId highest_id_ = 0;
    std::map<std::pair<SchemaId,FieldId>,float> quantization_;
    std::uint64_t changes_ = 0;
    std::map<ObjectId, Entry> entries_;
    std::map<Peer, Progress> peers_;
};

struct AppliedSnapshot {
    Tick send_tick = 0;
    std::vector<ObjectId> changed;
    Bytes ack;
    std::vector<ObjectId> hidden, destroyed, ready;
};

class ReplicaState {
public:
    explicit ReplicaState(const Schemas& schemas) : schemas_(schemas) {}
    void clear();
    bool apply(Reader& payload, RoomId room, RoundId round, AppliedSnapshot& result);
    AppliedSnapshot set_scene_ready(SceneId scene, bool loaded);
    const std::map<ObjectId, Object>& objects() const { return objects_; }

private:
    void resolve_dependencies(AppliedSnapshot& result, std::set<ObjectId> fresh);
    const Schemas& schemas_;
    std::map<ObjectId, Object> objects_;
    std::map<ObjectId, Object> waiting_for_dependencies_;
    std::map<ObjectId, Tick> removed_, hidden_;
    std::set<SceneId> loading_;
};

}
