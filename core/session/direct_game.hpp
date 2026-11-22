#pragma once
#include "session/script_game.hpp"

namespace netcode {
inline std::unique_ptr<ScriptGame> open_direct_game(DirectConfig endpoint, SessionOptions options,
                                                    const ReplicationLayout &layout) {
    auto open = [](bool host, const StandbyEndpoint &address) {
        DirectConfig target{address.address, address.port};
        return host ? listen_direct(target) : connect_direct(target);
    };
    return std::make_unique<ScriptGame>(options.host ? listen_direct(endpoint) : connect_direct(endpoint),
                                        options, layout, open);
}
} // namespace netcode
