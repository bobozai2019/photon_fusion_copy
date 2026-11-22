#include <netcode/transport_provider.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
constexpr size_t header_size = 3;
constexpr size_t io_quantum = 32 * 1024;
constexpr size_t max_peers = 64;
constexpr size_t max_queued_frames = 4096;
constexpr uint32_t default_queue = 512 * 1024;
constexpr uint32_t max_queue = 16 * 1024 * 1024;
constexpr uint64_t default_timeout_us = 3'000'000;
constexpr uint32_t lane_max[] = {NC_TRANSPORT_CONTROL_MAX, NC_TRANSPORT_GAME_MAX};

#ifdef _WIN32
using SocketId = SOCKET;
using AddressSize = int;
constexpr SocketId bad_socket = INVALID_SOCKET;
constexpr int refused = WSAECONNREFUSED;
constexpr int timed_out = WSAETIMEDOUT;
int socket_error() { return WSAGetLastError(); }
bool would_block(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
bool interrupted(int error) { return error == WSAEINTR; }
void release_socket(SocketId fd) { closesocket(fd); }
constexpr int send_flags = 0;
#else
using SocketId = int;
using AddressSize = socklen_t;
constexpr SocketId bad_socket = -1;
constexpr int refused = ECONNREFUSED;
constexpr int timed_out = ETIMEDOUT;
int socket_error() { return errno; }
bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS; }
bool interrupted(int error) { return error == EINTR; }
void release_socket(SocketId fd) { ::close(fd); }
#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif
#endif

struct Network {
    Network() {
#ifdef _WIN32
        WSADATA data{};
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
    }
    ~Network() {
#ifdef _WIN32
        if (ready) WSACleanup();
#endif
    }
    bool ready = true;
};

struct Socket {
    SocketId fd = bad_socket;
    explicit Socket(SocketId value = bad_socket) : fd(value) {}
    Socket(Socket &&other) noexcept : fd(std::exchange(other.fd, bad_socket)) {}
    Socket &operator=(Socket &&other) noexcept {
        reset();
        fd = std::exchange(other.fd, bad_socket);
        return *this;
    }
    ~Socket() { reset(); }
    explicit operator bool() const { return fd != bad_socket; }
    void reset() {
        if (*this) release_socket(std::exchange(fd, bad_socket));
    }
};

bool nonblocking(SocketId fd) {
#ifdef _WIN32
    u_long enable = 1;
    return ioctlsocket(fd, FIONBIO, &enable) == 0;
#else
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void tune_stream(SocketId fd) {
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char *>(&one), sizeof one);
#endif
}

struct Address {
    sockaddr_storage storage{};
    AddressSize size = 0;
    const sockaddr *get() const { return reinterpret_cast<const sockaddr *>(&storage); }
};

bool valid(nc_transport_bytes bytes) { return !bytes.size || bytes.data; }
std::string_view view(nc_transport_bytes bytes) {
    return bytes.size ? std::string_view(reinterpret_cast<const char *>(bytes.data), bytes.size) : std::string_view{};
}

bool parse_unsigned(std::string_view value, uint32_t &out) {
    if (value.empty()) return false;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), out);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

bool parse_endpoint(nc_transport_bytes endpoint, Address &out) {
    if (!valid(endpoint) || !endpoint.size || endpoint.size > 128) return false;
    const auto text = view(endpoint);
    const auto colon = text.rfind(':');
    if (!colon || colon == text.npos || text.find('\0') != text.npos) return false;
    auto host = text.substr(0, colon);
    if (host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    } else if (host.find(':') != host.npos) {
        return false;
    }
    const auto port = text.substr(colon + 1);
    uint32_t number = 0;
    if (host.empty() || !parse_unsigned(port, number) || number == 0 || number > 65535) return false;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo *result = nullptr;
    if (getaddrinfo(std::string(host).c_str(), std::string(port).c_str(), &hints, &result) != 0) return false;
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(result, freeaddrinfo);
    if (!result || result->ai_addrlen > sizeof out.storage) return false;
    out.size = static_cast<AddressSize>(result->ai_addrlen);
    std::memcpy(&out.storage, result->ai_addr, out.size);
    return true;
}

