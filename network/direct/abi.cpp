#include "direct_stats.hpp"

#include <charconv>
#include <cstring>
#include <deque>
#include <set>
#include <stdexcept>
#include <string_view>

struct nc_transport_instance {
    std::unique_ptr<netcode::Transport> link;
    std::deque<netcode::Event> pending;
    std::set<nc_transport_peer> opened, closing;
    netcode::TimePoint origin = netcode::Clock::now();
    uint64_t clock_origin = 0, clock_us = 0;
    bool polled = false;
};

namespace {
using namespace netcode;

bool read_number(std::string_view text, uint32_t& value) {
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

std::string_view as_text(nc_transport_bytes span) {
    return span.data && span.size ? std::string_view(reinterpret_cast<const char*>(span.data), span.size) : std::string_view{};
}

uint32_t abi_reason(Disconnect reason) {
    switch (reason) {
    case Disconnect::Local: return NC_TRANSPORT_CLOSE_LOCAL;
    case Disconnect::Remote: return NC_TRANSPORT_CLOSE_REMOTE;
    case Disconnect::Timeout: return NC_TRANSPORT_CLOSE_TIMEOUT;
    case Disconnect::Protocol: return NC_TRANSPORT_CLOSE_PROTOCOL;
    default: return NC_TRANSPORT_CLOSE_IO_ERROR;
    }
}

nc_transport_result NC_TRANSPORT_CALL open_direct(const nc_transport_open_args* input, nc_transport_instance** out) {
    if (!out) return NC_TRANSPORT_INVALID_ARGUMENT;
    *out = nullptr;
    if (!input || (input->role != NC_TRANSPORT_LISTEN && input->role != NC_TRANSPORT_CONNECT) ||
        !input->endpoint.data || !input->endpoint.size || input->endpoint.size > 21 ||
        input->config.size > 30 || (input->config.size && !input->config.data))
        return NC_TRANSPORT_INVALID_CONFIG;
    const auto endpoint = as_text(input->endpoint);
    const auto colon = endpoint.rfind(':');
    uint32_t port = 0, timeout = 3000;
    if (!colon || colon == endpoint.npos || endpoint.find('\0') != endpoint.npos ||
        !read_number(endpoint.substr(colon + 1), port) || port == 0 || port > 65535)
        return NC_TRANSPORT_INVALID_CONFIG;
    if (input->config.size) {
        constexpr std::string_view prefix = "timeout_ms=";
        const auto config = as_text(input->config);
        if (config.substr(0, prefix.size()) != prefix || !read_number(config.substr(prefix.size()), timeout) ||
            timeout <= 500)
            return NC_TRANSPORT_INVALID_CONFIG;
    }
    try {
        DirectConfig settings{std::string(endpoint.substr(0, colon)), static_cast<uint16_t>(port),
                              std::chrono::milliseconds(timeout)};
        auto instance = std::make_unique<nc_transport_instance>();
        instance->link = input->role == NC_TRANSPORT_LISTEN ? listen_direct(settings) : connect_direct(settings);
        *out = instance.release();
        return NC_TRANSPORT_OK;
    } catch (const std::invalid_argument&) {
        return NC_TRANSPORT_INVALID_CONFIG;
    } catch (...) {
        return NC_TRANSPORT_IO_ERROR;
    }
}

nc_transport_result NC_TRANSPORT_CALL send_batch(nc_transport_instance* instance, Peer peer, uint32_t lane,
        const nc_transport_bytes* messages, uint32_t count, uint32_t* accepted) {
    if (!accepted) return NC_TRANSPORT_INVALID_ARGUMENT;
    *accepted = 0;
    if (!instance || lane >= NC_TRANSPORT_LANE_COUNT || (count && !messages))
        return NC_TRANSPORT_INVALID_ARGUMENT;
    if (!instance->opened.count(peer) || instance->closing.count(peer)) return NC_TRANSPORT_PEER_CLOSED;
    const bool control = lane == NC_TRANSPORT_CONTROL;
    for (size_t i = 0; i < count; ++i) {
        if (messages[i].size > (control ? control_payload_limit : game_payload_limit))
            return NC_TRANSPORT_TOO_LARGE;
        // Legacy Direct reserves empty frames for connection management.
        if (!messages[i].data || !messages[i].size) return NC_TRANSPORT_INVALID_ARGUMENT;
    }
    try {
        while (*accepted < count) {
            const auto& message = messages[*accepted];
            if (!instance->link->send(peer, control ? Lane::Control : Lane::Game,
                                      Bytes(message.data, message.data + message.size)))
                return NC_TRANSPORT_WOULD_BLOCK;
            ++*accepted;
        }
        return NC_TRANSPORT_OK;
    } catch (...) {
        return *accepted ? NC_TRANSPORT_WOULD_BLOCK : NC_TRANSPORT_IO_ERROR;
    }
}

nc_transport_result NC_TRANSPORT_CALL poll(nc_transport_instance* instance, uint64_t now, uint32_t budget,
                                           nc_transport_receive receive, void* context) {
    if (!instance || !receive || now < instance->clock_us) return NC_TRANSPORT_INVALID_ARGUMENT;
    instance->clock_us = now;
    if (!instance->polled) {
        instance->polled = true;
        instance->clock_origin = now;
    }
    const auto elapsed = now - instance->clock_origin;
    if (elapsed > uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            TimePoint::max() - instance->origin).count())) return NC_TRANSPORT_INVALID_ARGUMENT;
    if (!budget) return NC_TRANSPORT_OK;
    try {
        if (instance->pending.empty()) {
            auto events = instance->link->poll(instance->origin + std::chrono::microseconds(elapsed));
            for (auto& event : events) instance->pending.push_back(std::move(event));
        }
        while (budget && !instance->pending.empty()) {
            const auto& event = instance->pending.front();
            nc_transport_event result{};
            result.peer = event.peer;
            result.lane = event.lane == Lane::Control ? NC_TRANSPORT_CONTROL : NC_TRANSPORT_GAME;
            switch (event.type) {
            case EventType::Open:
                result.type = NC_TRANSPORT_OPEN;
                instance->opened.insert(event.peer);
                break;
            case EventType::Message:
                result.type = NC_TRANSPORT_MESSAGE;
                result.payload = {event.payload.data(), static_cast<uint32_t>(event.payload.size())};
                break;
            case EventType::Closed:
                result.type = NC_TRANSPORT_CLOSED;
                result.reason = abi_reason(event.reason);
                instance->opened.erase(event.peer);
                instance->closing.erase(event.peer);
                break;
            }
            receive(context, &result);
            instance->pending.pop_front();
            --budget;
        }
        return NC_TRANSPORT_OK;
    } catch (...) {
        return NC_TRANSPORT_IO_ERROR;
    }
}

