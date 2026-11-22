#include "session/direct_game.hpp"
#include "build_info.hpp"
#include <algorithm>
#include <sstream>

using namespace netcode;
namespace {
struct Binding {
    std::unique_ptr<ScriptGame> script;
    std::deque<ScriptEvent> events;
    std::deque<RpcOutcome> outcomes;
};
struct GameFrame {
    std::uint64_t round;
    std::uint32_t player, seats;
    double time, rtt, rate, offset;
};
thread_local std::string error;
template <class F> int guarded(F f) noexcept {
    try {
        return f();
    } catch (const std::exception& problem) {
        error = problem.what();
        return -1;
    } catch (...) {
        error = "Native session failed";
        return -1;
    }
}
} // namespace
#define NG_API extern "C" __declspec(dllexport)
NG_API const char* ng_core_build_id() noexcept { return netcode::core_build_id(); }
NG_API const char* ng_platform_build_id() noexcept { return NETCODE_PLATFORM_BUILD_ID; }
NG_API int ng_validate_layout(const char* text) noexcept {
    return guarded([&] {
        if (!text)
            return 0;
        std::istringstream source(text);
        return int(!ReplicationLayout(source).schemas().all().empty());
    });
}
NG_API const char* ng_error() noexcept { return error.c_str(); }
NG_API Binding* ng_open(const char* config, int host, const char* address, int port, int capacity,
                        int watch) noexcept {
    if (!config || !address || port < 1 || port > 65535 || capacity < 1 || capacity > 255) {
        error = "Invalid session options";
        return nullptr;
    }
    std::unique_ptr<Binding> result;
    guarded([&] {
        std::istringstream stream(config);
        ReplicationLayout layout(stream);
        SessionOptions options;
        options.host = host != 0;
        options.spectator = watch != 0;
        options.extended_join = true;
        options.admission = {static_cast<uint8_t>(capacity), true, true};
        DirectConfig direct{address, static_cast<uint16_t>(port)};
        auto script = open_direct_game(direct,options,layout);
        result = std::make_unique<Binding>();
        result->script = std::move(script);
        return 1;
    });
    return result.release();
}
NG_API void ng_close(Binding* b) noexcept {
    if (b && b->script->active())
        guarded([&] {
            b->script->session().end(Clock::now());
            return 1;
        });
    delete b;
}
NG_API int ng_model(Binding* b, SchemaId schema, const FieldId* fields, int count, int policy,
                    Tick cooldown) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] {
        if (!fields || count <= 0 || count > 64 || policy < 0 || policy > int(ClaimMode::GameGlobal))
            return 0;
        return int(
            b->script->model(schema, {fields, fields + count}, static_cast<ClaimMode>(policy), cooldown));
    });
}
NG_API int ng_standby(Binding* b,PlayerId player,const char* address,int port) noexcept {
    return guarded([&]{return int(address && port>0 && port<=65535 &&
        b->script->migration().add_standby(player,{address,uint16_t(port)}));});
}
NG_API int ng_handoff(Binding* b) noexcept { return guarded([&]{return int(b->script->migration().handoff());}); }
NG_API int ng_migration(Binding* b,uint32_t* term,int* host,Tick* saved,int* armed) noexcept {
    auto* game=b->script->active();
    *term=game ? game->term() : 0;
    *host=game && game->host();
    *saved=b->script->migration().saved_tick();
    *armed=b->script->migration().armed();
    return int(b->script->migration().phase());
}
NG_API int ng_start(Binding* b) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] { return int(b->script->session().start(Clock::now())); });
}
NG_API int ng_pump(Binding* b, GameFrame* frame) noexcept {
    return guarded([&] {
        auto& script = *b->script;
        auto* prior = script.active();
        const auto term = prior ? prior->term() : 0;
        script.pump(Clock::now());
        if (!script.active() || script.active()->term() != term) { b->events.clear(); b->outcomes.clear(); }
        if (!script.active()) { *frame = {}; return 1; }
        auto& g = script.session();
        const auto& c = g.clock();
        *frame = {
            g.round(),   g.local(), uint32_t(g.host() ? g.host()->members().size() : g.guest()->seats()),
            c.seconds(), c.rtt(),   c.rate(),
            c.offset()};
        auto events = b->script->take_events();
        b->events.insert(b->events.end(), events.begin(), events.end());
        auto outcomes = g.rpc().take_outcomes();
        b->outcomes.insert(b->outcomes.end(), outcomes.begin(), outcomes.end());
        return 1;
    });
}
NG_API int ng_event(Binding* b, ScriptEvent* event) noexcept {
    if (b->events.empty())
        return 0;
    *event = b->events.front();
    b->events.pop_front();
    return 1;
}
NG_API int ng_fields(Binding* b, SchemaId schema, int index, int* kind, FieldId* id) noexcept {
    const auto* s = b->script->schemas().get(schema);
    if (!s || index < 0 || static_cast<size_t>(index) >= s->fields.size())
        return 0;
    *kind = int(s->fields[index].type);
    *id = s->fields[index].id;
    return 1;
}
NG_API int ng_schema(Binding* b, ObjectId id) noexcept {
    if (!b->script->active()) return 0;
    auto* o = b->script->session().object(id);
    return o ? o->schema : 0;
}
NG_API int ng_spawn_mode(Binding* b, SchemaId schema, const uint8_t* data, int n, SceneId scene, uint64_t key,
                         ObjectId parent, int mode, ObjectId* id) noexcept {
    if (!b->script->active()) return 0;
    if (mode < -1 || mode > int(ClaimMode::GameGlobal))
        return 0;
    return guarded([&] {
        Values fields;
        if (!data || n <= 0 || n > game_payload_limit ||
            !b->script->decode(schema, false, {data, data + n}, fields))
            return 0;
        *id = b->script->session().world().spawn(
            schema, b->script->session().local(), fields,
            {scene, key, parent, mode < 0 ? std::nullopt : std::optional{ClaimMode(mode)}});
        return *id ? 1 : 0;
    });
}
NG_API int ng_spawn(Binding* b, SchemaId schema, const uint8_t* data, int n, SceneId scene, uint64_t key,
                    ObjectId parent, ObjectId* id) noexcept {
    if (!b->script->active()) return 0;
    return ng_spawn_mode(b, schema, data, n, scene, key, parent, -1, id);
}
NG_API int ng_register_rpc(Binding* b, int method, int direction, int reliable, int limit, int writer,
                           int room) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] {
        return int(b->script->register_rpc(method, direction, reliable != 0, limit, writer != 0, room != 0));
    });
}
NG_API int ng_invoke_tracked(Binding* b, ObjectId id, MethodId method, const uint8_t* data, int size,
                             PlayerId target, uint64_t token) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] {
        if (size < 0 || size > 256 || (size && !data))
            return 0;
        Bytes args;
        if (size)
            args.assign(data, data + size);
        return int(b->script->session().rpc().invoke(id, method, args, target, token));
    });
}
NG_API int ng_invoke_rpc(Binding* b, ObjectId id, MethodId method, const uint8_t* data, int size,
                         PlayerId target) noexcept {
    if (!b->script->active()) return 0;
    return ng_invoke_tracked(b, id, method, data, size, target, 0);
}
NG_API int ng_rpc_outcome(Binding* b, uint64_t* token, PlayerId* player, ObjectId* id, MethodId* method,
                          int* status) noexcept {
    if (b->outcomes.empty())
        return 0;
    const auto& result = b->outcomes.front();
    *token = result.token;
    *player = result.player;
    *id = result.object;
    *method = result.method;
    *status = int(result.status);
    b->outcomes.pop_front();
    return 1;
}
NG_API int ng_next_rpc(Binding* b, ObjectId* id, PlayerId* sender, MethodId* method, uint8_t* bytes,
                       int capacity, int* size) noexcept {
    const auto* call = b->script->next_rpc();
    if (!call || capacity < 0 || call->arguments.size() > size_t(capacity))
        return 0;
    *id = call->object;
    *sender = call->sender;
    *method = call->method;
    *size = int(call->arguments.size());
    std::copy(call->arguments.begin(), call->arguments.end(), bytes);
    b->script->pop_rpc();
    return 1;
}
NG_API int ng_bind_method(Binding* b,const char* name,MethodId method,SchemaId args,int dir,int reliable,int writer,int room) noexcept {
    return guarded([&]{return int(name && b->script->bind_method(name,method,args,dir,reliable,writer,room));});
}
NG_API int ng_call_method(Binding* b,const char* name,ObjectId object,const uint8_t* data,int n,PlayerId target,uint64_t token) noexcept {
    return guarded([&]{
        if (!name || n<0 || n>252 || (n && !data)) return 0;
        auto schema=b->script->argument_schema(name);
        Values values;
        if (schema && (!n || !b->script->decode(schema,false,Bytes(data,data+n),values))) return 0;
        return int((schema || !n) && b->script->call_method(name,object,values,target,token));
    });
}
NG_API int ng_next_typed(Binding* b,ObjectId* id,PlayerId* sender,MethodId* method,SchemaId* schema,uint8_t* bytes,int capacity,int* n) noexcept {
    return guarded([&]{
        auto* call=b->script->next_typed(); if (!call) return 0;
        Writer out; for (const auto& v:call->values) write_value(out,v);
        if (capacity<0 || size_t(capacity)<out.bytes().size()) return 0;
        *id=call->object; *sender=call->sender; *method=call->method; *schema=call->arguments;
        *n=int(out.bytes().size());
        std::copy(out.bytes().begin(),out.bytes().end(),bytes);
        b->script->pop_typed(); return 1;
    });
}
NG_API void ng_unbind_replica(Binding* b,ObjectId id) noexcept { b->script->unbind_replica(id); }
NG_API int ng_sync_replica(Binding* b,ObjectId id,const uint8_t* data,int size,uint8_t* out,int capacity,int* count,double* age,int forecast) noexcept {
    return guarded([&] {
        auto* game=b->script->active(); auto* obj=game ? game->object(id) : nullptr;
        if (!obj) {b->script->unbind_replica(id); return 0;}
        Values sample,applied;
        if (!data || size<=0 || size>game_payload_limit || !b->script->decode(obj->schema,false,Bytes(data,data+size),sample)) return 0;
        int mode=b->script->sync_replica(id,sample,applied,forecast!=0);
        if (!mode) return 0;
        Writer w; for (auto& v:applied) write_value(w,v);
        if (capacity<0 || w.bytes().size()>size_t(capacity)) return 0;
        std::copy(w.bytes().begin(),w.bytes().end(),out);
        *count=int(w.bytes().size()); *age=game->clock().seconds()-game->clock().object_time(obj->sample_tick);
        return mode;
    });
}
NG_API int ng_submit_fields(Binding* b,ObjectId id,const uint8_t* data,int size) noexcept {
    return guarded([&]{
        auto* g=b->script->active();
        auto* obj=g ? g->object(id) : nullptr;
        Values full;
        return int(obj && size>0 && size<=game_payload_limit && data &&
            b->script->decode(obj->schema,false,Bytes(data,data+size),full) && b->script->submit_fields(id,full));
    });
}
NG_API int ng_distribute(Binding* b, ObjectId id, int kind, const uint64_t* keys, int count,
                         int divisor) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] {
        if (count < 0 || count > 64 || (count && !keys))
            return 0;
        std::vector<uint64_t> ids;
        if (count)
            ids.assign(keys, keys + count);
        return int(b->script->distribute(id, InterestKind(kind), ids, divisor));
    });
}
NG_API int ng_subscribe(Binding* b, PlayerId player, int kind, const uint64_t* keys, const int* rates,
                        int count) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] {
        if (count < 0 || count > 64 || (count && (!keys || !rates)))
            return 0;
        std::vector<uint64_t> ids;
        std::vector<int> divisors;
        if (count) {
            ids.assign(keys, keys + count);
            divisors.assign(rates, rates + count);
        }
        return int(b->script->subscribe(player, InterestKind(kind), ids, divisors));
    });
}
NG_API int ng_threshold_clock(Binding* b, int enabled) noexcept {
    if (!b->script->active()) return 0;
    return int(b->script->threshold_clock(enabled!=0));
}
NG_API int ng_player_ttl(Binding* b, int64_t ms) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] { return int(b->script->set_player_ttl(ms)); });
}
using ManagedStep = int (*)(void*, const uint8_t*, int, const uint8_t*, int, Tick, uint8_t*, int);
NG_API int ng_register_prediction(Binding* b, SchemaId schema, uint16_t version, const uint8_t* neutral,
                                  int size, ManagedStep step, void* context) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] {
        if (!step || !neutral || size < 1 || size > 96)
            return 0;
        SimulationModel model{version, size_t(size), {neutral, neutral + size}};
        model.step = [b, schema, step, context](Values& values, const Bytes& input, Tick tick) {
            Writer state;
            for (const auto& value : values)
                write_value(state, value);
            std::array<uint8_t, game_payload_limit> result{};
            int n = step(context, state.bytes().data(), int(state.bytes().size()), input.data(),
                         int(input.size()), tick, result.data(), int(result.size()));
            return n > 0 && size_t(n) <= result.size() &&
                   b->script->decode(schema, false, {result.data(), result.data() + n}, values);
        };
        return int(b->script->prediction(schema, std::move(model)));
    });
}
NG_API int ng_bind_prediction(Binding* b, ObjectId id, PlayerId player) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] { return int(b->script->session().prediction().bind(id, player)); });
}
NG_API int ng_predict(Binding* b, ObjectId id, const uint8_t* data, int size) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] {
        return int(data && size > 0 && size <= 96 &&
                   b->script->session().prediction().advance(id, {data, data + size}));
    });
}
NG_API int ng_submit(Binding* b, ObjectId id, const uint8_t* data, int n) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] {
        auto& g = b->script->session();
        const auto* o = g.object(id);
        Values fields;
        return int(o && data && n > 0 && n <= game_payload_limit &&
                   b->script->decode(o->schema, !g.host(), {data, data + n}, fields) &&
                   b->script->submit(id, fields));
    });
}
NG_API int ng_read(Binding* b, ObjectId id, int rendered, uint8_t* dst, int n) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] {
        Values values;
        Writer out;
        if (rendered == 2) {
            auto* prediction = b->script->session().prediction().state(id);
            if (!prediction)
                return 0;
            values = *prediction;
        } else if (!b->script->read(id, rendered != 0, values))
            return 0;
        for (const auto& v : values)
            write_value(out, v);
        if (n < 0 || out.bytes().size() > static_cast<size_t>(n))
            return -1;
        std::copy(out.bytes().begin(), out.bytes().end(), dst);
        return int(out.bytes().size());
    });
}
NG_API int ng_claim(Binding* b, ObjectId id, int drop) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] {
        auto& w = b->script->session().writers();
        return int(drop ? w.release(id) : w.request(id));
    });
}
NG_API int ng_answer(Binding* b, const ScriptEvent* e, int approve) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] {
        return int(e->kind == ScriptEventKind::Request &&
                   b->script->answer({e->object, e->player, e->epoch, e->sequence}, approve != 0));
    });
}
NG_API PlayerId ng_writer(Binding* b, ObjectId id) noexcept {
    if (!b->script->active()) return 0;
    return b->script->session().writers().presentation_writer(id);
}
NG_API int ng_loaded(Binding* b, SceneId id, uint32_t gen) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] { return int(b->script->session().world().loaded(id, gen)); });
}
NG_API uint32_t ng_revision(Binding* b, ObjectId id, double* time) noexcept {
    if (!b->script->active()) { *time = 0; return 0; }
    const auto* o = b->script->session().object(id);
    *time = o ? b->script->session().clock().object_time(o->sample_tick) : 0;
    return o ? o->revision : 0;
}