bool parse_config(nc_transport_bytes config, uint32_t &queue) {
    if (!valid(config) || config.size > 32) return false;
    if (!config.size) return true;
    constexpr std::string_view prefix = "queue_bytes=";
    const auto text = view(config);
    return text.substr(0, prefix.size()) == prefix && parse_unsigned(text.substr(prefix.size()), queue) &&
           queue >= NC_TRANSPORT_CONTROL_MAX + header_size && queue <= max_queue;
}

// A writable or exceptional connecting socket has a final result in SO_ERROR.
int connection_ready(SocketId fd) {
#ifdef _WIN32
    fd_set write, error;
    FD_ZERO(&write);
    FD_ZERO(&error);
    FD_SET(fd, &write);
    FD_SET(fd, &error);
    timeval immediate{};
    return select(0, nullptr, &write, &error, &immediate);
#else
    pollfd pending{fd, POLLOUT, 0};
    return ::poll(&pending, 1, 0);
#endif
}

uint32_t close_reason(int error) {
    if (error == refused) return NC_TRANSPORT_CLOSE_REFUSED;
    if (error == timed_out) return NC_TRANSPORT_CLOSE_TIMEOUT;
    return NC_TRANSPORT_CLOSE_IO_ERROR;
}

struct Frame {
    uint32_t lane;
    uint64_t queued_at;
    std::vector<uint8_t> wire;
    size_t offset = 0;
};

struct Peer {
    Peer(nc_transport_peer peer_id, Socket stream) : id(peer_id), socket(std::move(stream)) {}
    nc_transport_peer id;
    Socket socket;
    bool connecting = false;
    bool opened = false;
    uint32_t ended = NC_TRANSPORT_CLOSE_NONE;
    std::optional<uint64_t> connecting_since;
    std::optional<uint64_t> draining_since;
    std::list<Frame> send_queue;
    size_t queue_size = 0;
    std::vector<uint8_t> input;
    size_t read_offset = 0;
    nc_transport_stats stats{};
};

struct Sink {
    uint32_t left;
    nc_transport_receive receive;
    void *context;
    void emit(nc_transport_event event) {
        --left;
        receive(context, &event);
    }
};
}

struct nc_transport_instance {
    Network network;
    Socket listener;
    std::list<Peer> peers;
    nc_transport_peer next_peer = 1;
    uint32_t queue_limit = default_queue;
    uint64_t now = 0;
    nc_transport_stats stats{};

    auto find(nc_transport_peer id) {
        return std::find_if(peers.begin(), peers.end(), [id](const Peer &peer) { return peer.id == id; });
    }

    void finish(Peer &peer, uint32_t reason) {
        if (peer.ended) return;
        peer.ended = reason;
        peer.socket.reset();
        peer.send_queue.clear();
        peer.queue_size = 0;
        for (uint32_t lane = 0; lane < NC_TRANSPORT_LANE_COUNT; ++lane) {
            stats.lane[lane].queued_bytes -= peer.stats.lane[lane].queued_bytes;
            peer.stats.lane[lane].queued_bytes = 0;
        }
    }

    void accept_peers() {
        for (unsigned i = 0; i < 4 && peers.size() < max_peers; ++i) {
            Socket socket(accept(listener.fd, nullptr, nullptr));
            if (!socket) return;
            if (!nonblocking(socket.fd)) continue;
            tune_stream(socket.fd);
            peers.emplace_back(next_peer++, std::move(socket));
        }
    }

    void connect_step(Peer &peer) {
        if (!peer.connecting_since) peer.connecting_since = now;
        if (now - *peer.connecting_since >= default_timeout_us) {
            finish(peer, NC_TRANSPORT_CLOSE_TIMEOUT);
            return;
        }
        const int ready = connection_ready(peer.socket.fd);
        if (!ready) return;
        if (ready < 0) {
            const int error = socket_error();
            if (!interrupted(error)) finish(peer, close_reason(error));
            return;
        }
        int error = 0;
        AddressSize size = sizeof error;
        if (getsockopt(peer.socket.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size) != 0)
            error = socket_error();
        if (error) finish(peer, close_reason(error));
        else peer.connecting = false;
    }

