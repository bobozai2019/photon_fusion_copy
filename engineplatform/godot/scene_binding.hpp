#pragma once

#include "game.hpp"
#include "forecast_body.hpp"

#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/classes/script.hpp>
#include <memory>
#include <vector>

class NetcodeReplicator;

class NetcodeSceneBinding {
public:
    NetcodeSceneBinding(godot::Node* root, NetcodeReplicator* sync);

    godot::Dictionary definition();
    void set_game(const godot::Ref<NetcodeGame>& game);
    bool bind(const godot::Ref<NetcodeGame>& game, int64_t object);
    void unbind();
    bool initialized() const { return initialized_; }
    bool uses_body() const { return body2d_.is_valid() || body3d_.is_valid(); }
    bool can_apply_changes() const { return direct_; }
    bool is_write_ready() const;
    bool prime_writer();
    godot::Array sample();
    bool apply_model(const godot::Array& fields);
    void set_spatial_position_property(const godot::StringName& property);
    bool can_sample_spatial_position() const { return spatial_position_slot_ >= 0 && initialized_; }
    void step(double delta, godot::Variant* sampled_position = nullptr);
    enum class ReceiveResult { Unavailable, Unchanged, Applied, Writer, PendingCreator };
    ReceiveResult receive(bool check_pending_creator);

    static godot::Dictionary property_info(godot::Node* root, const godot::NodePath& path);
    static godot::String field_kind(const godot::Dictionary& info);
    static godot::String wire_kind(const godot::String& kind);

private:
    enum class Kind { Raw, Integer, Reference, Array, Dictionary };
    struct Field {
        godot::NodePath path;
        godot::NodePath target;
        godot::NodePath indexed;
        godot::StringName member;
        Kind kind = Kind::Raw;
        int interpolation = 0;
        int capacity = 0;
        godot::String hint;
        godot::Ref<godot::Script> record;
        godot::Array record_fields;
        godot::Variant previous_integer;
        godot::PackedByteArray encoded_integer;
        godot::ObjectID reference_node;
        int64_t reference = 0;
        godot::Variant goal;
        double goal_age = 0.0;
    };

    godot::Node* root() const;
    godot::Node* target(const Field& field) const;
    godot::Variant read(Field& field, godot::Node* target);
    bool write(Field& field, godot::Node* target, const godot::Variant& value);
    void step_root(godot::Node* actor, godot::Variant* sampled_position);
    void apply_change(const godot::Array& sampled, const NetcodeGame::ReplicaChange& change,
                      bool body, unsigned generation);
    bool apply(const godot::Array& sampled, const godot::Array& fields, bool baseline,
               int offset = 0, bool invalidated = false);
    void apply_body(const godot::Array& sampled, const godot::Array& fields, bool baseline, double age);
    void before_apply(bool baseline);
    void advance_interpolation(double delta);
    void clear_goals();

    godot::ObjectID root_id_;
    godot::ObjectID sync_id_;
    godot::Ref<NetcodeGame> game_;
    godot::ObjectID reference_game_id_;
    int64_t object_ = 0;
    int64_t baseline_epoch_ = -1;
    int schema_ = 0;
    unsigned binding_ = 0;
    uint64_t integer_fields_ = 0;
    int spatial_position_slot_ = -1;
    bool direct_ = false;
    bool initialized_ = false;
    bool snap_ = true;
    std::vector<Field> fields_;
    godot::Ref<NetcodeForecastBody2D> body2d_;
    godot::Ref<NetcodeForecastBody3D> body3d_;
    godot::Array body_fields_;
    int64_t teleport_ = 0;
    int64_t phase_ = 0;
};
