#pragma once

#include "session/game_wire.hpp"

#include <map>
#include <variant>

namespace netcode {

using ObjectId = std::uint32_t;
using SchemaId = std::uint16_t;
using FieldId = std::uint16_t;
using Tick = std::uint32_t;
using Value = std::variant<float, bool>;
using Values = std::vector<Value>;

enum class FieldType { Float, Flag };

struct Field {
    FieldId id;
    FieldType type;
};

struct Schema {
    SchemaId id;
    std::uint16_t version;
    std::vector<Field> fields;
};

class Schemas {
public:
    bool add(Schema schema);
    const Schema* get(SchemaId id) const;
    bool valid(SchemaId id, const Values& values) const;

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
};

class AuthorityState {
public:
    explicit AuthorityState(const Schemas& schemas) : schemas_(schemas) {}
    void clear();
    bool create(ObjectId id, SchemaId schema, PlayerId owner, const Values& values, Tick tick);
    bool capture(ObjectId id, const Values& values, Tick tick);
    bool destroy(ObjectId id, Tick tick);
    const Object* get(ObjectId id) const;
    std::vector<ObjectId> active() const;
    std::uint64_t changes() const { return changes_; }

    void attach(Peer peer);
    void detach(Peer peer);
    bool baseline_complete(Peer peer) const;
    std::vector<Bytes> packets(Peer peer, RoomId room, RoundId round, Tick send_tick);
    bool acknowledge(Peer peer, Reader& payload);

private:
    struct Entry {
        Object object;
        std::vector<Tick> changes;
        bool removed = false;
    };
    struct Progress {
        std::map<ObjectId, Tick> baseline, sent, ack;
    };
    void prune();
    const Schemas& schemas_;
    ObjectId highest_id_ = 0;
    std::uint64_t changes_ = 0;
    std::map<ObjectId, Entry> entries_;
    std::map<Peer, Progress> peers_;
};

struct AppliedSnapshot {
    Tick send_tick = 0;
    std::vector<ObjectId> changed;
    Bytes ack;
};

class ReplicaState {
public:
    explicit ReplicaState(const Schemas& schemas) : schemas_(schemas) {}
    void clear();
    bool apply(Reader& payload, RoomId room, RoundId round, AppliedSnapshot& result);
    const std::map<ObjectId, Object>& objects() const { return objects_; }

private:
    const Schemas& schemas_;
    std::map<ObjectId, Object> objects_;
    std::map<ObjectId, Tick> removed_;
};

}