nc_transport_result NC_TRANSPORT_CALL close_peer(nc_transport_instance* instance, Peer peer) {
    if (!instance) return NC_TRANSPORT_INVALID_ARGUMENT;
    if (!instance->opened.count(peer)) return NC_TRANSPORT_PEER_CLOSED;
    try {
        if (!instance->closing.insert(peer).second) return NC_TRANSPORT_PEER_CLOSED;
        instance->link->close(peer);
        return NC_TRANSPORT_OK;
    } catch (...) {
        return NC_TRANSPORT_IO_ERROR;
    }
}

void NC_TRANSPORT_CALL destroy(nc_transport_instance* instance) { delete instance; }

nc_transport_result NC_TRANSPORT_CALL caps(nc_transport_instance* instance, nc_transport_caps* out) {
    if (!instance || !out) return NC_TRANSPORT_INVALID_ARGUMENT;
    out->lane[NC_TRANSPORT_CONTROL] = {NC_TRANSPORT_CONTROL_MAX, NC_TRANSPORT_RELIABLE_ORDERED};
    out->lane[NC_TRANSPORT_GAME] = {NC_TRANSPORT_GAME_MAX, 0};
    return NC_TRANSPORT_OK;
}

nc_transport_result NC_TRANSPORT_CALL stats(nc_transport_instance* instance, Peer peer, nc_transport_stats* out) {
    if (!instance || !out) return NC_TRANSPORT_INVALID_ARGUMENT;
    return direct_provider_stats(*instance->link, peer, *out) ? NC_TRANSPORT_OK : NC_TRANSPORT_PEER_CLOSED;
}
}

nc_transport_result NC_TRANSPORT_CALL netcode_transport_get_api(uint32_t abi, uint32_t capacity, void* out) {
    static const uint8_t provider[] = "direct";
    static const uint8_t build[] = NETCODE_DIRECT_BUILD_ID;
    static const nc_transport_api_v1 api{
        NC_TRANSPORT_ABI_V1, sizeof api, {provider, sizeof provider - 1}, {build, sizeof build - 1},
        open_direct, send_batch, poll, close_peer, destroy, caps, stats
    };
    if (abi != NC_TRANSPORT_ABI_V1) return NC_TRANSPORT_UNSUPPORTED_ABI;
    if (!out) return NC_TRANSPORT_INVALID_ARGUMENT;
    if (capacity < sizeof api) return NC_TRANSPORT_SHORT_TABLE;
    std::memcpy(out, &api, sizeof api);
    return NC_TRANSPORT_OK;
}
