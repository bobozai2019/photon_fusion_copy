#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/rigid_body2d.hpp>
#include <godot_cpp/classes/rigid_body3d.hpp>

#define NETCODE_FORECAST_VALUE(type, name, initial) \
    type name = initial; \
    type get_##name() const { return name; } \
    void set_##name(type value) { name = value; }

class NetcodeForecastBody : public godot::RefCounted {
    GDCLASS(NetcodeForecastBody, godot::RefCounted)

public:
    enum Correction { VELOCITY, FORCE, BLEND };
    NETCODE_FORECAST_VALUE(int, mode, VELOCITY)
    NETCODE_FORECAST_VALUE(double, max_forecast, 0.0)
    NETCODE_FORECAST_VALUE(double, stiffness, 50.0)
    NETCODE_FORECAST_VALUE(double, damping, 10.0)
    NETCODE_FORECAST_VALUE(double, blend_factor, 0.1)
    NETCODE_FORECAST_VALUE(int64_t, generation, -1)

protected:
    static void _bind_methods();
    double forecast_age(double elapsed, bool snap) const;
};

VARIANT_ENUM_CAST(NetcodeForecastBody::Correction);

class NetcodeForecastBody2D : public NetcodeForecastBody {
    GDCLASS(NetcodeForecastBody2D, NetcodeForecastBody)

public:
    NetcodeForecastBody2D() { max_forecast = 0.1; }
    godot::Array sample(godot::RigidBody2D* body, int64_t teleport, int64_t phase) const;
    void apply(godot::RigidBody2D* body, const godot::Array& fields, double elapsed);
    NETCODE_FORECAST_VALUE(godot::Vector2, gravity, {})
    NETCODE_FORECAST_VALUE(double, units_per_metre, 50.0)

protected:
    static void _bind_methods();
};

class NetcodeForecastBody3D : public NetcodeForecastBody {
    GDCLASS(NetcodeForecastBody3D, NetcodeForecastBody)

public:
    NetcodeForecastBody3D() { max_forecast = 0.25; }
    godot::Array sample(godot::RigidBody3D* body, int64_t teleport, int64_t phase) const;
    void apply(godot::RigidBody3D* body, const godot::Array& fields, double elapsed);
    NETCODE_FORECAST_VALUE(godot::Vector3, gravity, {})

protected:
    static void _bind_methods();
};

#undef NETCODE_FORECAST_VALUE
