#include "provider.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>

namespace netcode {
namespace {

std::string result_name(nc_transport_result result) {
    switch (result) {
    case NC_TRANSPORT_OK: return "ok";
    case NC_TRANSPORT_WOULD_BLOCK: return "would_block";
    case NC_TRANSPORT_PEER_CLOSED: return "peer_closed";
    case NC_TRANSPORT_TOO_LARGE: return "too_large";
    case NC_TRANSPORT_INVALID_CONFIG: return "invalid_config";
    case NC_TRANSPORT_UNSUPPORTED_ABI: return "unsupported_abi";
    case NC_TRANSPORT_IO_ERROR: return "io_error";
    case NC_TRANSPORT_INVALID_ARGUMENT: return "invalid_argument";
    case NC_TRANSPORT_SHORT_TABLE: return "short_table";
    default: return "result " + std::to_string(result);
    }
}

std::string identity_string(nc_transport_bytes bytes, const char* field) {
    if (!bytes.data || !bytes.size || bytes.size > 1024 ||
        std::find(bytes.data, bytes.data + bytes.size, uint8_t{0}) != bytes.data + bytes.size)
        throw std::runtime_error(std::string("Invalid transport ") + field);
    return {reinterpret_cast<const char*>(bytes.data), bytes.size};
}

nc_transport_bytes span(std::string_view value) {
    if (value.size() > UINT32_MAX) throw std::invalid_argument("Transport configuration exceeds ABI limit");
    return {reinterpret_cast<const uint8_t*>(value.data()), static_cast<uint32_t>(value.size())};
}

std::size_t payload_limit(Lane lane) {
    return lane == Lane::Control ? control_payload_limit : game_payload_limit;
}

uint32_t lane_id(Lane lane) {
    return lane == Lane::Control ? NC_TRANSPORT_CONTROL : NC_TRANSPORT_GAME;
}

Disconnect disconnect(uint32_t reason) {
    switch (reason) {
    case NC_TRANSPORT_CLOSE_LOCAL: return Disconnect::Local;
    case NC_TRANSPORT_CLOSE_REMOTE: return Disconnect::Remote;
    case NC_TRANSPORT_CLOSE_TIMEOUT: return Disconnect::Timeout;
    case NC_TRANSPORT_CLOSE_PROTOCOL: return Disconnect::Protocol;
    default: return Disconnect::IoError;
    }
}

class PluginTransport final : public Transport {
public:
    PluginTransport(std::shared_ptr<const void> module, const nc_transport_api_v1& api,
                    nc_transport_instance* instance, const nc_transport_caps& caps, std::string id)
        : module_(std::move(module)), api_(api), instance_(instance, api.destroy), caps_(caps), id_(std::move(id)) {}

    std::vector<Event> poll(TimePoint now) override {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        now_us_ = std::max(now_us_, static_cast<uint64_t>(std::max<decltype(us)>(us, 0)));
        Receive receive;
        const auto result = api_.poll(instance_.get(), now_us_, 4096, collect, &receive);
        if (receive.error) std::rethrow_exception(receive.error);
        if (result != NC_TRANSPORT_OK) throw std::runtime_error("Transport " + id_ + " poll: " + result_name(result));
        return std::move(receive.events);
    }

    bool send(Peer peer, Lane lane, const Bytes& payload) override {
        const Bytes* message = &payload;
        return send_batch(peer, lane, &message, 1).accepted == 1;
    }

