#include "native_session.hpp"
#include "tcp/limits.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <utility>

using namespace godot;

namespace {
NetcodeSession* session = nullptr;
constexpr uint64_t connection_timeout = 12000;
}

void NetcodeRoomOptions::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("get_max_players"), &NetcodeRoomOptions::get_max_players);
    ClassDB::bind_method(D_METHOD("set_max_players", "capacity"), &NetcodeRoomOptions::set_max_players);
    ClassDB::bind_method(D_METHOD("get_custom_properties"), &NetcodeRoomOptions::get_custom_properties);
    ClassDB::bind_method(D_METHOD("set_custom_properties", "properties"), &NetcodeRoomOptions::set_custom_properties);
    ClassDB::bind_method(D_METHOD("get_allow_guest_spawns"), &NetcodeRoomOptions::get_allow_guest_spawns);
    ClassDB::bind_method(D_METHOD("set_allow_guest_spawns", "enabled"), &NetcodeRoomOptions::set_allow_guest_spawns);
    ClassDB::bind_method(D_METHOD("get_retain_spatial_writer_interest"), &NetcodeRoomOptions::get_retain_spatial_writer_interest);
    ClassDB::bind_method(D_METHOD("set_retain_spatial_writer_interest", "enabled"), &NetcodeRoomOptions::set_retain_spatial_writer_interest);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "max_players", PROPERTY_HINT_RANGE, vformat("1,%d,1", netcode::tcp_room_capacity)),
                 "set_max_players", "get_max_players");
    ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "custom_properties"), "set_custom_properties", "get_custom_properties");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "allow_guest_spawns"), "set_allow_guest_spawns", "get_allow_guest_spawns");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "retain_spatial_writer_interest"),
                 "set_retain_spatial_writer_interest", "get_retain_spatial_writer_interest");
}

void NetcodeRoomView::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("get_player_count"), &NetcodeRoomView::get_player_count);
    ClassDB::bind_method(D_METHOD("get_players"), &NetcodeRoomView::get_players);
    ClassDB::bind_method(D_METHOD("get_master_client_id"), &NetcodeRoomView::get_master_client_id);
    ClassDB::bind_method(D_METHOD("get_is_master_client"), &NetcodeRoomView::get_is_master_client);
    ClassDB::bind_method(D_METHOD("get_custom_properties"), &NetcodeRoomView::get_custom_properties);
    ClassDB::bind_method(D_METHOD("set_property", "key", "value"), &NetcodeRoomView::set_property);
    ClassDB::bind_method(D_METHOD("set_properties", "patch"), &NetcodeRoomView::set_properties);
    ClassDB::bind_method(D_METHOD("set_properties_cas", "patch", "expected"), &NetcodeRoomView::set_properties_cas);
    ClassDB::bind_method(D_METHOD("remove_properties", "keys"), &NetcodeRoomView::remove_properties);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "player_count"), "", "get_player_count");
    ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "players"), "", "get_players");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "master_client_id"), "", "get_master_client_id");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "is_master_client"), "", "get_is_master_client");
    ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "custom_properties"), "", "get_custom_properties");
    ADD_SIGNAL(MethodInfo("properties_changed", PropertyInfo(Variant::DICTIONARY, "properties")));
}

void NetcodePlayer::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("get_number"), &NetcodePlayer::get_number);
    ClassDB::bind_method(D_METHOD("get_is_master_client"), &NetcodePlayer::get_is_master_client);
    ClassDB::bind_method(D_METHOD("get_is_inactive"), &NetcodePlayer::get_is_inactive);
    ClassDB::bind_method(D_METHOD("get_user_id"), &NetcodePlayer::get_user_id);
    ClassDB::bind_method(D_METHOD("get_name"), &NetcodePlayer::get_name);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "number"), "", "get_number");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "is_master_client"), "", "get_is_master_client");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "is_inactive"), "", "get_is_inactive");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "user_id"), "", "get_user_id");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "name"), "", "get_name");
}

Array NetcodeRoomView::get_players() const
{
    Array players;
    for (auto number : players_) {
        Ref<NetcodePlayer> player;
        player.instantiate();
        player->number_ = number;
        player->master_ = number == master_;
        players.push_back(player);
    }
    return players;
}

