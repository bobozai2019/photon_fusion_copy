#pragma once

#include "2d/CCNode.h"
#include "layout.hpp"

#include <functional>

namespace yard::cocos {

class Replicator {
public:
    using Factory = std::function<cocos2d::Node*(const netcode::Object&)>;
    using Setter = std::function<void(cocos2d::Node&, const netcode::Value&)>;

    Replicator(const Layout& layout, cocos2d::Node& parent, Factory factory);
    void set_custom(const std::string& name, Setter setter);
    void reset();
    void apply(const netcode::Object& update, bool smooth);
    void discard_missing(const std::vector<netcode::ObjectId>& objects);
    void render(float dt);
    cocos2d::Node* node(netcode::ObjectId id) const;
    static cocos2d::Node* resolve(cocos2d::Node& root, std::string_view path);

private:
    struct Instance {
        cocos2d::Node* node;
        netcode::SchemaId schema;
        netcode::Tick revision;
        netcode::Values from, to, display;
        float elapsed;
    };
    void write(Instance& instance);

    const Layout& layout_;
    cocos2d::Node& parent_;
    Factory factory_;
    std::map<std::string, Setter> custom_;
    std::map<netcode::ObjectId, Instance> instances_;
};

}
