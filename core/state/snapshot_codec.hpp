#pragma once
#include "session/game_wire.hpp"
#include <map>

namespace netcode {
// Dictionary references only target packets acknowledged by this recipient.
// Expansion is bounded by the ordinary game datagram limit.
class SnapshotCodec {
  public:
    Bytes encode(const Bytes& legacy, RoomId room, RoundId round);
    bool decode(Reader& packet, Bytes& legacy_payload, std::uint32_t& seq,
                std::uint32_t& missing_anchor) const;
    void remember(std::uint32_t seq, Bytes payload);
    void acknowledge(std::uint32_t seq);
    void discard(std::uint32_t anchor);
    std::size_t retained_payload_bytes() const;
    std::uint64_t original_bytes = 0, encoded_bytes = 0;

  private:
    std::uint32_t next_ = 1, anchor_id_ = 0;
    Bytes anchor_;
    std::map<std::uint32_t, Bytes> history_;
};
}
