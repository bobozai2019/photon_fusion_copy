#pragma once

#include "transport/transport.hpp"

#include <array>
#include <deque>
#include <map>
#include <optional>
#include <string_view>

namespace netcode {

using RoomId = std::uint64_t;
using RoundId = std::uint64_t;
using PlayerId = std::uint32_t;
using Key = std::array<std::uint32_t, 4>;
inline constexpr PlayerId host_player = 1;
inline constexpr std::size_t room_capacity = 4;
inline constexpr std::size_t room_chat_limit = 512;

enum class RoomPhase { Connecting, Waiting, Playing, Ended };
enum class Reject : std::uint8_t { None, WrongRoom, Locked, Full, BadResume, Protocol, Kicked };

struct RoomMessage {
    PlayerId sender;
    std::string text;
};

struct ResumeTicket {
    RoomId room = 0;
    RoundId round = 0;
    PlayerId player = 0;
    Key key{};
};

struct RoomOptions {
    std::uint8_t capacity = room_capacity;
    bool late_join = false, spectators = false;
    PlayerId coordinator = host_player;
    std::optional<uint32_t> player_ttl_ms; // Missing keeps disconnected seats for the round.
};

struct Member {
    PlayerId id;
    Peer peer;
    bool ready;
    Key key{};
    bool spectator = false;
    std::optional<TimePoint> reserved_until;
};

class RoomHost {
public:
    explicit RoomHost(Transport& transport, RoomId room = 0, RoomOptions options = {});
    void receive(const Event& event, TimePoint now = Clock::now());
    std::vector<PlayerId> expire(TimePoint now);
    bool set_player_ttl(std::optional<uint32_t> ms);
    bool start();
    bool end_round();
    void end_room();
    bool kick(PlayerId player);
    bool chat(std::string_view text);
    const std::deque<RoomMessage>& messages() const { return messages_; }
    const std::map<PlayerId, Member>& members() const { return members_; }
    const Member* member(Peer peer) const;
    bool ready(Peer peer);
    RoomId id() const { return room_; }
    RoundId round() const { return round_; }
    RoomPhase phase() const { return phase_; }

private:
    void join(Peer peer, Reader& reader, bool extended);
    void resume(Peer peer, Reader& reader);
    void reject(Peer peer, Reject reason);
    void publish();
    void entered(const Member& member, bool resumed);
    void broadcast_chat(PlayerId sender, std::string text);

    Transport& transport_;
    RoomId room_;
    RoomOptions options_;
    RoundId round_ = 0;
    PlayerId next_player_ = host_player + 1;
    RoomPhase phase_ = RoomPhase::Waiting;
    friend class CheckpointCodec;
    std::map<PlayerId, Member> members_;
    std::deque<RoomMessage> messages_;
};

class RoomGuest {
public:
    explicit RoomGuest(Transport& transport, RoomId requested = 0,
                       std::optional<ResumeTicket> resume = std::nullopt,
                       bool spectator = false, bool extended = false, PlayerId coordinator = host_player);
    void receive(const Event& event);
    bool chat(std::string_view text);
    void mark_ready() { ready_ = phase_ == RoomPhase::Playing; }
    RoomPhase phase() const { return phase_; }
    const ResumeTicket& ticket() const { return ticket_; }
    Peer peer() const { return peer_; }
    std::size_t seats() const { return seats_; }
    bool ready() const { return ready_; }
    bool spectator() const { return spectator_; }
    bool resumed() const { return resumed_; }
    Reject rejected() const { return rejected_; }
    Disconnect disconnected() const { return disconnected_; }
    const std::vector<PlayerId>& players() const { return players_; }
    const std::deque<RoomMessage>& messages() const { return messages_; }

private:
    Transport& transport_;
    RoomId requested_;
    std::optional<ResumeTicket> resume_;
    ResumeTicket ticket_;
    RoomPhase phase_ = RoomPhase::Connecting;
    Peer peer_ = 0;
    bool spectator_, extended_;
    PlayerId coordinator_;
    std::size_t seats_ = 0;
    bool ready_ = false, resumed_ = false;
    Reject rejected_ = Reject::None;
    Disconnect disconnected_ = Disconnect::None;
    std::vector<PlayerId> players_;
    std::deque<RoomMessage> messages_;
};

}
