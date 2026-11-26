#pragma once
#include "session/script_game.hpp"
#include <memory_resource>
#include <set>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

namespace godot { class Node; }

class NetcodeGame final : public godot::RefCounted {
    GDCLASS(NetcodeGame, godot::RefCounted)
  public:
    static godot::String sdk_build_id();
    static bool validate_layout(const godot::String& config);
    static bool is_portable_value(const godot::Variant& value, int depth = 0);
    godot::String open(const godot::String &config, bool host, const godot::String &address, int port,
                       int capacity, bool watch);
    godot::String open_with_provider(const godot::String& layout, bool host, const godot::String& endpoint,
                                    int capacity, bool watch, const godot::String& provider,
                                    const godot::String& module, const godot::PackedByteArray& config,
                                    const godot::PackedByteArray& resume);
    godot::String transport_build_id() const { return transport_build_; }
    godot::String open_provider(const godot::String& layout, bool host, const godot::String& endpoint,
                               int capacity, bool watch, const godot::String& provider, const godot::String& module,
                               const godot::PackedByteArray& config, const godot::PackedByteArray& resume,
                               std::chrono::milliseconds step, bool retain_spatial_writer_interest = false);
    godot::Dictionary transport_stats();
    void disable_snapshot_change_events() { game_->set_snapshot_change_events(false); }
    godot::PackedByteArray resume_ticket();
    bool model(int schema, const godot::PackedInt32Array &fields, int policy, int cooldown);
    godot::Dictionary advance();
    godot::Dictionary routing_info(int64_t object, bool audience);
    bool has_replica_layout(int64_t object, int64_t schema);
    bool bind_array_method(const godot::String&,int,int,int,bool,const godot::Callable&);

