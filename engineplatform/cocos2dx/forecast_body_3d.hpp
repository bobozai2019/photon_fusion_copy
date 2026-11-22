#pragma once
#include "cocos2d.h"
#include "netcode_cocos_config.hpp"
#include "state/value.hpp"
#if CC_USE_3D_PHYSICS
#include <btBulletDynamicsCommon.h>
#include "physics3d/CCPhysics3DObject.h"
#include <algorithm>

namespace netcode::cocos {
class ForecastBody3D {
  public:
    explicit ForecastBody3D(cocos2d::Physics3DRigidBody &body) : body_(*body.getRigidBody()) {}
    enum class Correction { Velocity, Force, Blend };
    Correction mode = Correction::Velocity;
    float max_forecast = .25f, stiffness = 50, damping = 10, blend_factor = .1f;
    btVector3 gravity{0, 0, 0};
    Values sample(int32_t generation, int32_t phase) const {
        const auto &pose = body_.getCenterOfMassTransform();
        const auto q = pose.getRotation();
        return {vector(pose.getOrigin()),
                Rotation{q.x(), q.y(), q.z(), q.w()},
                vector(body_.getLinearVelocity()),
                vector(body_.getAngularVelocity()),
                !body_.isActive(),
                generation,
                phase};
    }
    void reset_baseline(int32_t next) { generation_=~next; }
    void apply(const Values &s, float elapsed) {
        const bool asleep = std::get<bool>(s[4]);
        const bool snap = asleep || generation_ != std::get<int32_t>(s[5]);
        generation_ = std::get<int32_t>(s[5]);
        const float age = snap ? 0.f : std::clamp(elapsed, 0.f, std::max(0.f, max_forecast));
        auto velocity = vector(std::get<Vector3>(s[2]));
        const auto spin = vector(std::get<Vector3>(s[3]));
        const auto position = vector(std::get<Vector3>(s[0])) + velocity * age + gravity * (.5f * age * age);
        velocity += gravity * age;
        const auto q = std::get<Rotation>(s[1]);
        btQuaternion rotation(q[0], q[1], q[2], q[3]);
        if (spin.length2() > 1e-6f)
            rotation = btQuaternion(spin.normalized(), age * spin.length()) * rotation;
        const auto &current = body_.getCenterOfMassTransform();
        const auto current_q = current.getRotation();
        auto delta = (rotation * current_q.inverse()).normalized();
        if (delta.w() < 0)
            delta = -delta;
        const auto angular = delta.w() < .999999f ? delta.getAxis() * delta.getAngle() : btVector3(0, 0, 0);
        body_.activate(true);
        if (snap || mode == Correction::Blend) {
            const float weight = snap ? 1.f : std::clamp(blend_factor, 0.f, 1.f);
            body_.setCenterOfMassTransform(
                btTransform(current_q.slerp(rotation, weight), current.getOrigin().lerp(position, weight)));
            body_.setLinearVelocity(velocity);
            body_.setAngularVelocity(spin);
        } else if (mode == Correction::Force && body_.getInvMass() > 0) {
            body_.applyCentralForce(((position - current.getOrigin()) * stiffness +
                                     (velocity - body_.getLinearVelocity()) * damping) /
                                    body_.getInvMass());
            const auto &inverse = body_.getInvInertiaTensorWorld();
            if (btFabs(inverse.determinant()) > 1e-7f)
                body_.applyTorque(inverse.inverse() *
                                  (angular * stiffness + (spin - body_.getAngularVelocity()) * damping));
        } else {
            body_.setLinearVelocity(velocity + (position - current.getOrigin()) * 6);
            body_.setAngularVelocity(spin + angular * 6);
        }
        if (asleep)
            body_.forceActivationState(ISLAND_SLEEPING);
    }

  private:
    static Vector3 vector(const btVector3 &v) { return {v.x(), v.y(), v.z()}; }
    static btVector3 vector(const Vector3 &v) { return {v[0], v[1], v[2]}; }
    btRigidBody &body_;
    int32_t generation_ = -1;
};
} // namespace netcode::cocos
#endif
