#pragma once

#include "game.hpp"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <memory>

class NetcodeSceneBinding;

class NetcodeRpcResult : public godot::RefCounted {
    GDCLASS(NetcodeRpcResult, godot::RefCounted)

public:
    bool is_finished() const { return finished_; }
    godot::String get_error() const { return error_; }
    godot::Ref<NetcodeRpcResult> on_fail(const godot::Callable& callback, double ttl);
    void complete(const godot::String& error);

protected:
    static void _bind_methods();

private:
    bool finished_ = false;
    godot::String error_;
    godot::Callable failure_;
    uint64_t deadline_ = 0;
};

class NetcodeReplicationConfig : public godot::Resource {
    GDCLASS(NetcodeReplicationConfig, godot::Resource)

public:
    godot::TypedArray<godot::NodePath> get_properties() const { return properties_.duplicate(); }
    void set_properties(const godot::TypedArray<godot::NodePath>& paths);
    godot::Dictionary get_limits() const { return limits_.duplicate(); }
    void set_limits(const godot::Dictionary& limits);
    godot::TypedArray<godot::NodePath> get_smooth_properties() const { return smooth_.duplicate(); }
    void set_smooth_properties(const godot::TypedArray<godot::NodePath>& paths);
    godot::TypedArray<godot::NodePath> get_angle_properties() const { return angles_.duplicate(); }
    void set_angle_properties(const godot::TypedArray<godot::NodePath>& paths);
    bool get_apply_proxy_changes_only() const { return proxy_changes_only_; }
    void set_apply_proxy_changes_only(bool enabled);

protected:
    static void _bind_methods();

private:
    godot::TypedArray<godot::NodePath> properties_;
    godot::Dictionary limits_;
    godot::TypedArray<godot::NodePath> smooth_, angles_;
    bool proxy_changes_only_ = false;
};

class NetcodeReplicator : public godot::Node {
    GDCLASS(NetcodeReplicator, godot::Node)

public:
    enum RootMode { NONE, AUTO };
    ~NetcodeReplicator() override;
    godot::NodePath get_root_path() const { return root_path_; }
    void set_root_path(const godot::NodePath& path);
    int get_root_replication_mode() const { return root_mode_; }
    void set_root_replication_mode(int mode) { root_mode_ = mode; }
    godot::Ref<NetcodeReplicationConfig> get_replication_config() const { return config_; }
    void set_replication_config(const godot::Ref<NetcodeReplicationConfig>& config) { config_ = config; }
    bool get_interest_destroy_on_exit() const { return destroy_on_exit_; }
    void set_interest_destroy_on_exit(bool enabled) { destroy_on_exit_ = enabled; }
    bool get_spatial_interest() const { return spatial_interest_; }
    void set_spatial_interest(bool enabled) { spatial_interest_ = enabled; }
    godot::StringName get_spatial_position_property() const { return spatial_position_property_; }
    void set_spatial_position_property(const godot::StringName& property);
    godot::PackedInt64Array get_user_keys() const { return user_keys_; }
    void set_user_keys(const godot::PackedInt64Array& keys) { user_keys_ = keys; }
    godot::Node* replication_root() const;
    godot::Node* target(const godot::NodePath& path) const;
    godot::NodePath canonical(const godot::NodePath& path) const;
    godot::TypedArray<godot::NodePath> auto_paths() const;
    bool has_authority() const;
    bool is_write_ready() const;
    int64_t get_object_id() const { return object_id_; }
    godot::PackedStringArray get_rpc_methods() const { return rpc_methods_; }
    void set_rpc_methods(const godot::PackedStringArray& methods) { rpc_methods_ = methods; }

protected:
    static void _bind_methods();
    void _notification(int what);

private:
    friend class NetcodeSession;
    friend class NetcodeSpawner;
    friend class NetcodeSharedReplicator;
    friend class NetcodeServerReplicator;
    godot::Dictionary prepare_adapter();
    bool bind_adapter(const godot::Ref<NetcodeGame>& game, int64_t id);
    bool adapter_uses_body() const;
    godot::Array sample_adapter(const godot::Ref<NetcodeGame>& game = {});
    bool prime_adapter();
    bool apply_adapter_model(const godot::Array& fields);
    bool apply_baseline();
    virtual bool apply_model_frame() { return false; }
    uint64_t spatial_cell() const;
    void update_spatial_distribution(const godot::Variant* sampled_position = nullptr);
    godot::Variant rpc_to(const godot::Variant** args, GDExtensionInt count, GDExtensionCallError& error);
    void unbind();
    godot::NodePath root_path_{".."};
    bool root_is_parent_ = true;
    int root_mode_ = NONE;
    godot::Ref<NetcodeReplicationConfig> config_;
    bool destroy_on_exit_ = false;
    bool spatial_interest_ = false;
    godot::StringName spatial_position_property_;
    uint64_t spatial_key_ = 0;
    godot::PackedInt64Array user_keys_;
    godot::PackedStringArray rpc_methods_;
    std::shared_ptr<NetcodeSceneBinding> scene_binding_;
    godot::Ref<NetcodeGame> game_;
    int64_t object_id_ = 0;
    bool provisional_ = false;
    bool last_authority_ = false;
    bool proxy_changes_only_ = false;
    uint32_t write_interval_ = 0;
    uint32_t last_write_tick_ = 0;
};

