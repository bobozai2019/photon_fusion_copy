#pragma once

#include "game.hpp"
#include "native_nodes.hpp"
#include <array>

#include <godot_cpp/classes/node.hpp>

class NetcodeRoomOptions : public godot::RefCounted {
    GDCLASS(NetcodeRoomOptions, godot::RefCounted)

public:
    int get_max_players() const { return max_players_; }
    void set_max_players(int capacity) { max_players_ = capacity; }
    godot::Dictionary get_custom_properties() const { return properties_.duplicate(true); }
    void set_custom_properties(const godot::Dictionary& properties) { properties_ = properties.duplicate(true); }
    bool get_allow_guest_spawns() const { return allow_guest_spawns_; }
    void set_allow_guest_spawns(bool enabled) { allow_guest_spawns_ = enabled; }
    bool get_retain_spatial_writer_interest() const { return retain_spatial_writer_interest_; }
    void set_retain_spatial_writer_interest(bool enabled) { retain_spatial_writer_interest_ = enabled; }

protected:
    static void _bind_methods();

private:
    int max_players_ = 4;
    bool allow_guest_spawns_ = true;
    bool retain_spatial_writer_interest_ = false;
    godot::Dictionary properties_;
};

class NetcodePlayer : public godot::RefCounted {
    GDCLASS(NetcodePlayer, godot::RefCounted)

public:
    int64_t get_number() const { return number_; }
    bool get_is_master_client() const { return master_; }
    bool get_is_inactive() const { return false; }
    godot::String get_user_id() const { return {}; }
    godot::String get_name() const { return {}; }

protected:
    static void _bind_methods();

private:
    friend class NetcodeRoomView;
    int64_t number_ = 0;
    bool master_ = false;
};

class NetcodeRoomView : public godot::RefCounted {
    GDCLASS(NetcodeRoomView, godot::RefCounted)

public:
    int get_player_count() const { return int(players_.size()); }
    godot::Array get_players() const;
    int64_t get_master_client_id() const { return master_; }
    bool get_is_master_client() const { return local_ && local_ == master_; }
    godot::Dictionary get_custom_properties() const;
    bool set_property(const godot::String& key, const godot::Variant& value);
    bool set_properties(const godot::Dictionary& patch);
    bool set_properties_cas(const godot::Dictionary& patch, const godot::Dictionary& expected);
    bool remove_properties(const godot::PackedStringArray& keys);

protected:
    static void _bind_methods();

private:
    friend class NetcodeSession;
    godot::Ref<NetcodeGame> game_;
    godot::PackedInt64Array players_;
    int64_t master_ = 0;
    int64_t local_ = 0;
};

class NetcodeClientProcessor : public godot::Node {
    GDCLASS(NetcodeClientProcessor, godot::Node)

protected:
    static void _bind_methods() {}
    void _notification(int what);
};

class NetcodeRpcInfo : public godot::RefCounted {
    GDCLASS(NetcodeRpcInfo, godot::RefCounted)

public:
    int64_t get_sender() const { return sender_; }
    int64_t get_target() const { return target_; }
    godot::Node* get_target_node() const;

protected:
    static void _bind_methods();

private:
    friend class NetcodeSession;
    int64_t sender_ = 0;
    int64_t target_ = 0;
    godot::ObjectID target_node_;
};

