#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace yard::cocos {
namespace {

using namespace cocos2d;
using namespace netcode;
using KeyCode = EventKeyboard::KeyCode;
using namespace std::chrono_literals;

Color4F rgba(int r, int g, int b, int a = 255)
{
    return {r / 255.f, g / 255.f, b / 255.f, a / 255.f};
}

Label* label(Node& parent, const std::string& text, float size, Vec2 position, Color4B color)
{
    auto* node = Label::createWithSystemFont(text, "Segoe UI", size);
    node->setAnchorPoint({0.f, 0.5f});
    node->setPosition(position);
    node->setTextColor(color);
    parent.addChild(node);
    return node;
}

DrawNode* drawing(Node& parent)
{
    auto* node = DrawNode::create();
    parent.addChild(node);
    return node;
}

void background(Node& scene)
{
    auto* draw = drawing(scene);
    draw->drawSolidRect({0, 0}, {960, 640}, rgba(15, 24, 29));
    draw->drawSolidRect({25, 35}, {935, 565}, rgba(39, 56, 62));
    draw->drawRect({25, 35}, {935, 565}, rgba(98, 132, 139));
    draw->drawSolidRect({43, 53}, {917, 547}, rgba(29, 43, 49));
    for (float y = 82.f; y < 546.f; y += 42.f) {
        draw->drawLine({44.f, y}, {917.f, y}, rgba(47, 64, 70));
    }
    for (float x = 77.f; x < 917.f; x += 42.f) {
        draw->drawLine({x, 54.f}, {x, 546.f}, rgba(47, 64, 70));
    }
    draw->drawSolidRect({50, 218}, {905, 356}, rgba(53, 69, 73));
    for (float x = 68.f; x < 882.f; x += 45.f) {
        draw->drawSolidRect({x, 285}, {x + 22.f, 289}, rgba(129, 137, 116));
    }
    draw->drawRect({590, 372}, {816, 515}, rgba(105, 136, 146));
    draw->drawDot({690, 420}, 67.f, rgba(34, 62, 68));
    draw->drawCircle({690, 420}, 67.f, 0.f, 48, false, rgba(94, 148, 154));
    draw->drawSolidRect({60, 412}, {265, 529}, rgba(37, 72, 75));
    for (float x = 60; x <= 265; x += 41) draw->drawLine({x, 412}, {x, 529}, rgba(102, 128, 132));
    label(scene, "CAR YARD", 25, {26, 610}, {236, 246, 245, 255});
    label(scene, "PARKING", 13, {69, 511}, {168, 209, 206, 255});
    label(scene, "3D BEACON", 13, {600, 499}, {168, 209, 206, 255});
    label(scene, "GATE", 12, {350, 373}, {229, 206, 147, 255});
    label(scene, "WASD / arrows  Drive     E  Gate     B  Place     X  Remove     H  Horn",
          14, {27, 16}, {179, 195, 201, 255});
}

void car_shape(Node& node, bool local, PlayerId owner)
{
    auto* paint = drawing(node);
    const auto trim = rgba(16, 24, 27);
    paint->drawSolidRect({-17, -13}, {-8, 13}, trim);
    paint->drawSolidRect({8, -13}, {16, 13}, trim);
    const Vec2 profile[]{{-22, -9}, {10, -9}, {23, -6}, {23, 6}, {10, 9}, {-22, 9}};
    const auto enamel = local ? rgba(58, 216, 180) :
                      owner == host_player ? rgba(255, 165, 79) : rgba(117, 167, 239);
    paint->drawSolidPoly(profile, 6, enamel);
    paint->drawSolidRect({-9, -7}, {6, 7}, rgba(30, 60, 67));
    paint->drawSolidRect({12, -6}, {21, 6}, enamel);
    auto* lamp = drawing(node);
    lamp->setName("lamp");
    lamp->drawDot({21, 5}, 2.3f, rgba(247, 246, 190));
    lamp->drawDot({21, -5}, 2.3f, rgba(247, 246, 190));
}

std::string status_text(const Match& match, bool host)
{
    const auto seats = std::to_string(match.seats()) + "/4 seats";
    switch (match.phase()) {
    case RoomPhase::Connecting: return "Connecting to host";
    case RoomPhase::Waiting: return seats + (host ? "    F2  Start    Q  Close" : "    Waiting for host to start");
    case RoomPhase::Playing:
        if (!match.ready()) return "Applying current world";
        return "Player " + std::to_string(match.player()) + "   Round " +
            std::to_string(match.round()) + (host ? "    F3  End round    Q  Close" : "    F4  Disconnect");
    case RoomPhase::Ended:
        if (match.rejected() != Reject::None) return "Join rejected    R  Retry";
        if (match.loss() == Disconnect::Timeout) return "Host timed out    R  Reconnect";
        return host ? "Room closed" : "Disconnected    R  Reconnect";
    }
    return {};
}

bool close_enough(float a, float b) { return std::abs(a - b) < 0.05f; }

}

