#pragma once

#include "transport/transport.hpp"

#include <array>
#include <map>
#include <optional>

namespace netcode {

using RoomId = std::uint64_t;
using RoundId = std::uint64_t;
using PlayerId = std::uint32_t;
using Key = std::array<std::uint32_t, 4>;
inline constexpr PlayerId host_player = 1;
inline constexpr std::size_t room_capacity = 4;

enum class RoomPhase { Connecting, Waiting, Playing, Ended };
enum class Reject : std::uint8_t { None, WrongRoom, Locked, Full, BadResume, Protocol };

struct ResumeTicket {
    RoomId room = 0;
    RoundId round = 0;
    PlayerId player = 0;
    Key key{};
};

struct Member {
    PlayerId id;
    Peer peer;
    bool ready;
    Key key{};
};

class RoomHost {
public:
    explicit RoomHost(Transport& transport, RoomId room = 0);
    void receive(const Event& event);
    bool start();
    bool end_round();
    void end_room();
    const std::map<PlayerId, Member>& members() const { return members_; }
    const Member* member(Peer peer) const;
    bool ready(Peer peer);
    RoomId id() const { return room_; }
    RoundId round() const { return round_; }
    RoomPhase phase() const { return phase_; }

private:
    void join(Peer peer, Reader& reader);
    void resume(Peer peer, Reader& reader);
    void reject(Peer peer, Reject reason);
    void publish();
    void entered(const Member& member, bool resumed);

    Transport& transport_;
    RoomId room_;
    RoundId round_ = 0;
    PlayerId next_player_ = host_player + 1;
    RoomPhase phase_ = RoomPhase::Waiting;
    std::map<PlayerId, Member> members_{{host_player, {host_player, 0, true}}};
};

class RoomGuest {
public:
    explicit RoomGuest(Transport& transport, RoomId requested = 0,
                       std::optional<ResumeTicket> resume = std::nullopt);
    void receive(const Event& event);
    void mark_ready() { ready_ = phase_ == RoomPhase::Playing; }
    RoomPhase phase() const { return phase_; }
    const ResumeTicket& ticket() const { return ticket_; }
    Peer peer() const { return peer_; }
    std::size_t seats() const { return seats_; }
    bool ready() const { return ready_; }
    bool resumed() const { return resumed_; }
    Reject rejected() const { return rejected_; }
    Disconnect disconnected() const { return disconnected_; }

private:
    Transport& transport_;
    RoomId requested_;
    std::optional<ResumeTicket> resume_;
    ResumeTicket ticket_;
    RoomPhase phase_ = RoomPhase::Connecting;
    Peer peer_ = 0;
    std::size_t seats_ = 0;
    bool ready_ = false, resumed_ = false;
    Reject rejected_ = Reject::None;
    Disconnect disconnected_ = Disconnect::None;
};

}
