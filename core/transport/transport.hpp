#pragma once

#include "transport/wire.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace netcode {

using Peer = std::uint64_t;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

inline constexpr std::size_t game_payload_limit = 1176;
inline constexpr std::size_t control_payload_limit = 65535;

enum class Lane { Control, Game };
enum class EventType { Open, Message, Closed };
enum class Disconnect { None, Local, Remote, Timeout, IoError, Protocol };

struct Event {
    EventType type;
    Peer peer;
    Lane lane = Lane::Control;
    Bytes payload{};
    Disconnect reason = Disconnect::None;
};

struct Traffic {
    std::uint64_t tcp_sent = 0, tcp_received = 0;
    std::uint64_t udp_sent = 0, udp_received = 0;
    std::uint64_t udp_dropped = 0;
    std::uint64_t udp_duplicated = 0, udp_reordered = 0;
    std::size_t queued_bytes = 0, peak_queued_bytes = 0;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual std::vector<Event> poll(TimePoint now) = 0;
    virtual bool send(Peer peer, Lane lane, const Bytes& payload) = 0;
    virtual void close(Peer peer) = 0;
    virtual Traffic traffic() const = 0;
};

struct DirectConfig {
    std::string address = "127.0.0.1";
    std::uint16_t port = 43210;
    std::chrono::milliseconds timeout{3000};
};

// The caller owns the event loop. A new transport creates a new connection.
std::unique_ptr<Transport> listen_direct(const DirectConfig& config);
std::unique_ptr<Transport> connect_direct(const DirectConfig& config);

}