YardScene::YardScene(Runner& runner, std::function<void(int)> quit)
    : runner_(runner), quit_(std::move(quit)) {}

YardScene* YardScene::create(Runner& runner, std::function<void(int)> quit)
{
    auto* scene = new YardScene(runner, std::move(quit));
    if (scene->init_yard()) {
        scene->autorelease();
        return scene;
    }
    delete scene;
    return nullptr;
}

bool YardScene::init_yard()
{
    if (!Scene::init()) return false;
    background(*this);
    auto* stage = Node::create();
    addChild(stage);
    replicator_ = std::make_unique<Replicator>(runner_.layout(), *stage,
        [this](const Object& object) { return create_object(object); });
    replicator_->set_custom("net_input_x", [](Node& node, const netcode::Value& value) {
        // The body tint exposes the directly applied root field.
        node.getChildren().front()->setOpacity(static_cast<GLubyte>(218 + std::abs(std::get<float>(value)) * 37.f));
    });
    replicator_->set_custom("glow", [](Node& node, const netcode::Value& value) {
        node.setOpacity(static_cast<GLubyte>(65 + 190 * std::clamp(std::get<float>(value), 0.f, 1.f)));
    });
    replicator_->set_custom("is_open", [](Node& node, const netcode::Value& value) {
        node.getChildren().front()->setColor(std::get<bool>(value) ? Color3B(81, 231, 160) : Color3B(239, 164, 76));
    });
    replicator_->set_custom("marker", [](Node& node, const netcode::Value& value) {
        const auto tone = static_cast<GLubyte>(145 + std::fmod(std::abs(std::get<float>(value)), 110.f));
        node.getChildren().front()->setColor(Color3B(143, tone, 246));
    });
    status_ = label(*this, "", 17, {235, 610}, {188, 215, 217, 255});
    notice_ = label(*this, "", 14, {28, 581}, {250, 215, 147, 255});
    auto* listener = EventListenerKeyboard::create();
    listener->onKeyPressed = [this](KeyCode key, cocos2d::Event*) { key_down(key); };
    listener->onKeyReleased = [this](KeyCode key, cocos2d::Event*) { key_up(key); };
    getEventDispatcher()->addEventListenerWithSceneGraphPriority(listener, this);
    scheduleUpdate();
    return true;
}

Node* YardScene::create_object(const Object& object)
{
    auto* node = Node::create();
    switch (object.schema) {
    case Car:
        car_shape(*node, runner_.match().player() == object.owner, object.owner);
        break;
    case Gate: {
        auto* paint = drawing(*node);
        paint->drawSolidRect({-6, -65}, {6, 65}, rgba(255, 255, 255));
        for (float y = -62.f; y < 62.f; y += 18.f) {
            paint->drawSolidRect({-6, y}, {6, y + 5.f}, rgba(63, 66, 65));
        }
        break;
    }
    case Beacon: {
        auto* model = Sprite3D::create((runner_.options().assets / "beacon.obj").string());
        if (!model) throw std::runtime_error("Cannot load beacon.obj");
        model->setName("model");
        model->setForce2DQueue(true);
        node->addChild(model);
        break;
    }
    case Barrier: {
        auto* paint = drawing(*node);
        paint->drawSolidRect({-18, -13}, {18, 13}, rgba(241, 244, 246));
        paint->drawLine({-15, -10}, {15, 10}, rgba(15, 28, 35));
        break;
    }
    case Load:
        drawing(*node)->drawSolidRect({-5, -5}, {5, 5}, rgba(178, 197, 213));
        break;
    default:
        return nullptr;
    }
    return node;
}

void YardScene::key_down(KeyCode key)
{
    if (!keys_.insert(key).second) return;
    auto& match = runner_.match();
    switch (key) {
    case KeyCode::KEY_F2: match.start(); break;
    case KeyCode::KEY_F3: match.end_round(); break;
    case KeyCode::KEY_E: match.open_gate(); break;
    case KeyCode::KEY_B: match.barrier(true); break;
    case KeyCode::KEY_X: match.barrier(false); break;
    case KeyCode::KEY_H: match.horn(); break;
    case KeyCode::KEY_R: runner_.rejoin(); break;
    case KeyCode::KEY_F4: match.disconnect(); break;
    case KeyCode::KEY_Q:
        match.end_room();
        quitting_ = Clock::now() + 350ms;
        break;
    default: break;
    }
}

void YardScene::key_up(KeyCode key)
{
    keys_.erase(key);
}

void YardScene::clear_input()
{
    keys_.clear();
    runner_.match().set_input({});
}

