#include "session/direct_game.hpp"
#include "game.hpp"
#include "build_info.hpp"
#include <string>
#include "field_value.hpp"
#include <godot_cpp/core/class_db.hpp>
#include <sstream>
#include <algorithm>

using namespace godot;
namespace {
bool values_of(const std::vector<netcode::Field>& fields, const Array& array, netcode::Values& result) {
    using namespace netcode;
    if (array.size() != fields.size())
        return false;
    for (int i = 0; i < array.size(); ++i) {
        const Variant v = array[i];
        const auto t = v.get_type();
        Value out;
        switch (fields[i].type) {
        case FieldType::Float:
            if (t != Variant::FLOAT && t != Variant::INT)
                return false;
            out = float(double(v));
            break;
        case FieldType::Flag:
            if (t != Variant::BOOL)
                return false;
            out = bool(v);
            break;
        case FieldType::Int:
            if (t != Variant::INT || int64_t(v) < INT32_MIN || int64_t(v) > INT32_MAX)
                return false;
            out = int32_t(int64_t(v));
            break;
        case FieldType::Vec2: {
            if (t != Variant::VECTOR2)
                return false;
            const godot::Vector2 vec = v;
            out = netcode::Vector2{vec.x, vec.y};
            break;
        }
        case FieldType::Vec3: {
            if (t != Variant::VECTOR3)
                return false;
            const godot::Vector3 vec = v;
            out = netcode::Vector3{vec.x, vec.y, vec.z};
            break;
        }
        case FieldType::Quat: {
            if (t != Variant::QUATERNION)
                return false;
            const Quaternion q = v;
            out = Rotation{q.x, q.y, q.z, q.w};
            break;
        }
        case FieldType::Vec4: {
            if (t!=Variant::VECTOR4) return false;
            godot::Vector4 p=v; out=netcode::Vector4{{p.x,p.y,p.z,p.w}}; break;
        }
        case FieldType::Color4: {
            if (t!=Variant::COLOR) return false;
            godot::Color p=v; out=netcode::Color{{p.r,p.g,p.b,p.a}}; break;
        }
        case FieldType::Reference:
            if (t!=Variant::INT) return false;
            out=ObjectReference{uint64_t(int64_t(v))}; break;
        case FieldType::ByteArray: {
            if (t!=Variant::PACKED_BYTE_ARRAY) return false;
            PackedByteArray a=v; if (a.size()>fields[i].limit) return false;
            Bytes b; if (a.size()) b.assign(a.ptr(),a.ptr()+a.size());
            out=std::move(b); break;
        }
        case FieldType::Vec2List: {
            if (t!=Variant::PACKED_VECTOR2_ARRAY) return false;
            PackedVector2Array a=v; if (a.size()>fields[i].limit) return false;
            Vector2Array b; for (int j=0;j<a.size();++j) b.push_back({a[j].x,a[j].y});
            out=std::move(b); break;
        }
        case FieldType::IntDictionary: {
            if (t!=Variant::DICTIONARY) return false;
            Dictionary a=v; if (a.size()>fields[i].limit) return false;
            IntMap b; Array keys=a.keys();
            for (int j=0;j<keys.size();++j) {
                Variant k=keys[j], n=a[k];
                if (k.get_type()!=Variant::INT || n.get_type()!=Variant::INT ||
                    int64_t(k)<INT32_MIN || int64_t(k)>INT32_MAX || int64_t(n)<INT32_MIN || int64_t(n)>INT32_MAX) return false;
                b.emplace(int32_t(int64_t(k)),int32_t(int64_t(n)));
            }
            out=std::move(b); break;
        }
        case FieldType::LinkedInts: {
            if (t!=Variant::ARRAY) return false;
            Array a=v; if (a.size()>fields[i].limit) return false;
            IntLinkedList b;
            for (int j=0;j<a.size();++j) {
                Variant n=a[j];
                if (n.get_type()!=Variant::INT || int64_t(n)<INT32_MIN || int64_t(n)>INT32_MAX) return false;
                b.push_back(int32_t(int64_t(n)));
            }
            out=std::move(b); break;
        }
        case FieldType::String: {
            if (t != Variant::STRING)
                return false;
            const auto str = String(v).utf8();
            out = std::string(str.get_data(), str.length());
            break;
        }
        case FieldType::IntList: {
            if (t != Variant::PACKED_INT32_ARRAY)
                return false;
            const PackedInt32Array a = v;
            IntArray items;
            for (int64_t n = 0; n < a.size(); ++n)
                items.push_back(a[n]);
            out = std::move(items);
            break;
        }
        case FieldType::FloatList: {
            if (t != Variant::PACKED_FLOAT32_ARRAY)
                return false;
            const PackedFloat32Array a = v;
            FloatArray items;
            for (int64_t n = 0; n < a.size(); ++n)
                items.push_back(a[n]);
            out = std::move(items);
            break;
        }
        }
        if (!valid_value(fields[i], out))
            return false;
        result.push_back(std::move(out));
    }
    return true;
}
} // namespace
bool NetcodeGame::validate_layout(const String& config) {
    try {
        std::istringstream text(config.utf8().get_data());
        return !netcode::ReplicationLayout(text).schemas().all().empty();
    } catch (const std::exception&) {
        return false;
    }
}
godot::String NetcodeGame::sdk_build_id() {
    return godot::String(netcode::core_build_id()) + "/" + NETCODE_PLATFORM_BUILD_ID;
}

