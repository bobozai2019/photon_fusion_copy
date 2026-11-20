#pragma once

#include "session/room.hpp"

namespace netcode {

enum class GameMessage : std::uint8_t { Input = 1, Snapshot, StateAck, Ready, Rpc, RpcAck };

inline Writer game_packet(GameMessage type, RoomId room, RoundId round)
{
    Writer writer;
    writer.u8(static_cast<std::uint8_t>(type));
    writer.u64(room);
    writer.u64(round);
    return writer;
}

inline bool read_game_header(Reader& reader, GameMessage& type, RoomId room, RoundId round)
{
    std::uint8_t code;
    RoomId packet_room;
    RoundId packet_round;
    if (!room || !round || !reader.u8(code) || !reader.u64(packet_room) || !reader.u64(packet_round) ||
        packet_room != room || packet_round != round) return false;
    type = static_cast<GameMessage>(code);
    return true;
}

}
