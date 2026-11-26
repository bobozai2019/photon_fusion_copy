#pragma once

#include "session/room.hpp"
#include <functional>
#include <set>

namespace netcode {

class GameSession;

// The engine owns value serialization. The coordinator merges bounded, atomic
// patches and sends a full baseline to every peer that negotiates this feature.
class RoomProperties {
public:
    using Values = std::map<std::string, Bytes>;
    using AreaRates = std::map<uint64_t, uint16_t>;
    using Equal = std::function<bool(const Bytes&, const Bytes&)>;
    using Validate = std::function<bool(const std::string&, const Bytes&)>;
    static constexpr uint16_t version = 5;
    static constexpr size_t max_interest_keys = 64;
    void enable(uint64_t catalog, Equal equal, Validate validate) {
        enabled_ = true;
        catalog_ = catalog;
        equal_ = std::move(equal);
        validate_ = std::move(validate);
    }
    void reset(bool host);
    bool ready() const { return revision_ != 0; }
    uint32_t revision() const { return revision_; }
    std::vector<std::vector<PlayerId>> take_rosters();
    bool negotiated(Peer peer) const { return peers_.count(peer); }
    const Values& values() const { return values_; }
    bool set(GameSession& game, Transport& link, const Values& patch, const Values& expected = {});
    bool subscribe(GameSession& game, Transport& link, uint8_t kind, const std::set<uint64_t>& keys);
    bool subscribe_area_rates(GameSession& game, Transport& link, const AreaRates& rates);
    bool supports_area_rates() const { return area_rates_supported_; }
    bool receive(GameSession& game, Transport& link, const Event& event);
    void flush(GameSession& game, Transport& link);
    void detach(Peer peer) { peers_.erase(peer); pending_.erase(peer); }

private:
    friend class CheckpointCodec;
    void write_checkpoint(Writer& out) const;
    bool read_checkpoint(Reader& in, uint32_t& revision, Values& values) const;
    void apply_roster(std::vector<PlayerId> players);
    bool valid_values(const Values& values) const;
    bool enabled_ = false;
    uint64_t catalog_ = 0;
    bool hello_sent_ = false;
    bool area_rates_supported_ = false;
    uint32_t revision_ = 0;
    std::vector<PlayerId> players_;
    std::vector<std::vector<PlayerId>> rosters_;
    Equal equal_;
    Validate validate_;
    Values values_;
    std::map<Peer, uint32_t> peers_;
    std::map<Peer, TimePoint> pending_;
};

} // namespace netcode
