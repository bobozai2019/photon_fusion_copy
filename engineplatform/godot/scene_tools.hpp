#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/rigid_body3d.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/typed_dictionary.hpp>
#include <map>

class NetcodePhysicsIsland3D : public godot::RefCounted {
    GDCLASS(NetcodePhysicsIsland3D, godot::RefCounted)

public:
    bool add(int64_t id, godot::RigidBody3D* body);
    godot::Dictionary get_bodies() const;
    godot::Array capture(int64_t tick) const;
    bool restore(const godot::Array& fields) const;

protected:
    static void _bind_methods();

private:
    std::map<uint32_t, godot::ObjectID> bodies_;
};

class NetcodeLayoutExporter : public godot::Node {
    GDCLASS(NetcodeLayoutExporter, godot::Node)

public:
    godot::String build_text(godot::Node* root) const;
    void export_layout() const;

#define NETCODE_LAYOUT_VALUE(Type, name, initial) \
    Type name = initial; \
    Type get_##name() const { return name; } \
    void set_##name(const Type& value) { name = value; }

    using Fields = godot::TypedDictionary<int64_t, godot::NodePath>;
    using Limits = godot::TypedDictionary<int64_t, int64_t>;
    NETCODE_LAYOUT_VALUE(int, schema_id, 1)
    NETCODE_LAYOUT_VALUE(int, version, 1)
    NETCODE_LAYOUT_VALUE(godot::String, schema_name, "Actor")
    NETCODE_LAYOUT_VALUE(Fields, fields, {})
    NETCODE_LAYOUT_VALUE(godot::PackedInt32Array, reference_fields, {})
    NETCODE_LAYOUT_VALUE(Limits, limits, {})
    NETCODE_LAYOUT_VALUE(godot::String, output, "res://replicators.cfg")
#undef NETCODE_LAYOUT_VALUE
    godot::Callable get_export_action() { return godot::Callable(this, "export_layout"); }

protected:
    static void _bind_methods();
};
