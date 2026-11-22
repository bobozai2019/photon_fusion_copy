#pragma once
#include "session/migration.hpp"
#include "state/layout.hpp"

namespace netcode {

enum class ScriptEventKind {
    Ready,
    Hidden,
    Destroyed,
    Changed,
    Grant,
    Request,
    Result,
    PredictionFailed,
    MapAdded,
    MapRemoved,
    SpawnResult,
    Reconciled,
    SessionReplaced
};
struct ScriptEvent {
    ScriptEventKind kind;
    std::uint32_t object = 0, player = 0, epoch = 0, sequence = 0, status = 0;
};

// Engine adapters convert native values at their boundary. All game rules and
// transfer policies are supplied by the calling script.
struct TypedDelivery {
    MethodId method;
    SchemaId arguments;
    ObjectId object;
    PlayerId sender;
    Values values;
};
class ScriptGame {
  public:
    ScriptGame(std::unique_ptr<Transport> link, SessionOptions options, const ReplicationLayout& layout, MigratingSession::Open open = {});
    GameSession* active() { return runtime_->active(); }
    GameSession& session();
    void pump(TimePoint now);
    MigratingSession& migration() { return *runtime_; }
    bool prediction(SchemaId id, SimulationModel model);
    bool threshold_clock(bool enabled);
    bool spawn_factory(SchemaId id, Values values, bool creator);
    const Schemas& schemas() const { return schemas_; }
    bool model(SchemaId schema, std::vector<FieldId> writable, ClaimMode policy, Tick cooldown);
    // One mounted publisher per local object. 2 = install baseline, 1 = apply update, 0 = no application.
    int sync_replica(ObjectId id,const Values& sample,Values& applied,bool forecast=false);
    void unbind_replica(ObjectId id) { replicas_.erase(id); }
    bool submit_fields(ObjectId id,const Values& full);
    bool submit(ObjectId id, const Values& fields);
    bool answer(const Claim& claim, bool accept) {
        return active() && session().writers().answer(claim, accept, session().tick());
    }
    bool read(ObjectId id, bool presentation, Values& out);
    bool decode(SchemaId schema, bool model_only, const Bytes& encoded, Values& values) const;
    std::deque<ScriptEvent> take_events();
    bool distribute(ObjectId object, InterestKind kind, const std::vector<uint64_t>& keys, int divisor);
    bool subscribe(PlayerId player, InterestKind kind, const std::vector<uint64_t>& keys, const std::vector<int>& rates);
    bool set_player_ttl(int64_t ms);
    bool register_rpc(int method, int direction, bool reliable, int max_bytes, bool writer, bool room);
    bool bind_method(const std::string& name, MethodId method, SchemaId arguments, int direction,
                     bool reliable, bool writer, bool room);
    SchemaId argument_schema(const std::string& name) const;
    bool call_method(const std::string& name,ObjectId object,const Values& values,PlayerId target=0,uint64_t token=0);
    const TypedDelivery* next_typed() const { return typed_calls_.empty() ? nullptr : &typed_calls_.front(); }
    void pop_typed() { typed_calls_.pop_front(); }
    const RpcCall* next_rpc() const { return calls_.empty() ? nullptr : &calls_.front(); }
    void pop_rpc() { calls_.pop_front(); }

  private:
    struct ReplicaCursor {
        RoundId round;
        PlayerId writer;
        Tick epoch;
        Values previous;
    };
    std::map<ObjectId,ReplicaCursor> replicas_;
    struct Signature { SchemaId schema; uint32_t hash; };
    std::map<std::string,MethodId> method_names_;
    std::map<MethodId,Signature> signatures_;
    std::deque<TypedDelivery> typed_calls_;
    void receive_call(const RpcCall& call);
    void configure(GameSession& game);
    bool remember(std::function<bool(GameSession&)> install);
    std::vector<std::function<bool(GameSession&)>> registrations_;
    std::map<SchemaId,std::pair<Values,bool>> factories_;
    ClockDiscipline discipline_ = ClockDiscipline::Monotonic;
    uint32_t term_ = 1;
    Schemas schemas_;
    std::map<SchemaId, std::vector<Field>> writable_;
    std::deque<ScriptEvent> events_;
    std::deque<RpcCall> calls_;
    std::unique_ptr<MigratingSession> runtime_;
};

}
