#include "forecast_body.hpp"

#include <godot_cpp/classes/physics_direct_body_state2d.hpp>
#include <godot_cpp/classes/physics_server2d.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>

using namespace godot;

#define NETCODE_FORECAST_PROPERTY(Class, type, name) \
    ClassDB::bind_method(D_METHOD("get_" #name), &Class::get_##name); \
    ClassDB::bind_method(D_METHOD("set_" #name, "value"), &Class::set_##name); \
    ADD_PROPERTY(PropertyInfo(Variant::type, #name), "set_" #name, "get_" #name)

void NetcodeForecastBody::_bind_methods()
{
    BIND_ENUM_CONSTANT(VELOCITY);
    BIND_ENUM_CONSTANT(FORCE);
    BIND_ENUM_CONSTANT(BLEND);
    NETCODE_FORECAST_PROPERTY(NetcodeForecastBody, INT, mode);
    NETCODE_FORECAST_PROPERTY(NetcodeForecastBody, FLOAT, max_forecast);
    NETCODE_FORECAST_PROPERTY(NetcodeForecastBody, FLOAT, stiffness);
    NETCODE_FORECAST_PROPERTY(NetcodeForecastBody, FLOAT, damping);
    NETCODE_FORECAST_PROPERTY(NetcodeForecastBody, FLOAT, blend_factor);
    NETCODE_FORECAST_PROPERTY(NetcodeForecastBody, INT, generation);
}

double NetcodeForecastBody::forecast_age(double elapsed, bool snap) const
{
    return snap ? 0.0 : std::clamp(elapsed, 0.0, std::max(0.0, max_forecast));
}

void NetcodeForecastBody2D::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("sample", "body", "teleport", "phase"), &NetcodeForecastBody2D::sample);
    ClassDB::bind_method(D_METHOD("apply", "body", "fields", "elapsed"), &NetcodeForecastBody2D::apply);
    NETCODE_FORECAST_PROPERTY(NetcodeForecastBody2D, VECTOR2, gravity);
    NETCODE_FORECAST_PROPERTY(NetcodeForecastBody2D, FLOAT, units_per_metre);
}

Array NetcodeForecastBody2D::sample(RigidBody2D* body, int64_t teleport, int64_t phase) const
{
    if (!body || units_per_metre <= 0.0) return {};
    const auto position = body->get_global_position() / units_per_metre;
    const auto velocity = body->get_linear_velocity() / units_per_metre;
    return Array::make(Vector2(position.x, -position.y), -body->get_global_rotation(),
                       Vector2(velocity.x, -velocity.y), -body->get_angular_velocity(),
                       body->is_sleeping(), teleport, phase);
}

void NetcodeForecastBody2D::apply(RigidBody2D* body, const Array& fields, double elapsed)
{
    if (!body || fields.size() < 7) return;
    const bool sleeping = fields[4];
    const bool snap = generation != int64_t(fields[5]) || sleeping;
    const auto age = forecast_age(elapsed, snap);
    const Vector2 position = fields[0], velocity = fields[2];
    auto v = Vector2(velocity.x, -velocity.y) * units_per_metre;
    const auto target = Vector2(position.x, -position.y) * units_per_metre + v * age + gravity * (0.5 * age * age);
    v += gravity * age;
    const auto angle = -double(fields[1]) - age * double(fields[3]);
    generation = fields[5];
    body->set_sleeping(false);
    if (snap) {
        body->set_global_position(target);
        body->set_global_rotation(angle);
        body->set_linear_velocity(v);
    } else if (mode == BLEND) {
        const auto weight = std::clamp(blend_factor, 0.0, 1.0);
        body->set_global_position(body->get_global_position().lerp(target, weight));
        body->set_global_rotation(Math::lerp_angle(double(body->get_global_rotation()), angle, weight));
        body->set_linear_velocity(v);
    } else if (mode == FORCE) {
        body->apply_central_force(body->get_mass() * (stiffness * (target - body->get_global_position()) +
                                                   damping * (v - body->get_linear_velocity())));
    } else {
        body->set_linear_velocity(v + (target - body->get_global_position()) * 6);
    }
    const auto spin = -double(fields[3]);
    const auto error = UtilityFunctions::angle_difference(body->get_global_rotation(), angle);
    if (!snap && mode == FORCE) {
        auto* state = PhysicsServer2D::get_singleton()->body_get_direct_state(body->get_rid());
        if (state && state->get_inverse_inertia() > 0) {
            body->apply_torque((stiffness * error + damping * (spin - body->get_angular_velocity())) /
                               state->get_inverse_inertia());
        }
    } else {
        body->set_angular_velocity(spin + (!snap && mode == VELOCITY ? error * 6 : 0.0));
    }
    if (sleeping) body->set_sleeping(true);
}

void NetcodeForecastBody3D::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("sample", "body", "teleport", "phase"), &NetcodeForecastBody3D::sample);
    ClassDB::bind_method(D_METHOD("apply", "body", "fields", "elapsed"), &NetcodeForecastBody3D::apply);
    NETCODE_FORECAST_PROPERTY(NetcodeForecastBody3D, VECTOR3, gravity);
}

Array NetcodeForecastBody3D::sample(RigidBody3D* body, int64_t teleport, int64_t phase) const
{
    if (!body) return {};
    return Array::make(body->get_global_position(), body->get_global_basis().get_rotation_quaternion(),
                       body->get_linear_velocity(), body->get_angular_velocity(), body->is_sleeping(), teleport, phase);
}

void NetcodeForecastBody3D::apply(RigidBody3D* body, const Array& fields, double elapsed)
{
    if (!body || fields.size() < 7) return;
    const bool sleeping = fields[4];
    const bool snap = generation != int64_t(fields[5]) || sleeping;
    generation = fields[5];
    const auto age = forecast_age(elapsed, snap);
    Vector3 velocity = fields[2];
    const Vector3 position = fields[0], spin = fields[3];
    const auto target = position + velocity * age + gravity * (0.5 * age * age);
    velocity += gravity * age;
    Quaternion rotation = fields[1];
    if (spin.length_squared() > 0.000001) rotation = Quaternion(spin.normalized(), spin.length() * age) * rotation;
    const auto current = body->get_global_basis().get_rotation_quaternion();
    auto error = (rotation * current.inverse()).normalized();
    if (error.w < 0) error = -error;
    const auto angular_error = error.w < 0.999999 ? error.get_axis() * error.get_angle() : Vector3();
    body->set_sleeping(false);
    if (snap) {
        body->set_global_transform(Transform3D(Basis(rotation), target));
        body->set_linear_velocity(velocity);
        body->set_angular_velocity(spin);
    } else if (mode == BLEND) {
        const auto weight = std::clamp(blend_factor, 0.0, 1.0);
        body->set_global_transform(Transform3D(Basis(current.slerp(rotation, weight)),
                                             body->get_global_position().lerp(target, weight)));
        body->set_linear_velocity(velocity);
        body->set_angular_velocity(spin);
    } else if (mode == FORCE) {
        body->apply_central_force(body->get_mass() * (stiffness * (target - body->get_global_position()) +
                                                     damping * (velocity - body->get_linear_velocity())));
        const auto inverse = body->get_inverse_inertia_tensor();
        if (std::abs(inverse.determinant()) > 0.0000001) {
            body->apply_torque(inverse.inverse().xform(stiffness * angular_error + damping * (spin - body->get_angular_velocity())));
        }
    } else {
        body->set_linear_velocity(velocity + 6 * (target - body->get_global_position()));
        body->set_angular_velocity(spin + 6 * angular_error);
    }
    if (sleeping) body->set_sleeping(true);
}