VARIANT_ENUM_CAST(NetcodeReplicator::RootMode);

class NetcodeSharedReplicator : public NetcodeReplicator {
    GDCLASS(NetcodeSharedReplicator, NetcodeReplicator)

public:
    enum OwnerMode { TRANSACTION, PLAYER_ATTACHED, DYNAMIC, MASTER_CLIENT };
    int get_owner_mode() const { return owner_mode_; }
    void set_owner_mode(int mode) { owner_mode_ = mode; }
    bool is_proxy_changes_only_enabled() const { return proxy_changes_only_; }
    bool want_authority(bool want, int send_interval);
    bool answer_authority(int64_t requester, bool approved);
    bool reclaim_authority();

protected:
    static void _bind_methods();

private:
    friend class NetcodeSession;
    friend class NetcodeReplicator;
    void on_authority_event(const godot::Dictionary& event);
    int owner_mode_ = TRANSACTION;
    godot::Dictionary authority_request_;
};

VARIANT_ENUM_CAST(NetcodeSharedReplicator::OwnerMode);

class NetcodeServerReplicator : public NetcodeReplicator {
    GDCLASS(NetcodeServerReplicator, NetcodeReplicator)

public:
    godot::PackedByteArray get_neutral_input() const { return neutral_; }
    void set_neutral_input(const godot::PackedByteArray& neutral) { neutral_ = neutral; }
    int get_input_model_version() const { return input_model_version_; }
    void set_input_model_version(int version) { input_model_version_ = version; }
    int64_t get_input_authority() const;
    bool set_input_authority(int64_t player);
    bool has_input_authority() const;
    bool queue_input(const godot::Variant& delta_or_payload, const godot::Variant& payload = {});
    int64_t get_input_queue_count() const;
    int64_t get_confirmed_input() const;
    int64_t get_replayed_input_count() const;

protected:
    static void _bind_methods();

private:
    friend class NetcodeReplicator;
    friend class NetcodeSession;
    bool apply_model_frame() override;
    godot::Array simulate(const godot::Array&, const godot::PackedByteArray&, double delta, int64_t tick, netcode::SimulationStep);
    godot::PackedByteArray neutral_;
    int input_model_version_ = 1;
    int64_t staged_input_player_ = 0;
    int64_t reported_input_player_ = 0;
};

class NetcodeInterestArea : public godot::Node {
    GDCLASS(NetcodeInterestArea, godot::Node)

public:
    godot::NodePath get_target_path() const { return target_path_; }
    void set_target_path(const godot::NodePath& path) { target_path_ = path; }
    int get_radius() const { return radius_; }
    void set_radius(int radius);
    int get_send_divisor() const { return send_divisor_; }
    void set_send_divisor(int divisor);
    static uint64_t cell(godot::Node* target, const godot::StringName& position_property);
    static uint64_t cell(const godot::Variant& position);

protected:
    static void _bind_methods();
    void _notification(int what);

private:
    godot::NodePath target_path_{".."};
    int radius_ = 1;
    uint16_t send_divisor_ = 1;
};

class NetcodeSpawner : public godot::Node {
    GDCLASS(NetcodeSpawner, godot::Node)

public:
    godot::NodePath get_spawn_path() const { return spawn_path_; }
    void set_spawn_path(const godot::NodePath& path) { spawn_path_ = path; }
    godot::TypedArray<godot::PackedScene> get_spawnable_scenes() const { return scenes_.duplicate(); }
    void set_spawnable_scenes(const godot::TypedArray<godot::PackedScene>& scenes) { scenes_ = scenes.duplicate(); }
    bool add_spawnable_scene(const godot::Ref<godot::PackedScene>& scene);
    godot::Node* spawn(const godot::Ref<godot::PackedScene>& scene, const godot::Callable& pre_spawn);
    bool despawn(godot::Node* actor);

protected:
    static void _bind_methods();
    void _notification(int what);

private:
    godot::NodePath spawn_path_{".."};
    godot::TypedArray<godot::PackedScene> scenes_;
};
