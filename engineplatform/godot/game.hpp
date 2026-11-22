#pragma once
#include "session/script_game.hpp"
#include <set>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

class NetcodeGame final : public godot::RefCounted {
    GDCLASS(NetcodeGame, godot::RefCounted)
  public:
    static godot::String sdk_build_id();
    static bool validate_layout(const godot::String& config);
    godot::String open(const godot::String &config, bool host, const godot::String &address, int port,
                       int capacity, bool watch);
    godot::String open_with_provider(const godot::String& layout, bool host, const godot::String& endpoint,
                                    int capacity, bool watch, const godot::String& provider,
                                    const godot::String& module, const godot::PackedByteArray& config,
                                    const godot::PackedByteArray& resume);
    godot::String transport_build_id() const { return transport_build_; }
    godot::Dictionary transport_stats();
    godot::PackedByteArray resume_ticket();
    bool model(int schema, const godot::PackedInt32Array &fields, int policy, int cooldown);
    godot::Dictionary advance();
    godot::Dictionary routing_info(int64_t object, bool audience);
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
    bool register_prediction(int schema, int version, const godot::PackedByteArray &neutral,
                             const godot::Callable &step);
    bool bind_prediction(int64_t object, int64_t player);
    bool predict(int64_t object, const godot::PackedByteArray &input);
    godot::Array predicted_fields(int64_t object);
    bool bind_method(const godot::String&,int,int,int,bool,bool,bool,const godot::Callable&);
    bool call_method(const godot::String&,int64_t,const godot::Array&,int64_t,int64_t);
    godot::Dictionary sync_replica(int64_t,const godot::Array&,bool forecast);
    void unbind_replica(int64_t);
    bool submit_fields(int64_t,const godot::Array&);
    bool register_rpc(int method, int direction, bool reliable, int max_bytes, bool writer, bool room);
    bool invoke_rpc(int64_t id, int method, const godot::PackedByteArray &data, int64_t target, int64_t token);
    bool submit(int64_t id, const godot::Array &values);
    godot::Array read_fields(int64_t id, bool presentation);
    bool claim(int64_t id);
    bool release(int64_t id);
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

  protected:
    static void _bind_methods();

  private:
    bool available() const { return game_ && game_->active(); }
    std::map<int,godot::Callable> handlers_;
    std::set<int> array_handlers_;
    bool stepping_ = false;
    bool provider_game_ = false;
    godot::String transport_build_;
    std::unique_ptr<netcode::ScriptGame> game_;
    std::map<int, std::vector<netcode::Field>> writable_;
};
