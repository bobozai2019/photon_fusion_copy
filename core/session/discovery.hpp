#pragma once
#include "session/room.hpp"
#include <functional>

namespace netcode {
struct RoomListing {
    RoomId room = 0;
    std::string name, region, build, endpoint;
    std::uint16_t port = 0;
    std::uint8_t occupied = 0, capacity = 0;
    bool joinable = false;
    std::map<std::string, std::string> properties;
};
struct RoomFilter {
    std::string region, build;
    std::map<std::string, std::string> properties;
    bool require_space = true;
    bool matches(const RoomListing& room) const;
};
bool valid_listing(const RoomListing& listing);
Bytes encode_listing(const RoomListing& listing);
bool decode_listing(const Bytes& bytes, RoomListing& listing);
enum class IdentityStatus { LocalNetwork, PlatformAuthenticated, Unavailable };
class RoomDirectory {
  public:
    virtual ~RoomDirectory() = default;
    virtual bool publish(const RoomListing& listing) = 0;
    virtual void withdraw() = 0;
    virtual void poll(TimePoint now) = 0;
    virtual std::vector<RoomListing> search(const RoomFilter& filter) const = 0;
    using Joined = std::function<void(std::optional<RoomListing>)>;
    virtual bool join(RoomId room, Joined complete) = 0;
    // Select from the currently advertised matching rooms. Admission is still decided by the room host.
    bool join_random(const RoomFilter& filter, Joined complete);
    virtual IdentityStatus identity() const = 0;
};
struct LanDiscoveryOptions {
    std::string network_interface = "0.0.0.0";
    std::uint16_t port = 43881;
};
std::unique_ptr<RoomDirectory> lan_directory(const LanDiscoveryOptions& options = {});
class SteamRuntime;
std::unique_ptr<RoomDirectory> steam_directory(std::shared_ptr<SteamRuntime> steam);
}
