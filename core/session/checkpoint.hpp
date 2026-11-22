#pragma once
#include "session/game_session.hpp"

namespace netcode {
// A trusted standby keeps this data private. Tickets never enter the public roster.
struct CheckpointHeader {
    RoomId room = 0;
    RoundId round = 0;
    uint32_t term = 0;
    PlayerId coordinator = 0;
    Tick tick = 0;
    std::uint32_t step_ms = 0;
    bool has_seat_ttl = false, has_namespace = false;
};
class CheckpointCodec {
  public:
    static constexpr std::size_t max_size = 4 * 1024 * 1024;
    static Bytes capture(const GameSession& host);
    static bool header(Reader& in, CheckpointHeader& header);
    static bool restore(GameSession& replacement, const Bytes& image, TimePoint now);
    static void save_interest(GameSession& game, Peer peer, PlayerId player);
    static void reattach(GameSession& host, Peer peer, PlayerId player);
};
}
