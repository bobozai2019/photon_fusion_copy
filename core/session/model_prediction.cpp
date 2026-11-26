#include "session/model_prediction.hpp"
#include "session/game_session.hpp"
#include <algorithm>
#include <cmath>

namespace netcode {
namespace {
constexpr unsigned max_models = 64, redundant = 10;
}

float SimulationModel::input_delta(const Bytes& input) {
    Reader reader(input);
    float delta;
    return reader.f32(delta) && std::isfinite(delta) && delta > 0 && delta <= max_input_delta ? delta : 0.0f;
}

bool ModelPrediction::register_model(SchemaId id, SimulationModel model) {
    auto schema = game_.schemas().get(id);
    const auto max_input = SimulationModel::payload_limit + (model.timed_input ? SimulationModel::timed_header_size : 0);
    if (game_.round() || !schema || !model.version || (!model.step && !model.object_step) || !model.input_bytes ||
        model.input_bytes > max_input || model.neutral.size() != model.input_bytes ||
        (model.timed_input && model.input_bytes <= SimulationModel::timed_header_size) ||
        models_.size() >= max_models)
        return false;
    return models_.emplace(id, std::move(model)).second;
}
void ModelPrediction::reset() {
    bindings_.clear();
    peers_.clear();
    last_hello_.reset();
}
void ModelPrediction::forget(ObjectId id) { bindings_.erase(id); }
void ModelPrediction::retire_player(PlayerId player) {
    for (const auto &[id, state] : bindings_)
        if (state.player == player)
            bind(id, game_.coordinator());
}
void ModelPrediction::detach(Peer peer) { peers_.erase(peer); }
std::uint32_t ModelPrediction::signature(SchemaId id) const {
    auto schema = game_.schemas().get(id);
    auto hash = (schema_fingerprint(*schema) ^ schema->version) * 16777619u;
    const auto& model = models_.at(id);
    hash = (hash ^ model.version) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(model.input_bytes)) * 16777619u;
    return model.timed_input ? (hash ^ 1u) * 16777619u : hash;
}
bool ModelPrediction::compatible(Peer peer, SchemaId id) const {
    auto p = peers_.find(peer);
    if (p == peers_.end())
        return false;
    auto m = p->second.find(id);
    return m != p->second.end() && m->second == signature(id);
}
bool ModelPrediction::bind(ObjectId id, PlayerId player) {
    auto *host = game_.host();
    auto *obj = game_.object(id);
    if (!host || !game_.round() || !obj || !models_.count(obj->schema) || !player)
        return false;
    auto m = host->members().find(player);
    if (m == host->members().end() || m->second.spectator ||
        (player != game_.coordinator() && !m->second.peer))
        return false;
    auto &binding = bindings_[id];
    if (binding.player == player)
        return true;
    if (binding.epoch == UINT32_MAX)
        return false;
    const auto epoch = binding.epoch + 1;
    const auto model_tick = std::max(binding.model_tick, game_.tick());
    binding = {};
    binding.epoch = epoch;
    binding.schema = obj->schema;
    binding.player = player;
    binding.simulated = game_.tick();
    binding.model_tick = model_tick;
    return true;
}
bool ModelPrediction::advance(ObjectId id, Bytes input) {
    auto it = bindings_.find(id);
    if (it == bindings_.end() || it->second.player != game_.local() || !game_.object(id))
        return false;
    auto &b = it->second;
    const auto &model = models_.at(b.schema);
    const auto delta = model.timed_input ? SimulationModel::input_delta(input) : 0.0f;
    if (input.size() != model.input_bytes || (model.timed_input && !delta))
        return false;
    if (game_.host()) {
        if (model.timed_input) {
            if (b.consumed == UINT32_MAX || b.model_tick == UINT32_MAX ||
                !commit_step(id, model, input, b.model_tick + 1, game_.tick(), SimulationStep::Input))
                return false;
            ++b.model_tick;
            ++b.consumed;
            b.local_input_credit += delta;
            return true;
        }
        if (b.inputs.size() >= Prediction<Values, Bytes>::capacity)
            return false;
        auto seq = b.inputs.empty() ? b.consumed : b.inputs.rbegin()->first;
        if (seq == UINT32_MAX)
            return false;
        b.inputs.emplace(seq + 1, std::move(input));
        return true;
    }
    if (!b.history.can_advance())
        return false;
    auto candidate = b.history.state();
    if (!model.simulate(id, candidate, input, b.history.tick() + 1, SimulationStep::Input) ||
        !game_.schemas().valid(b.schema, candidate))
        return false;
    return b.history.advance(input,
                             [&](Values &values, const Bytes &, Tick) { values = std::move(candidate); });
}
const Values *ModelPrediction::state(ObjectId id) const {
    auto it = bindings_.find(id);
    return it == bindings_.end() || it->second.player != game_.local() || !it->second.history.ready()
               ? nullptr
               : &it->second.history.state();
}
Tick ModelPrediction::confirmed(ObjectId id) const {
    auto it = bindings_.find(id);
    return it == bindings_.end() ? 0 : game_.host() ? it->second.consumed : it->second.history.confirmed();
}
std::uint64_t ModelPrediction::replayed(ObjectId id) const {
    auto it = bindings_.find(id);
    return it == bindings_.end() ? 0 : it->second.history.replayed();
}
PlayerId ModelPrediction::input_player(ObjectId id) const {
    const auto it = bindings_.find(id);
    return it == bindings_.end() ? 0 : it->second.player;
}
std::size_t ModelPrediction::queued(ObjectId id) const {
    const auto it = bindings_.find(id);
    return it == bindings_.end() ? 0 : game_.host() ? it->second.inputs.size() : it->second.history.pending().size();
}
void ModelPrediction::frame(Peer peer, ObjectId id, const Binding &b) {
    auto *obj = game_.object(id);
    auto out = game_packet(GameMessage::ModelFrame, game_.room(), game_.round());
    out.u32(id);
    out.u16(b.schema);
    out.u32(signature(b.schema));
    out.u32(b.player);
    out.u32(b.epoch);
    out.u32(models_.at(b.schema).timed_input ? b.model_tick : b.simulated);
    out.u32(b.consumed);
    for (const auto &value : obj->values)
        write_value(out, value);
    link_.send(peer, Lane::Game, out.bytes());
}

