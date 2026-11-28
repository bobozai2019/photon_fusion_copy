#pragma once
#include "session/network_clock.hpp"
#include "session/world.hpp"
#include "session/model_prediction.hpp"
#include "session/rpc.hpp"
#include "session/discovery.hpp"
#include "state/snapshot_codec.hpp"
#include "session/object_authority.hpp"
#include "session/outbound.hpp"
#include "session/room_properties.hpp"
#include "session/native_rpc.hpp"

namespace netcode {

struct SessionOptions {
    RoomId room = 0;
    RoomOptions admission{};
    bool host = false, spectator = false, extended_join = false;
    std::chrono::milliseconds step{30};
    std::optional<ResumeTicket> resume;
    bool compact = true;
    uint32_t term = 1;
    ClockDiscipline clock_discipline = ClockDiscipline::Monotonic;
    bool retain_spatial_writer_interest = false;
};

// Single-threaded engine entry point. The caller owns the transport and can
// either pump it here or feed events from its own event loop.
class GameSession {
  public:
    GameSession(Transport& link, SessionOptions options, Schemas schemas = {});
    GameSession(Outbound& link, SessionOptions options, Schemas schemas = {});
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;
    bool advertise(std::unique_ptr<RoomDirectory> directory, RoomListing listing);
    bool start(TimePoint now);
    void end(TimePoint now);
    void pump(TimePoint now);
    void receive(const Event& event, TimePoint now);
    void flush(TimePoint now);
    RoundId round() const;
    PlayerId local() const;
    PlayerId coordinator() const { return options_.admission.coordinator; }
    std::uint32_t term() const { return term_; }
    RoomId room() const;
    Tick tick() const { return tick_; }
    std::chrono::milliseconds step() const { return options_.step; }
    const Object* object(ObjectId id) const;
    std::vector<ObjectId> objects() const;
    AppliedSnapshot scene_ready(SceneId scene, bool ready);
    RoomHost* host() { return host_ ? &*host_ : nullptr; }
    RoomGuest* guest() { return guest_ ? &*guest_ : nullptr; }
    RoomProperties& room_properties() { return properties_; }
    bool set_room_properties(const RoomProperties::Values& patch, const RoomProperties::Values& expected = {}) {
        return properties_.set(*this, link_, patch, expected);
    }
    bool subscribe_local(InterestKind kind, const std::set<uint64_t>& keys) {
        return properties_.subscribe(*this, link_, uint8_t(kind), keys);
    }
    bool subscribe_local_area_rates(const RoomProperties::AreaRates& rates) {
        return properties_.subscribe_area_rates(*this, link_, rates);
    }
    NativeRpc& native_rpc() { return native_rpc_; }
    bool send_native_rpc(ObjectId id, int32_t target, uint32_t token, const Bytes& data, bool sender_handled = false) {
        return native_rpc_.send(*this, link_, id, target, token, data, sender_handled);
    }
    void finish_native_rpc(uint32_t receipt, bool delivered) {
        native_rpc_.finish(*this, link_, receipt, delivered);
    }
    const SnapshotCodec* codec(Peer peer) const;
    Rpc& rpc() { return rpc_; }
    std::function<void(const RpcCall&)> on_rpc;
    ModelPrediction& prediction() { return prediction_; }
    SessionWorld& world() { return world_; }
    Schemas& schemas() { return schemas_; }
    AuthorityState& authority() { return state_; }
    ObjectAuthority& writers() { return writers_; }
    bool set_clock_discipline(ClockDiscipline discipline) {
        if (round()) return false;
        options_.clock_discipline = discipline;
        return true;
    }
    const NetworkClock& clock() const { return clock_; }
    Traffic traffic() const {
        auto stats = link_.traffic();
        stats.authority_ack_coalesces = writers_.receipt_coalesces();
        return stats;
    }
    std::function<void(const AppliedSnapshot&)> on_snapshot;
    std::function<void(const ObjectGrant&)> on_grant;

  private:
    GameSession(std::unique_ptr<Outbound> owned, Outbound* link, SessionOptions options, Schemas schemas);
    friend class CheckpointCodec;
    void expire_members(TimePoint now);
    std::uint32_t term_ = 1;
    struct SavedInterest {
        SceneId scene = 0;
        std::optional<std::set<ObjectId>> selected;
        std::map<std::uint64_t, std::uint16_t> areas, users;
    };
    std::map<PlayerId, SavedInterest> resumed_interest_;
    void reset(TimePoint now);
    void apply(const AppliedSnapshot& snapshot);
    void ready(Peer peer);
    std::unique_ptr<Outbound> owned_outbound_;
    Outbound& link_;
    SessionOptions options_;
    std::unique_ptr<RoomDirectory> directory_;
    RoomListing listing_;
    std::optional<RoomHost> host_;
    std::optional<RoomGuest> guest_;
    RoomProperties properties_;
    NativeRpc native_rpc_;
    Schemas schemas_;
    AuthorityState state_;
    ReplicaState replica_;
    ObjectAuthority writers_;
    NetworkClock clock_;
    SessionWorld world_;
    ModelPrediction prediction_;
    Rpc rpc_;
    std::map<Peer, SnapshotCodec> codecs_;
    SnapshotCodec decoder_;
    std::optional<Tick> codec_probe_;
    bool codec_enabled_ = false;
    Tick tick_ = 1, sent_tick_ = 0;
    TimePoint origin_{};
};

}
