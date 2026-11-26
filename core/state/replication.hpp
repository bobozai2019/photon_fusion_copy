#pragma once
#include <functional>

#include "session/game_wire.hpp"
#include "state/value.hpp"

#include <map>
#include <set>
#include <unordered_map>

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
    WordLayout layout{};
    std::uint32_t wire_fingerprint = 0; // Computed when the schema is registered.
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

    void attach(Peer peer, bool word_deltas = false, PlayerId player = 0);
    void detach(Peer peer);
    // Scene zero subscribes to all scenes. A missing selection includes all
    // matching objects; an empty selection includes none.
    bool set_interest(Peer peer, SceneId scene, std::optional<std::set<ObjectId>> selected = {});
    bool subscribe(Peer peer, InterestKind kind, std::set<std::uint64_t> keys);
    bool subscribe_rates(Peer peer, InterestKind kind, std::map<std::uint64_t, std::uint16_t> keys);
    bool set_distribution(ObjectId object, Distribution distribution);
    bool set_quantum(SchemaId schema, FieldId field, float quantum);
    void enable_quantization(Peer peer, bool enabled);
    bool enable_word_deltas(Peer peer);
    // Keep the revision named by a grant reachable even when captures coalesce.
    void require_baseline(Peer peer, ObjectId id, Tick revision);
    bool set_scope_parent(ObjectId child, ObjectId parent);
    bool gate_scene(Peer peer, SceneId scene, bool ready);
    ObjectId next_id() const { return highest_id_ == UINT32_MAX ? 0 : highest_id_ + 1; }
    bool baseline_complete(Peer peer) const;
    bool known(Peer peer, ObjectId id) const;
    bool subscribes_to(Peer peer, const Distribution& distribution) const;
    std::vector<Bytes> packets(Peer peer, RoomId room, RoundId round, Tick send_tick, Tick retry_ticks = 0);
    bool acknowledge(Peer peer, Reader& payload);

private:
    friend class CheckpointCodec;
    friend class GameSession;
    struct Entry {
        Entry(Object state, const Schema& schema, Distribution distribution);
        Object object;
        WordState state;
        bool removed = false;
        Distribution distribution{};
        ObjectId scope_parent = 0;
        std::size_t dependent_count = 0;
        PlayerId spatial_writer = 0;
    };
    struct Progress {
        struct Sent {
            Tick revision = 0;
            Tick tick = 0;
        };
        struct Receipt {
            Tick revision = 0;
            bool ready = false;
        };
        std::map<ObjectId, Tick> baseline;
        std::unordered_map<ObjectId, Sent> sent;
        std::map<ObjectId, Receipt> ack;
        Tick acknowledged(ObjectId id) const {
            const auto it = ack.find(id);
            return it == ack.end() ? 0 : it->second.revision;
        }
        void forget_readiness(ObjectId id) {
            const auto it = ack.find(id);
            if (it != ack.end()) it->second.ready = false;
        }
        SceneId scene = 0;
        PlayerId player = 0;
        bool quantize = false;
        bool word_deltas = false;
        std::optional<std::set<ObjectId>> selected;
        std::set<SceneId> pending_scenes;
        std::map<std::uint64_t, std::uint16_t> areas, users;
        std::map<ObjectId, Tick> hidden, reentered;
        // Hidden objects with an unconfirmed hide or destruction outside the baseline set.
        std::map<ObjectId, Tick> pending_exits;
        std::map<ObjectId, Tick> required_baselines;
    };
    std::uint16_t send_divisor(const Entry& entry, const Progress& progress) const;
    static std::uint16_t send_divisor(const Distribution& distribution, const Progress& progress);
    bool matches_interest(const Entry& entry, const Progress& progress) const;
    bool capture_sample(ObjectId id, const Values& values, Tick tick, const Words* validated_words);
    void set_spatial_writer(ObjectId id, PlayerId writer);
    void refresh_entry(Entry& entry, Progress& progress);
    void adjust_dependents(ObjectId id, bool add);
    void track_dependencies(const Entry& entry, bool add);
    void update_visibility(Entry& entry, Progress& progress, bool visible);
    bool has_quantization(SchemaId schema) const;
    void refresh(Progress& progress);
    void prune();
    const Schemas& schemas_;
    ObjectId highest_id_ = 0;
    std::map<std::pair<SchemaId,FieldId>,float> quantization_;
    std::uint64_t changes_ = 0;
    Words sampled_words_;
    std::map<ObjectId, Entry> entries_;
    std::set<ObjectId> tombstones_;
    std::map<Peer, Progress> peers_;
    std::function<void(ObjectId)> on_destroy_;
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
    bool destroyed(ObjectId id) const { return removed_.count(id) != 0; }

private:
    void resolve_dependencies(AppliedSnapshot& result, std::set<ObjectId> fresh);
    const Schemas& schemas_;
    std::map<ObjectId, Object> objects_;
    std::map<ObjectId, Object> waiting_for_dependencies_;
    std::map<ObjectId, Words> words_;
    Words staged_words_;
    Values staged_values_;
    std::map<ObjectId, Tick> removed_, hidden_;
    std::set<SceneId> loading_;
};

}