void NetcodeGame::_bind_methods() {
    ClassDB::bind_static_method("NetcodeGame", D_METHOD("sdk_build_id"), &NetcodeGame::sdk_build_id);
    ClassDB::bind_method(D_METHOD("bind_method","name","id","arguments","direction","reliable","writer","room","handler"),&NetcodeGame::bind_method);
    ClassDB::bind_method(D_METHOD("call_method","name","object","arguments","target","token"),&NetcodeGame::call_method,DEFVAL(0),DEFVAL(0));
    ClassDB::bind_method(D_METHOD("bind_array_method","name","id","arguments","direction","reliable","handler"),&NetcodeGame::bind_array_method);
    ClassDB::bind_method(D_METHOD("routing_info","object","audience"),&NetcodeGame::routing_info,DEFVAL(true));
    ClassDB::bind_method(D_METHOD("submit_fields","object","values"),&NetcodeGame::submit_fields);
    ClassDB::bind_method(D_METHOD("sync_replica","object","sample","forecast"),&NetcodeGame::sync_replica,DEFVAL(false));
    ClassDB::bind_method(D_METHOD("unbind_replica","object"),&NetcodeGame::unbind_replica);
    ClassDB::bind_static_method("NetcodeGame", D_METHOD("validate_layout", "config"),
                                &NetcodeGame::validate_layout);
    ClassDB::bind_method(D_METHOD("open", "config", "host", "address", "port", "capacity", "watch"),
                         &NetcodeGame::open);
    ClassDB::bind_method(D_METHOD("model", "schema", "fields", "policy", "cooldown"), &NetcodeGame::model);
    ClassDB::bind_method(D_METHOD("distribute", "object", "kind", "keys", "divisor"),
                         &NetcodeGame::distribute);
    ClassDB::bind_method(D_METHOD("subscribe", "player", "kind", "keys", "rates"), &NetcodeGame::subscribe);
    ClassDB::bind_method(D_METHOD("use_threshold_clock", "enabled"), &NetcodeGame::use_threshold_clock);
    ClassDB::bind_method(D_METHOD("set_player_ttl", "milliseconds"), &NetcodeGame::set_player_ttl);
    ClassDB::bind_method(D_METHOD("register_prediction", "schema", "version", "neutral", "step"),
                         &NetcodeGame::register_prediction);
    ClassDB::bind_method(D_METHOD("bind_prediction", "object", "player"), &NetcodeGame::bind_prediction);
    ClassDB::bind_method(D_METHOD("predict", "object", "input"), &NetcodeGame::predict);
    ClassDB::bind_method(D_METHOD("predicted_fields", "object"), &NetcodeGame::predicted_fields);
    ClassDB::bind_method(D_METHOD("advance"), &NetcodeGame::advance);
    ClassDB::bind_method(D_METHOD("add_standby","player","address","port"),&NetcodeGame::add_standby);
    ClassDB::bind_method(D_METHOD("handoff"),&NetcodeGame::handoff);
    ClassDB::bind_method(D_METHOD("migration_phase"),&NetcodeGame::migration_phase);
    ClassDB::bind_method(D_METHOD("start"), &NetcodeGame::start);
    ClassDB::bind_method(D_METHOD("close"), &NetcodeGame::close);
    ClassDB::bind_method(D_METHOD("spawn", "schema", "values", "scene", "key", "parent", "mode"),
                         &NetcodeGame::spawn, DEFVAL(-1));
    ClassDB::bind_method(
        D_METHOD("register_rpc", "method", "direction", "reliable", "max_bytes", "writer", "room"),
        &NetcodeGame::register_rpc);
    ClassDB::bind_method(D_METHOD("invoke_rpc", "id", "method", "data", "target", "token"),
                         &NetcodeGame::invoke_rpc, DEFVAL(0), DEFVAL(0));
    ClassDB::bind_method(D_METHOD("submit", "id", "values"), &NetcodeGame::submit);
    ClassDB::bind_method(D_METHOD("read_fields", "id", "presentation"), &NetcodeGame::read_fields);
    ClassDB::bind_method(D_METHOD("claim", "id"), &NetcodeGame::claim);
    ClassDB::bind_method(D_METHOD("release", "id"), &NetcodeGame::release);
    ClassDB::bind_method(D_METHOD("answer", "event", "approve"), &NetcodeGame::answer);
    ClassDB::bind_method(D_METHOD("sample_time", "id"), &NetcodeGame::sample_time);
    ClassDB::bind_method(D_METHOD("revision", "id"), &NetcodeGame::revision);
    ClassDB::bind_method(D_METHOD("writer", "id"), &NetcodeGame::writer);
    ClassDB::bind_method(D_METHOD("load_map", "scene", "asset"), &NetcodeGame::load_map);
    ClassDB::bind_method(D_METHOD("change_map", "scene", "asset"), &NetcodeGame::change_map);
    ClassDB::bind_method(D_METHOD("unload_map", "scene"), &NetcodeGame::unload_map);
    ClassDB::bind_method(D_METHOD("map_asset", "scene"), &NetcodeGame::map_asset);
    ClassDB::bind_method(D_METHOD("find_object", "scene", "key"), &NetcodeGame::find_object);
    ClassDB::bind_method(D_METHOD("parent_of", "id"), &NetcodeGame::parent_of);
    ClassDB::bind_method(D_METHOD("reparent_object", "child", "parent"), &NetcodeGame::reparent_object);
    ClassDB::bind_method(D_METHOD("destroy_object", "id"), &NetcodeGame::destroy_object);
    ClassDB::bind_method(D_METHOD("allow_spawn", "schema", "defaults", "creator_writes"),
                         &NetcodeGame::allow_spawn, DEFVAL(false));
    ClassDB::bind_method(D_METHOD("request_reference", "schema", "scene"), &NetcodeGame::request_reference);
    ClassDB::bind_method(D_METHOD("field_reference","object"), &NetcodeGame::field_reference);
    ClassDB::bind_method(D_METHOD("resolve_field","reference"), &NetcodeGame::resolve_field);
    ClassDB::bind_method(D_METHOD("resolve_reference", "reference"), &NetcodeGame::resolve_reference);
    ClassDB::bind_method(D_METHOD("request_spawn", "schema", "scene"), &NetcodeGame::request_spawn);
    ClassDB::bind_method(D_METHOD("loaded", "scene", "generation"), &NetcodeGame::loaded);
}
String NetcodeGame::open(const String& config, bool host, const String& address, int port, int capacity,
                         bool watch) {
    if (game_ || port < 1 || port > 65535 || capacity < 1 || capacity > 255)
        return "Invalid session options";
    try {
        std::istringstream stream(config.utf8().get_data());
        netcode::ReplicationLayout layout(stream);
        netcode::SessionOptions options;
        options.host = host;
        options.spectator = watch;
        options.extended_join = true;
        options.admission = {static_cast<uint8_t>(capacity), true, true};
        netcode::DirectConfig direct{address.utf8().get_data(), static_cast<uint16_t>(port)};
        game_ = netcode::open_direct_game(direct,options,layout);
        return {};
    } catch (const std::exception& error) {
        return String::utf8(error.what());
    }
}
bool NetcodeGame::model(int schema, const PackedInt32Array& fields, int policy, int cooldown) {
    if (!available() || schema < 1 || schema > UINT16_MAX || policy < 0 ||
        policy > int(netcode::ClaimMode::GameGlobal) || cooldown < 0)
        return false;
    std::vector<netcode::FieldId> ids;
    for (int64_t i = 0; i < fields.size(); ++i) {
        if (fields[i] < 1 || fields[i] > UINT16_MAX)
            return false;
        ids.push_back(fields[i]);
    }
    if (!game_->model(schema, ids, static_cast<netcode::ClaimMode>(policy), cooldown))
        return false;
    for (const auto field : game_->session().schemas().get(schema)->fields)
        if (std::binary_search(ids.begin(), ids.end(), field.id))
            writable_[schema].push_back(field);
    return true;
}
Dictionary NetcodeGame::advance() {
    Dictionary frame;
    if (!game_ || stepping_)
        return frame;
    game_->pump(netcode::Clock::now());
    if (!available()) return frame;
    auto& g = game_->session();
    frame["term"] = g.term();
    frame["host"] = g.host() != nullptr;
    frame["saved_tick"] = game_->migration().saved_tick();
    frame["armed"] = game_->migration().armed();
    frame["player"] = g.local();
    frame["round"] = static_cast<int64_t>(g.round());
    frame["seats"] = int(g.host() ? g.host()->members().size() : g.guest()->seats());
    frame["time"] = g.clock().seconds();
    frame["rtt"] = g.clock().rtt();
    frame["rate"] = g.clock().rate();
    frame["offset"] = g.clock().offset();
    PackedInt32Array ids;
    for (auto id : g.objects())
        ids.push_back(id);
    frame["objects"] = ids;
    Array events;
    for (const auto& e : game_->take_events()) {
        Dictionary d;
        d["kind"] = int(e.kind);
        d["object"] = e.object;
        d["player"] = e.player;
        d["epoch"] = e.epoch;
        d["sequence"] = e.sequence;
        d["status"] = e.status;
        events.push_back(d);
    }
    frame["events"] = events;
    Array rpcs;
    while (const auto* call = game_->next_rpc()) {
        Dictionary item;
        item["object"] = call->object;
        item["sender"] = call->sender;
        item["method"] = call->method;
        PackedByteArray data;
        for (auto byte : call->arguments)
            data.push_back(byte);
        item["data"] = data;
        rpcs.push_back(item);
        game_->pop_rpc();
    }
    frame["rpcs"] = rpcs;
    Array outcomes;
    for (const auto& outcome : g.rpc().take_outcomes()) {
        Dictionary item;
        item["token"] = int64_t(outcome.token);
        item["object"] = outcome.object;
        item["player"] = outcome.player;
        item["method"] = outcome.method;
        item["status"] = int(outcome.status);
        outcomes.push_back(item);
    }
    frame["rpc_outcomes"] = outcomes;
    // Remove all deliveries before invoking user code, which may close or replace the session.
    Array callbacks, arguments;
    while (const auto* call=game_->next_typed()) {
        auto it=handlers_.find(call->method);
        if (it!=handlers_.end() && it->second.is_valid()) {
            Array args; args.push_back(call->sender); args.push_back(call->object);
            Array values;
            for (const auto& value : call->values) values.push_back(netcode_godot::field_value(value));
            if (array_handlers_.count(call->method)) args.push_back(values);
            else args.append_array(values);
            callbacks.push_back(it->second); arguments.push_back(args);
        }
        game_->pop_typed();
    }
    auto* source=game_.get();
    for (int i=0;i<callbacks.size() && source==game_.get();++i) {
        Callable fn=callbacks[i]; if (fn.is_valid()) fn.callv(arguments[i]);
    }
    return frame;
}
bool NetcodeGame::add_standby(int64_t player,const godot::String& address,int port) {
    return available() && !stepping_ && player > 0 && player <= UINT32_MAX && port > 0 && port <= 65535 &&
        game_->migration().add_standby(uint32_t(player),{address.utf8().get_data(),uint16_t(port)});
}
bool NetcodeGame::handoff() { return available() && !stepping_ && game_->migration().handoff(); }
int NetcodeGame::migration_phase() const { return game_ ? int(game_->migration().phase()) : int(netcode::MigrationPhase::Ended); }
bool NetcodeGame::start() { return available() && game_->session().start(netcode::Clock::now()); }
void NetcodeGame::close() {
    if (stepping_)
        return;
    if (available())
        game_->session().end(netcode::Clock::now());
    game_.reset();
    writable_.clear();
    handlers_.clear();
    array_handlers_.clear();
}
int64_t NetcodeGame::spawn(int schema, const Array& values, int scene, int64_t key, int64_t parent,
                           int mode) {
    if (stepping_)
        return 0;
    if (mode < -1 || mode > int(netcode::ClaimMode::GameGlobal))
        return 0;
    if (!available() || schema < 1 || schema > UINT16_MAX || scene < 0 || parent < 0 || parent > UINT32_MAX)
        return 0;
    auto* def = game_->session().schemas().get(schema);
    netcode::Values out;
    if (!def || !values_of(def->fields, values, out))
        return 0;
    return game_->session().world().spawn(
        schema, game_->session().local(), out,
        {uint32_t(scene), uint64_t(key), uint32_t(parent),
         mode < 0 ? std::nullopt : std::optional{netcode::ClaimMode(mode)}});
}
bool NetcodeGame::distribute(int64_t object, int kind, const PackedInt64Array& keys, int divisor) {
    if (!available() || stepping_ || object <= 0 || object > UINT32_MAX || keys.size() > 64)
        return false;
    std::vector<uint64_t> ids;
    for (int i = 0; i < keys.size(); ++i)
        ids.push_back(keys[i]);
    return game_->distribute(object, netcode::InterestKind(kind), ids, divisor);
}
bool NetcodeGame::subscribe(int64_t player, int kind, const PackedInt64Array& keys,
                            const PackedInt32Array& rates) {
    if (!available() || stepping_ || player <= 0 || player > UINT32_MAX || keys.size() > 64 ||
        rates.size() != keys.size())
        return false;
    std::vector<uint64_t> ids;
    std::vector<int> divisors;
    for (int i = 0; i < keys.size(); ++i) {
        ids.push_back(keys[i]);
        divisors.push_back(rates[i]);
    }
    return game_->subscribe(player, netcode::InterestKind(kind), ids, divisors);
}
bool NetcodeGame::use_threshold_clock(bool enabled) {
    return available() && !stepping_ &&
           game_->threshold_clock(enabled);
}
bool NetcodeGame::set_player_ttl(int64_t ms) { return available() && !stepping_ && game_->set_player_ttl(ms); }
bool NetcodeGame::register_prediction(int schema, int version, const PackedByteArray& neutral,
                                      const Callable& step) {
    if (!available() || stepping_ || !step.is_valid() || schema < 1 || schema > UINT16_MAX || version < 1 ||
        version > UINT16_MAX || neutral.is_empty() || neutral.size() > 96)
        return false;
    auto* def = game_->session().schemas().get(schema);
    if (!def)
        return false;
    netcode::SimulationModel model;
    model.version = version;
    model.input_bytes = neutral.size();
    model.neutral.assign(neutral.ptr(), neutral.ptr() + neutral.size());
    model.step = [this, fields = def->fields, step](netcode::Values& values, const netcode::Bytes& input,
                                                    netcode::Tick tick) {
        if (!step.is_valid() || stepping_)
            return false;
        Array state;
        for (const auto& v : values)
            state.push_back(netcode_godot::field_value(v));
        PackedByteArray bytes;
        bytes.resize(input.size());
        std::copy(input.begin(), input.end(), bytes.ptrw());
        struct StepGuard {
            bool& active;
            ~StepGuard() { active = false; }
        };
        StepGuard guard{stepping_};
        stepping_ = true;
        Variant next = step.call(state, bytes, int64_t(tick));
        netcode::Values result;
        if (next.get_type() != Variant::ARRAY || !values_of(fields, Array(next), result))
            return false;
        values = std::move(result);
        return true;
    };
    return game_->prediction(schema, std::move(model));
}
bool NetcodeGame::bind_prediction(int64_t object, int64_t player) {
    return available() && !stepping_ && object > 0 && object <= UINT32_MAX && player > 0 && player <= UINT32_MAX &&
           game_->session().prediction().bind(object, player);
}
bool NetcodeGame::predict(int64_t object, const PackedByteArray& input) {
    return available() && !stepping_ && object > 0 && object <= UINT32_MAX && !input.is_empty() &&
           input.size() <= 96 &&
           game_->session().prediction().advance(object, {input.ptr(), input.ptr() + input.size()});
}
Array NetcodeGame::predicted_fields(int64_t object) {
    Array fields;
    auto* values =
        available() && object > 0 && object <= UINT32_MAX ? game_->session().prediction().state(object) : nullptr;
    if (values)
        for (const auto& v : *values)
            fields.push_back(netcode_godot::field_value(v));
    return fields;
}
bool NetcodeGame::register_rpc(int method, int direction, bool reliable, int max_bytes, bool writer,
                               bool room) {
    return available() && game_->register_rpc(method, direction, reliable, max_bytes, writer, room);
}
bool NetcodeGame::bind_method(const String& name,int method,int schema,int dir,bool reliable,bool writer,bool room,const Callable& fn) {
    if (!available() || stepping_ || method<1 || method>UINT16_MAX || schema<0 || schema>UINT16_MAX) return false;
    auto args = game_->schemas().get(schema);
    if (!fn.is_null() && (!fn.is_valid() || fn.get_argument_count()!=int64_t(2+(args ? args->fields.size() : 0)))) return false;
    if (!game_->bind_method(name.utf8().get_data(),method,schema,dir,reliable,writer,room)) return false;
    handlers_[method]=fn;
    return true;
}
bool NetcodeGame::bind_array_method(const String& name,int method,int schema,int dir,bool reliable,const Callable& fn) {
    if (!fn.is_valid() || fn.get_argument_count()!=3 ||
        !bind_method(name,method,schema,dir,reliable,false,true,Callable())) return false;
    handlers_[method]=fn;
    array_handlers_.insert(method);
    return true;
}
Dictionary NetcodeGame::routing_info(int64_t id,bool audience) {
    Dictionary info;
    if (!available() || id<0 || id>UINT32_MAX) return info;
    auto& g=game_->session();
    auto* obj=id ? g.object(id) : nullptr;
    if (!g.round() || (id && !obj)) return info;
    info["player"]=g.local();
    info["master"]=g.coordinator();
    info["schema"]=obj ? obj->schema : 0;
    info["writer"]=obj ? g.writers().writer(id) : g.coordinator();
    auto* grant=obj ? g.writers().grant(id) : nullptr;
    info["epoch"]=grant ? grant->epoch : 0;
    info["watch"]=g.guest() && g.guest()->spectator();
    if (!audience) return info;
    PackedInt64Array players;
    if (auto* host=g.host()) {
        players.push_back(g.local());
        for (const auto& [player,member] : host->members())
            if (player!=g.local() && member.peer && member.ready && (!obj || g.authority().known(member.peer,id)))
                players.push_back(player);
    }
    info["audience"]=players;
    return info;
}
bool NetcodeGame::call_method(const String& method,int64_t object,const Array& args,int64_t target,int64_t token) {
    if (!available() || stepping_ || object<0 || object>UINT32_MAX || target<0 || target>UINT32_MAX) return false;
    std::string name=method.utf8().get_data();
    auto schema=game_->schemas().get(game_->argument_schema(name));
    netcode::Values values;
    if (schema ? !values_of(schema->fields,args,values) : !args.is_empty()) return false;
    return game_->call_method(name,object,values,target,uint64_t(token));
}
void NetcodeGame::unbind_replica(int64_t id) {
    if (game_ && !stepping_ && id>0 && id<=UINT32_MAX) game_->unbind_replica(id);
}
Dictionary NetcodeGame::sync_replica(int64_t id,const Array& sample,bool forecast) {
    Dictionary result;
    if (!available() || stepping_ || id<=0 || id>UINT32_MAX) return result;
    auto& g=game_->session(); auto* obj=g.object(id);
    if (!obj) {game_->unbind_replica(id); return result;}
    netcode::Values fields,applied;
    if (!values_of(game_->schemas().get(obj->schema)->fields,sample,fields)) return result;
    int mode=game_->sync_replica(id,fields,applied,forecast);
    if (mode) {
        Array out; for (const auto& v:applied) out.push_back(netcode_godot::field_value(v));
        result["fields"]=out; result["baseline"]=mode==2;
        result["age"]=g.clock().seconds()-g.clock().object_time(obj->sample_tick);
    }
    return result;
}
bool NetcodeGame::submit_fields(int64_t id,const Array& values) {
    if (!available() || stepping_ || id<=0 || id>UINT32_MAX) return false;
    auto* obj=game_->session().object(id);
    netcode::Values full;
    return obj && values_of(game_->schemas().get(obj->schema)->fields,values,full) && game_->submit_fields(id,full);
}
bool NetcodeGame::invoke_rpc(int64_t id, int method, const PackedByteArray& data, int64_t target,
                             int64_t token) {
    if (stepping_)
        return false;
    if (!available() || id < 0 || id > UINT32_MAX || method <= 0 || method > UINT16_MAX || target < 0 ||
        target > UINT32_MAX || data.size() > 256)
        return false;
    netcode::Bytes bytes;
    if (data.size())
        bytes.assign(data.ptr(), data.ptr() + data.size());
    return game_->session().rpc().invoke(id, method, bytes, target, uint64_t(token));
}
bool NetcodeGame::submit(int64_t id, const Array& values) {
    if (stepping_)
        return false;
    auto* obj = available() ? game_->session().object(id) : nullptr;
    if (!obj)
        return false;
    const auto& def = *game_->session().schemas().get(obj->schema);
    netcode::Values out;
    return values_of(game_->session().host() ? def.fields : writable_[obj->schema], values, out) &&
           game_->submit(id, out);
}
Array NetcodeGame::read_fields(int64_t id, bool presentation) {
    Array data;
    netcode::Values fields;
    if (available() && game_->read(id, presentation, fields))
        for (const auto& v : fields)
            data.push_back(netcode_godot::field_value(v));
    return data;
}
bool NetcodeGame::claim(int64_t id) {
    if (stepping_)
        return false;
    return available() && game_->session().writers().request(id);
}
bool NetcodeGame::release(int64_t id) {
    if (stepping_)
        return false;
    return available() && game_->session().writers().release(id);
}
bool NetcodeGame::answer(const Dictionary& e, bool approve) {
    if (stepping_)
        return false;
    if (!available() || int(e.get("kind", -1)) != int(netcode::ScriptEventKind::Request))
        return false;
    return game_->answer({uint32_t(int64_t(e["object"])), uint32_t(int64_t(e["player"])),
                          uint32_t(int64_t(e["epoch"])), uint32_t(int64_t(e["sequence"]))},
                         approve);
}
int64_t NetcodeGame::writer(int64_t id) {
    return available() ? game_->session().writers().presentation_writer(id) : 0;
}
bool NetcodeGame::loaded(int id, int generation) {
    if (stepping_)
        return false;
    return available() && game_->session().world().loaded(id, generation);
}
double NetcodeGame::sample_time(int64_t id) {
    auto* o = available() ? game_->session().object(id) : nullptr;
    return o ? game_->session().clock().object_time(o->sample_tick) : 0;
}
int64_t NetcodeGame::revision(int64_t id) {
    auto* o = available() ? game_->session().object(id) : nullptr;
    return o ? o->revision : 0;
}