    void write_frames(Peer &peer) {
        size_t budget = io_quantum;
        while (budget && !peer.send_queue.empty()) {
            auto &frame = peer.send_queue.front();
            const int amount = static_cast<int>(std::min(budget, frame.wire.size() - frame.offset));
            const auto sent = ::send(peer.socket.fd, reinterpret_cast<const char *>(frame.wire.data() + frame.offset),
                                     amount, send_flags);
            if (sent <= 0) {
                const int error = sent < 0 ? socket_error() : 0;
                if (sent == 0 || (!would_block(error) && !interrupted(error))) finish(peer, NC_TRANSPORT_CLOSE_IO_ERROR);
                return;
            }
            frame.offset += sent;
            budget -= sent;
            if (frame.offset != frame.wire.size()) continue;
            const size_t payload_size = frame.wire.size() - header_size;
            auto &total = stats.lane[frame.lane];
            auto &local = peer.stats.lane[frame.lane];
            total.sent_bytes += payload_size;
            local.sent_bytes += payload_size;
            total.queued_bytes -= payload_size;
            local.queued_bytes -= payload_size;
            peer.queue_size -= frame.wire.size();
            peer.send_queue.pop_front();
        }
    }

    void deliver_frames(Peer &peer, Sink &sink) {
        while (sink.left && peer.input.size() - peer.read_offset >= header_size) {
            const auto *frame = peer.input.data() + peer.read_offset;
            const uint32_t lane = frame[0];
            const uint32_t size = (uint32_t(frame[1]) << 8) | frame[2];
            if (lane >= NC_TRANSPORT_LANE_COUNT || size > lane_max[lane]) {
                finish(peer, NC_TRANSPORT_CLOSE_PROTOCOL);
                return;
            }
            if (peer.input.size() - peer.read_offset < header_size + size) return;
            peer.read_offset += header_size + size;
            stats.lane[lane].received_bytes += size;
            peer.stats.lane[lane].received_bytes += size;
            sink.emit({NC_TRANSPORT_MESSAGE, lane, peer.id, {frame + header_size, size}, NC_TRANSPORT_CLOSE_NONE});
        }
    }

    void read_frames(Peer &peer, Sink &sink, std::array<uint8_t, io_quantum> &scratch) {
        deliver_frames(peer, sink);
        if (peer.read_offset) {
            peer.input.erase(peer.input.begin(), peer.input.begin() + peer.read_offset);
            peer.read_offset = 0;
        }
        if (!sink.left || peer.ended) return;
        const auto size = recv(peer.socket.fd, reinterpret_cast<char *>(scratch.data()), static_cast<int>(scratch.size()), 0);
        if (size > 0) {
            peer.input.insert(peer.input.end(), scratch.data(), scratch.data() + size);
            deliver_frames(peer, sink);
        } else if (size == 0) {
            finish(peer, peer.input.empty() ? NC_TRANSPORT_CLOSE_REMOTE : NC_TRANSPORT_CLOSE_PROTOCOL);
        } else {
            const int error = socket_error();
            if (!would_block(error) && !interrupted(error)) finish(peer, NC_TRANSPORT_CLOSE_IO_ERROR);
        }
    }

    void step(Peer &peer, Sink &sink, std::array<uint8_t, io_quantum> &scratch) {
        if (peer.connecting && !peer.ended) connect_step(peer);
        if (peer.ended || peer.connecting) return;
        if (!peer.opened) {
            peer.opened = true;
            sink.emit({NC_TRANSPORT_OPEN, NC_TRANSPORT_CONTROL, peer.id, {}, NC_TRANSPORT_CLOSE_NONE});
        }
        if (!sink.left) return;
        // Deliver already-buffered bytes before a write can observe a socket error.
        if (!peer.draining_since) read_frames(peer, sink, scratch);
        if (!peer.ended) write_frames(peer);
        if (!peer.ended && peer.draining_since &&
            (peer.send_queue.empty() || now - *peer.draining_since >= default_timeout_us))
            finish(peer, NC_TRANSPORT_CLOSE_LOCAL);
    }
};

