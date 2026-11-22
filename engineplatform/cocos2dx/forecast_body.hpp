#pragma once
#include "cocos2d.h"
#include "netcode_cocos_config.hpp"
#include "state/value.hpp"
#include <algorithm>
#include <cmath>

#if CC_USE_PHYSICS
namespace netcode::cocos {
// Canonical units are metres, seconds and counterclockwise radians.
class ForecastBody {
  public:
    enum class Correction { Velocity, Force, Blend };
    Correction mode = Correction::Velocity;
    float max_forecast = .1f, stiffness = 50.f, damping = 10.f, blend_factor = .1f;
    cocos2d::Vec2 gravity{}; // Effective acceleration in scene units per second squared.
    ForecastBody(cocos2d::Node &node, float units_per_metre)
        : node_(node), body_(*node.getPhysicsBody()), scale_(units_per_metre) {}
    Values sample(int32_t teleport, int32_t phase) {
        auto p = node_.getPosition(), v = body_.getVelocity();
        return {Vector2{p.x / scale_, p.y / scale_},
                -CC_DEGREES_TO_RADIANS(node_.getRotation()),
                Vector2{v.x / scale_, v.y / scale_},
                body_.getAngularVelocity(),
                body_.isResting(),
                teleport,
                phase};
    }
    void reset_baseline(int32_t next) { generation_=~next; }
    void apply(const Values &s, float elapsed) {
        const auto p = std::get<Vector2>(s[0]), v = std::get<Vector2>(s[2]);
        const auto gen = std::get<int32_t>(s[5]);
        const auto sleep = std::get<bool>(s[4]);
        const bool snap = gen != generation_ || sleep;
        const float age = snap ? 0.f : std::clamp(elapsed, 0.f, std::max(0.f, max_forecast));
        const auto position =
            cocos2d::Vec2(p[0] + age * v[0], p[1] + age * v[1]) * scale_ + gravity * (.5f * age * age);
        auto velocity = cocos2d::Vec2(v[0], v[1]) * scale_ + gravity * age;
        const float target = std::get<float>(s[1]) + age * std::get<float>(s[3]);
        generation_ = gen;
        body_.setResting(false);
        if (snap) {
            node_.setPosition(position);
            node_.setRotation(-CC_RADIANS_TO_DEGREES(target));
            body_.setVelocity(cocos2d::Vec2(v[0], v[1]) * scale_);
        } else if (mode == Correction::Blend) {
            const auto weight = std::clamp(blend_factor, 0.f, 1.f);
            node_.setPosition(node_.getPosition().lerp(position, weight));
            const float current = -CC_DEGREES_TO_RADIANS(node_.getRotation());
            node_.setRotation(
                -CC_RADIANS_TO_DEGREES(current + weight * std::remainder(target - current, 6.2831853f)));
            body_.setVelocity(velocity);
        } else if (mode == Correction::Force) {
            body_.applyForce(
                ((position - node_.getPosition()) * stiffness + (velocity - body_.getVelocity()) * damping) *
                body_.getMass());
        } else {
            body_.setVelocity(velocity + (position - node_.getPosition()) * 6);
        }
        const auto angle = -CC_DEGREES_TO_RADIANS(node_.getRotation());
        const auto spin = std::get<float>(s[3]);
        const auto error = std::remainder(target - angle, 6.2831853f);
        if (!snap && mode == Correction::Force)
            body_.applyTorque(body_.getMoment() *
                              (stiffness * error + damping * (spin - body_.getAngularVelocity())));
        else
            body_.setAngularVelocity(spin + (!snap && mode == Correction::Velocity ? 6 * error : 0));
        if (sleep)
            body_.setResting(true);
    }

  private:
    cocos2d::Node &node_;
    cocos2d::PhysicsBody &body_;
    float scale_;
    int32_t generation_ = -1;
};
} // namespace netcode::cocos
#endif
