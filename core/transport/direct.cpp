#include "transport/ipv4.hpp"
#ifdef _WIN32
#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

namespace netcode {
namespace {

constexpr std::uint32_t signature = 0x31434e59;
constexpr std::size_t cookie_size = 16;
constexpr std::size_t udp_header = 4 + cookie_size;
constexpr std::size_t queue_limit = 256 * 1024;
constexpr auto heartbeat = std::chrono::milliseconds(250);
enum class Frame : std::uint8_t { Heartbeat, Cookie, Message };

using namespace netcode::ipv4;

void bind_socket(SocketHandle socket, const sockaddr_in& address)
{
#ifdef _WIN32
    const BOOL exclusive = TRUE;
    if (setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) < 0) {
        socket_error("setsockopt");
    }
#endif
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        socket_error("bind");
    }
}

Bytes connection_cookie()
{
    Bytes cookie(cookie_size);
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, cookie.data(), static_cast<ULONG>(cookie.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("Cannot generate connection cookie");
    }
#else
    std::size_t offset = 0;
    while (offset < cookie.size()) {
        const auto count = getrandom(cookie.data() + offset, cookie.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("Cannot generate connection cookie");
        offset += static_cast<std::size_t>(count);
    }
#endif
    return cookie;
}

enum class ConnectState { Pending, Open, Failed };

ConnectState connect_state(SocketHandle socket)
{
#ifdef _WIN32
    fd_set ready, failed;
    FD_ZERO(&ready);
    FD_ZERO(&failed);
    FD_SET(socket, &ready);
    FD_SET(socket, &failed);
    timeval immediate{};
    if (select(0, nullptr, &ready, &failed, &immediate) < 0 || FD_ISSET(socket, &failed))
        return ConnectState::Failed;
    return FD_ISSET(socket, &ready) ? ConnectState::Open : ConnectState::Pending;
#else
    pollfd pending{socket, POLLOUT, 0};
    const int ready = ::poll(&pending, 1, 0);
    if (ready == 0 || (ready < 0 && errno == EINTR)) return ConnectState::Pending;
    if (ready < 0) return ConnectState::Failed;
    int error = 0;
    AddressSize length = sizeof(error);
    return ::getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0
        ? ConnectState::Open : ConnectState::Failed;
#endif
}

bool same_address(const sockaddr_in& a, const sockaddr_in& b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

struct Connection {
    Socket tcp;
    sockaddr_in tcp_address{};
    sockaddr_in udp_address{};
    TimePoint created = Clock::now(), received = created, next_heartbeat{};
    Bytes cookie, incoming;
    std::deque<Bytes> outgoing;
    std::size_t offset = 0, queued = 0;
    bool connecting = false, open = false, closing = false;
};

class Direct final : public Transport {
public:
    Direct(const DirectConfig& config, bool server)
        : udp_(make_socket(SOCK_DGRAM, IPPROTO_UDP)), timeout_(config.timeout), server_(server)
    {
        if (timeout_ <= heartbeat * 2) throw std::invalid_argument("Timeout must exceed 500 ms");
        const auto addr = endpoint(config);
        if (server_) {
            listener_ = make_socket(SOCK_STREAM, IPPROTO_TCP);
            bind_socket(listener_, addr);
            bind_socket(udp_, addr);
            if (::listen(listener_, SOMAXCONN) < 0) socket_error("listen");
            return;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        bind_socket(udp_, local);
        auto& link = peers_[next_peer_++];
        link.tcp = make_socket(SOCK_STREAM, IPPROTO_TCP);
        link.tcp_address = link.udp_address = addr;
        if (::connect(link.tcp, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            if (!connection_pending(last_socket_error())) socket_error("connect");
            link.connecting = true;
        }
    }

    std::vector<Event> poll(TimePoint now) override
    {
        std::vector<Event> events;
        if (server_) accept_clients(now);
        for (auto it = peers_.begin(); it != peers_.end();) {
            auto& [peer, link] = *it;
            const auto reason = step(peer, link, now, events);
            if (reason == Disconnect::None) {
                ++it;
                continue;
            }
            traffic_.queued_bytes -= link.queued;
            events.push_back({EventType::Closed, peer, Lane::Control, {}, reason});
            it = peers_.erase(it);
        }
        receive_datagrams(events);
        return events;
    }

    bool send(Peer peer, Lane lane, const Bytes& payload) override
    {
        const auto found = peers_.find(peer);
        if (found == peers_.end() || !found->second.open || found->second.closing || payload.empty()) {
            return false;
        }
        if (lane == Lane::Control) {
            return payload.size() <= control_payload_limit &&
                   enqueue(found->second, Frame::Message, payload);
        }
        return payload.size() <= game_payload_limit && datagram(found->second, payload);
    }

    void close(Peer peer) override
    {
        if (const auto it = peers_.find(peer); it != peers_.end()) it->second.closing = true;
    }

    Traffic traffic() const override { return traffic_; }

private:
    void accept_clients(TimePoint now)
    {
        for (unsigned n = 0; n < 16; ++n) {
            sockaddr_in address{};
            AddressSize size = sizeof(address);
            Socket tcp{::accept(listener_, reinterpret_cast<sockaddr*>(&address), &size)};
            if (!tcp) return;
            if (peers_.size() >= 32) continue;
            nonblocking(tcp);
            auto& link = peers_[next_peer_++];
            link.tcp = std::move(tcp);
            link.tcp_address = address;
            link.created = link.received = now;
            link.cookie = connection_cookie();
            enqueue(link, Frame::Cookie, link.cookie);
        }
    }

    bool enqueue(Connection& link, Frame frame, const Bytes& payload)
    {
        const auto size = 5 + payload.size();
        if (link.queued + size > queue_limit) return false;
        Writer writer;
        writer.u32(static_cast<std::uint32_t>(payload.size() + 1));
        writer.u8(static_cast<std::uint8_t>(frame));
        writer.append(payload);
        link.outgoing.push_back(writer.take());
        link.queued += size;
        traffic_.queued_bytes += size;
        traffic_.peak_queued_bytes = std::max(traffic_.peak_queued_bytes, traffic_.queued_bytes);
        return true;
    }

    Disconnect write_control(Connection& link)
    {
        while (!link.outgoing.empty()) {
            const Bytes& data = link.outgoing.front();
            const auto count = ::send(link.tcp, reinterpret_cast<const char*>(data.data() + link.offset),
                                      static_cast<int>(data.size() - link.offset), send_flags);
            if (count < 0) {
                return retry_io(last_socket_error()) ? Disconnect::None : Disconnect::IoError;
            }
            if (!count) return Disconnect::IoError;
            link.queued -= count;
            traffic_.queued_bytes -= count;
            traffic_.tcp_sent += count;
            link.offset += count;
            if (link.offset == data.size()) {
                link.outgoing.pop_front();
                link.offset = 0;
            }
        }
        return Disconnect::None;
    }

    Disconnect read_control(Peer peer, Connection& link, TimePoint now, std::vector<Event>& events)
    {
        std::array<std::uint8_t, 8192> buffer{};
        for (unsigned n = 0; n < 16; ++n) {
            const auto count = ::recv(link.tcp, reinterpret_cast<char*>(buffer.data()),
                                     static_cast<int>(buffer.size()), 0);
            if (count < 0) {
                return retry_io(last_socket_error()) ? Disconnect::None : Disconnect::IoError;
            }
            if (!count) return Disconnect::Remote;
            traffic_.tcp_received += count;
            link.incoming.insert(link.incoming.end(), buffer.begin(), buffer.begin() + count);
            if (!parse_control(peer, link, now, events)) return Disconnect::Protocol;
        }
        return Disconnect::None;
    }

    bool parse_control(Peer peer, Connection& link, TimePoint now, std::vector<Event>& events)
    {
        Reader reader(link.incoming);
        std::size_t consumed = 0;
        std::uint32_t size;
        while (reader.u32(size)) {
            if (!size || size > control_payload_limit + 1) return false;
            if (reader.remaining() < size) break;
            link.received = now;
            std::uint8_t type;
            Bytes payload;
            reader.u8(type);
            reader.read(size - 1, payload);
            switch (static_cast<Frame>(type)) {
            case Frame::Heartbeat:
                if (!payload.empty()) return false;
                break;
            case Frame::Cookie:
                if (server_ || !link.cookie.empty() || payload.size() != cookie_size) return false;
                link.cookie = std::move(payload);
                link.next_heartbeat = now;
                break;
            case Frame::Message:
                if (payload.empty() || link.cookie.empty()) return false;
                if (!link.open) {
                    // A server may send TCP immediately after receiving a UDP registration.
                    if (server_) return false;
                    mark_open(peer, link, events);
                }
                events.push_back({EventType::Message, peer, Lane::Control, std::move(payload)});
                break;
            default:
                return false;
            }
            consumed = link.incoming.size() - reader.remaining();
        }
        link.incoming.erase(link.incoming.begin(), link.incoming.begin() + consumed);
        return true;
    }

    Disconnect step(Peer peer, Connection& link, TimePoint now, std::vector<Event>& events)
    {
        if (link.connecting) {
            const auto state = connect_state(link.tcp);
            if (state == ConnectState::Failed) return Disconnect::IoError;
            link.connecting = state == ConnectState::Pending;
            if (link.connecting) {
                if (link.closing) return Disconnect::Local;
                return now - link.received >= timeout_ ? Disconnect::Timeout : Disconnect::None;
            }
        }
        const auto read = read_control(peer, link, now, events);
        if (read != Disconnect::None) return read;
        if (now - (link.open ? link.received : link.created) >= timeout_) {
            return Disconnect::Timeout;
        }
        if (!link.closing && now >= link.next_heartbeat) {
            enqueue(link, Frame::Heartbeat, {});
            if (!server_ && !link.cookie.empty() && !link.open) datagram(link, {});
            link.next_heartbeat = now + heartbeat;
        }
        const auto write = write_control(link);
        return link.closing && link.outgoing.empty() ? Disconnect::Local : write;
    }

    bool datagram(const Connection& link, const Bytes& payload)
    {
        Writer writer;
        writer.u32(signature);
        writer.append(link.cookie);
        writer.append(payload);
        const auto& packet = writer.bytes();
        const auto count = ::sendto(udp_, reinterpret_cast<const char*>(packet.data()),
                                   static_cast<int>(packet.size()), send_flags,
                                   reinterpret_cast<const sockaddr*>(&link.udp_address),
                                   sizeof(link.udp_address));
        if (count < 0) {
            ++traffic_.udp_dropped;
            return false;
        }
        traffic_.udp_sent += count;
        return true;
    }

    static void mark_open(Peer peer, Connection& link, std::vector<Event>& events)
    {
        if (std::exchange(link.open, true)) return;
        events.push_back({EventType::Open, peer});
    }

    void receive_datagrams(std::vector<Event>& events)
    {
        std::array<std::uint8_t, udp_header + game_payload_limit> buffer{};
        for (unsigned n = 0; n < 512; ++n) {
            sockaddr_in from{};
            AddressSize size = sizeof(from);
            const auto count = ::recvfrom(udp_, reinterpret_cast<char*>(buffer.data()),
                                         static_cast<int>(buffer.size()), datagram_receive_flags,
                                         reinterpret_cast<sockaddr*>(&from), &size);
            if (count < 0) {
                if (retry_io(last_socket_error())) return;
                ++traffic_.udp_dropped;
                continue;
            }
            if (static_cast<std::size_t>(count) > buffer.size()) {
                ++traffic_.udp_dropped;
                continue;
            }
            traffic_.udp_received += count;
            Reader reader(buffer.data(), static_cast<std::size_t>(count));
            std::uint32_t prefix;
            Bytes cookie;
            if (!reader.u32(prefix) || prefix != signature || !reader.read(cookie_size, cookie)) {
                ++traffic_.udp_dropped;
                continue;
            }
            for (auto& [peer, link] : peers_) {
                if (link.closing || link.cookie != cookie) continue;
                if (server_ && !link.open && reader.done() &&
                    link.tcp_address.sin_addr.s_addr == from.sin_addr.s_addr) {
                    link.udp_address = from;
                }
                if (!same_address(link.udp_address, from)) break;
                if (reader.done()) {
                    mark_open(peer, link, events);
                    if (server_) datagram(link, {});
                } else if (link.open) {
                    Bytes payload;
                    reader.read(reader.remaining(), payload);
                    events.push_back({EventType::Message, peer, Lane::Game, std::move(payload)});
                }
                break;
            }
        }
    }

#ifdef _WIN32
    Winsock winsock_;
#endif
    Socket listener_, udp_;
    std::chrono::milliseconds timeout_;
    bool server_;
    Peer next_peer_ = 1;
    std::map<Peer, Connection> peers_;
    Traffic traffic_;
};

}

std::unique_ptr<Transport> listen_direct(const DirectConfig& config)
{
    return std::make_unique<Direct>(config, true);
}

std::unique_ptr<Transport> connect_direct(const DirectConfig& config)
{
    return std::make_unique<Direct>(config, false);
}

}
