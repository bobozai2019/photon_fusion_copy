#include "replicator.hpp"

#include <algorithm>
#include <cmath>

namespace yard::cocos {
namespace {

using cocos2d::Node;
using netcode::Value;
constexpr float interpolation_time = 0.10f;

bool angular(std::string_view property)
{
    return property == "angle" || property == "rx" || property == "ry" || property == "rz";
}

void write_transform(Node& node, const std::string& property, float value)
{
    if (property == "x") node.setPositionX(value);
    else if (property == "y") node.setPositionY(value);
    else if (property == "z") node.setPositionZ(value);
    else if (property == "sx") node.setScaleX(value);
    else if (property == "sy") node.setScaleY(value);
    else if (property == "sz") node.setScaleZ(value);
    else if (property == "angle") node.setRotation(value);
    else {
        auto rotation = node.getRotation3D();
        if (property == "rx") rotation.x = value;
        else if (property == "ry") rotation.y = value;
        else if (property == "rz") rotation.z = value;
        node.setRotation3D(rotation);
    }
}

}

Replicator::Replicator(const Layout& layout, Node& parent, Factory factory)
    : layout_(layout), parent_(parent), factory_(std::move(factory)) {}

void Replicator::set_custom(const std::string& name, Setter setter)
{
    custom_.insert_or_assign(name, std::move(setter));
}

void Replicator::reset() { discard_missing({}); }

Node* Replicator::resolve(Node& root, std::string_view path)
{
    auto* node = &root;
    if (path == ".") return node;
    while (node && !path.empty()) {
        const auto separator = path.find('/');
        node = node->getChildByName(std::string(path.substr(0, separator)));
        if (separator == std::string_view::npos) break;
        path.remove_prefix(separator + 1);
    }
    return node;
}

void Replicator::apply(const netcode::Object& update, bool smooth)
{
    auto found = instances_.find(update.id);
    if (found == instances_.end()) {
        auto* root = factory_(update);
        if (!root) return;
        parent_.addChild(root);
        instances_.emplace(update.id, Instance{root, update.schema, update.revision,
                            update.values, update.values, update.values, interpolation_time});
        return;
    }
    auto& instance = found->second;
    if (update.revision <= instance.revision) return;
    instance.from = smooth ? instance.display : update.values;
    instance.to = update.values;
    instance.elapsed = smooth ? 0.f : interpolation_time;
    instance.revision = update.revision;
}

void Replicator::discard_missing(const std::vector<netcode::ObjectId>& objects)
{
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (std::binary_search(objects.begin(), objects.end(), it->first)) {
            ++it;
        } else {
            it->second.node->removeFromParent();
            it = instances_.erase(it);
        }
    }
}

Node* Replicator::node(netcode::ObjectId id) const
{
    const auto found = instances_.find(id);
    return found == instances_.end() ? nullptr : found->second.node;
}

void Replicator::render(float dt)
{
    for (auto& [id, instance] : instances_) {
        instance.elapsed = std::min(interpolation_time, instance.elapsed + dt);
        const auto alpha = instance.elapsed / interpolation_time;
        const auto& fields = layout_.bindings(instance.schema);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const auto& property = fields[i].property;
            if (alpha >= 1.f || !fields[i].transform) {
                instance.display[i] = instance.to[i];
                continue;
            }
            const auto a = std::get<float>(instance.from[i]), b = std::get<float>(instance.to[i]);
            const auto delta = angular(property) ? std::remainder(b - a, 360.f) : b - a;
            instance.display[i] = a + alpha * delta;
        }
        write(instance);
    }
}

void Replicator::write(Instance& instance)
{
    const auto& fields = layout_.bindings(instance.schema);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& binding = fields[i];
        auto* target = resolve(*instance.node, binding.path);
        if (!target) continue;
        if (binding.transform) {
            write_transform(*target, binding.property, std::get<float>(instance.display[i]));
        } else if (const auto handler = custom_.find(binding.property); handler != custom_.end()) {
            handler->second(*target, instance.display[i]);
        }
    }
}

}