Dictionary NetcodeRoomView::get_custom_properties() const
{
    return game_.is_valid() ? game_->room_properties() : Dictionary();
}

bool NetcodeRoomView::set_property(const String& key, const Variant& value)
{
    Dictionary patch;
    patch[key] = value;
    return set_properties(patch);
}

bool NetcodeRoomView::set_properties(const Dictionary& patch)
{
    return game_.is_valid() && game_->set_room_properties(patch);
}

bool NetcodeRoomView::set_properties_cas(const Dictionary& patch, const Dictionary& expected)
{
    return game_.is_valid() && game_->set_room_properties(patch, expected);
}

bool NetcodeRoomView::remove_properties(const PackedStringArray& keys)
{
    return game_.is_valid() && game_->remove_room_properties(keys);
}

void NetcodeClientProcessor::_notification(int what)
{
    if (what == NOTIFICATION_ENTER_TREE) {
        set_process_internal(!Engine::get_singleton()->is_editor_hint());
    } else if (what == NOTIFICATION_INTERNAL_PROCESS) {
        if (auto* current = NetcodeSession::instance()) current->advance(ObjectID(get_instance_id()));
    }
}

void NetcodeSession::_bind_methods()
{
    BIND_CONSTANT(TARGET_OWNER);
    BIND_CONSTANT(TARGET_MASTER);
    BIND_CONSTANT(TARGET_PLUGIN);
    BIND_CONSTANT(TARGET_ALL);
    ClassDB::bind_method(D_METHOD("create_room", "bind_ip", "port", "options"),
                         &NetcodeSession::create_room, DEFVAL(Ref<NetcodeRoomOptions>()));
    ClassDB::bind_method(D_METHOD("join_room", "host_ip", "port"), &NetcodeSession::join_room);
    ClassDB::bind_method(D_METHOD("leave_room"), &NetcodeSession::leave_room);
    ClassDB::bind_method(D_METHOD("get_room"), &NetcodeSession::get_room);
    ClassDB::bind_method(D_METHOD("get_local_player_id"), &NetcodeSession::get_local_player_id);
    ClassDB::bind_method(D_METHOD("is_in_room"), &NetcodeSession::is_in_room);
    ClassDB::bind_method(D_METHOD("is_master_client"), &NetcodeSession::is_master_client);
    ClassDB::bind_method(D_METHOD("is_simulation_ready"), &NetcodeSession::is_simulation_ready);
    ClassDB::bind_method(D_METHOD("get_network_time"), &NetcodeSession::get_network_time);
    ClassDB::bind_method(D_METHOD("get_rtt"), &NetcodeSession::get_rtt);
    ClassDB::bind_method(D_METHOD("get_transport_stats"), &NetcodeSession::get_transport_stats);
    ClassDB::bind_method(D_METHOD("add_user_key", "key"), &NetcodeSession::add_user_key);
    ClassDB::bind_method(D_METHOD("remove_user_key", "key"), &NetcodeSession::remove_user_key);
    ClassDB::bind_method(D_METHOD("get_rpc_info"), &NetcodeSession::get_rpc_info);
    ClassDB::bind_method(D_METHOD("get_rpc_sender"), &NetcodeSession::get_rpc_sender);
    ClassDB::bind_method(D_METHOD("register_broadcast_receiver", "node"), &NetcodeSession::register_broadcast_receiver);
    ClassDB::bind_method(D_METHOD("unregister_broadcast_receiver", "node"), &NetcodeSession::unregister_broadcast_receiver);
    const PropertyInfo callable(Variant::CALLABLE, "method");
    const PropertyInfo result(Variant::OBJECT, "", PROPERTY_HINT_RESOURCE_TYPE, "NetcodeRpcResult");
    MethodInfo rpc("rpc", callable);
    MethodInfo rpc_to("rpc_to", PropertyInfo(Variant::INT, "target"), callable);
    MethodInfo rpc_to_player("rpc_to_player", PropertyInfo(Variant::INT, "player_id"), callable);
    rpc.return_val = rpc_to.return_val = rpc_to_player.return_val = result;
    ClassDB::bind_vararg_method(METHOD_FLAG_NORMAL, "rpc", &NetcodeSession::rpc, rpc);
    ClassDB::bind_vararg_method(METHOD_FLAG_NORMAL, "rpc_to", &NetcodeSession::rpc_to, rpc_to);
    ClassDB::bind_vararg_method(METHOD_FLAG_NORMAL, "rpc_to_player", &NetcodeSession::rpc_to_player, rpc_to_player);
    ADD_SIGNAL(MethodInfo("room_joined"));
    ADD_SIGNAL(MethodInfo("connection_failed", PropertyInfo(Variant::STRING, "reason")));
    ADD_SIGNAL(MethodInfo("room_left"));
    ADD_SIGNAL(MethodInfo("player_joined", PropertyInfo(Variant::INT, "player_id"), PropertyInfo(Variant::STRING, "user_id")));
    ADD_SIGNAL(MethodInfo("player_left", PropertyInfo(Variant::INT, "player_id"), PropertyInfo(Variant::BOOL, "is_inactive")));
    ADD_SIGNAL(MethodInfo("interest_enter", PropertyInfo(Variant::OBJECT, "node")));
    ADD_SIGNAL(MethodInfo("interest_exit", PropertyInfo(Variant::OBJECT, "node")));
    ADD_SIGNAL(MethodInfo("interest_area_overflow", PropertyInfo(Variant::INT, "cells"), PropertyInfo(Variant::INT, "limit")));
}

