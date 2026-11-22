#pragma once

#include "host/provider.hpp"
#include "session/script_game.hpp"

namespace netcode {

struct ProviderConfig {
    std::string id = "tcp";
    std::filesystem::path module;
    std::string config;
};

struct ProviderGame {
    std::unique_ptr<ScriptGame> game;
    ProviderIdentity provider;
};

// Relative plugin paths resolve beside the caller's native module or executable.
ProviderGame open_provider_game(const ProviderConfig& provider, std::string_view endpoint, SessionOptions options,
                                const ReplicationLayout& layout, const void* module_anchor);
std::filesystem::path native_module_directory(const void* module_anchor);
Bytes encode_resume_ticket(const ResumeTicket& ticket);
ResumeTicket decode_resume_ticket(const uint8_t* bytes, size_t size);

}
