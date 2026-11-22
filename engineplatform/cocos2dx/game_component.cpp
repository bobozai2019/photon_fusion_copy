#include "game_component.hpp"

namespace netcode::cocos {
namespace {
const char module_anchor = 0;
}
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
GameComponent* GameComponent::create_with_provider(std::string_view endpoint, SessionOptions options,
                                                   const ReplicationLayout& layout, const ProviderConfig& provider) {
    auto opened = open_provider_game(provider, endpoint, options, layout, &module_anchor);
    auto* component = create(std::move(opened.game));
    if (component) component->provider_ = std::move(opened.provider);
    return component;
}
}