void NetcodeSession::install()
{
    session = memnew(NetcodeSession);
    Engine::get_singleton()->register_singleton("Netcode", session);
}

void NetcodeSession::uninstall()
{
    Engine::get_singleton()->unregister_singleton("Netcode");
    auto* retired = session;
    session = nullptr;
    retired->close();
    memdelete(retired);
}

NetcodeSession* NetcodeSession::instance() { return session; }

Error NetcodeSession::create_room(const String& ip, int port, const Ref<NetcodeRoomOptions>& options)
{
    return open(true, ip, port, options.is_valid() ? options->get_max_players() : 4,
                options.is_valid() ? options->get_custom_properties() : Dictionary(),
                options.is_null() || options->get_allow_guest_spawns(),
                options.is_valid() && options->get_retain_spatial_writer_interest());
}

Error NetcodeSession::join_room(const String& ip, int port)
{
    return open(false, ip, port, 4, Dictionary(), true, false);
}

Error NetcodeSession::open(bool host, const String& ip, int port, int capacity, const Dictionary& initial,
                           bool allow_guest_spawns, bool retain_spatial_writer_interest)
{
    if (game_.is_valid()) return ERR_ALREADY_IN_USE;
    if (!ip.is_valid_ip_address() || port < 1 || port > 65535 ||
        capacity < 1 || capacity > netcode::tcp_room_capacity) return ERR_INVALID_PARAMETER;
    auto* engine = Engine::get_singleton();
    auto* tree = Object::cast_to<SceneTree>(engine->get_main_loop());
    if (!tree || !tree->get_root() || engine->is_editor_hint()) return ERR_UNCONFIGURED;

    Ref<NetcodeGame> next;
    next.instantiate();
    String layout;
    uint64_t fingerprint;
    if (!build_catalog(layout, fingerprint)) return ERR_INVALID_DATA;
    const auto endpoint = (ip.contains(":") ? "[" + ip + "]" : ip) + ":" + String::num_int64(port);
    const auto step = prediction_.empty() ? netcode::SessionOptions{}.step : std::chrono::milliseconds(16);
    const auto error = next->open_provider(layout, host, endpoint, capacity, false, "tcp", {}, {}, {}, step,
                                          retain_spatial_writer_interest);
    if (!error.is_empty()) return host ? ERR_CANT_CREATE : ERR_CANT_CONNECT;
    // Native nodes consume replica revisions directly. The scene loop only needs lifecycle events.
    next->disable_snapshot_change_events();
    next->allow_guest_spawns(allow_guest_spawns);
    if (!register_catalog(next) || !next->enable_room_properties(fingerprint) ||
        (host && (!next->set_player_ttl(0) || !next->start()))) {
        next->close();
        return ERR_CANT_CREATE;
    }
    if (host && !initial.is_empty() && !next->set_room_properties(initial)) {
        next->close();
        return ERR_INVALID_PARAMETER;
    }
    if (!ObjectDB::get_instance(processor_)) {
        auto* processor = memnew(NetcodeClientProcessor);
        processor->set_name("NetcodeClientProcessor");
        processor->set_process_mode(Node::PROCESS_MODE_ALWAYS);
        processor_ = processor->get_instance_id();
        tree->get_root()->call_deferred("add_child", processor);
    }
    game_ = next;
    host_ = host;
    room_.instantiate();
    room_->game_ = next;
    deadline_ = Time::get_singleton()->get_ticks_msec() + connection_timeout;
    return OK;
}

