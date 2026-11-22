#include <netcode/transport_provider.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr DWORD pipe_mode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
constexpr DWORD pipe_buffer = 128 * 1024;
constexpr size_t max_frames = 16;
constexpr size_t max_peers = 8;
constexpr uint64_t drain_limit = 1'000'000;
constexpr uint8_t end_of_messages = 0xff;
constexpr uint32_t payload_limits[] = {NC_TRANSPORT_CONTROL_MAX, NC_TRANSPORT_GAME_MAX};

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        std::swap(value, other.value);
        return *this;
    }
    ~Handle() { if (*this) CloseHandle(value); }
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};

struct Outgoing {
    std::vector<uint8_t> message;
    uint64_t enqueued;
};

struct Peer {
    Peer(nc_transport_peer id, Handle&& pipe, uint32_t failure = 0)
        : id(id), pipe(std::move(pipe)), reason(failure) {}
    nc_transport_peer id;
    Handle pipe;
    uint32_t reason;
    bool opened = false;
    bool sent_end = false;
    std::optional<uint64_t> closing;
    std::list<Outgoing> queue;
    nc_transport_stats stats{};
};

uint32_t read_error(DWORD error) {
    if (error == ERROR_MORE_DATA) return NC_TRANSPORT_CLOSE_PROTOCOL;
    if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) return NC_TRANSPORT_CLOSE_REMOTE;
    return NC_TRANSPORT_CLOSE_IO_ERROR;
}

std::string name_for(nc_transport_bytes endpoint) {
    if (!endpoint.data || !endpoint.size || endpoint.size > 96) return {};
    std::string name = R"(\\.\pipe\netcode-example-)";
    for (uint32_t i = 0; i < endpoint.size; ++i) {
        const char ch = static_cast<char>(endpoint.data[i]);
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == ':' || ch == '.' || ch == '-' || ch == '_')
            name += ch;
        else
            return {};
    }
    return name;
}
}

struct nc_transport_instance {
    std::string name;
    bool server = false;
    Handle listener;
    std::list<Peer> peers;
    nc_transport_peer sequence = 1;
    uint64_t now = 0;
    nc_transport_stats stats{};

    auto find(nc_transport_peer id) {
        return std::find_if(peers.begin(), peers.end(), [id](const Peer& peer) { return peer.id == id; });
    }

    Handle make_listener(bool first) const {
        return Handle(CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                      PIPE_TYPE_MESSAGE | pipe_mode | PIPE_REJECT_REMOTE_CLIENTS,
                      static_cast<DWORD>(max_peers + 1), pipe_buffer, pipe_buffer, 0, nullptr));
    }

    void accept() {
        if (peers.size() == max_peers) return;
        if (!listener) listener = make_listener(false);
        if (!listener) return;
        if (ConnectNamedPipe(listener.value, nullptr)) return;
        const auto error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            peers.emplace_back(sequence++, std::move(listener));
            if (peers.size() < max_peers) listener = make_listener(false);
        } else if (error != ERROR_PIPE_LISTENING) {
            listener = make_listener(false);
        }
    }

    void write(Peer& peer) {
        for (size_t i = 0; i < max_frames && !peer.queue.empty(); ++i) {
            const auto& message = peer.queue.front().message;
            DWORD amount = 0;
            if (!WriteFile(peer.pipe.value, message.data(), static_cast<DWORD>(message.size()), &amount, nullptr)) {
                peer.reason = read_error(GetLastError());
                return;
            }
            if (!amount) return;
            const auto lane = message.front();
            for (auto* counts : {&stats.lane[lane], &peer.stats.lane[lane]}) {
                counts->sent_bytes += message.size() - 1;
                counts->queued_bytes -= message.size() - 1;
            }
            peer.queue.pop_front();
        }
        if (peer.closing && !peer.sent_end && peer.queue.empty()) {
            DWORD amount = 0;
            if (!WriteFile(peer.pipe.value, &end_of_messages, 1, &amount, nullptr))
                peer.reason = NC_TRANSPORT_CLOSE_LOCAL;
            peer.sent_end = amount == 1;
        }
        if (peer.closing && now - *peer.closing >= drain_limit) peer.reason = NC_TRANSPORT_CLOSE_LOCAL;
    }

    void read(Peer& peer, uint32_t& budget, nc_transport_receive receive, void* context,
              std::array<uint8_t, NC_TRANSPORT_CONTROL_MAX + 1>& incoming) {
        for (size_t i = 0; i < max_frames && budget; ++i) {
            DWORD amount = 0;
            if (!ReadFile(peer.pipe.value, incoming.data(), static_cast<DWORD>(incoming.size()), &amount, nullptr)) {
                const auto error = GetLastError();
                if (error != ERROR_NO_DATA) peer.reason = peer.closing ? NC_TRANSPORT_CLOSE_LOCAL : read_error(error);
                return;
            }
            const uint8_t lane = incoming[0];
            if (amount == 1 && lane == end_of_messages) {
                peer.reason = peer.closing ? NC_TRANSPORT_CLOSE_LOCAL : NC_TRANSPORT_CLOSE_REMOTE;
                return;
            }
            if (!amount || lane >= NC_TRANSPORT_LANE_COUNT || amount - 1 > payload_limits[lane]) {
                peer.reason = NC_TRANSPORT_CLOSE_PROTOCOL;
                return;
            }
            stats.lane[lane].received_bytes += amount - 1;
            peer.stats.lane[lane].received_bytes += amount - 1;
            const nc_transport_event event{NC_TRANSPORT_MESSAGE, lane, peer.id, {incoming.data() + 1, amount - 1}, 0};
            --budget;
            receive(context, &event);
        }
    }
};

