#pragma once
#include "forecast_body.hpp"
#include "forecast_body_3d.hpp"
#include "game_component.hpp"
#include <cmath>

namespace netcode::cocos {
// Bind one field accessor pair per schema slot. Cocos owns the component through the node.
class ReplicaComponent final : public cocos2d::Component {
  public:
    static ReplicaComponent *attach(cocos2d::Node &node, GameComponent &network, ObjectId id) {
        if (!id || network.replica_nodes()->count(id) || node.getComponent("NetcodeReplica"))
            return nullptr;
        auto c = std::unique_ptr<ReplicaComponent>(new ReplicaComponent);
        if (!c->init())
            return nullptr;
        c->setName("NetcodeReplica");
        c->session_ = network.session_handle();
        c->id_ = id;
        c->nodes_ = network.replica_nodes();
        auto result = c.release();
        result->autorelease();
        if (!node.addComponent(result))
            return nullptr;
        (*network.replica_nodes())[id] = &node;
        node.scheduleUpdate();
        return result;
    }
    void field(std::function<Value()> read, std::function<void(const Value &)> write) {
        fields_.push_back({std::move(read), std::move(write)});
    }
    template <class T> void member(T &target) {
        field([&target]() -> Value { return target; },
              [&target](const Value &value) { target = std::get<T>(value); });
    }
#if CC_USE_PHYSICS
    bool use_body_2d(float units_per_metre = 50.f) {
        auto *node = getOwner();
        if (!node || !node->getPhysicsBody() || !(units_per_metre > 0))
            return false;
        auto body = std::make_shared<ForecastBody>(*node, units_per_metre);
        bind_body(body);
        return true;
    }
#endif
#if CC_USE_3D_PHYSICS
    void use_body_3d(cocos2d::Physics3DRigidBody &native) {
        bind_body(std::make_shared<ForecastBody3D>(native));
    }
#endif
    ObjectReference reference() const {
        auto game = session_.lock();
        return game && game->active() ? game->session().world().field_reference(id_) : ObjectReference{};
    }
    void onEnter() override {
        Component::onEnter();
        if (auto nodes = nodes_.lock())
            nodes->emplace(id_, getOwner());
    }
    bool initialized() const { return initialized_; }
    int32_t teleport_generation = 0, phase = 0;
    float teleport_distance = 100.f, teleport_angle_degrees = 15.f;
    void onExit() override {
        reset();
        Component::onExit();
    }
    void onRemove() override {
        reset();
        Component::onRemove();
    }
    void update(float) override {
        auto game = session_.lock();
        if (!game || !game->active())
            return;
        auto nodes = nodes_.lock();
        if (!nodes || nodes->find(id_) == nodes->end() || nodes->at(id_) != getOwner())
            return;
#if CC_USE_PHYSICS
        if (fields_.empty() && !capture_)
            use_body_2d();
#endif
        Values sample, applied;
        if (capture_)
            sample = capture_();
        else
            for (const auto &item : fields_)
                sample.push_back(item.first());
        int mode = game->sync_replica(id_, sample, applied, bool(capture_));
        if (!mode)
            return;
        if (apply_) {
            auto &g = game->session();
            auto *obj = g.object(id_);
            float age = float(g.clock().seconds() - g.clock().object_time(obj->sample_tick));
            apply_(sample, applied, age, mode == 2);
        } else {
            for (size_t i = 0; i < fields_.size(); ++i)
                fields_[i].second(applied[i]);
        }
        initialized_ = true;
    }

  private:
    void reset() {
        if (auto nodes = nodes_.lock()) {
            auto it = nodes->find(id_);
            if (it != nodes->end() && it->second == getOwner()) {
                nodes->erase(it);
                if (auto g = session_.lock())
                    g->unbind_replica(id_);
            }
        }
        initialized_ = false;
    }
    std::weak_ptr<GameComponent::ReplicaNodes> nodes_;
    template <class Body> void bind_body(std::shared_ptr<Body> body) {
        capture_ = [this, body] { return body->sample(teleport_generation, phase); };
        apply_ = [this, body](const Values &sample, const Values &v, float age, bool baseline) {
            teleport_generation = std::get<int32_t>(v[5]);
            phase = std::get<int32_t>(v[6]);
            float distance2 = 0, radians = 0;
            if (std::holds_alternative<Vector2>(v[0])) {
                auto a = std::get<Vector2>(sample[0]), b = std::get<Vector2>(v[0]);
                for (size_t i = 0; i < a.size(); ++i)
                    distance2 += (a[i] - b[i]) * (a[i] - b[i]);
                radians =
                    std::abs(std::remainder(std::get<float>(sample[1]) - std::get<float>(v[1]), 6.2831853f));
            } else {
                auto a = std::get<Vector3>(sample[0]), b = std::get<Vector3>(v[0]);
                for (size_t i = 0; i < a.size(); ++i)
                    distance2 += (a[i] - b[i]) * (a[i] - b[i]);
                auto q = std::get<Rotation>(sample[1]), r = std::get<Rotation>(v[1]);
                float dot = 0;
                for (size_t i = 0; i < q.size(); ++i)
                    dot += q[i] * r[i];
                radians = 2 * std::acos(std::min(1.f, std::abs(dot)));
            }
            if (baseline || (teleport_distance > 0 && distance2 > teleport_distance * teleport_distance) ||
                (teleport_angle_degrees > 0 && radians * 57.29578f > teleport_angle_degrees))
                body->reset_baseline(teleport_generation);
            body->apply(v, age);
        };
    }
    std::weak_ptr<ScriptGame> session_;
    ObjectId id_ = 0;
    bool initialized_ = false;
    std::vector<std::pair<std::function<Value()>, std::function<void(const Value &)>>> fields_;
    std::function<Values()> capture_;
    std::function<void(const Values &, const Values &, float, bool)> apply_;
};
} // namespace netcode::cocos
