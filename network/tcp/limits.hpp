#pragma once
#include <cstdint>

namespace netcode {
// One spare socket allows a full room to send a rejection to the next guest.
inline constexpr std::uint8_t tcp_room_capacity = 64;
}