namespace {
nc_transport_result NC_TRANSPORT_CALL open(const nc_transport_open_args* args, nc_transport_instance** out) {
    if (!out) return NC_TRANSPORT_INVALID_ARGUMENT;
    *out = nullptr;
    if (!args || args->config.size || (args->role != NC_TRANSPORT_LISTEN && args->role != NC_TRANSPORT_CONNECT))
        return NC_TRANSPORT_INVALID_CONFIG;
    try {
        auto instance = std::make_unique<nc_transport_instance>();
        instance->name = name_for(args->endpoint);
        if (instance->name.empty()) return NC_TRANSPORT_INVALID_CONFIG;
        instance->server = args->role == NC_TRANSPORT_LISTEN;
        if (instance->server) {
            instance->listener = instance->make_listener(true);
            if (!instance->listener) return NC_TRANSPORT_IO_ERROR;
        } else {
            Handle pipe(CreateFileA(instance->name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   0, nullptr, OPEN_EXISTING, 0, nullptr));
            DWORD mode = pipe_mode;
            const auto failure = !pipe ? NC_TRANSPORT_CLOSE_REFUSED :
                (SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr) ? 0 : NC_TRANSPORT_CLOSE_IO_ERROR);
            instance->peers.emplace_back(instance->sequence++, std::move(pipe), failure);
        }
        *out = instance.release();
        return NC_TRANSPORT_OK;
    } catch (...) {
        return NC_TRANSPORT_IO_ERROR;
    }
}

nc_transport_result NC_TRANSPORT_CALL send_batch(nc_transport_instance* instance, nc_transport_peer id, uint32_t lane,
        const nc_transport_bytes* messages, uint32_t count, uint32_t* accepted) {
    if (!accepted) return NC_TRANSPORT_INVALID_ARGUMENT;
    *accepted = 0;
    if (!instance || lane >= NC_TRANSPORT_LANE_COUNT || (count && !messages)) return NC_TRANSPORT_INVALID_ARGUMENT;
    auto peer = instance->find(id);
    if (peer == instance->peers.end() || !peer->opened || peer->closing || peer->reason) return NC_TRANSPORT_PEER_CLOSED;
    for (uint32_t i = 0; i < count; ++i) {
        if (messages[i].size > payload_limits[lane]) return NC_TRANSPORT_TOO_LARGE;
        if (messages[i].size && !messages[i].data) return NC_TRANSPORT_INVALID_ARGUMENT;
    }
    try {
        std::list<Outgoing> staged;
        const auto prefix = static_cast<uint32_t>(std::min<size_t>(count, max_frames - peer->queue.size()));
        size_t bytes = 0;
        for (uint32_t i = 0; i < prefix; ++i) {
            const auto size = messages[i].size;
            std::vector<uint8_t> wire(size + 1);
            wire[0] = static_cast<uint8_t>(lane);
            if (size) std::memcpy(wire.data() + 1, messages[i].data, size);
            staged.push_back({std::move(wire), instance->now});
            bytes += size;
        }
        peer->queue.splice(peer->queue.end(), staged);
        for (auto* stats : {&instance->stats.lane[lane], &peer->stats.lane[lane]}) {
            stats->queued_bytes += bytes;
            stats->peak_queued_bytes = std::max(stats->peak_queued_bytes, stats->queued_bytes);
        }
        *accepted = prefix;
        return prefix == count ? NC_TRANSPORT_OK : NC_TRANSPORT_WOULD_BLOCK;
    } catch (...) {
        return NC_TRANSPORT_IO_ERROR;
    }
}