class NetcodeSession : public godot::Object {
    GDCLASS(NetcodeSession, godot::Object)

public:
    static void install();
    static void uninstall();
    static NetcodeSession* instance();
    enum RpcTarget { TARGET_OWNER = -3, TARGET_PLUGIN = -2, TARGET_MASTER = -1, TARGET_ALL = 0 };
    godot::Error create_room(const godot::String& bind_ip, int port,
                             const godot::Ref<NetcodeRoomOptions>& options);
    godot::Error join_room(const godot::String& host_ip, int port);
    void leave_room();
    godot::Ref<NetcodeRoomView> get_room() const;
    int64_t get_local_player_id() const { return player_; }
    bool is_in_room() const { return joined_; }
    bool is_master_client() const { return joined_ && host_; }
    double get_network_time() const { return time_; }
    double get_rtt() const { return rtt_; }
    bool is_simulation_ready() const { return time_ > 0; }
    godot::Dictionary get_transport_stats() const;
    bool has_open_session() const { return game_.is_valid(); }
    bool is_host() const { return host_; }
    void register_spawner(NetcodeSpawner* spawner);
    void unregister_spawner(NetcodeSpawner* spawner);
    godot::Node* spawn(NetcodeSpawner* spawner, const godot::Ref<godot::PackedScene>& scene,
                       const godot::Callable& pre_spawn);
    bool despawn(NetcodeSpawner* spawner, godot::Node* actor);
    bool add_user_key(int64_t key);
    bool remove_user_key(int64_t key);
    using AreaCells = std::array<uint64_t, 27>;
    void area_interest(godot::ObjectID source, const AreaCells& cells, uint16_t divisor);
    bool want_authority(int64_t id, bool want);
    int64_t get_rpc_sender() const { return rpc_info_.is_valid() ? rpc_info_->get_sender() : 0; }
    godot::Ref<NetcodeRpcInfo> get_rpc_info() const { return rpc_info_; }

protected:
    static void _bind_methods();

private:
    friend class NetcodeClientProcessor;
    friend class NetcodeReplicator;
    godot::Error open(bool host, const godot::String& ip, int port, int capacity,
                      const godot::Dictionary& initial, bool allow_guest_spawns, bool retain_spatial_writer_interest);
    void advance(godot::ObjectID processor);
    void close();
    void failed(const godot::String& reason);
    struct Template {
        godot::ObjectID spawner;
        godot::Ref<godot::PackedScene> scene;
        int schema;
        netcode::ClaimMode mode;
        godot::PackedInt64Array user_keys;
        bool spatial;
    };
    struct Instance {
        uint32_t template_id;
        godot::ObjectID actor;
        godot::ObjectID sync;
        uint64_t deadline = 0;
        bool mounted() const;
        bool mounted(const NetcodeReplicator* replica) const;
        bool bound_to(const godot::Ref<NetcodeGame>& game, int64_t object) const;
    };
    struct StagedSpawn { Instance instance; netcode::Values fields; };
    bool build_catalog(godot::String& layout, uint64_t& fingerprint);
    bool register_catalog(const godot::Ref<NetcodeGame>& game);
    std::deque<StagedSpawn>::iterator find_staged_spawn(godot::ObjectID actor);
    bool submit_guest_spawn(const Instance& record, const netcode::Values& fields);
    void flush_staged_spawns();
    void capture_host_spawn_baselines();
    void update_spawns(const godot::Dictionary& frame);
    void flush_area_interest();
    void abandon_spawns();
    void update_rpc();
    void close_rpc();
    godot::Variant rpc(const godot::Variant** args, GDExtensionInt count, GDExtensionCallError& error);
    godot::Variant rpc_to(const godot::Variant** args, GDExtensionInt count, GDExtensionCallError& error);
    godot::Variant rpc_to_player(const godot::Variant** args, GDExtensionInt count, GDExtensionCallError& error);
    void register_broadcast_receiver(godot::Node* node);
    void unregister_broadcast_receiver(godot::Node* node);
    NetcodeReplicator* find_rpc_replica(const godot::Object* node, bool allow_sync) const;
    godot::Ref<NetcodeRpcResult> dispatch_rpc(int64_t target, const godot::Callable& method,
                                             const godot::Variant** args, GDExtensionInt count,
                                             NetcodeReplicator* replica = nullptr);
    godot::Ref<NetcodeRpcResult> send_object_rpc(int64_t target, const godot::Array& invocation,
                                               NetcodeReplicator* replica, bool annotated, bool call_local);
    bool deliver_rpc(const netcode::NativeRpc::Delivery& delivery, NetcodeReplicator* replica);
    bool invoke_rpc(godot::Node* destination, const godot::StringName& name, const godot::Array& args,
                    const netcode::NativeRpc::Delivery& delivery);
    void remove_instance(const Instance& instance, const godot::String& failure = {});
    std::vector<godot::ObjectID> spawners_;
    std::vector<Template> catalog_;
    std::map<int, godot::PackedInt32Array> writable_;
    std::map<int, std::pair<int, godot::PackedByteArray>> prediction_;
    std::map<int64_t, Instance> instances_;
    std::map<int64_t, Instance> retained_;
    std::map<uint32_t, Instance> pending_;
    std::deque<StagedSpawn> staged_spawns_;
    std::set<uint32_t> expired_;
    std::set<int64_t> failed_mounts_;
    std::set<int64_t> initial_host_spawns_;
    std::set<int64_t> deferred_host_despawns_;
    uint32_t creation_namespace_ = 0;
    std::set<uint64_t> user_keys_;
    std::map<godot::ObjectID, std::pair<AreaCells, uint16_t>> area_interests_;
    netcode::RoomProperties::AreaRates sent_area_interests_;
    bool area_interest_dirty_ = false, area_interest_overflow_ = false, area_rates_supported_ = false;
    struct PendingRpc { godot::Ref<NetcodeRpcResult> result; uint64_t deadline; bool local_failed = false; };
    struct DeferredRpc { netcode::NativeRpc::Delivery delivery; uint64_t deadline; };
    std::map<uint32_t, PendingRpc> pending_rpcs_;
    std::deque<DeferredRpc> deferred_rpcs_;
    std::set<godot::ObjectID> broadcast_receivers_;
    godot::Ref<NetcodeRpcInfo> rpc_info_;
    uint32_t rpc_token_ = 0;
    godot::Ref<NetcodeGame> game_;
    godot::Ref<NetcodeRoomView> room_;
    godot::ObjectID processor_;
    bool joined_ = false;
    bool host_ = false;
    uint32_t properties_revision_ = 0;
    uint64_t deadline_ = 0;
    int64_t player_ = 0;
    double time_ = 0;
    double rtt_ = 0;
};
