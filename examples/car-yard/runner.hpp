#pragma once

#include "match.hpp"

namespace yard {

enum class Scenario { None, Play, Stress };

struct Options {
    netcode::DirectConfig address;
    std::filesystem::path assets;
    std::filesystem::path capture;
    bool host = true, smoke = false, binding_smoke = false, trace = false, faults = false;
    Scenario scenario = Scenario::None;
    int seconds = 10;
};

Options parse_options(int argc, char** argv);

class Runner {
public:
    explicit Runner(Options options);
    Runner(Options options, Layout layout);
    void update(netcode::TimePoint now);
    void rejoin();
    Match& match() { return *match_; }
    const Layout& layout() const { return layout_; }
    const Options& options() const { return options_; }
    bool finished() const { return status_ >= 0; }
    int status() const { return status_; }

private:
    void connect(std::optional<netcode::ResumeTicket> ticket = {});
    void script(netcode::TimePoint now);
    void host_script(netcode::TimePoint now);
    void guest_script(netcode::TimePoint now);
    void summary(netcode::TimePoint now);

    Options options_;
    Layout layout_;
    std::unique_ptr<Match> match_;
    netcode::Traffic previous_;
    netcode::TimePoint initial_ = netcode::Clock::now(), ready_at_{}, lost_at_{}, freeze_at_{}, close_at_{};
    netcode::RoomPhase previous_phase_ = netcode::RoomPhase::Connecting;
    netcode::PlayerId original_player_ = 0;
    netcode::ObjectId original_car_ = 0;
    int status_ = -1, action_ = 0, offline_action_ = 0;
    bool resumed_ = false, summary_written_ = false;
    unsigned recovery_ms_ = 0;
    std::size_t previous_rpc_peak_ = 0;
    std::uint64_t measured_samples_ = 0, measured_changes_ = 0;
    netcode::Tick measured_ticks_ = 0, measured_sends_ = 0;
};

}