void NetcodeSession::advance(ObjectID processor)
{
    if (processor != processor_ || game_.is_null()) return;
    auto game = game_;
    if (game->is_stepping()) return;
    for (auto object : std::exchange(deferred_host_despawns_, {})) game->destroy_object(object);
    if (host_) capture_host_spawn_baselines();
    if (game_ != game) return;
    const auto frame = game->advance();
    if (game_ != game) return;
    const int phase = frame.get("phase", int(netcode::RoomPhase::Connecting));
    if (phase == int(netcode::RoomPhase::Ended) || frame.is_empty()) {
        if (joined_) {
            leave_room();
        } else if (int(frame.get("rejected", 0)) == int(netcode::Reject::Protocol)) {
            failed(vformat("Incompatible room protocol: the peer rejected native session v%d", netcode::RoomProperties::version));
        } else {
            failed(vformat("Room connection ended before native session v%d negotiation (reject=%d, disconnect=%d)",
                           netcode::RoomProperties::version,
                           frame.get("rejected", 0), frame.get("disconnect", 0)));
        }
        return;
    }
    const uint32_t revision = int64_t(frame.get("properties_revision", 0));
    if (!revision) {
        if (Time::get_singleton()->get_ticks_msec() >= deadline_) {
            failed(int64_t(frame.get("round", 0)) != 0
                   ? vformat("The peer did not negotiate native session v%d with this template catalog", netcode::RoomProperties::version)
                   : String("The connection timed out"));
        }
        return;
    }
    const Array rosters = frame.get("native_rosters", Array());
    room_->master_ = frame.get("native_master", 0);
    player_ = frame.get("player", 0);
    room_->local_ = player_;
    const double sampled_time = host_ || bool(frame.get("native_spawn_ready", false))
        ? double(frame.get("time", 0.0)) : 0.0;
    time_ = host_ || joined_ ? sampled_time : 0.0;
    rtt_ = frame.get("rtt", 0.0);
    int64_t first_change = 0;
    if (!joined_) {
        if (rosters.is_empty()) return;
        room_->players_ = rosters[0];
        first_change = 1;
        joined_ = true;
        properties_revision_ = revision;
        emit_signal("room_joined");
        if (game_ != game) return;
        time_ = sampled_time;
    }
    for (int64_t i = first_change; i < rosters.size(); ++i) {
        const PackedInt64Array next = rosters[i];
        const auto previous = std::exchange(room_->players_, next);
        for (auto player : previous) {
            if (player != player_ && !next.has(player)) emit_signal("player_left", player, false);
            if (game_ != game) return;
        }
        for (auto player : next) {
            if (player != player_ && !previous.has(player)) emit_signal("player_joined", player, String());
            if (game_ != game) return;
        }
    }
    if (properties_revision_ != revision) {
        properties_revision_ = revision;
        auto room = room_;
        room->emit_signal("properties_changed", room->get_custom_properties());
    }
    if (game_ == game && !host_) flush_area_interest();
    if (game_ == game && !host_) flush_staged_spawns();
    if (game_ == game) update_spawns(frame);
    if (game_ == game) update_rpc();
}