nc_transport_result NC_TRANSPORT_CALL poll(nc_transport_instance* instance, uint64_t now, uint32_t budget,
                                           nc_transport_receive receive, void* context) {
    if (!instance || !receive || now < instance->now) return NC_TRANSPORT_INVALID_ARGUMENT;
    instance->now = now;
    if (!budget) return NC_TRANSPORT_OK;
    try {
        if (instance->server) instance->accept();
        std::array<uint8_t, NC_TRANSPORT_CONTROL_MAX + 1> input;
        for (auto remaining = instance->peers.size(); remaining && budget; --remaining) {
            auto peer = instance->peers.begin();
            if (!peer->opened && !peer->reason) {
                peer->opened = true;
                const nc_transport_event event{NC_TRANSPORT_OPEN, NC_TRANSPORT_CONTROL, peer->id, {}, 0};
                --budget;
                receive(context, &event);
            }
            if (budget && !peer->reason) instance->read(*peer, budget, receive, context, input);
            if (!peer->reason) instance->write(*peer);
            if (budget && peer->reason) {
                const nc_transport_event event{NC_TRANSPORT_CLOSED, NC_TRANSPORT_CONTROL, peer->id, {}, peer->reason};
                --budget;
                receive(context, &event);
                for (size_t lane = 0; lane < NC_TRANSPORT_LANE_COUNT; ++lane)
                    instance->stats.lane[lane].queued_bytes -= peer->stats.lane[lane].queued_bytes;
                instance->peers.erase(peer);
            } else {
                instance->peers.splice(instance->peers.end(), instance->peers, peer);
            }
        }
        return NC_TRANSPORT_OK;
    } catch (...) {
        return NC_TRANSPORT_IO_ERROR;
    }
}

nc_transport_result NC_TRANSPORT_CALL close_peer(nc_transport_instance* instance, nc_transport_peer id) {
    if (!instance) return NC_TRANSPORT_INVALID_ARGUMENT;
    auto peer = instance->find(id);
    if (peer == instance->peers.end() || peer->closing || peer->reason) return NC_TRANSPORT_PEER_CLOSED;
    peer->closing = instance->now;
    return NC_TRANSPORT_OK;
}

void NC_TRANSPORT_CALL destroy(nc_transport_instance* instance) { delete instance; }

nc_transport_result NC_TRANSPORT_CALL get_caps(nc_transport_instance* instance, nc_transport_caps* caps) {
    if (!instance || !caps) return NC_TRANSPORT_INVALID_ARGUMENT;
    for (size_t lane = 0; lane < NC_TRANSPORT_LANE_COUNT; ++lane)
        caps->lane[lane] = {payload_limits[lane], NC_TRANSPORT_RELIABLE_ORDERED};
    return NC_TRANSPORT_OK;
}

nc_transport_result NC_TRANSPORT_CALL get_stats(nc_transport_instance* instance, nc_transport_peer id,
                                                nc_transport_stats* out) {
    if (!instance || !out) return NC_TRANSPORT_INVALID_ARGUMENT;
    auto peer = instance->find(id);
    if (id && peer == instance->peers.end()) return NC_TRANSPORT_PEER_CLOSED;
    *out = id ? peer->stats : instance->stats;
    for (const auto& entry : instance->peers) {
        if (id && entry.id != id) continue;
        for (const auto& message : entry.queue) {
            auto& oldest = out->lane[message.message.front()].oldest_queued_us;
            oldest = std::max(oldest, instance->now - message.enqueued);
        }
    }
    return NC_TRANSPORT_OK;
}
}

nc_transport_result NC_TRANSPORT_CALL netcode_transport_get_api(uint32_t version, uint32_t capacity, void* out) {
    static const uint8_t provider[] = "example-pipe";
    static const uint8_t build[] = "win-pipe-message-v1";
    static const nc_transport_api_v1 api{
        NC_TRANSPORT_ABI_V1, sizeof api, {provider, sizeof provider - 1}, {build, sizeof build - 1},
        open, send_batch, poll, close_peer, destroy, get_caps, get_stats
    };
    if (version != NC_TRANSPORT_ABI_V1) return NC_TRANSPORT_UNSUPPORTED_ABI;
    if (!out) return NC_TRANSPORT_INVALID_ARGUMENT;
    if (capacity < sizeof api) return NC_TRANSPORT_SHORT_TABLE;
    std::memcpy(out, &api, sizeof api);
    return NC_TRANSPORT_OK;
}