    bool start();
    bool add_standby(int64_t player,const godot::String& address,int port);
    bool add_provider_standby(int64_t player, const godot::String& endpoint);
    bool handoff();
    int migration_phase() const;
    void close();
    int64_t spawn(int schema, const godot::Array &values, int scene, int64_t key, int64_t parent,
                  int mode = -1);
    bool distribute(int64_t object, int kind, const godot::PackedInt64Array &keys, int divisor);
    bool subscribe(int64_t player, int kind, const godot::PackedInt64Array &keys,
                   const godot::PackedInt32Array &rates);
    bool use_threshold_clock(bool enabled);
    bool set_player_ttl(int64_t milliseconds);
    bool enable_room_properties(uint64_t catalog);
    bool set_room_properties(const godot::Dictionary& patch, const godot::Dictionary& expected = {});
    bool remove_room_properties(const godot::PackedStringArray& keys);
    godot::Dictionary room_properties() const;
    bool register_prediction(int schema, int version, const godot::PackedByteArray &neutral,
                             const godot::Callable &step);
    bool bind_prediction(int64_t object, int64_t player);
    bool predict(int64_t object, const godot::PackedByteArray &input);
    bool predict_native(int64_t object, double delta, const godot::PackedByteArray& payload);
    godot::Array predicted_fields(int64_t object);
    bool bind_method(const godot::String&,int,int,int,bool,bool,bool,const godot::Callable&);
    bool call_method(const godot::String&,int64_t,const godot::Array&,int64_t,int64_t);
    godot::Dictionary sync_replica(int64_t,const godot::Array&,bool forecast);
    struct ReplicaChange {
        godot::Array fields;
        double age;
        bool baseline;
    };
    std::optional<ReplicaChange> sync_scene_replica(int64_t id, const godot::Array& sample, bool forecast);
    std::optional<ReplicaChange> sync_root_replica(int64_t id, const std::pmr::vector<godot::Variant>& sample,
                                                   uint64_t integer_fields);
    struct ReceivedReplica {
        uint64_t mask = 0;
        bool baseline = false;
        netcode::ScriptGame::ReplicaRole role = netcode::ScriptGame::ReplicaRole::Observer;
    };
    ReceivedReplica take_received_replica(int64_t id, uint64_t integer_fields,
                                          std::pmr::vector<godot::Variant>& fields, bool check_pending_creator);
    int64_t replica_epoch(int64_t id, bool local_only = false) const;
    void unbind_replica(int64_t);
    bool submit_fields(int64_t,const godot::Array&);
    bool register_rpc(int method, int direction, bool reliable, int max_bytes, bool writer, bool room);
    bool invoke_rpc(int64_t id, int method, const godot::PackedByteArray &data, int64_t target, int64_t token);
    bool submit(int64_t id, const godot::Array &values);
    godot::Array read_fields(int64_t id, bool presentation);
    bool claim(int64_t id);
    bool release(int64_t id);
    bool reclaim(int64_t id);
    bool claim_coordinator(int64_t id);
    bool answer(const godot::Dictionary &event, bool approve);
    int64_t writer(int64_t id);
    double sample_time(int64_t id);
    int64_t revision(int64_t id);
    bool loaded(int scene, int generation);
    bool load_map(int scene, const godot::String &asset);
    bool change_map(int scene, const godot::String &asset);
    bool unload_map(int scene);
    godot::String map_asset(int scene);
    int64_t find_object(int scene, int64_t key);
    int64_t parent_of(int64_t id);
    bool reparent_object(int64_t child, int64_t parent);
    bool destroy_object(int64_t id);
    bool allow_spawn(int schema, const godot::Array &defaults, bool creator_writes = false);
    int64_t request_reference(int schema, int scene);
    int64_t resolve_reference(int64_t reference);
    int64_t field_reference(int64_t id);
    int64_t resolve_field(int64_t reference);
    int64_t request_spawn(int schema, int scene);
    bool register_spawn_template(uint32_t template_id, int schema, netcode::ClaimMode mode,
                                 const godot::PackedInt64Array& keys, bool spatial);
    bool can_request_native_spawn() const;
    bool prepare_spawn(int schema, const godot::Array& fields, netcode::Values& values) const;
    int64_t spawn_template(uint32_t template_id, const netcode::Values& values, bool host);
    bool native_authority(int64_t id) const;
    bool distribute_native_cell(int64_t object, uint64_t cell);
    bool pending_creator(int64_t id) const;
    bool request_despawn(int64_t id, uint64_t reference = 0);
    bool subscribe_local(netcode::InterestKind kind, const std::set<uint64_t>& keys);
    bool subscribe_local_area_rates(const netcode::RoomProperties::AreaRates& rates);
    bool supports_area_rates() const;
    void allow_guest_spawns(bool allowed);
    bool send_native_rpc(int64_t id, int32_t target, uint32_t token, const godot::PackedByteArray& data, bool sender_handled);
    void finish_native_rpc(uint32_t receipt, bool delivered);
    std::deque<netcode::NativeRpc::Delivery> native_rpc_deliveries();
    std::deque<netcode::NativeRpc::Outcome> native_rpc_outcomes();
    using PredictionStep = std::function<godot::Array(
        int64_t, const godot::Array&, const godot::PackedByteArray&, double, int64_t, netcode::SimulationStep)>;
    bool register_native_prediction(int schema, int version, const godot::PackedByteArray& neutral,
                                    PredictionStep step);
    int64_t input_player(int64_t object) const;
    int64_t input_queue(int64_t object) const;
    int64_t input_confirmed(int64_t object) const;
    int64_t input_replayed(int64_t object) const;
    uint32_t simulation_tick() const;
    double simulation_delta() const { return simulation_delta_; }
    bool is_stepping() const { return stepping_; }

  protected:
    static void _bind_methods();

  private:
    template<class Sample>
    std::optional<ReplicaChange> sync_scene_sample(int64_t id, const Sample& sample, bool forecast,
                                                  uint64_t integer_fields);
    double simulation_delta_ = 0.03;
    bool available() const { return game_ && game_->active(); }
    std::map<int,godot::Callable> handlers_;
    std::set<int> array_handlers_;
    bool stepping_ = false;
    bool provider_game_ = false;
    godot::String transport_build_;
    std::unique_ptr<netcode::ScriptGame> game_;
    netcode::Values scene_sample_;
    std::map<int, std::vector<netcode::Field>> writable_;
};