NG_API int ng_load_map(Binding* b, SceneId scene, const char* asset) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] { return int(asset && b->script->session().world().load_map(scene, asset)); });
}
NG_API int ng_change_map(Binding* b, SceneId scene, const char* asset) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] { return int(asset && b->script->session().world().change_map(scene, asset)); });
}
NG_API int ng_unload_map(Binding* b, SceneId scene) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] { return int(b->script->session().world().unload_map(scene)); });
}
NG_API int ng_map_asset(Binding* b, SceneId scene, uint8_t* dst, int capacity) noexcept {
    if (!b->script->active()) return 0;
    auto& maps = b->script->session().world().maps();
    auto it = maps.find(scene);
    if (it == maps.end() || capacity < 0 || it->second.asset.size() > size_t(capacity))
        return 0;
    const auto& name = it->second.asset;
    std::copy(name.begin(), name.end(), dst);
    return int(name.size());
}
NG_API ObjectId ng_find(Binding* b, SceneId scene, uint64_t key) noexcept {
    if (!b->script->active()) return 0;
    return b->script->session().world().find(scene, key);
}
NG_API ObjectId ng_parent(Binding* b, ObjectId id) noexcept {
    if (!b->script->active()) return 0;
    auto* meta = b->script->session().world().identity(id);
    return meta ? meta->parent : 0;
}
NG_API int ng_reparent(Binding* b, ObjectId child, ObjectId parent) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] { return int(b->script->session().world().reparent(child, parent)); });
}
NG_API int ng_destroy(Binding* b, ObjectId id) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] { return int(b->script->session().world().destroy(id)); });
}
NG_API int ng_allow_spawn_policy(Binding* b, SchemaId schema, const uint8_t* data, int size,
                                 int creator) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] {
        Values fields;
        if (!data || size <= 0 || size > game_payload_limit ||
            !b->script->decode(schema, false, {data, data + size}, fields))
            return 0;
        return int(b->script->spawn_factory(schema,std::move(fields),creator!=0));
    });
}
NG_API int ng_allow_spawn(Binding* b, SchemaId schema, const uint8_t* data, int size) noexcept {
    if (!b->script->active()) return 0;
    return ng_allow_spawn_policy(b, schema, data, size, 0);
}
NG_API uint64_t ng_request_reference(Binding* b, SchemaId schema, SceneId scene) noexcept {
    if (!b->script->active()) return 0;
    uint64_t ref = 0;
    guarded([&] {
        ref = b->script->session().world().request_reference(schema, scene);
        return 1;
    });
    return ref;
}
NG_API uint64_t ng_field_reference(Binding* b, ObjectId id) noexcept {
    return b && b->script->active() ? b->script->session().world().field_reference(id).value : 0;
}
NG_API ObjectId ng_resolve_field(Binding* b, uint64_t ref) noexcept {
    return b && b->script->active() ? b->script->session().world().resolve_field({ref}) : 0;
}
NG_API ObjectId ng_resolve_reference(Binding* b, uint64_t ref) noexcept {
    if (!b->script->active()) return 0;
    return b->script->session().world().resolve(ref);
}
NG_API int ng_request_spawn(Binding* b, SchemaId schema, SceneId scene, uint32_t* sequence) noexcept {
    if (!b->script->active()) return 0;
    return guarded([&] {
        *sequence = b->script->session().world().request_spawn(schema, scene);
        return *sequence ? 1 : 0;
    });
}