bool NetcodeGame::load_map(int scene, const String& asset) {
    if (stepping_)
        return false;
    auto name = asset.utf8();
    return available() && scene > 0 &&
           game_->session().world().load_map(scene, {name.get_data(), size_t(name.length())});
}
bool NetcodeGame::change_map(int scene, const String& asset) {
    auto name = asset.utf8();
    return !stepping_ && available() && scene > 0 &&
           game_->session().world().change_map(scene, {name.get_data(), size_t(name.length())});
}
bool NetcodeGame::unload_map(int scene) {
    if (stepping_)
        return false;
    return available() && scene > 0 && game_->session().world().unload_map(scene);
}
String NetcodeGame::map_asset(int scene) {
    if (!available())
        return {};
    const auto& maps = game_->session().world().maps();
    auto it = maps.find(scene);
    return it == maps.end() ? String{} : String::utf8(it->second.asset.c_str());
}
int64_t NetcodeGame::find_object(int scene, int64_t key) {
    return available() && scene >= 0 ? game_->session().world().find(scene, key) : 0;
}
int64_t NetcodeGame::parent_of(int64_t id) {
    auto* meta = available() && id > 0 && id <= UINT32_MAX ? game_->session().world().identity(id) : nullptr;
    return meta ? meta->parent : 0;
}
bool NetcodeGame::reparent_object(int64_t child, int64_t parent) {
    if (stepping_)
        return false;
    return available() && child > 0 && child <= UINT32_MAX && parent >= 0 && parent <= UINT32_MAX &&
           game_->session().world().reparent(child, parent);
}
bool NetcodeGame::destroy_object(int64_t id) {
    if (stepping_)
        return false;
    return available() && id > 0 && id <= UINT32_MAX && game_->session().world().destroy(id);
}
bool NetcodeGame::allow_spawn(int schema, const Array& defaults, bool creator_writes) {
    if (!available() || schema < 1 || schema > UINT16_MAX)
        return false;
    const auto* def = game_->session().schemas().get(schema);
    netcode::Values fields;
    if (!def || !values_of(def->fields, defaults, fields))
        return false;
    return game_->spawn_factory(schema,std::move(fields),creator_writes);
}
int64_t NetcodeGame::request_reference(int schema, int scene) {
    return available() && !stepping_ && schema > 0 && schema <= UINT16_MAX && scene >= 0
               ? game_->session().world().request_reference(schema, scene)
               : 0;
}
int64_t NetcodeGame::field_reference(int64_t id) {
    return available() && id > 0 && id <= UINT32_MAX ? game_->session().world().field_reference(id).value : 0;
}
int64_t NetcodeGame::resolve_field(int64_t reference) {
    return available() ? game_->session().world().resolve_field({uint64_t(reference)}) : 0;
}
int64_t NetcodeGame::resolve_reference(int64_t reference) {
    return available() ? game_->session().world().resolve(reference) : 0;
}
int64_t NetcodeGame::request_spawn(int schema, int scene) {
    return available() && schema > 0 && schema <= UINT16_MAX && scene >= 0
               ? game_->session().world().request_spawn(schema, scene)
               : 0;
}
