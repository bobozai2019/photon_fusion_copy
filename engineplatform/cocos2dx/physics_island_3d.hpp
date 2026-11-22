#pragma once
#include "cocos2d.h"
#include "netcode_cocos_config.hpp"
#include "state/replication.hpp"
#if CC_USE_3D_PHYSICS
#include "physics3d/CCPhysics3DObject.h"
#include <algorithm>
#include <btBulletDynamicsCommon.h>
#include <cmath>
#include <map>
#include <set>

namespace netcode::cocos {
// The world, geometry and constraints are recreated by the caller before a cold replay.
class PhysicsIsland3D {
  public:
    bool add(ObjectId id, cocos2d::Physics3DRigidBody &body) {
        auto *b = body.getRigidBody();
        if (!id || !b || b->isStaticOrKinematicObject())
            return false;
        for (auto [key, existing] : bodies_)
            if (existing == b)
                return false;
        return bodies_.emplace(id, b).second;
    }
    Values capture(Tick tick) const {
        IntArray ids, flags;
        FloatArray poses;
        for (auto [id, b] : bodies_) {
            ids.push_back(int32_t(id));
            flags.push_back(!b->isActive());
            auto pose = b->getCenterOfMassTransform();
            auto p = pose.getOrigin(), v = b->getLinearVelocity(), w = b->getAngularVelocity();
            auto q = pose.getRotation();
            poses.insert(poses.end(), {p.x(), p.y(), p.z(), q.x(), q.y(), q.z(), q.w(), v.x(), v.y(), v.z(),
                                       w.x(), w.y(), w.z()});
        }
        return {int32_t(tick), std::move(ids), std::move(flags), std::move(poses)};
    }
    bool restore(const Values &fields) const {
        if (fields.size() != 4 || !std::holds_alternative<int32_t>(fields[0]))
            return false;
        auto ids = std::get_if<IntArray>(&fields[1]), flags = std::get_if<IntArray>(&fields[2]);
        auto poses = std::get_if<FloatArray>(&fields[3]);
        if (!ids || !flags || !poses || ids->size() != bodies_.size() || flags->size() != ids->size() ||
            poses->size() != ids->size() * 13)
            return false;
        std::set<ObjectId> seen;
        for (size_t i = 0; i < ids->size(); ++i) {
            ObjectId id = (*ids)[i];
            auto p = poses->data() + i * 13;
            if (!bodies_.count(id) || !seen.insert(id).second || ((*flags)[i] != 0 && (*flags)[i] != 1) ||
                !std::all_of(p, p + 13, [](float v) { return std::isfinite(v); }) ||
                std::abs(btQuaternion(p[3], p[4], p[5], p[6]).length2() - 1) > .001f)
                return false;
        }
        for (size_t i = 0; i < ids->size(); ++i) {
            auto b = bodies_.at((*ids)[i]);
            auto p = poses->data() + i * 13;
            btTransform pose(btQuaternion(p[3], p[4], p[5], p[6]), {p[0], p[1], p[2]});
            b->setCenterOfMassTransform(pose);
            b->setInterpolationWorldTransform(pose);
            b->setLinearVelocity({p[7], p[8], p[9]});
            b->setAngularVelocity({p[10], p[11], p[12]});
            b->clearForces();
            if ((*flags)[i])
                b->forceActivationState(ISLAND_SLEEPING);
            else
                b->activate(true);
        }
        return true;
    }

  private:
    std::map<ObjectId, btRigidBody *> bodies_;
};
} // namespace netcode::cocos
#endif