namespace {
nc_transport_result NC_TRANSPORT_CALL open_tcp(const nc_transport_open_args *args, nc_transport_instance **out) {
    if (!out) return NC_TRANSPORT_INVALID_ARGUMENT;
    *out = nullptr;
    if (!args || (args->role != NC_TRANSPORT_LISTEN && args->role != NC_TRANSPORT_CONNECT))
        return NC_TRANSPORT_INVALID_CONFIG;
    try {
        auto instance = std::make_unique<nc_transport_instance>();
        if (!instance->network.ready) return NC_TRANSPORT_IO_ERROR;
        Address address;
        if (!parse_endpoint(args->endpoint, address) || !parse_config(args->config, instance->queue_limit))
            return NC_TRANSPORT_INVALID_CONFIG;
        Socket socket(::socket(address.get()->sa_family, SOCK_STREAM, IPPROTO_TCP));
        if (!socket || !nonblocking(socket.fd)) return NC_TRANSPORT_IO_ERROR;
        if (args->role == NC_TRANSPORT_LISTEN) {
            const int one = 1;
#ifdef _WIN32
            constexpr auto bind_option = SO_EXCLUSIVEADDRUSE;
#else
            constexpr auto bind_option = SO_REUSEADDR;
#endif
            if (setsockopt(socket.fd, SOL_SOCKET, bind_option, reinterpret_cast<const char *>(&one), sizeof one) != 0 ||
                bind(socket.fd, address.get(), address.size) != 0 || listen(socket.fd, SOMAXCONN) != 0)
                return NC_TRANSPORT_IO_ERROR;
            instance->listener = std::move(socket);
        } else {
            auto &peer = instance->peers.emplace_back(instance->next_peer++, std::move(socket));
            tune_stream(peer.socket.fd);
            if (connect(peer.socket.fd, address.get(), address.size) != 0) {
                const int error = socket_error();
                if (would_block(error) || interrupted(error)) peer.connecting = true;
                else instance->finish(peer, close_reason(error));
            }
        }
        *out = instance.release();
        return NC_TRANSPORT_OK;
    } catch (...) {
        return NC_TRANSPORT_IO_ERROR;
    }
}

nc_transport_result NC_TRANSPORT_CALL send_tcp(nc_transport_instance *instance, nc_transport_peer id, uint32_t lane,
        const nc_transport_bytes *messages, uint32_t count, uint32_t *accepted) {
    if (!accepted) return NC_TRANSPORT_INVALID_ARGUMENT;
    *accepted = 0;
    if (!instance || lane >= NC_TRANSPORT_LANE_COUNT || (count && !messages)) return NC_TRANSPORT_INVALID_ARGUMENT;
    auto peer = instance->find(id);
    if (peer == instance->peers.end() || !peer->opened || peer->ended || peer->draining_since)
        return NC_TRANSPORT_PEER_CLOSED;
    for (uint32_t i = 0; i < count; ++i) {
        if (messages[i].size > lane_max[lane]) return NC_TRANSPORT_TOO_LARGE;
        if (!valid(messages[i])) return NC_TRANSPORT_INVALID_ARGUMENT;
    }
    try {
        std::list<Frame> staged;
        size_t available = instance->queue_limit - peer->queue_size;
        const size_t frame_slots = max_queued_frames - peer->send_queue.size();
        size_t payload_size = 0, wire_size = 0;
        uint32_t i = 0;
        for (; i < count && i < frame_slots; ++i) {
            const auto message = messages[i];
            const size_t size = header_size + message.size;
            if (size > available) break;
            Frame frame{lane, instance->now, std::vector<uint8_t>(size)};
            frame.wire[0] = static_cast<uint8_t>(lane);
            frame.wire[1] = static_cast<uint8_t>(message.size >> 8);
            frame.wire[2] = static_cast<uint8_t>(message.size);
            if (message.size) std::memcpy(frame.wire.data() + header_size, message.data, message.size);
            staged.push_back(std::move(frame));
            available -= size;
            wire_size += size;
            payload_size += message.size;
        }
        peer->send_queue.splice(peer->send_queue.end(), staged);
        peer->queue_size += wire_size;
        for (auto *stats : {&instance->stats.lane[lane], &peer->stats.lane[lane]}) {
            stats->queued_bytes += payload_size;
            stats->peak_queued_bytes = std::max(stats->peak_queued_bytes, stats->queued_bytes);
        }
        *accepted = i;
        return i == count ? NC_TRANSPORT_OK : NC_TRANSPORT_WOULD_BLOCK;
    } catch (...) {
        return NC_TRANSPORT_IO_ERROR;
    }
}

nc_transport_result NC_TRANSPORT_CALL poll_tcp(nc_transport_instance *instance, uint64_t now, uint32_t budget,
                                                  nc_transport_receive receive, void *context) {
    if (!instance || !receive || now < instance->now) return NC_TRANSPORT_INVALID_ARGUMENT;
    instance->now = now;
    if (!budget) return NC_TRANSPORT_OK;
    try {
        if (instance->listener) instance->accept_peers();
        Sink sink{budget, receive, context};
        std::array<uint8_t, io_quantum> scratch;
        // Move visited peers to the tail so even a one-event budget progresses every connection.
        for (auto remaining = instance->peers.size(); remaining && sink.left; --remaining) {
            auto peer = instance->peers.begin();
            instance->step(*peer, sink, scratch);
            if (peer->ended && sink.left) {
                sink.emit({NC_TRANSPORT_CLOSED, NC_TRANSPORT_CONTROL, peer->id, {}, peer->ended});
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

nc_transport_result NC_TRANSPORT_CALL close_tcp(nc_transport_instance *instance, nc_transport_peer id) {
    if (!instance) return NC_TRANSPORT_INVALID_ARGUMENT;
    auto peer = instance->find(id);
    if (peer == instance->peers.end() || peer->draining_since || peer->ended) return NC_TRANSPORT_PEER_CLOSED;
    if (peer->send_queue.empty()) instance->finish(*peer, NC_TRANSPORT_CLOSE_LOCAL);
    else peer->draining_since = instance->now;
    return NC_TRANSPORT_OK;
}

void NC_TRANSPORT_CALL destroy_tcp(nc_transport_instance *instance) { delete instance; }

nc_transport_result NC_TRANSPORT_CALL caps_tcp(nc_transport_instance *instance, nc_transport_caps *out) {
    if (!instance || !out) return NC_TRANSPORT_INVALID_ARGUMENT;
    for (uint32_t lane = 0; lane < NC_TRANSPORT_LANE_COUNT; ++lane)
        out->lane[lane] = {lane_max[lane], NC_TRANSPORT_RELIABLE_ORDERED};
    return NC_TRANSPORT_OK;
}

nc_transport_result NC_TRANSPORT_CALL stats_tcp(nc_transport_instance *instance, nc_transport_peer id,
                                                nc_transport_stats *out) {
    if (!instance || !out) return NC_TRANSPORT_INVALID_ARGUMENT;
    auto peer = instance->find(id);
    if (id && peer == instance->peers.end()) return NC_TRANSPORT_PEER_CLOSED;
    *out = id ? peer->stats : instance->stats;
    for (const auto &entry : instance->peers) {
        if (id && id != entry.id) continue;
        bool found[NC_TRANSPORT_LANE_COUNT]{};
        for (const auto &frame : entry.send_queue) {
            if (found[frame.lane]) continue;
            found[frame.lane] = true;
            auto &age = out->lane[frame.lane].oldest_queued_us;
            age = std::max(age, instance->now - frame.queued_at);
            if (found[0] && found[1]) break;
        }
    }
    return NC_TRANSPORT_OK;
}
}

nc_transport_result NC_TRANSPORT_CALL netcode_transport_get_api(uint32_t abi, uint32_t capacity, void *out) {
    static const uint8_t provider[] = "tcp";
    static const uint8_t build[] = NETCODE_TCP_BUILD_ID;
    static const nc_transport_api_v1 api{
        NC_TRANSPORT_ABI_V1, sizeof api, {provider, sizeof provider - 1}, {build, sizeof build - 1},
        open_tcp, send_tcp, poll_tcp, close_tcp, destroy_tcp, caps_tcp, stats_tcp
    };
    if (abi != NC_TRANSPORT_ABI_V1) return NC_TRANSPORT_UNSUPPORTED_ABI;
    if (!out) return NC_TRANSPORT_INVALID_ARGUMENT;
    if (capacity < sizeof api) return NC_TRANSPORT_SHORT_TABLE;
    std::memcpy(out, &api, sizeof api);
    return NC_TRANSPORT_OK;
}
