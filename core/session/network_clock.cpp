#include "session/network_clock.hpp"
#include <algorithm>
#include <cmath>

namespace netcode {
namespace {
constexpr double probe_timeout = 2.0;

double elapsed(TimePoint end, TimePoint start) { return std::chrono::duration<double>(end - start).count(); }
}

void NetworkClock::reset(TimePoint local, bool host, double step_seconds, ClockDiscipline discipline) {
    *this = {};
    last_ = origin_ = local;
    host_ = synchronized_ = host;
    step_ = step_seconds;
    discipline_ = discipline;
}

void NetworkClock::update(TimePoint local) {
    if (local <= last_)
        return;
    const double delta = elapsed(local, last_);
    if (!host_ && discipline_ == ClockDiscipline::Threshold) {
        estimate_ += delta * rate_;
        last_ = local;
        return;
    }
    const double target = elapsed(local, origin_) + offset_;
    const double normal = estimate_ + delta;
    // Slew to each new sample while keeping time monotonic. A suspended
    // process advances by elapsed wall time on its first frame back.
    estimate_ = host_ ? target : normal + std::clamp(target - normal, -delta * .1, delta * .1);
    rate_ = host_ ? 1 : (estimate_ - (normal - delta)) / delta;
    last_ = local;
}

bool NetworkClock::probe_due(TimePoint local) {
    const double interval = outstanding_ ? probe_timeout : synchronized_ ? 1.0 : .25;
    if (host_ || (sequence_ && elapsed(local, sent_) < interval))
        return false;
    sent_ = local;
    if (!++sequence_)
        ++sequence_;
    outstanding_ = true;
    return true;
}

bool NetworkClock::sample(std::uint32_t sequence, TimePoint local, double host_seconds) {
    if (!outstanding_ || sequence != sequence_ || local < sent_ || !std::isfinite(host_seconds) ||
        host_seconds < 0)
        return false;
    outstanding_ = false;
    const auto latency = elapsed(local, sent_);
    if (latency > probe_timeout)
        return false;
    update(local);
    const auto sample_offset = host_seconds + latency / 2 - elapsed(local, origin_);
    if (!synchronized_) {
        offset_ = sample_offset;
        rtt_ = latency;
        const auto received = host_seconds + latency / 2;
        estimate_ = discipline_ == ClockDiscipline::Threshold ? received : std::max(estimate_, received);
    } else {
        rtt_ += .25 * (latency - rtt_);
        if (discipline_ == ClockDiscipline::Threshold) {
            offset_ = sample_offset;
            const double difference = host_seconds + latency * .5 - estimate_;
            if (std::abs(difference) > .25) {
                estimate_ += difference;
                rate_ = 1;
            } else {
                rate_ = std::abs(difference) > .1 ? 1.0 + std::copysign(1.0 / 60.0, difference) : 1.0;
            }
        } else offset_ += .25 * (sample_offset - offset_);
    }
    synchronized_ = true;
    return true;
}

}
