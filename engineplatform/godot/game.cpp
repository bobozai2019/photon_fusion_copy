#include "session/direct_game.hpp"
#include "game.hpp"
#include "build_info.hpp"
#include <string>
#include "field_value.hpp"
#include "host/game.hpp"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace godot;
namespace {
const char module_anchor = 0;

std::string byte_string(const PackedByteArray& bytes) {
    return bytes.size() ? std::string(reinterpret_cast<const char*>(bytes.ptr()), bytes.size()) : std::string{};
}

netcode::Bytes encode_native_input(float delta, const PackedByteArray& payload)
{
    netcode::Writer input;
    input.f32(delta);
    input.append(netcode::Bytes(payload.ptr(), payload.ptr() + payload.size()));
    return input.bytes();
}

Variant decode_room_value(const netcode::Bytes& value)
{
    PackedByteArray encoded;
    encoded.resize(value.size());
    std::copy(value.begin(), value.end(), encoded.ptrw());
    return UtilityFunctions::bytes_to_var(encoded);
}

bool valid_room_key(const std::string& key)
{
    if (key.empty() || key.size() > 128) return false;
    const auto decoded = String::utf8(key.data(), key.size()).utf8();
    return size_t(decoded.length()) == key.size() && std::equal(key.begin(), key.end(), decoded.get_data());
}

bool valid_room_value(const std::string& key, const netcode::Bytes& bytes)
{
    if (!valid_room_key(key)) return false;
    if (bytes.empty()) return true;
    const auto value = decode_room_value(bytes);
    if (!NetcodeGame::is_portable_value(value)) return false;
    const auto encoded = UtilityFunctions::var_to_bytes(value);
    return size_t(encoded.size()) == bytes.size() && std::equal(bytes.begin(), bytes.end(), encoded.ptr());
}

bool encode_room_properties(const Dictionary& input, netcode::RoomProperties::Values& values)
{
    if (input.size() > 64) return false;
    const auto keys = input.keys();
    for (int64_t i = 0; i < keys.size(); ++i) {
        const Variant key = keys[i];
        if (key.get_type() != Variant::STRING && key.get_type() != Variant::STRING_NAME) return false;
        const auto name = String(key).utf8();
        if (!name.length() || name.length() > 128) return false;
        const std::string property(name.get_data(), name.length());
        const auto value = input[key];
        if (!NetcodeGame::is_portable_value(value)) return false;
        const auto encoded = UtilityFunctions::var_to_bytes(value);
        if (encoded.is_empty() || encoded.size() > 4096) return false;
        netcode::Bytes bytes(encoded.ptr(), encoded.ptr() + encoded.size());
        if (!valid_room_value(property, bytes) || !values.emplace(property, std::move(bytes)).second) return false;
    }
    return true;
}

bool value_of(netcode::Field field, const Variant& v, netcode::Value& out) {
    using namespace netcode;
    const auto t = v.get_type();
    switch (field.type) {
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
        PackedByteArray a=v; if (a.size()>field.limit) return false;
        if (!std::holds_alternative<Bytes>(out)) out.emplace<Bytes>();
        auto& bytes = std::get<Bytes>(out);
        if (a.is_empty()) bytes.clear();
        else bytes.assign(a.ptr(), a.ptr() + a.size());
        break;
    }
    case FieldType::Vec2List: {
        if (t!=Variant::PACKED_VECTOR2_ARRAY) return false;
        PackedVector2Array a=v; if (a.size()>field.limit) return false;
        Vector2Array b; for (int j=0;j<a.size();++j) b.push_back({a[j].x,a[j].y});
        out=std::move(b); break;
    }
    case FieldType::IntDictionary: {
        if (t!=Variant::DICTIONARY) return false;
        Dictionary a=v; if (a.size()>field.limit) return false;
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
        Array a=v; if (a.size()>field.limit) return false;
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
    return valid_value(field, out);
}

bool values_of(const std::vector<netcode::Field>& fields, const Array& array, netcode::Values& result) {
    if (array.size() != fields.size())
        return false;
    result.reserve(result.size() + fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        netcode::Value out;
        if (!value_of(fields[i], array[i], out)) return false;
        result.push_back(std::move(out));
    }
    return true;
}
} // namespace

bool NetcodeGame::is_portable_value(const Variant& value, int depth)
{
    if (depth > 8) return false;
    switch (value.get_type()) {
    case Variant::OBJECT:
    case Variant::CALLABLE:
    case Variant::SIGNAL:
    case Variant::RID:
        return false;
    case Variant::ARRAY: {
        const Array values = value;
        if (values.size() > 256 || values.get_typed_builtin() == Variant::OBJECT) return false;
        for (int64_t i = 0; i < values.size(); ++i)
            if (!is_portable_value(values[i], depth + 1)) return false;
        return true;
    }
    case Variant::DICTIONARY: {
        const Dictionary values = value;
        if (values.size() > 256 || values.get_typed_key_builtin() == Variant::OBJECT ||
            values.get_typed_value_builtin() == Variant::OBJECT) return false;
        const auto keys = values.keys();
        for (int64_t i = 0; i < keys.size(); ++i)
            if (!is_portable_value(keys[i], depth + 1) || !is_portable_value(values[keys[i]], depth + 1)) return false;
        return true;
    }
    default:
        return true;
    }
}
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
    ClassDB::bind_method(D_METHOD("_has_replica_layout", "object", "schema"), &NetcodeGame::has_replica_layout);
    ClassDB::bind_method(D_METHOD("submit_fields","object","values"),&NetcodeGame::submit_fields);
    ClassDB::bind_method(D_METHOD("sync_replica","object","sample","forecast"),&NetcodeGame::sync_replica,DEFVAL(false));
    ClassDB::bind_method(D_METHOD("unbind_replica","object"),&NetcodeGame::unbind_replica);
    ClassDB::bind_static_method("NetcodeGame", D_METHOD("validate_layout", "config"),
                                &NetcodeGame::validate_layout);
    ClassDB::bind_method(D_METHOD("open", "config", "host", "address", "port", "capacity", "watch"),
                         &NetcodeGame::open);
    ClassDB::bind_method(D_METHOD("open_with_provider", "layout", "host", "endpoint", "capacity", "watch",
                                 "provider", "module", "config", "resume"),
                         &NetcodeGame::open_with_provider, DEFVAL(String("tcp")), DEFVAL(String()),
                         DEFVAL(PackedByteArray()), DEFVAL(PackedByteArray()));
    ClassDB::bind_method(D_METHOD("transport_build_id"), &NetcodeGame::transport_build_id);
    ClassDB::bind_method(D_METHOD("transport_stats"), &NetcodeGame::transport_stats);
    ClassDB::bind_method(D_METHOD("resume_ticket"), &NetcodeGame::resume_ticket);
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
    ClassDB::bind_method(D_METHOD("add_provider_standby", "player", "endpoint"), &NetcodeGame::add_provider_standby);
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
    ClassDB::bind_method(D_METHOD("reclaim", "id"), &NetcodeGame::reclaim);
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
        transport_build_ = "direct";
        return {};
    } catch (const std::exception& error) {
        return String::utf8(error.what());
    }
}
String NetcodeGame::open_with_provider(const String& text, bool host, const String& endpoint, int capacity,
                                       bool watch, const String& provider, const String& module,
                                       const PackedByteArray& config, const PackedByteArray& resume) {
    return open_provider(text, host, endpoint, capacity, watch, provider, module, config, resume,
                         netcode::SessionOptions{}.step);
}
String NetcodeGame::open_provider(const String& text, bool host, const String& endpoint, int capacity,
                                  bool watch, const String& provider, const String& module,
                                  const PackedByteArray& config, const PackedByteArray& resume,
                                  std::chrono::milliseconds step, bool retain_spatial_writer_interest) {
    if (game_ || capacity < 1 || capacity > 255) return "Invalid session options";
    try {
        std::istringstream stream(text.utf8().get_data());
        netcode::ReplicationLayout layout(stream);
        netcode::SessionOptions options;
        options.step = step;
        options.retain_spatial_writer_interest = retain_spatial_writer_interest;
        options.host = host;
        options.spectator = watch;
        options.extended_join = true;
        options.admission = {static_cast<uint8_t>(capacity), true, true};
        if (!resume.is_empty()) options.resume = netcode::decode_resume_ticket(resume.ptr(), resume.size());
        String resolved = module;
        auto* project = ProjectSettings::get_singleton();
        if (resolved.begins_with("res://") || resolved.begins_with("user://"))
            resolved = project->globalize_path(resolved);
        if (resolved.is_empty() && provider == "tcp") {
            const auto installed = project->globalize_path("res://addons/netcode/bin/netcode_transport_tcp.dll").utf8();
            std::error_code error;
            if (std::filesystem::is_regular_file(std::filesystem::u8path(installed.get_data()), error))
                resolved = String::utf8(installed.get_data());
        }
        netcode::ProviderConfig settings{provider.utf8().get_data(), std::filesystem::u8path(resolved.utf8().get_data()),
                                        byte_string(config)};
        auto opened = netcode::open_provider_game(settings, endpoint.utf8().get_data(), options, layout, &module_anchor);
        transport_build_ = String::utf8(opened.provider.label().c_str());
        game_ = std::move(opened.game);
        provider_game_ = true;
        simulation_delta_ = std::chrono::duration<double>(step).count();
        return {};
    } catch (const std::exception& error) {
        return String::utf8(error.what());
    }
}

