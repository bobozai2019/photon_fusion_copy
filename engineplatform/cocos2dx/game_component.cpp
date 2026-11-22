#include "game_component.hpp"

namespace netcode::cocos {
GameComponent* GameComponent::create(std::unique_ptr<ScriptGame> game) {
        if (!game)
            return nullptr;
        auto instance = std::unique_ptr<GameComponent>(new GameComponent);
        if (!instance->init())
            return nullptr;
        instance->game_ = std::move(game);
        instance->autorelease();
        return instance.release();
    }

const char* GameComponent::platform_build_id() noexcept { return NETCODE_PLATFORM_BUILD_ID; }
}