    BatchResult send_batch(Peer peer, Lane lane, const Bytes* const* messages, std::size_t count) override {
        if (count > UINT32_MAX) return {SendStatus::Invalid, 0};
        // Most control sends and small state batches fit without allocation.
        nc_transport_bytes inline_messages[16];
        std::vector<nc_transport_bytes> overflow;
        auto* input = inline_messages;
        if (count > 16) {
            overflow.resize(count);
            input = overflow.data();
        }
        for (size_t i = 0; i < count; ++i) {
            if (messages[i]->size() > payload_limit(lane)) return {SendStatus::Invalid, 0};
            input[i] = {messages[i]->data(), static_cast<uint32_t>(messages[i]->size())};
        }
        uint32_t accepted = 0;
        const auto result = api_.send_batch(instance_.get(), peer, lane_id(lane), input,
                                            static_cast<uint32_t>(count), &accepted);
        if (result == NC_TRANSPORT_OK && accepted == count) return {SendStatus::Accepted, accepted};
        if (result == NC_TRANSPORT_WOULD_BLOCK && accepted < count) return {SendStatus::Blocked, accepted};
        if (accepted) throw std::runtime_error("Transport " + id_ + " returned an invalid accepted prefix");
        switch (result) {
        case NC_TRANSPORT_PEER_CLOSED: return {SendStatus::Closed, 0};
        case NC_TRANSPORT_INVALID_ARGUMENT:
        case NC_TRANSPORT_TOO_LARGE: return {SendStatus::Invalid, 0};
        default: return {SendStatus::Failed, 0};
        }
    }

    bool supports_backpressure() const override { return true; }
    bool reliable_ordered(Lane lane) const override {
        return (caps_.lane[lane_id(lane)].flags & NC_TRANSPORT_RELIABLE_ORDERED) != 0;
    }

    void close(Peer peer) override { api_.close_peer(instance_.get(), peer); }

    Traffic traffic() const override {
        nc_transport_stats stats{};
        if (api_.get_stats(instance_.get(), NC_TRANSPORT_ALL_PEERS, &stats) != NC_TRANSPORT_OK) return {};
        Traffic out;
        const auto& control = stats.lane[NC_TRANSPORT_CONTROL];
        const auto& game = stats.lane[NC_TRANSPORT_GAME];
        out.tcp_sent = control.sent_bytes;
        out.tcp_received = control.received_bytes;
        if (caps_.lane[NC_TRANSPORT_GAME].flags & NC_TRANSPORT_RELIABLE_ORDERED) {
            out.tcp_sent += game.sent_bytes;
            out.tcp_received += game.received_bytes;
        } else {
            out.udp_sent = game.sent_bytes;
            out.udp_received = game.received_bytes;
        }
        out.queued_bytes = control.queued_bytes + game.queued_bytes;
        out.peak_queued_bytes = control.peak_queued_bytes + game.peak_queued_bytes;
        out.oldest_queued_us = std::max(control.oldest_queued_us, game.oldest_queued_us);
        return out;
    }

private:
    struct Receive {
        std::vector<Event> events;
        std::exception_ptr error;
    };

    static void NC_TRANSPORT_CALL collect(void* context, const nc_transport_event* event) noexcept {
        auto& state = *static_cast<Receive*>(context);
        if (state.error) return;
        try {
            if (!event || !event->peer || event->lane >= NC_TRANSPORT_LANE_COUNT ||
                state.events.size() >= 4096)
                throw std::runtime_error("Invalid transport event header");
            Event out{EventType::Open, event->peer, event->lane == NC_TRANSPORT_CONTROL ? Lane::Control : Lane::Game};
            switch (event->type) {
            case NC_TRANSPORT_OPEN: break;
            case NC_TRANSPORT_CLOSED:
                out.type = EventType::Closed;
                out.reason = disconnect(event->reason);
                break;
            case NC_TRANSPORT_MESSAGE:
                out.type = EventType::Message;
                if (event->payload.size > payload_limit(out.lane) || (event->payload.size && !event->payload.data))
                    throw std::runtime_error("Invalid transport event payload");
                if (event->payload.size)
                    out.payload.assign(event->payload.data, event->payload.data + event->payload.size);
                break;
            default: throw std::runtime_error("Unknown transport event type");
            }
            state.events.push_back(std::move(out));
        } catch (...) {
            state.error = std::current_exception();
        }
    }

    std::shared_ptr<const void> module_;  // Outlives instance_ and every function pointer.
    const nc_transport_api_v1& api_;
    std::unique_ptr<nc_transport_instance, decltype(api_.destroy)> instance_;
    nc_transport_caps caps_;
    std::string id_;
    uint64_t now_us_ = 0;
};

}

struct TransportProvider::Module {
    nc_transport_api_v1 api{};
    ProviderIdentity identity;
#ifdef _WIN32
    HMODULE library = nullptr;
    ~Module() { if (library) FreeLibrary(library); }
#else
    void* library = nullptr;
    ~Module() { if (library) dlclose(library); }
#endif