bool ModelPrediction::commit_step(ObjectId id, const SimulationModel& model, const Bytes& input,
                                  Tick model_tick, Tick service_tick, SimulationStep stage) {
    const auto* object = game_.object(id);
    if (!object)
        return false;
    auto candidate = object->values;
    const auto sample_tick = std::max(object->sample_tick, service_tick);
    return model.simulate(id, candidate, input, model_tick, stage) &&
           game_.authority().capture(id, candidate, sample_tick);
}

void ModelPrediction::step_timed(ObjectId id, Binding& b, const SimulationModel& model, Tick service_tick) {
    if (b.model_tick == UINT32_MAX)
        return;
    const double step = std::chrono::duration<float>(game_.step()).count();
    if (b.local_input_credit > 0) {
        // The coordinator already ran the input callback at submission time.
        b.local_input_credit = std::max(0.0, b.local_input_credit - step);
        return;
    }
    auto next = b.inputs.find(b.consumed + 1);
    if (next == b.inputs.end()) {
        b.input_credit = 0;
        commit_step(id, model, model.neutral, ++b.model_tick, service_tick, SimulationStep::Neutral);
        return;
    }
    // Use the same float quantization as queue_input()'s default wire duration.
    b.input_credit += step;
    while (next != b.inputs.end() && b.model_tick != UINT32_MAX) {
        const auto delta = SimulationModel::input_delta(next->second);
        if (delta > b.input_credit)
            break;
        auto command = b.inputs.extract(next);
        ++b.consumed;
        ++b.model_tick;
        if (delta > 0) {
            b.input_credit -= delta;
            commit_step(id, model, command.mapped(), b.model_tick, service_tick, SimulationStep::Input);
        }
        next = b.inputs.find(b.consumed + 1);
    }
}