PackedByteArray NetcodeGame::resume_ticket() {
    PackedByteArray result;
    if (!available() || !game_->session().guest()) return result;
    const auto bytes = netcode::encode_resume_ticket(game_->session().guest()->ticket());
    result.resize(bytes.size());
    if (!bytes.empty()) std::copy(bytes.begin(), bytes.end(), result.ptrw());
    return result;
}

Dictionary NetcodeGame::transport_stats() {
    Dictionary result;
    if (!available()) return result;
    const auto stats = game_->session().traffic();
    result["queued_bytes"] = static_cast<int64_t>(stats.queued_bytes);
    result["provider_peak_queued_bytes"] = static_cast<int64_t>(stats.peak_queued_bytes);
    result["core_peak_queued_bytes"] = static_cast<int64_t>(stats.core_peak_queued_bytes);
    result["oldest_queued_us"] = static_cast<int64_t>(stats.oldest_queued_us);
    result["snapshot_backpressure_skips"] = static_cast<int64_t>(stats.snapshot_backpressure_skips);
    return result;
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
    frame["phase"] = int(g.host() ? g.host()->phase() : g.guest()->phase());
    if (g.guest()) {
        frame["rejected"] = int(g.guest()->rejected());
        frame["disconnect"] = int(g.guest()->disconnected());
    }
    frame["properties_revision"] = g.room_properties().revision();
    frame["can_request_spawn"] = g.world().can_request_initialized();
    frame["native_spawn_ready"] = g.world().native_spawn_ready();
    frame["round"] = static_cast<int64_t>(g.round());
    frame["seats"] = int(g.host() ? g.host()->members().size() : g.guest()->seats());
    if (g.room_properties().ready()) {
        Array rosters;
        for (const auto& roster : g.room_properties().take_rosters()) {
            PackedInt64Array players;
            for (auto player : roster) players.push_back(player);
            rosters.push_back(players);
        }
        frame["native_rosters"] = rosters;
        frame["native_master"] = g.coordinator();
    }
    frame["time"] = g.clock().seconds();
    frame["rtt"] = g.clock().rtt();
    frame["rate"] = g.clock().rate();
    frame["offset"] = g.clock().offset();
    PackedInt32Array ids;
    const auto object_ids = g.objects();
    if (!object_ids.empty()) {
        ids.resize(object_ids.size());
        std::copy(object_ids.begin(), object_ids.end(), ids.ptrw());
    }
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

bool NetcodeGame::enable_room_properties(uint64_t catalog)
{
    if (!available() || game_->session().round()) return false;
    game_->session().room_properties().enable(catalog, [](const auto& a, const auto& b) {
        const Variant left = decode_room_value(a), right = decode_room_value(b);
        return left.get_type() == right.get_type() && left == right;
    }, valid_room_value);
    return true;
}

bool NetcodeGame::set_room_properties(const Dictionary& patch, const Dictionary& expected)
{
    netcode::RoomProperties::Values values, conditions;
    return available() && encode_room_properties(patch, values) && encode_room_properties(expected, conditions) &&
           game_->session().set_room_properties(values, conditions);
}

bool NetcodeGame::remove_room_properties(const PackedStringArray& keys)
{
    if (!available() || keys.size() > 64) return false;
    netcode::RoomProperties::Values values;
    for (int64_t i = 0; i < keys.size(); ++i) {
        const auto name = keys[i].utf8();
        if (!name.length() || name.length() > 128) return false;
        const std::string property(name.get_data(), name.length());
        if (!valid_room_key(property)) return false;
        values.try_emplace(property);
    }
    return game_->session().set_room_properties(values);
}

Dictionary NetcodeGame::room_properties() const
{
    Dictionary result;
    if (!available()) return result;
    for (const auto& [key, value] : game_->session().room_properties().values())
        result[String::utf8(key.data(), key.size())] = decode_room_value(value);
    return result;
}
bool NetcodeGame::add_standby(int64_t player,const godot::String& address,int port) {
    return available() && !stepping_ && !provider_game_ && player > 0 && player <= UINT32_MAX && port > 0 && port <= 65535 &&
        game_->migration().add_standby(uint32_t(player),{address.utf8().get_data(),uint16_t(port)});
}
bool NetcodeGame::add_provider_standby(int64_t player, const String& endpoint) {
    return available() && !stepping_ && provider_game_ && player > 0 && player <= UINT32_MAX &&
        game_->migration().add_standby(uint32_t(player), {{}, 0, endpoint.utf8().get_data()});
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
    scene_sample_.clear();
    provider_game_ = false;
    transport_build_ = String();
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

bool NetcodeGame::distribute_native_cell(int64_t object, uint64_t cell)
{
    return available() && !stepping_ && object > 0 && object <= UINT32_MAX && game_->session().host() &&
           game_->session().authority().set_distribution(object, {netcode::InterestKind::Area, cell});
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

bool NetcodeGame::register_native_prediction(int schema, int version, const PackedByteArray& neutral, PredictionStep step)
{
    if (!available() || !step || schema < 1 || schema > UINT16_MAX || version < 1 ||
        version > UINT16_MAX || neutral.is_empty() || neutral.size() > netcode::SimulationModel::payload_limit) return false;
    const auto* def = game_->session().schemas().get(schema);
    if (!def) return false;
    netcode::SimulationModel model;
    model.version = version;
    model.neutral = encode_native_input(0, neutral);
    model.input_bytes = model.neutral.size();
    model.timed_input = true;
    model.object_step = [this, fields = def->fields, step = std::move(step)](
            netcode::ObjectId id, netcode::Values& values, const netcode::Bytes& input,
            netcode::Tick tick, netcode::SimulationStep stage) {
        if (stepping_) return false;
        struct StepGuard {
            bool& active;
            ~StepGuard() { active = false; }
        } guard{stepping_};
        stepping_ = true;
        Array state;
        for (const auto& value : values) state.push_back(netcode_godot::field_value(value));
        const auto delta = netcode::SimulationModel::input_delta(input);
        if (!delta && stage != netcode::SimulationStep::Neutral) return false;
        PackedByteArray payload;
        payload.resize(input.size() - netcode::SimulationModel::timed_header_size);
        std::copy(input.begin() + netcode::SimulationModel::timed_header_size, input.end(), payload.ptrw());
        netcode::Values next;
        if (!values_of(fields, step(id, state, payload, delta ? double(delta) : simulation_delta_, tick, stage), next))
            return false;
        values = std::move(next);
        return true;
    };
    return game_->session().prediction().register_model(schema, std::move(model));
}

int64_t NetcodeGame::input_player(int64_t object) const {
    return available() ? game_->session().prediction().input_player(object) : 0;
}
int64_t NetcodeGame::input_queue(int64_t object) const {
    return available() ? game_->session().prediction().queued(object) : 0;
}
int64_t NetcodeGame::input_confirmed(int64_t object) const {
    return available() ? game_->session().prediction().confirmed(object) : 0;
}
int64_t NetcodeGame::input_replayed(int64_t object) const {
    return available() ? game_->session().prediction().replayed(object) : 0;
}
uint32_t NetcodeGame::simulation_tick() const {
    return available() ? game_->session().tick() : 0;
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
bool NetcodeGame::predict_native(int64_t object, double delta, const PackedByteArray& payload)
{
    if (!std::isfinite(delta) || delta <= 0 || delta > netcode::SimulationModel::max_input_delta) return false;
    const float wire_delta = float(delta);
    return available() && !stepping_ && object > 0 && object <= UINT32_MAX &&
           wire_delta > 0 && !payload.is_empty() &&
           payload.size() <= netcode::SimulationModel::payload_limit &&
           game_->session().prediction().advance(object, encode_native_input(wire_delta, payload));
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
    if (const auto* identity = obj ? g.world().identity(id) : nullptr) {
        info["key"] = int64_t(identity->key);
        info["reference"] = int64_t(identity->reference);
    }
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
bool NetcodeGame::has_replica_layout(int64_t id, int64_t schema) {
    if (!available() || id <= 0 || id > UINT32_MAX) return false;
    const auto& g = game_->session();
    const auto* object = g.object(id);
    return g.round() && object && object->schema == schema;
}
bool NetcodeGame::register_spawn_template(uint32_t template_id, int schema, netcode::ClaimMode mode,
                                          const PackedInt64Array& keys, bool spatial)
{
    if (!available() || keys.size() > 64) return false;
    netcode::Distribution distribution;
    if (spatial) {
        distribution.kind = netcode::InterestKind::Area;
    } else if (!keys.is_empty()) {
        distribution.kind = netcode::InterestKind::User;
        distribution.key = keys[0];
        for (int64_t i = 1; i < keys.size(); ++i) distribution.additional_keys.insert(keys[i]);
    }
    return game_->session().world().register_spawn_template(template_id, schema, mode, std::move(distribution));
}

bool NetcodeGame::can_request_native_spawn() const
{
    return available() && !stepping_ && game_->session().world().can_request_initialized();
}

bool NetcodeGame::prepare_spawn(int schema, const Array& fields, netcode::Values& values) const
{
    if (!available()) return false;
    const auto& schemas = game_->session().schemas();
    const auto* def = schemas.get(schema);
    return def && values_of(def->fields, fields, values) && schemas.valid(schema, values);
}

int64_t NetcodeGame::spawn_template(uint32_t template_id, const netcode::Values& values, bool host)
{
    if (!available() || stepping_) return 0;
    auto& game = game_->session();
    return host ? game.world().spawn_template(template_id, values) : game.world().request_initialized(template_id, values);
}
bool NetcodeGame::native_authority(int64_t id) const
{
    auto* game = game_ ? game_->active() : nullptr;
    if (!game || !game->object(id) || game->local() != game->writers().writer(id)) return false;
    const auto* identity = game->host() ? game->world().identity(id) : nullptr;
    return !identity || !identity->grant_creator;
}

bool NetcodeGame::pending_creator(int64_t id) const
{
    if (!available()) return false;
    const auto* identity = game_->session().world().identity(id);
    return identity && identity->grant_creator;
}
bool NetcodeGame::request_despawn(int64_t id, uint64_t reference)
{
    return available() && game_->session().world().request_despawn(id, reference);
}

bool NetcodeGame::subscribe_local(netcode::InterestKind kind, const std::set<uint64_t>& keys)
{
    return available() && (game_->session().host() || game_->session().subscribe_local(kind, keys));
}

bool NetcodeGame::subscribe_local_area_rates(const netcode::RoomProperties::AreaRates& rates)
{
    return available() && !game_->session().host() && game_->session().subscribe_local_area_rates(rates);
}

bool NetcodeGame::supports_area_rates() const
{
    return available() && game_->session().room_properties().supports_area_rates();
}

void NetcodeGame::allow_guest_spawns(bool allowed)
{
    if (available()) game_->session().world().set_guest_spawns_allowed(allowed);
}

bool NetcodeGame::send_native_rpc(int64_t id, int32_t target, uint32_t token, const PackedByteArray& data, bool sender_handled)
{
    return available() && !data.is_empty() &&
           game_->session().send_native_rpc(id, target, token, {data.ptr(), data.ptr() + data.size()}, sender_handled);
}

void NetcodeGame::finish_native_rpc(uint32_t receipt, bool delivered)
{
    if (available()) game_->session().finish_native_rpc(receipt, delivered);
}

std::deque<netcode::NativeRpc::Delivery> NetcodeGame::native_rpc_deliveries()
{
    return available() ? game_->session().native_rpc().take_deliveries() : std::deque<netcode::NativeRpc::Delivery>();
}

std::deque<netcode::NativeRpc::Outcome> NetcodeGame::native_rpc_outcomes()
{
    return available() ? game_->session().native_rpc().take_outcomes() : std::deque<netcode::NativeRpc::Outcome>();
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
    if (auto change = sync_scene_replica(id, sample, forecast)) {
        result["fields"] = change->fields;
        result["baseline"] = change->baseline;
        result["age"] = change->age;
    }
    return result;
}

std::optional<NetcodeGame::ReplicaChange> NetcodeGame::sync_scene_replica(int64_t id, const Array& sample, bool forecast) {
    return sync_scene_sample(id, sample, forecast, 0);
}

std::optional<NetcodeGame::ReplicaChange> NetcodeGame::sync_root_replica(int64_t id, const std::pmr::vector<Variant>& sample,
                                                                       uint64_t integer_fields) {
    return sync_scene_sample(id, sample, false, integer_fields);
}

template<class Sample>
std::optional<NetcodeGame::ReplicaChange> NetcodeGame::sync_scene_sample(int64_t id, const Sample& sample, bool forecast,
                                                                        uint64_t integer_fields) {
    if (!available() || stepping_ || id<=0 || id>UINT32_MAX) return {};
    auto& g=game_->session(); auto* obj=g.object(id);
    if (!obj) {game_->unbind_replica(id); return {};}
    const auto& fields = game_->schemas().get(obj->schema)->fields;
    if (sample.size() != fields.size()) return {};
    // Scene getters run before this call. Converting and committing a sample
    // here is synchronous and does not run any Godot property hooks.
    scene_sample_.resize(fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& value = sample[i];
        auto& converted = scene_sample_[i];
        if (!(integer_fields & (uint64_t{1} << i))) {
            if (!value_of(fields[i], value, converted)) return {};
            continue;
        }
        if (fields[i].type != netcode::FieldType::ByteArray || fields[i].limit < 8 || value.get_type() != Variant::INT)
            return {};
        if (!std::holds_alternative<netcode::Bytes>(converted)) converted.emplace<netcode::Bytes>();
        auto& bytes = std::get<netcode::Bytes>(converted);
        bytes.resize(8);
        // Match the scene binder's existing little-endian 64-bit integer encoding.
        auto bits = uint64_t(int64_t(value));
        for (auto& byte : bytes) {
            byte = uint8_t(bits);
            bits >>= 8;
        }
    }
    netcode::Values applied;
    const int mode = game_->sync_replica(id, scene_sample_, applied, forecast);
    if (!mode) return {};
    Array out;
    out.resize(applied.size());
    for (std::size_t i = 0; i < applied.size(); ++i) out[i] = netcode_godot::field_value(applied[i]);
    return ReplicaChange{std::move(out), g.clock().seconds() - g.clock().object_time(obj->sample_tick), mode == 2};
}
int64_t NetcodeGame::replica_epoch(int64_t id, bool local_only) const {
    auto* game = game_ ? game_->active() : nullptr;
    if (!game || !game->object(id)) return -1;
    const auto* grant = game->writers().grant(id);
    if (local_only) {
        const auto writer = grant ? grant->writer : game->writers().default_writer();
        if (writer != game->local()) return -1;
        const auto* identity = game->host() ? game->world().identity(id) : nullptr;
        if (identity && identity->grant_creator) return -1;
    }
    return grant ? grant->epoch : 0;
}

NetcodeGame::ReceivedReplica NetcodeGame::take_received_replica(
    int64_t id, uint64_t integer_fields, std::pmr::vector<Variant>& fields, bool check_pending_creator) {
    if (!available() || stepping_) return {};
    const auto update = check_pending_creator ? game_->receive_native_replica_view(id) : game_->receive_replica_view(id);
    if (!update.values) return {0, false, update.role};
    // Convert borrowed Core values before the binding invokes any script setter.
    fields.clear();
    fields.reserve(update.values->size());
    for (std::size_t i = 0; i < update.values->size(); ++i) {
        const auto bit = uint64_t{1} << i;
        if (!(update.mask & bit)) continue;
        const auto& field = (*update.values)[i];
        if (integer_fields & bit) {
            const auto* bytes = std::get_if<netcode::Bytes>(&field);
            uint64_t bits;
            // Leave earlier fields available if a malformed integer stops the apply pass.
            if (!bytes || bytes->size() != sizeof(bits) || !netcode::Reader(*bytes).u64(bits)) break;
            int64_t value;
            std::memcpy(&value, &bits, sizeof(value));
            fields.emplace_back(value);
        } else {
            fields.push_back(netcode_godot::field_value(field));
        }
    }
    return {update.mask, update.baseline};
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
bool NetcodeGame::reclaim(int64_t id) {
    if (stepping_ || !available() || id <= 0 || id > UINT32_MAX)
        return false;
    auto& session = game_->session();
    if (!session.host())
        return false;
    auto& writers = session.writers();
    const auto* grant = writers.grant(id);
    return grant && grant->writer != session.local() && writers.assign(id, session.local(), session.tick());
}
bool NetcodeGame::claim_coordinator(int64_t id)
{
    if (stepping_ || !available() || id <= 0 || id > UINT32_MAX) return false;
    auto& session = game_->session();
    return session.writers().request_coordinator(id, session.tick());
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