    void initialize(nc_transport_get_api_fn entry, std::string_view expected_id) {
        const std::string label = "Transport " + std::string(expected_id);
        if (!entry) throw std::runtime_error(label + ": missing " NC_TRANSPORT_ENTRY);
        const auto result = entry(NC_TRANSPORT_ABI_V1, sizeof api, &api);
        if (result != NC_TRANSPORT_OK)
            throw std::runtime_error(label + ": ABI v1 negotiation failed (" + result_name(result) + ")");
        if (api.abi_version != NC_TRANSPORT_ABI_V1)
            throw std::runtime_error(label + ": ABI version mismatch");
        if (api.struct_size != sizeof api || !api.open || !api.send_batch || !api.poll || !api.close_peer ||
            !api.destroy || !api.get_caps || !api.get_stats)
            throw std::runtime_error(label + ": incomplete ABI v1 function table");
        identity = {identity_string(api.provider_id, "provider ID"), identity_string(api.build_id, "build ID"), api.abi_version};
        if (!expected_id.empty() && identity.id != expected_id)
            throw std::runtime_error(label + ": DLL identifies itself as " + identity.id);
    }
};

TransportProvider::TransportProvider(std::shared_ptr<const Module> module) : module_(std::move(module)) {}

TransportProvider TransportProvider::load(const std::filesystem::path& path, std::string_view expected_id) {
    if (path.empty()) throw std::invalid_argument("No DLL path for transport " + std::string(expected_id));
    const auto absolute = std::filesystem::absolute(path);
    auto module = std::make_shared<Module>();
#ifdef _WIN32
    module->library = LoadLibraryExW(absolute.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module->library)
        throw std::runtime_error("Cannot load transport " + std::string(expected_id) + " at " +
                                 absolute.u8string() + " (Windows error " + std::to_string(GetLastError()) + ")");
    const auto entry = reinterpret_cast<nc_transport_get_api_fn>(GetProcAddress(module->library, NC_TRANSPORT_ENTRY));
#else
    module->library = dlopen(absolute.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module->library) throw std::runtime_error("Cannot load transport " + std::string(expected_id) + ": " + dlerror());
    const auto entry = reinterpret_cast<nc_transport_get_api_fn>(dlsym(module->library, NC_TRANSPORT_ENTRY));
#endif
    module->initialize(entry, expected_id);
    return TransportProvider(std::move(module));
}

TransportProvider TransportProvider::registered(nc_transport_get_api_fn entry, std::string_view expected_id) {
    auto module = std::make_shared<Module>();
    module->initialize(entry, expected_id);
    return TransportProvider(std::move(module));
}

const ProviderIdentity& TransportProvider::identity() const { return module_->identity; }

std::unique_ptr<Transport> TransportProvider::open(bool listen, std::string_view endpoint, std::string_view config) const {
    nc_transport_open_args args{uint32_t(listen ? NC_TRANSPORT_LISTEN : NC_TRANSPORT_CONNECT), span(endpoint), span(config)};
    const auto& api = module_->api;
    nc_transport_instance* instance = nullptr;
    const auto result = api.open(&args, &instance);
    std::unique_ptr<nc_transport_instance, decltype(api.destroy)> guard(instance, api.destroy);
    if (result != NC_TRANSPORT_OK || !instance)
        throw std::runtime_error("Transport " + identity().id + " open failed (" + result_name(result) + ")");
    nc_transport_caps caps{};
    if (api.get_caps(instance, &caps) != NC_TRANSPORT_OK ||
        caps.lane[NC_TRANSPORT_CONTROL].max_payload < control_payload_limit ||
        !(caps.lane[NC_TRANSPORT_CONTROL].flags & NC_TRANSPORT_RELIABLE_ORDERED) ||
        caps.lane[NC_TRANSPORT_GAME].max_payload < game_payload_limit)
        throw std::runtime_error("Transport " + identity().id + " does not support the required channels");
    auto transport = std::make_unique<PluginTransport>(module_, api, instance, caps, identity().id);
    guard.release();
    return transport;
}

}
