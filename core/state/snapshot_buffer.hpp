#pragma once

#include "state/replication.hpp"

#include <algorithm>
#include <deque>
#include <iterator>
#include <optional>

namespace netcode {

// One history per visible object. The game's binding chooses which fields
// to interpolate and how to interpolate rotations and discrete values.
class SnapshotBuffer {
public:
    struct Window { const Object& from; const Object& to; float weight; };

    void observe(const Object& sample)
    {
        if (!samples_.empty() && sample.sample_tick < samples_.back().sample_tick) return;
        if (!samples_.empty() && sample.sample_tick == samples_.back().sample_tick) samples_.back() = sample;
        else samples_.push_back(sample);
        if (samples_.size() > 16) samples_.pop_front();
    }

    bool ready() const { return samples_.size() >= 2; }
    std::optional<Window> at(double tick) const
    {
        if (samples_.empty()) return {};
        auto hi = std::lower_bound(samples_.begin(), samples_.end(), tick,
            [](const Object& o, double t) { return o.sample_tick < t; });
        if (hi == samples_.end()) hi = std::prev(hi);
        const auto lo = hi == samples_.begin() ? hi : std::prev(hi);
        const float weight = hi == lo ? 1.f : std::clamp(float((tick - lo->sample_tick) /
            (hi->sample_tick - lo->sample_tick)), 0.f, 1.f);
        return Window{*lo, *hi, weight};
    }

private:
    std::deque<Object> samples_;
};

}
