#pragma once

#include "state/replication.hpp"

#include <algorithm>
#include <deque>

namespace netcode {

// The model supplies one deterministic step, including the environment in
// effect at that simulation tick. Input numbers are local to the current grant.
template<class State, class Input>
class Prediction {
public:
    struct Frame { Tick sequence; Input input; };
    static constexpr std::size_t capacity = 128;
    bool ready() const { return checkpoint_tick_ != 0; }
    const State& state() const { return state_; }
    const auto& pending() const { return pending_; }
    Tick confirmed() const { return confirmed_; }
    Tick checkpoint_tick() const { return checkpoint_tick_; }
    std::uint64_t replayed() const { return replayed_; }
    Tick tick() const { return checkpoint_tick_ + static_cast<Tick>(pending_.size()); }
    void clear() { *this = {}; }

    template<class Step>
    bool advance(const Input& input, Step step)
    {
        if (!ready() || pending_.size() == capacity || issued_ == UINT32_MAX || tick() == UINT32_MAX) return false;
        pending_.push_back({++issued_, input});
        step(state_, input, tick());
        return true;
    }

    template<class Step>
    bool reconcile(Tick tick, Tick consumed, const State& authority, Step step)
    {
        if (!tick || tick <= checkpoint_tick_ || consumed < confirmed_ || (ready() && consumed > issued_)) return false;
        confirmed_ = consumed;
        issued_ = std::max(issued_, consumed);
        checkpoint_tick_ = tick;
        checkpoint_ = authority;
        while (!pending_.empty() && pending_.front().sequence <= consumed) pending_.pop_front();
        replay(step);
        return true;
    }

    template<class Step> void replay(Step step)
    {
        state_ = checkpoint_;
        Tick tick = checkpoint_tick_;
        for (const auto& frame : pending_) step(state_, frame.input, ++tick);
        replayed_ += pending_.size();
    }

private:
    State state_{}, checkpoint_{};
    Tick checkpoint_tick_ = 0, confirmed_ = 0, issued_ = 0;
    std::uint64_t replayed_ = 0;
    std::deque<Frame> pending_;
};

}
