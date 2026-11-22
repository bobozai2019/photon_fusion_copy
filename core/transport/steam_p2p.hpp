#pragma once

#include "transport/transport.hpp"

namespace netcode {

class SteamRuntime;

std::shared_ptr<SteamRuntime> start_steam();
std::unique_ptr<Transport> listen_steam(const std::shared_ptr<SteamRuntime>& runtime);
std::unique_ptr<Transport> connect_steam(const std::shared_ptr<SteamRuntime>& runtime,
                                       std::uint64_t host_steam_id);

}
