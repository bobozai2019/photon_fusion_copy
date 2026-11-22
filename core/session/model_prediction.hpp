#pragma once
#include "state/prediction.hpp"
#include <functional>

namespace netcode {
class GameSession;
struct SimulationModel {
    std::uint16_t version = 1;
    std::size_t input_bytes = 0;
    Bytes neutral;
    // Return false for invalid input. The candidate state is committed atomically.
    std::function<bool(Values&, const Bytes&, Tick)> step;
};
class ModelPrediction {
  public:
    ModelPrediction(GameSession& game, Transport& link) : game_(game), link_(link) {}
    bool register_model(SchemaId schema, SimulationModel model);
    bool bind(ObjectId object, PlayerId input_player);
    bool advance(ObjectId object, Bytes input);
    const Values* state(ObjectId object) const;
    Tick confirmed(ObjectId object) const;
    std::uint64_t replayed(ObjectId object) const;
    void forget(ObjectId object);
    // Fired after installing an authoritative frame and replaying outstanding inputs.
    std::function<void(ObjectId, Tick, Tick)> on_reconciled;

  private:
    friend class CheckpointCodec;
    friend class GameSession;
    struct Binding {
        SchemaId schema = 0;
        PlayerId player = 0;
        Tick epoch = 0, consumed = 0, simulated = 0;
        std::map<Tick, Bytes> inputs;
        Prediction<Values, Bytes> history;
    };
    void reset();
    void detach(Peer peer);
    void retire_player(PlayerId player);
    void flush();
    bool receive(const Event& e);
    void frame(Peer peer, ObjectId id, const Binding& binding);
    bool compatible(Peer peer, SchemaId id) const;
    std::uint32_t signature(SchemaId id) const;
    void accept_frame(Reader& in);
    void accept_inputs(Reader& in, Peer peer);
    GameSession& game_;
    Transport& link_;
    std::map<SchemaId, SimulationModel> models_;
    std::map<ObjectId, Binding> bindings_;
    std::map<Peer, std::map<SchemaId, std::uint32_t>> peers_;
    std::optional<Tick> last_hello_;
};
}
