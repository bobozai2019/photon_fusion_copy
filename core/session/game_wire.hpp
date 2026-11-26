#pragma once

#include "session/room.hpp"

#define NETCODE_HAS_WORD_STATE 1

namespace netcode {

enum class GameMessage : std::uint8_t {
    Input = 1, Snapshot, StateAck, Ready, Rpc, RpcAck,
    AuthorityOffer = 64, AuthorityModels, AuthorityGrant, AuthorityState, AuthorityAck, AuthorityInput,
    PredictionHello, InputHistory, Checkpoint, CollisionWorld,
    ClaimHello, ClaimSupported, ClaimRequest, ClaimPrompt, ClaimAnswer, ClaimResult, ClockProbe, ClockSample,
    WorldHello, WorldOffer, WorldMaps, MapReady, WorldObjects, SpawnRequest, SpawnResult,
    ModelHello, ModelFrame, ModelInputs, RpcSkip, CodecHello, CompactSnapshot, CodecAck, CodecReset,
    MigrationHello, MigrationPermit, MigrationPulse, MigrationEcho, MigrationChunk, MigrationVote, MigrationCommit,
    WorldExtendedHello, WorldExtendedObjects, MigrationYield, MigrationYieldAck, WorldNamespace, SpawnReferenced,
    RoomPropertiesHello, RoomPropertiesUpdate, RoomPropertiesSnapshot, SpawnInitialized, DespawnRequested,
    RoomInterestKeys, WorldNativeReady, NativeRpcRequest, NativeRpcDelivery, NativeRpcReceipt, NativeRpcResult,
    WordStateHello = 121, WordStateSupported, AuthorityStateWords,
    AuthorityBatchHello, AuthorityBatchSupported, AuthorityStateBatch, AuthorityAckBatch,
    RoomAreaRatesSupported, RoomAreaRates
};

inline void write_game_header(Writer& writer, GameMessage type, RoomId room, RoundId round)
{
    writer.u8(static_cast<std::uint8_t>(type));
    writer.u64(room);
    writer.u64(round);
}

inline Writer game_packet(GameMessage type, RoomId room, RoundId round)
{
    Writer writer;
    write_game_header(writer, type, room, round);
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
