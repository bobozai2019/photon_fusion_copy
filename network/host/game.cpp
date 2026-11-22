#include "host/game.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <stdexcept>

namespace netcode {

std::filesystem::path native_module_directory(const void* anchor) {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!anchor || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(anchor), &module))
        throw std::runtime_error("Cannot resolve the host native module");
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) throw std::runtime_error("Cannot read the host native module path");
    path.resize(length);
    return std::filesystem::path(path).parent_path();
#else
    Dl_info info{};
    if (!anchor || !dladdr(anchor, &info) || !info.dli_fname)
        throw std::runtime_error("Cannot resolve the host native module");
    return std::filesystem::absolute(info.dli_fname).parent_path();
#endif
}

ProviderGame open_provider_game(const ProviderConfig& config, std::string_view endpoint, SessionOptions options,
                                const ReplicationLayout& layout, const void* anchor) {
    if (config.id.empty()) throw std::invalid_argument("A transport provider ID is required");
    if (options.host && options.resume) throw std::invalid_argument("A host cannot use a guest resume ticket");
    auto path = config.module;
    if (path.empty() && config.id == "tcp") {
#ifdef _WIN32
        path = "netcode_transport_tcp.dll";
#else
        path = "netcode_transport_tcp.so";
#endif
    }
    if (!path.empty() && path.is_relative()) path = native_module_directory(anchor) / path;
    auto provider = TransportProvider::load(path, config.id);
    auto identity = provider.identity();
    auto link = provider.open(options.host, endpoint, config.config);
    auto open = [provider = std::move(provider), settings = config.config](bool host, const StandbyEndpoint& standby) {
        if (standby.provider_endpoint.empty())
            throw std::invalid_argument("A provider standby requires a provider endpoint");
        return provider.open(host, standby.provider_endpoint, settings);
    };
    return {std::make_unique<ScriptGame>(std::move(link), options, layout, std::move(open)), std::move(identity)};
}

Bytes encode_resume_ticket(const ResumeTicket& ticket) {
    if (!ticket.room || !ticket.round || !ticket.player) return {};
    Writer out;
    out.u64(ticket.room);
    out.u64(ticket.round);
    out.u32(ticket.player);
    for (const auto word : ticket.key) out.u32(word);
    return out.take();
}

ResumeTicket decode_resume_ticket(const uint8_t* bytes, size_t size) {
    constexpr size_t encoded_size = 2 * sizeof(uint64_t) + 5 * sizeof(uint32_t);
    if (!bytes || size != encoded_size) throw std::invalid_argument("Invalid resume ticket");
    Reader in(bytes, size);
    ResumeTicket ticket{};
    if (!in.u64(ticket.room) || !in.u64(ticket.round) || !in.u32(ticket.player) ||
        !ticket.room || !ticket.round || !ticket.player)
        throw std::invalid_argument("Invalid resume ticket");
    for (auto& word : ticket.key) in.u32(word);
    return ticket;
}

}
