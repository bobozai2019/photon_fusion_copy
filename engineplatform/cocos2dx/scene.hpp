#pragma once

#include "cocos2d.h"
#include "replicator.hpp"
#include "runner.hpp"

#include <set>

namespace yard::cocos {

class YardScene final : public cocos2d::Scene {
public:
    static YardScene* create(Runner& runner, std::function<void(int)> quit);
    void clear_input();

private:
    YardScene(Runner& runner, std::function<void(int)> quit);
    bool init_yard();
    void update(float dt) override;
    void key_down(cocos2d::EventKeyboard::KeyCode key);
    void key_up(cocos2d::EventKeyboard::KeyCode key);
    cocos2d::Node* create_object(const netcode::Object& object);

    Runner& runner_;
    std::function<void(int)> quit_;
    std::unique_ptr<Replicator> replicator_;
    cocos2d::Label* status_ = nullptr;
    cocos2d::Label* notice_ = nullptr;
    std::set<cocos2d::EventKeyboard::KeyCode> keys_;
    netcode::RoundId round_ = 0;
    netcode::TimePoint quitting_{};
    bool interpolation_logged_ = false, captured_ = false;
};

bool verify_bindings(const Layout& layout, cocos2d::Node& parent);

}
