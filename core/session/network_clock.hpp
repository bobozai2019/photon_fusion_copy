#pragma once

#include "state/replication.hpp"

namespace netcode {

enum class ClockDiscipline { Monotonic, Threshold };

// All exposed times use seconds on the coordinator's round-relative clock.
class NetworkClock {
  public:
    void reset(TimePoint local, bool host, double step_seconds, ClockDiscipline discipline = ClockDiscipline::Monotonic);
    void update(TimePoint local);
    bool probe_due(TimePoint local);
    std::uint32_t sequence() const { return sequence_; }
    bool sample(std::uint32_t sequence, TimePoint local, double host_seconds);
    double seconds() const { return estimate_; }
    double rtt() const { return rtt_; }
    double rate() const { return rate_; }
    double offset() const { return offset_; }
    bool synchronized() const { return synchronized_; }
    void set_step(std::chrono::milliseconds step) {
        if (step.count() > 0)
            step_ = std::chrono::duration<double>(step).count();
    }
    double object_time(Tick sample_tick) const { return sample_tick ? (sample_tick - 1) * step_ : 0; }

  private:
    ClockDiscipline discipline_ = ClockDiscipline::Monotonic;
    TimePoint origin_{}, last_{}, sent_{};
    std::uint32_t sequence_ = 0;
    bool host_ = false, synchronized_ = false, outstanding_ = false;
    double estimate_ = 0, rate_ = 1, offset_ = 0, rtt_ = 0, step_ = 0;
};

}