void ModelPrediction::flush() {
    if (models_.empty())
        return;
    if (auto host = game_.host()) {
        for (auto it = bindings_.begin(); it != bindings_.end();) {
            const auto id = it->first;
            auto &b = it->second;
            auto obj = game_.object(id);
            if (!obj) {
                it = bindings_.erase(it);
                continue;
            }
            // Execute bounded catch-up per engine frame; simulation ticks stay consecutive after a pause.
            auto &model = models_.at(b.schema);
            for (unsigned n = 0; n < 8 && b.simulated < game_.tick(); ++n) {
                const auto tick = ++b.simulated;
                if (model.timed_input) {
                    step_timed(id, b, model, tick);
                    continue;
                }
                auto cmd = b.inputs.extract(b.consumed + 1);
                if (!cmd.empty())
                    ++b.consumed;
                commit_step(id, model, cmd.empty() ? model.neutral : cmd.mapped(), tick, tick,
                            cmd.empty() ? SimulationStep::Neutral : SimulationStep::Input);
            }
            for (const auto &[peer, support] : peers_)
                if (compatible(peer, b.schema) && game_.authority().known(peer, id))
                    frame(peer, id, b);
            ++it;
        }
    } else {
        if (!last_hello_ || game_.tick() - *last_hello_ >= 90) {
            auto out = game_packet(GameMessage::ModelHello, game_.room(), game_.round());
            out.u8(uint8_t(models_.size()));
            for (const auto &[id, model] : models_) {
                out.u16(id);
                out.u32(signature(id));
            }
            link_.send(game_.guest()->peer(), Lane::Game, out.bytes());
            last_hello_ = game_.tick();
        }
        for (const auto &[id, b] : bindings_) {
            const auto &pending = b.history.pending();
            if (b.player != game_.local() || pending.empty())
                continue;
            auto msg = game_packet(GameMessage::ModelInputs, game_.room(), game_.round());
            msg.u32(id);
            msg.u32(b.epoch);
            msg.u32(pending.front().sequence);
            const auto count = std::min<size_t>(redundant, pending.size());
            msg.u8(uint8_t(count));
            for (size_t i = 0; i < count; ++i)
                msg.append(pending[i].input);
            link_.send(game_.guest()->peer(), Lane::Game, msg.bytes());
        }
    }
}
void ModelPrediction::accept_inputs(Reader &in, Peer peer) {
    ObjectId id;
    Tick epoch, first;
    uint8_t count;
    if (!in.u32(id) || !in.u32(epoch) || !in.u32(first) || !first || !in.u8(count) || !count ||
        count > redundant || first > UINT32_MAX - (count - 1) || !bindings_.count(id))
        return;
    auto &b = bindings_.at(id);
    auto member = game_.host()->member(peer);
    const auto size = models_.at(b.schema).input_bytes;
    if (!member || member->spectator || member->id != b.player || b.epoch != epoch ||
        !compatible(peer, b.schema) || !game_.authority().known(peer, id) || in.remaining() != count * size)
        return;
    for (unsigned i = 0; i < count; ++i) {
        Bytes data;
        in.read(size, data);
        const Tick seq = first + i;
        if (seq > b.consumed && seq - b.consumed <= Prediction<Values, Bytes>::capacity)
            b.inputs.try_emplace(seq, std::move(data));
    }
}
void ModelPrediction::accept_frame(Reader &in) {
    ObjectId id;
    SchemaId schema;
    uint32_t sig;
    PlayerId player;
    Tick epoch, tick, through;
    if (!in.u32(id) || !in.u16(schema) || !models_.count(schema) || !in.u32(sig) ||
        sig != signature(schema) || !in.u32(player) || !player || !in.u32(epoch) || !epoch || !in.u32(tick) ||
        !tick || !in.u32(through))
        return;
    const auto *obj = game_.object(id);
    if (!obj || obj->schema != schema)
        return;
    Values values;
    for (auto field : game_.schemas().get(schema)->fields) {
        Value v;
        if (!read_value(in, field, v))
            return;
        values.push_back(std::move(v));
    }
    if (!in.done())
        return;
    auto &b = bindings_[id];
    if (b.epoch > epoch || (b.epoch == epoch && b.player != player))
        return;
    if (b.epoch != epoch) {
        b = {};
        b.epoch = epoch;
        b.schema = schema;
        b.player = player;
    }
    if (b.player == game_.local() &&
        b.history.reconcile(tick, through, values,
                            [&](Values &state, const Bytes &data, Tick step) {
                                auto candidate = state;
                                if (models_.at(schema).simulate(id, candidate, data, step, SimulationStep::Replay) &&
                                    game_.schemas().valid(schema, candidate))
                                    state = std::move(candidate);
                            }) &&
        on_reconciled)
        on_reconciled(id, tick, through);
}
bool ModelPrediction::receive(const Event &e) {
    if (e.type != EventType::Message || e.lane != Lane::Game || e.payload.empty() ||
        e.payload[0] < static_cast<unsigned>(GameMessage::ModelHello) ||
        e.payload[0] > static_cast<unsigned>(GameMessage::ModelInputs))
        return false;
    Reader in(e.payload);
    GameMessage type;
    if (!read_game_header(in, type, game_.room(), game_.round()))
        return true;
    if (game_.host() && game_.host()->member(e.peer)) {
        if (type == GameMessage::ModelInputs)
            accept_inputs(in, e.peer);
        if (type == GameMessage::ModelHello) {
            uint8_t count;
            if (!in.u8(count) || count > max_models || in.remaining() != count * 6u)
                return true;
            std::map<SchemaId, uint32_t> supported;
            for (unsigned i = 0; i < count; ++i) {
                SchemaId id;
                uint32_t sig;
                in.u16(id);
                in.u32(sig);
                supported[id] = sig;
            }
            peers_[e.peer] = std::move(supported);
        }
    } else if (game_.guest() && e.peer == game_.guest()->peer() && type == GameMessage::ModelFrame)
        accept_frame(in);
    return true;
}
} // namespace netcode
