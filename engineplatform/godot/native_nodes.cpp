#include "native_nodes.hpp"
#include "native_session.hpp"
#include "scene_binding.hpp"

#include <godot_cpp/classes/character_body2d.hpp>
#include <godot_cpp/classes/character_body3d.hpp>
#include <godot_cpp/classes/rigid_body2d.hpp>
#include <godot_cpp/classes/rigid_body3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

#define NETCODE_PROPERTY(Class, name, ...) \
    ClassDB::bind_method(D_METHOD("get_" #name), &Class::get_##name); \
    ClassDB::bind_method(D_METHOD("set_" #name, "value"), &Class::set_##name); \
    ADD_PROPERTY(PropertyInfo(__VA_ARGS__), "set_" #name, "get_" #name)

void NetcodeReplicationConfig::_bind_methods()
{
    NETCODE_PROPERTY(NetcodeReplicationConfig, properties, Variant::ARRAY, "properties",
                     PROPERTY_HINT_ARRAY_TYPE, "NodePath");
    NETCODE_PROPERTY(NetcodeReplicationConfig, limits, Variant::DICTIONARY, "limits");
    NETCODE_PROPERTY(NetcodeReplicationConfig, smooth_properties, Variant::ARRAY, "smooth_properties",
                     PROPERTY_HINT_ARRAY_TYPE, "NodePath");
    NETCODE_PROPERTY(NetcodeReplicationConfig, angle_properties, Variant::ARRAY, "angle_properties",
                     PROPERTY_HINT_ARRAY_TYPE, "NodePath");
    NETCODE_PROPERTY(NetcodeReplicationConfig, apply_proxy_changes_only, Variant::BOOL, "apply_proxy_changes_only");
}

void NetcodeReplicationConfig::set_properties(const TypedArray<NodePath>& paths)
{
    properties_ = paths.duplicate();
    emit_changed();
}

void NetcodeReplicationConfig::set_limits(const Dictionary& limits)
{
    limits_ = limits.duplicate();
    emit_changed();
}

void NetcodeReplicationConfig::set_smooth_properties(const TypedArray<NodePath>& paths)
{
    smooth_ = paths.duplicate();
    emit_changed();
}

void NetcodeReplicationConfig::set_angle_properties(const TypedArray<NodePath>& paths)
{
    angles_ = paths.duplicate();
    emit_changed();
}

void NetcodeReplicationConfig::set_apply_proxy_changes_only(bool enabled)
{
    if (enabled == proxy_changes_only_) return;
    proxy_changes_only_ = enabled;
    emit_changed();
}

void NetcodeReplicator::_bind_methods()
{
    NETCODE_PROPERTY(NetcodeReplicator, root_path, Variant::NODE_PATH, "root_path");
    NETCODE_PROPERTY(NetcodeReplicator, root_replication_mode, Variant::INT, "root_replication_mode",
                     PROPERTY_HINT_ENUM, "None,Auto");
    NETCODE_PROPERTY(NetcodeReplicator, replication_config, Variant::OBJECT, "replication_config",
                     PROPERTY_HINT_RESOURCE_TYPE, "NetcodeReplicationConfig");
    NETCODE_PROPERTY(NetcodeReplicator, interest_destroy_on_exit, Variant::BOOL, "interest_destroy_on_exit");
    NETCODE_PROPERTY(NetcodeReplicator, spatial_interest, Variant::BOOL, "spatial_interest");
    NETCODE_PROPERTY(NetcodeReplicator, spatial_position_property, Variant::STRING_NAME, "spatial_position_property");
    NETCODE_PROPERTY(NetcodeReplicator, user_keys, Variant::PACKED_INT64_ARRAY, "user_keys");
    NETCODE_PROPERTY(NetcodeReplicator, rpc_methods, Variant::PACKED_STRING_ARRAY, "rpc_methods");
    BIND_ENUM_CONSTANT(NONE);
    BIND_ENUM_CONSTANT(AUTO);
    ClassDB::bind_method(D_METHOD("replication_root"), &NetcodeReplicator::replication_root);
    ClassDB::bind_method(D_METHOD("_target", "path"), &NetcodeReplicator::target);
    ClassDB::bind_method(D_METHOD("_canonical", "path"), &NetcodeReplicator::canonical);
    ClassDB::bind_method(D_METHOD("_auto_paths"), &NetcodeReplicator::auto_paths);
    ClassDB::bind_method(D_METHOD("has_authority"), &NetcodeReplicator::has_authority);
    ClassDB::bind_method(D_METHOD("is_write_ready"), &NetcodeReplicator::is_write_ready);
    ClassDB::bind_method(D_METHOD("get_object_id"), &NetcodeReplicator::get_object_id);
    MethodInfo rpc_info("rpc_to", PropertyInfo(Variant::INT, "target"), PropertyInfo(Variant::CALLABLE, "method"));
    rpc_info.return_val = PropertyInfo(Variant::OBJECT, "", PROPERTY_HINT_RESOURCE_TYPE, "NetcodeRpcResult");
    ClassDB::bind_vararg_method(METHOD_FLAG_NORMAL, "rpc_to", &NetcodeReplicator::rpc_to, rpc_info);
    ADD_SIGNAL(MethodInfo("spawned"));
    ADD_SIGNAL(MethodInfo("authority_changed", PropertyInfo(Variant::BOOL, "has_authority")));
}

void NetcodeReplicator::set_root_path(const NodePath& path)
{
    root_path_ = path;
    root_is_parent_ = path == NodePath("..");
}

Node* NetcodeReplicator::replication_root() const
{
    if (!root_is_parent_ && (root_path_.is_empty() || root_path_.is_absolute() || root_path_.get_subname_count())) {
        return nullptr;
    }
    auto* root = root_is_parent_ ? get_parent() : get_node_or_null(root_path_);
    return Object::cast_to<NetcodeReplicator>(root) ? nullptr : root;
}

Node* NetcodeReplicator::target(const NodePath& path) const
{
    auto* root = replication_root();
    if (!root || path.is_absolute() || !path.get_subname_count()) {
        return nullptr;
    }
    auto* node = path.get_name_count()
        ? root->get_node_or_null(NodePath(path.get_concatenated_names()))
        : root;
    return node == root || (node && root->is_ancestor_of(node)) ? node : nullptr;
}

NodePath NetcodeReplicator::canonical(const NodePath& path) const
{
    auto* node = target(path);
    if (!node) {
        return {};
    }
    return NodePath(String(replication_root()->get_path_to(node)) + ":" + path.get_concatenated_subnames());
}

TypedArray<NodePath> NetcodeReplicator::auto_paths() const
{
    TypedArray<NodePath> paths;
    auto* root = replication_root();
    if (root_mode_ != AUTO || !root || Object::cast_to<RigidBody2D>(root) || Object::cast_to<RigidBody3D>(root)) {
        return paths;
    }
    if (Object::cast_to<Node3D>(root)) {
        paths.push_back(NodePath(".:position"));
        paths.push_back(NodePath(".:quaternion"));
    } else if (Object::cast_to<Node2D>(root)) {
        paths.push_back(NodePath(".:position"));
        paths.push_back(NodePath(".:rotation"));
    }
    if (Object::cast_to<CharacterBody2D>(root) || Object::cast_to<CharacterBody3D>(root)) {
        paths.push_back(NodePath(".:velocity"));
    }
    return paths;
}

void NetcodeSharedReplicator::_bind_methods()
{
    NETCODE_PROPERTY(NetcodeSharedReplicator, owner_mode, Variant::INT, "owner_mode",
                     PROPERTY_HINT_ENUM, "Transaction,Player Attached,Dynamic,Master Client");
    BIND_ENUM_CONSTANT(TRANSACTION);
    BIND_ENUM_CONSTANT(PLAYER_ATTACHED);
    BIND_ENUM_CONSTANT(DYNAMIC);
    BIND_ENUM_CONSTANT(MASTER_CLIENT);
    ClassDB::bind_method(D_METHOD("want_authority", "want", "send_interval"), &NetcodeSharedReplicator::want_authority, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("is_proxy_changes_only_enabled"), &NetcodeSharedReplicator::is_proxy_changes_only_enabled);
    ClassDB::bind_method(D_METHOD("answer_authority", "requester", "approved"), &NetcodeSharedReplicator::answer_authority);
    ClassDB::bind_method(D_METHOD("reclaim_authority"), &NetcodeSharedReplicator::reclaim_authority);
    ADD_SIGNAL(MethodInfo("authority_requested", PropertyInfo(Variant::INT, "requester_id")));
    ADD_SIGNAL(MethodInfo("authority_response", PropertyInfo(Variant::BOOL, "accepted")));
}

bool NetcodeSharedReplicator::want_authority(bool want, int send_interval)
{
    auto* session = NetcodeSession::instance();
    if (send_interval < 0 || !session || !session->want_authority(get_object_id(), want)) return false;
    write_interval_ = uint32_t(send_interval);
    return true;
}

bool NetcodeSharedReplicator::answer_authority(int64_t requester, bool approved)
{
    if (game_.is_null() || authority_request_.is_empty() || int64_t(authority_request_["object"]) != get_object_id() ||
        int64_t(authority_request_["player"]) != requester || !game_->native_authority(get_object_id())) return false;
    const auto state = game_->routing_info(get_object_id(), false);
    if (state.get("epoch", -1) != authority_request_["epoch"] || !game_->answer(authority_request_, approved)) return false;
    authority_request_.clear();
    return true;
}

bool NetcodeSharedReplicator::reclaim_authority()
{
    return game_.is_valid() && game_->reclaim(get_object_id());
}

void NetcodeSharedReplicator::on_authority_event(const Dictionary& event)
{
    if (int(event["kind"]) == int(netcode::ScriptEventKind::Request)) {
        authority_request_ = event.duplicate();
        if (is_blocking_signals()) return;
        const StringName signal("authority_requested");
        const auto listeners = get_signal_connection_list(signal);
        const ObjectID self(get_instance_id());
        const auto game = game_;
        const auto object = get_object_id();
        const Variant requester = event["player"];
        const Variant source(this);
        bool voted = false, approved = false;
        for (int64_t i = 0; i < listeners.size(); ++i) {
            const Dictionary listener = listeners[i];
            const Callable callback = listener["callable"];
            const int flags = listener["flags"];
            if (flags & CONNECT_ONE_SHOT) disconnect(signal, callback);
        }
        for (int64_t i = 0; i < listeners.size(); ++i) {
            const Dictionary listener = listeners[i];
            const Callable callback = listener["callable"];
            if (!callback.is_valid()) continue;
            const int flags = listener["flags"];
            const auto unbound = callback.get_unbound_arguments_count();
            const bool append_source = (flags & CONNECT_APPEND_SOURCE_OBJECT) && unbound <= 1;
            // Godot inserts the source before the first argument removed by unbind().
            const bool source_first = append_source && unbound == 1;
            const Variant* args[]{ source_first ? &source : &requester, source_first ? &requester : &source };
            if (flags & CONNECT_DEFERRED) {
                if (append_source) callback.call_deferred(*args[0], *args[1]);
                else callback.call_deferred(requester);
            } else {
                GDExtensionCallError error{};
                Variant callable(callback), vote;
                callable.callp("call", args, append_source ? 2 : 1, vote, error);
                if (error.error == GDEXTENSION_CALL_OK && vote.get_type() == Variant::BOOL) {
                    voted = true;
                    approved |= bool(vote);
                }
            }
            if (!ObjectDB::get_instance(self) || game_ != game || get_object_id() != object || is_queued_for_deletion()) return;
        }
        if (voted && authority_request_ == event) answer_authority(requester, approved);
    } else {
        emit_signal("authority_response", int(event["status"]) == int(netcode::ClaimStatus::Granted));
    }
}

void NetcodeSpawner::_bind_methods()
{
    NETCODE_PROPERTY(NetcodeSpawner, spawn_path, Variant::NODE_PATH, "spawn_path");
    NETCODE_PROPERTY(NetcodeSpawner, spawnable_scenes, Variant::ARRAY, "spawnable_scenes",
                     PROPERTY_HINT_ARRAY_TYPE, "PackedScene");
    ClassDB::bind_method(D_METHOD("add_spawnable_scene", "scene"), &NetcodeSpawner::add_spawnable_scene);
    ClassDB::bind_method(D_METHOD("spawn", "scene", "pre_spawn"), &NetcodeSpawner::spawn,
                         DEFVAL(Ref<PackedScene>()), DEFVAL(Callable()));
    ClassDB::bind_method(D_METHOD("despawn", "node"), &NetcodeSpawner::despawn);
    ADD_SIGNAL(MethodInfo("spawned", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_NODE_TYPE, "Node")));
    ADD_SIGNAL(MethodInfo("despawned", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_NODE_TYPE, "Node")));
    ADD_SIGNAL(MethodInfo("spawn_failed", PropertyInfo(Variant::OBJECT, "node"), PropertyInfo(Variant::STRING, "reason")));
}

bool NetcodeSpawner::add_spawnable_scene(const Ref<PackedScene>& scene)
{
    if (scene.is_null() || (is_inside_tree() && NetcodeSession::instance()->has_open_session())) {
        return false;
    }
    if (!scenes_.has(scene)) {
        scenes_.push_back(scene);
    }
    return true;
}

Node* NetcodeSpawner::spawn(const Ref<PackedScene>& scene, const Callable& pre_spawn)
{
    return NetcodeSession::instance()->spawn(this, scene, pre_spawn);
}

bool NetcodeSpawner::despawn(Node* actor)
{
    return NetcodeSession::instance()->despawn(this, actor);
}

void NetcodeSpawner::_notification(int what)
{
    if (Engine::get_singleton()->is_editor_hint()) return;
    auto* session = NetcodeSession::instance();
    if (!session) return;
    if (what == NOTIFICATION_ENTER_TREE) session->register_spawner(this);
    if (what == NOTIFICATION_EXIT_TREE) session->unregister_spawner(this);
}

NetcodeReplicator::~NetcodeReplicator()
{
    if (scene_binding_) scene_binding_->unbind();
}

Dictionary NetcodeReplicator::prepare_adapter()
{
    auto* root = replication_root();
    if (!root) return {};
    if (!scene_binding_) scene_binding_ = std::make_shared<NetcodeSceneBinding>(root, this);
    const auto binding = scene_binding_;
    return binding->definition();
}

bool NetcodeReplicator::bind_adapter(const Ref<NetcodeGame>& game, int64_t id)
{
    const bool requested = Object::cast_to<NetcodeSharedReplicator>(this) &&
                           config_.is_valid() && config_->get_apply_proxy_changes_only();
    if (!scene_binding_ || !scene_binding_->bind(game, id)) return false;
    proxy_changes_only_ = requested && scene_binding_->can_apply_changes();
    game_ = game;
    object_id_ = id;
    return true;
}

bool NetcodeReplicator::adapter_uses_body() const
{
    return !scene_binding_ || scene_binding_->uses_body();
}

Array NetcodeReplicator::sample_adapter(const Ref<NetcodeGame>& game)
{
    const auto binding = scene_binding_;
    if (!binding) return {};
    if (game.is_valid()) binding->set_game(game);
    return binding->sample();
}

bool NetcodeReplicator::prime_adapter()
{
    return scene_binding_ && scene_binding_->prime_writer();
}

bool NetcodeReplicator::apply_adapter_model(const Array& fields)
{
    const auto binding = scene_binding_;
    return binding && binding->apply_model(fields);
}

bool NetcodeReplicator::apply_baseline()
{
    if (!object_id_) return false;
    const auto binding = scene_binding_;
    if (!binding) return false;
    const ObjectID self(get_instance_id());
    const auto game = game_;
    const auto object = object_id_;
    binding->step(0.0);
    return ObjectDB::get_instance(self) && scene_binding_ == binding && game_ == game &&
           object_id_ == object && binding->initialized();
}

bool NetcodeReplicator::has_authority() const
{
    return provisional_ || (object_id_ && game_.is_valid() && game_->native_authority(object_id_));
}

bool NetcodeReplicator::is_write_ready() const
{
    return object_id_ && scene_binding_ && scene_binding_->is_write_ready();
}

void NetcodeReplicator::unbind()
{
    if (auto* shared = Object::cast_to<NetcodeSharedReplicator>(this)) shared->authority_request_.clear();
    set_physics_process_internal(false);
    if (scene_binding_) scene_binding_->unbind();
    game_.unref();
    object_id_ = 0;
    provisional_ = false;
    last_authority_ = false;
    proxy_changes_only_ = false;
    spatial_key_ = 0;
    last_write_tick_ = 0;
}

void NetcodeReplicator::set_spatial_position_property(const StringName& property)
{
    spatial_position_property_ = property;
    if (scene_binding_) scene_binding_->set_spatial_position_property(property);
}

void NetcodeReplicator::_notification(int what)
{
    if (what == NOTIFICATION_EXIT_TREE) {
        unbind();
    } else if (what == NOTIFICATION_INTERNAL_PHYSICS_PROCESS && object_id_) {
        const ObjectID self(get_instance_id());
        const auto game = game_;
        const auto object = object_id_;
        const auto bound = [&] {
            return UtilityFunctions::is_instance_id_valid(self) && game_ == game && object_id_ == object &&
                   !is_queued_for_deletion();
        };
        const bool spatial_property = spatial_interest_ && !spatial_position_property_.is_empty();
        const auto* session = spatial_property ? NetcodeSession::instance() : nullptr;
        const bool reuse_position = session && session->is_host() && !provisional_ && write_interval_ <= 1 &&
            proxy_changes_only_ && scene_binding_ && scene_binding_->can_sample_spatial_position() &&
            game->native_authority(object);
        if (!reuse_position) {
            update_spatial_distribution();
            if (spatial_property && !bound()) return;
        }
        const bool applied_model = apply_model_frame();
        if (!bound()) return;
        bool unchanged_proxy = false;
        if (!applied_model) {
            using Result = NetcodeSceneBinding::ReceiveResult;
            auto direct_step = Result::Unavailable;
            if (proxy_changes_only_ && !provisional_ && !reuse_position && write_interval_ <= 1 && scene_binding_) {
                const auto binding = scene_binding_;
                last_write_tick_ = 0;
                direct_step = binding->receive(true);
                if (direct_step == Result::PendingCreator) return;
                if (direct_step == Result::Writer) binding->step(0.0);
                unchanged_proxy = direct_step == Result::Unchanged;
            }
            if (direct_step == Result::Unavailable) {
                if ((!reuse_position && game->pending_creator(object)) || (provisional_ && !game->native_authority(object))) return;
                if (provisional_) prime_adapter();
                provisional_ = false;
                if (write_interval_ > 1 && is_write_ready()) {
                    const auto tick = game->simulation_tick();
                    if (last_write_tick_ && tick - last_write_tick_ < write_interval_) return;
                    last_write_tick_ = tick;
                } else {
                    last_write_tick_ = 0;
                }
                const bool receive_only = proxy_changes_only_ && !reuse_position && !game->native_authority(object);
                if (const auto binding = scene_binding_) {
                    if (receive_only) unchanged_proxy = binding->receive(false) != Result::Applied;
                    else if (reuse_position) {
                        Variant position;
                        binding->step(0.0, &position);
                        if (!bound()) return;
                        update_spatial_distribution(position.get_type() != Variant::NIL ? &position : nullptr);
                    } else binding->step(binding->can_apply_changes() ? 0.0 : get_physics_process_delta_time());
                }
            }
            if (!unchanged_proxy && !bound()) return;
        }
        const bool authority = !unchanged_proxy && has_authority();
        if (authority != last_authority_) {
            last_authority_ = authority;
            emit_signal("authority_changed", authority);
        }
    }
}

uint64_t NetcodeReplicator::spatial_cell() const
{
    return NetcodeInterestArea::cell(replication_root(), spatial_position_property_);
}

void NetcodeReplicator::update_spatial_distribution(const Variant* sampled_position)
{
    if (!spatial_interest_ || !object_id_) return;
    auto* session = NetcodeSession::instance();
    if (!session || !session->is_host()) return;
    const auto object = object_id_;
    const bool uses_property = !sampled_position && !spatial_position_property_.is_empty();
    const ObjectID self(uses_property ? get_instance_id() : 0);
    const auto game = uses_property ? game_ : Ref<NetcodeGame>();
    const auto cell = sampled_position ? NetcodeInterestArea::cell(*sampled_position) : spatial_cell();
    if (uses_property && (!UtilityFunctions::is_instance_id_valid(self) || game_ != game || object_id_ != object ||
                          is_queued_for_deletion())) return;
    if (cell == spatial_key_) return;
    if (game_->distribute_native_cell(object, cell)) spatial_key_ = cell;
}

#undef NETCODE_PROPERTY
