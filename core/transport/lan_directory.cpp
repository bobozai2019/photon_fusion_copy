#include "transport/ipv4.hpp"
#include "session/discovery.hpp"

namespace netcode {
namespace {
using namespace ipv4;
using namespace std::chrono_literals;
class LanDirectory final : public RoomDirectory {
  public:
    explicit LanDirectory(const LanDiscoveryOptions& options)
        : socket_(make_socket(SOCK_DGRAM, IPPROTO_UDP)), group_(endpoint({"239.255.77.81", options.port})) {
        auto local = endpoint({"0.0.0.0", options.port});
        const auto iface = endpoint({options.network_interface, options.port}).sin_addr;
        int enabled = 1;
        if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled),
                       sizeof(enabled)) < 0 ||
            ::bind(socket_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0)
            socket_error("bind lobby discovery");
        ip_mreq request{};
        request.imr_multiaddr = group_.sin_addr;
        request.imr_interface = iface;
        if (setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&request),
                       sizeof(request)) < 0 ||
            setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&iface),
                       sizeof(iface)) < 0)
            socket_error("join lobby discovery group");
    }
    bool publish(const RoomListing& r) override {
        auto bytes = encode_listing(r);
        if (bytes.empty() || !r.port)
            return false;
        if (bytes != ad_) {
            ad_ = std::move(bytes);
            next_ = {};
        }
        return true;
    }
    bool join(RoomId id, Joined done) override {
        auto it = recent_.find(id);
        if (!done || it == recent_.end() || !RoomFilter{}.matches(it->second.first))
            return false;
        done(it->second.first);
        return true;
    }
    void withdraw() override { ad_.clear(); }
    IdentityStatus identity() const override { return IdentityStatus::LocalNetwork; }
    void poll(TimePoint now) override {
        if (!ad_.empty() && now >= next_) {
            next_ = now + 400ms;
            sendto(socket_, reinterpret_cast<const char*>(ad_.data()), int(ad_.size()), 0,
                   reinterpret_cast<const sockaddr*>(&group_), sizeof(group_));
        }
        for (unsigned i = 0; i < 64; ++i) {
            std::array<uint8_t, 1100> buffer{};
            sockaddr_in from{};
            AddressSize len = sizeof(from);
            const auto n = recvfrom(socket_, reinterpret_cast<char*>(buffer.data()), int(buffer.size()),
                                    datagram_receive_flags, reinterpret_cast<sockaddr*>(&from), &len);
            if (n < 0) {
                if (retry_io(last_socket_error()))
                    break;
                continue;
            }
            RoomListing listing;
            if (size_t(n) > buffer.size() ||
                !decode_listing(Bytes(buffer.begin(), buffer.begin() + n), listing) || !listing.port)
                continue;
            char ip[INET_ADDRSTRLEN]{};
            if (!inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip)))
                continue;
            listing.endpoint = ip;
            if (recent_.size() < 512 || recent_.count(listing.room))
                recent_[listing.room] = {std::move(listing), now + 1500ms};
        }
        for (auto it = recent_.begin(); it != recent_.end();) {
            if (now >= it->second.second)
                it = recent_.erase(it);
            else
                ++it;
        }
    }
    std::vector<RoomListing> search(const RoomFilter& f) const override {
        std::vector<RoomListing> result;
        for (const auto& [id, record] : recent_)
            if (f.matches(record.first))
                result.push_back(record.first);
        return result;
    }

  private:
#ifdef _WIN32
    Winsock winsock_;
#endif
    Socket socket_;
    sockaddr_in group_{};
    Bytes ad_;
    TimePoint next_{};
    std::map<RoomId, std::pair<RoomListing, TimePoint>> recent_;
};
}
std::unique_ptr<RoomDirectory> lan_directory(const LanDiscoveryOptions& options) {
    return std::make_unique<LanDirectory>(options);
}
}
