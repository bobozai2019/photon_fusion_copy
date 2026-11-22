#pragma once
#include "session/checkpoint.hpp"

namespace netcode {
enum class MigrationPhase { Running, Voting, Fenced, Ended, Retiring };
struct StandbyEndpoint {
    std::string address;
    uint16_t port = 0;
    std::string provider_endpoint;
    bool valid() const {
        return provider_endpoint.empty() ? !address.empty() && port != 0 : address.empty() && port == 0;
    }
};
struct MigrationOptions {
    // Only explicitly trusted players receive the private coordinator checkpoint.
    std::map<PlayerId, StandbyEndpoint> standbys;
    std::chrono::milliseconds lease{750};
};
class MigratingSession {
  public:
    using Open = std::function<std::unique_ptr<Transport>(bool listen, const StandbyEndpoint&)>;
    using Configure = std::function<void(GameSession&)>;
    MigratingSession(std::unique_ptr<Transport> primary, SessionOptions options, Schemas schemas,
                     MigrationOptions migration, Open open, Configure configure);
    void pump(TimePoint now);
    // A two-player host freezes its last state and explicitly releases authority to its connected standby.
    bool handoff();
    bool add_standby(PlayerId player, StandbyEndpoint endpoint);

    GameSession* active() { return phase_ == MigrationPhase::Running ? game_.get() : nullptr; }
    MigrationPhase phase() const { return phase_; }
    bool armed() const { return armed_; }
    Tick saved_tick() const { return saved_tick_; }

  private:
    std::unique_ptr<GameSession> make(Outbound& link, const SessionOptions& options);
    bool protocol(const Event& e, TimePoint now);
    void coordinator(TimePoint now);
    void publish_image();
    void guest(TimePoint now);
    void vote(TimePoint now);
    void standby(TimePoint now);
    void promote(TimePoint now);
    void resume(Peer peer, TimePoint now);
    bool send_permit(Peer peer);
    void retire(TimePoint now);
    void reset_election();
    void fence();
    std::unique_ptr<Outbound> primary_, backup_, draining_;
    TimePoint drain_deadline_{};
    SessionOptions options_;
    Schemas schemas_;
    MigrationOptions policy_;
    Open open_;
    Configure configure_;
    std::unique_ptr<GameSession> game_, prepared_;
    MigrationPhase phase_ = MigrationPhase::Running;
    RoomId room_ = 0;
    RoundId round_ = 0;
    uint32_t term_ = 0;
    PlayerId local_ = 0, successor_ = 0;
    ResumeTicket ticket_;
    bool armed_ = false, disconnected_ = false, handed_off_ = false;
    bool released_ = false, permit_sent_ = false;
    std::set<PlayerId> electorate_, votes_;
    std::set<Peer> optin_;
    std::map<Peer, PlayerId> ballot_peers_;
    std::map<PlayerId, TimePoint> echoes_;
    std::map<uint32_t, TimePoint> pulses_;
    uint32_t sequence_ = 0, serial_ = 0, assembling_ = 0, image_size_ = 0;
    Bytes image_, staging_, sending_;
    std::size_t offset_ = 0;
    Tick saved_tick_ = 0, last_capture_ = 0;
    Peer vote_peer_ = 0;
    TimePoint last_contact_{}, next_pulse_{}, next_join_{}, lease_started_{};
};
}