void YardScene::update(float dt)
{
    const auto now = Clock::now();
    runner_.update(now);
    auto& match = runner_.match();
    if (runner_.finished() || (quitting_ != TimePoint{} && now >= quitting_)) {
        quit_(runner_.finished() ? runner_.status() : 0);
        return;
    }
    if (runner_.options().scenario == Scenario::None) {
        const auto pressed = [this](KeyCode a, KeyCode b) { return keys_.count(a) || keys_.count(b); };
        match.set_input({
            float(pressed(KeyCode::KEY_W, KeyCode::KEY_UP_ARROW)) - float(pressed(KeyCode::KEY_S, KeyCode::KEY_DOWN_ARROW)),
            float(pressed(KeyCode::KEY_D, KeyCode::KEY_RIGHT_ARROW)) - float(pressed(KeyCode::KEY_A, KeyCode::KEY_LEFT_ARROW)),
        });
    }
    if (round_ != match.round()) {
        round_ = match.round();
        replicator_->reset();
    }
    const auto objects = match.objects();
    replicator_->discard_missing(objects);
    const bool guest = !runner_.options().host;
    for (const auto id : objects) replicator_->apply(*match.object(id), guest);
    const auto car = match.car();
    const auto* node = replicator_->node(car);
    const auto before = node ? node->getPositionX() : 0.f;
    replicator_->render(dt);
    if (guest && runner_.options().trace && match.ready() && node && before && !interpolation_logged_) {
        const auto shown = node->getPositionX();
        const auto authority = std::get<float>(match.object(car)->values[0]);
        if (std::abs(shown - before) > 0.02f && std::abs(authority - shown) > 0.02f &&
            (shown - before) * (authority - shown) > 0.f) {
            interpolation_logged_ = true;
            std::cout << "visual id=" << car << " previous_x=" << before << " display_x=" << shown
                      << " authority_x=" << authority << std::endl;
        }
    }
    status_->setString(status_text(match, !guest));
    notice_->setString(match.notice());
    if (!runner_.options().capture.empty() && !captured_ && match.ready() && match.tick() > 20) {
        captured_ = true;
        cocos2d::utils::captureScreen([](bool ok, const std::string&) {
            std::cout << (ok ? "render_capture_ok" : "render_capture_failed") << std::endl;
        }, runner_.options().capture.generic_string());
    }
}

bool verify_bindings(const Layout& layout, Node& parent)
{
    Replicator adapter(layout, parent, [](const Object& source) {
        auto* root = Node::create();
        if (source.schema == Car || source.schema == Beacon) {
            auto* child = Node::create();
            child->setName(source.schema == Car ? "lamp" : "model");
            root->addChild(child);
        }
        return root;
    });
    adapter.set_custom("net_input_x", [](Node& node, const netcode::Value& v) { node.setTag(int(std::get<float>(v) * 100)); });
    adapter.set_custom("glow", [](Node& node, const netcode::Value& v) { node.setTag(int(std::get<float>(v) * 100)); });
    Object car{1, Car, 2, 1, 1, {110.f, 125.f, 45.f, 1.3f, 1.5f, 0.75f, 0.50f}};
    Object model{2, Beacon, 0, 1, 1, {405.f, 340.f, 12.f, 16.f, 32.f, 46.f, 1.1f, 1.2f, 1.3f, 1.f}};
    adapter.apply(car, false);
    adapter.apply(model, false);
    adapter.render(0.f);
    auto* root = adapter.node(1);
    auto* lamp = Replicator::resolve(*root, "lamp");
    auto* mesh = Replicator::resolve(*adapter.node(2), "model");
    const auto euler = mesh->getRotation3D();
    if (lamp->getTag() != 50 || root->getTag() != 75 || !close_enough(root->getPositionX(), 110.f) ||
        !close_enough(root->getPositionY(), 125.f) || !close_enough(root->getRotation(), 45.f) ||
        !close_enough(root->getScaleX(), 1.3f) || !close_enough(root->getScaleY(), 1.5f) ||
        !close_enough(mesh->getPositionX(), 405.f) || !close_enough(mesh->getPositionY(), 340.f) ||
        !close_enough(mesh->getPositionZ(), 12.f) || !close_enough(euler.x, 16.f) || !close_enough(euler.y, 32.f) ||
        !close_enough(euler.z, 46.f) || !close_enough(mesh->getScaleX(), 1.1f) || !close_enough(mesh->getScaleY(), 1.2f) ||
        !close_enough(mesh->getScaleZ(), 1.3f)) return false;
    car.revision = 2;
    car.values[5] = -0.5f;
    adapter.apply(car, true);
    adapter.render(0.016f);
    if (root->getTag() != -50 || lamp->getTag() != 50) return false;
    lamp->removeFromParent();
    adapter.render(0.016f);
    auto* replacement = Node::create();
    replacement->setName("lamp");
    root->addChild(replacement);
    adapter.render(0.016f);
    const bool replaced = replacement->getTag() == 50;
    adapter.discard_missing({2});
    const bool removed = !adapter.node(1) && adapter.node(2);
    adapter.reset();
    return replaced && removed;
}

}