void NetcodeSession::close()
{
    if (game_.is_valid()) game_->close();
    if (room_.is_valid()) {
        room_->game_.unref();
        room_->players_.clear();
        room_->master_ = 0;
        room_->local_ = 0;
    }
    room_.unref();
    game_.unref();
    joined_ = false;
    host_ = false;
    player_ = 0;
    time_ = 0;
    rtt_ = 0;
    user_keys_.clear();
    properties_revision_ = 0;
    deadline_ = 0;
    abandon_spawns();
    close_rpc();
}

void NetcodeSession::failed(const String& reason)
{
    close();
    emit_signal("connection_failed", reason);
}

void NetcodeSession::leave_room()
{
    if (game_.is_null()) return;
    if (game_->is_stepping()) return;
    close();
    emit_signal("room_left");
}

Ref<NetcodeRoomView> NetcodeSession::get_room() const
{
    return joined_ ? room_ : Ref<NetcodeRoomView>();
}

Dictionary NetcodeSession::get_transport_stats() const
{
    return game_.is_valid() ? game_->transport_stats() : Dictionary();
}

bool NetcodeSession::add_user_key(int64_t key)
{
    if (!joined_ || host_) return false;
    auto next = user_keys_;
    if (!next.insert(key).second) return true;
    if (next.size() > 64) return false;
    if (!game_->subscribe_local(netcode::InterestKind::User, next)) return false;
    user_keys_ = std::move(next);
    return true;
}

bool NetcodeSession::remove_user_key(int64_t key)
{
    if (!joined_ || host_) return false;
    auto next = user_keys_;
    if (!next.erase(key)) return true;
    if (!game_->subscribe_local(netcode::InterestKind::User, next)) return false;
    user_keys_ = std::move(next);
    return true;
}

bool NetcodeSession::want_authority(int64_t id, bool want)
{
    if (!joined_ || !instances_.count(id)) return false;
    if (want && game_->native_authority(id)) return true;
    if (host_) return want && game_->claim_coordinator(id);
    return want ? game_->claim(id) : game_->release(id);
}

void NetcodeSession::area_interest(ObjectID source, const AreaCells& cells, uint16_t divisor)
{
    if (!joined_ || host_) return;
    const auto existing = area_interests_.find(source);
    const auto interest = std::make_pair(cells, divisor);
    if (existing == area_interests_.end() ? !cells.front() : existing->second == interest) return;
    if (!cells.front()) area_interests_.erase(source);
    else area_interests_.insert_or_assign(source, interest);
    area_interest_dirty_ = true;
}

void NetcodeSession::flush_area_interest()
{
    const bool support = game_->supports_area_rates();
    if (!area_interest_dirty_ && support == area_rates_supported_) return;
    area_rates_supported_ = support;
    area_interest_dirty_ = true;
    netcode::RoomProperties::AreaRates merged;
    for (const auto& [_, area] : area_interests_) {
        const auto rate = support ? area.second : uint16_t{1};
        for (const auto key : area.first) {
            if (!key) break;
            auto [cell, inserted] = merged.try_emplace(key, rate);
            if (!inserted) cell->second = std::min(cell->second, rate);
        }
    }
    const auto requested = merged.size();
    constexpr auto limit = netcode::RoomProperties::max_interest_keys;
    const bool overflow = requested > limit;
    if (overflow) {
        // Keep still-requested cells from the last successful subscription.
        // Departed sources can be removed even while the union is over budget.
        for (auto it = merged.begin(); it != merged.end();) {
            if (!sent_area_interests_.count(it->first)) it = merged.erase(it);
            else ++it;
        }
    }
    if (merged == sent_area_interests_) area_interest_dirty_ = false;
    else if (game_->subscribe_local_area_rates(merged)) {
        sent_area_interests_ = std::move(merged);
        area_interest_dirty_ = false;
    }
    if (!std::exchange(area_interest_overflow_, overflow) && overflow) {
        UtilityFunctions::push_warning(vformat("Netcode spatial interest requests %d cells. The limit is %d.",
                                               int64_t(requested), int64_t(limit)));
        emit_signal("interest_area_overflow", int64_t(requested), int64_t(limit));
    }
}
