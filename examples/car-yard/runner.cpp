#include "runner.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace yard {
namespace {

using namespace netcode;
using namespace std::chrono_literals;

void line(const std::string& text) { std::cout << text << std::endl; }

Traffic combine(Traffic a, const Traffic& b)
{
    a.tcp_sent += b.tcp_sent;
    a.tcp_received += b.tcp_received;
    a.udp_sent += b.udp_sent;
    a.udp_received += b.udp_received;
    a.udp_dropped += b.udp_dropped;
    a.udp_duplicated += b.udp_duplicated;
    a.udp_reordered += b.udp_reordered;
    a.queued_bytes = b.queued_bytes;
    a.peak_queued_bytes = std::max(a.peak_queued_bytes, b.peak_queued_bytes);
    return a;
}

class FaultLink final : public Transport {
public:
    explicit FaultLink(std::unique_ptr<Transport> direct) : direct_(std::move(direct)) {}
    std::vector<Event> poll(TimePoint now) override
    {
        auto events = direct_->poll(now);
        for (const auto& event : events) {
            if (event.type == EventType::Closed) peers_.erase(event.peer);
        }
        return events;
    }
    bool send(Peer peer, Lane lane, const Bytes& bytes) override
    {
        if (lane == Lane::Control) return direct_->send(peer, lane, bytes);
        auto& s = peers_[peer];
        const auto ordinal = ++s.count;
        if (ordinal % 17 == 0) {
            ++dropped_;
            return true;
        }
        if (ordinal % 13 == 0 && s.held.empty()) {
            s.held = bytes;
            return true;
        }
        const bool ok = direct_->send(peer, lane, bytes);
        if (ordinal % 11 == 0 && direct_->send(peer, lane, bytes)) ++duplicated_;
        if (!s.held.empty()) {
            if (ok && direct_->send(peer, lane, s.held)) ++reordered_;
            else if (!ok) direct_->send(peer, lane, s.held);
            s.held.clear();
        }
        return ok;
    }
    void close(Peer peer) override
    {
        peers_.erase(peer);
        direct_->close(peer);
    }
    Traffic traffic() const override
    {
        auto t = direct_->traffic();
        t.udp_dropped += dropped_;
        t.udp_duplicated += duplicated_;
        t.udp_reordered += reordered_;
        return t;
    }

private:
    struct Faults { unsigned count = 0; Bytes held; };
    std::unique_ptr<Transport> direct_;
    std::map<Peer, Faults> peers_;
    std::uint64_t dropped_ = 0, duplicated_ = 0, reordered_ = 0;
};

unsigned parse_number(std::string_view arg, unsigned min, unsigned max)
{
    std::size_t end = 0;
    const auto number = std::stoul(std::string(arg), &end);
    if (end != arg.size() || number < min || number > max) throw std::invalid_argument("Option is out of range");
    return static_cast<unsigned>(number);
}

}

Options parse_options(int argc, char** argv)
{
    Options options;
    options.assets = std::filesystem::absolute(argv[0]).parent_path();
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        const auto value = [&]() -> std::string_view {
            if (++i >= argc) throw std::invalid_argument("Missing value for " + std::string(arg));
            return argv[i];
        };
        if (arg == "--host") {
            options.host = true;
        } else if (arg == "--join") {
            options.host = false;
            options.address.address = value();
        } else if (arg == "--bind") {
            options.address.address = value();
        } else if (arg == "--port") {
            options.address.port = static_cast<std::uint16_t>(parse_number(value(), 1, 65535));
        } else if (arg == "--assets") {
            options.assets = value();
        } else if (arg == "--capture") {
            options.capture = std::filesystem::absolute(value());
        } else if (arg == "--smoke") {
            options.smoke = true;
        } else if (arg == "--binding-smoke") {
            options.binding_smoke = true;
        } else if (arg == "--trace") {
            options.trace = true;
        } else if (arg == "--fault") {
            options.faults = true;
        } else if (arg == "--seconds") {
            options.seconds = static_cast<int>(parse_number(value(), 5, 300));
        } else if (arg == "--scenario") {
            const auto mode = value();
            if (mode == "play") options.scenario = Scenario::Play;
            else if (mode == "stress") options.scenario = Scenario::Stress;
            else throw std::invalid_argument("Expected play or stress");
        } else {
            throw std::invalid_argument("Unknown option: " + std::string(arg));
        }
    }
    return options;
}

Runner::Runner(Options options)
    : Runner(options, Layout(options.assets / "replicators.cfg")) {}

Runner::Runner(Options options, Layout layout)
    : options_(std::move(options)), layout_(std::move(layout))
{
    connect();
}

void Runner::connect(std::optional<ResumeTicket> ticket)
{
    auto link = options_.host ? listen_direct(options_.address) : connect_direct(options_.address);
    if (options_.faults) link = std::make_unique<FaultLink>(std::move(link));
    if (match_) {
        previous_ = combine(previous_, match_->traffic());
        previous_rpc_peak_ = std::max(previous_rpc_peak_, match_->rpc_peak());
    }
    match_ = std::make_unique<Match>(layout_, std::move(link), options_.host,
        ticket ? ticket->room : 0, ticket, options_.trace ? Match::Log{line} : Match::Log{});
    previous_phase_ = RoomPhase::Connecting;
    if (options_.host) line("listening port=" + std::to_string(options_.address.port));
}

void Runner::rejoin()
{
    if (options_.host || match_->phase() != RoomPhase::Ended) return;
    const auto ticket = match_->ticket();
    try {
        connect(ticket.round ? std::optional<ResumeTicket>{ticket} : std::nullopt);
    } catch (const std::exception& error) {
        line(error.what());
    }
}

void Runner::update(TimePoint now)
{
    match_->update(now);
    const auto phase = match_->phase();
    if (phase != previous_phase_) {
        if (phase == RoomPhase::Waiting) line("waiting player=" + std::to_string(match_->player()));
        if (phase == RoomPhase::Ended && !options_.host) {
            line("disconnected reason=" + std::to_string(static_cast<int>(match_->loss())) +
                 " reject=" + std::to_string(static_cast<int>(match_->rejected())));
        }
    }
    previous_phase_ = phase;
    if (options_.scenario != Scenario::None) script(now);
}

void Runner::script(TimePoint now)
{
    if (now - initial_ > std::chrono::seconds(options_.seconds + 30)) {
        line("scenario deadline exceeded");
        status_ = 1;
        return;
    }
    if (options_.host) host_script(now);
    else guest_script(now);
}

void Runner::host_script(TimePoint now)
{
    const auto count = options_.scenario == Scenario::Stress ? 4u : 2u;
    if (match_->phase() == RoomPhase::Waiting && match_->seats() == count) {
        match_->start(count == 4 ? 100 : 0);
    }
    if (close_at_ != TimePoint{}) {
        if (now >= close_at_) status_ = 0;
        return;
    }
    if (match_->ready_guests() == count - 1 && ready_at_ == TimePoint{}) {
        ready_at_ = now;
        measured_samples_ = match_->samples();
        measured_changes_ = match_->changes();
        measured_ticks_ = match_->tick();
        measured_sends_ = match_->send_tick();
        line("measurement_begin guests=" + std::to_string(count - 1));
    }
    if (ready_at_ == TimePoint{}) return;
    const auto elapsed = now - ready_at_;
    if (count == 2 && match_->ready_guests() == 0 && lost_at_ == TimePoint{}) {
        lost_at_ = now;
        match_->barrier(true);
        match_->set_input({0.65f, 0.2f});
        offline_action_ = 1;
    }
    if (offline_action_ == 1 && now - lost_at_ >= 250ms) {
        match_->barrier(false);
        offline_action_ = 2;
    }
    if (offline_action_ == 2 && now - lost_at_ >= 500ms) {
        match_->barrier(true);
        match_->set_input({});
        offline_action_ = 3;
    }
    if (count == 4 && freeze_at_ == TimePoint{}) match_->set_input({0.58f, 0.45f});
    const bool complete = count == 4 ? elapsed >= std::chrono::seconds(options_.seconds) :
        elapsed >= 6200ms && offline_action_ == 3 && match_->ready_guests() == 1;
    if (!complete && freeze_at_ == TimePoint{}) return;
    if (freeze_at_ == TimePoint{}) {
        match_->freeze(true);
        freeze_at_ = now;
        line("measurement_frozen samples=" + std::to_string(match_->samples() - measured_samples_) +
             " changes=" + std::to_string(match_->changes() - measured_changes_) +
             " physics_ticks=" + std::to_string(match_->tick() - measured_ticks_) +
             " send_ticks=" + std::to_string(match_->send_tick() - measured_sends_) +
             " ms=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                 now - ready_at_).count()));
    } else if (!summary_written_ && now - freeze_at_ > 1500ms) {
        summary(now);
    } else if (now - freeze_at_ > 2400ms) {
        match_->end_room();
        close_at_ = now + 500ms;
    }
}

void Runner::guest_script(TimePoint now)
{
    if (match_->ready() && ready_at_ == TimePoint{}) {
        ready_at_ = now;
        original_player_ = match_->player();
        original_car_ = match_->car();
    }
    const bool play = options_.scenario == Scenario::Play;
    if (play && action_ == 6 && match_->phase() == RoomPhase::Ended) {
        if (now - lost_at_ >= 1050ms) {
            rejoin();
            if (match_->phase() == RoomPhase::Connecting) action_ = 7;
        }
        return;
    }
    if (play && action_ == 7 && match_->ready()) {
        resumed_ = match_->car() == original_car_ && match_->player() == original_player_;
        recovery_ms_ = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lost_at_).count());
        line("recovered player=" + std::to_string(match_->player()) + " car=" +
             std::to_string(match_->car()) + " ms=" + std::to_string(recovery_ms_));
        match_->set_input({-0.5f, -0.2f});
        ++action_;
    }
    if (match_->phase() == RoomPhase::Ended) {
        status_ = summary_written_ && (!play || resumed_) ? 0 : 1;
        return;
    }
    if (!match_->ready()) return;
    const auto elapsed = now - ready_at_;
    if (!play) {
        match_->set_input({0.75f, match_->player() % 2 ? -0.38f : 0.43f});
        if (action_ == 0 && match_->player() == 2 && elapsed > 300ms && match_->open_gate()) ++action_;
    } else if (action_ == 0 && elapsed > 150ms) {
        const bool allowed = match_->open_gate(true);
        line(allowed ? "public_rpc accepted" : "public_rpc rejected");
        if (allowed) status_ = 1;
        match_->set_input({0.75f, 0.f});
        ++action_;
    } else if (action_ == 1 && elapsed > 500ms) {
        match_->open_gate();
        ++action_;
    } else if (action_ == 2 && elapsed > 850ms) {
        match_->barrier(true);
        ++action_;
    } else if (action_ == 3 && elapsed > 1250ms) {
        match_->open_gate();
        ++action_;
    } else if (action_ == 4 && elapsed > 1650ms) {
        match_->barrier(false);
        ++action_;
    } else if (action_ == 5 && elapsed > 2350ms) {
        match_->disconnect();
        lost_at_ = now;
        ++action_;
    } else if (action_ == 8 && now - lost_at_ > 2450ms) {
        match_->set_input({});
        ++action_;
    }
    if (!summary_written_ && elapsed >= (play ? 7300ms : std::chrono::seconds(options_.seconds) + 1250ms)) {
        summary(now);
    }
}

void Runner::summary(TimePoint now)
{
    summary_written_ = true;
    const auto t = combine(previous_, match_->traffic());
    line("summary player=" + std::to_string(match_->player()) + " car=" + std::to_string(match_->car()) +
         " objects=" + std::to_string(match_->objects().size()) + " digest=" + std::to_string(match_->digest()) +
         " tick=" + std::to_string(match_->tick()) + " tcp_tx=" + std::to_string(t.tcp_sent) +
         " tcp_rx=" + std::to_string(t.tcp_received) + " udp_tx=" + std::to_string(t.udp_sent) +
         " udp_rx=" + std::to_string(t.udp_received) + " udp_drop=" + std::to_string(t.udp_dropped) +
         " udp_dup=" + std::to_string(t.udp_duplicated) + " udp_reorder=" + std::to_string(t.udp_reordered) +
         " tcp_queue_peak=" + std::to_string(t.peak_queued_bytes) + " rpc_peak=" +
         std::to_string(std::max(previous_rpc_peak_, match_->rpc_peak())) +
         " recovery_ms=" + std::to_string(recovery_ms_) +
         " elapsed_ms=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now - ready_at_).count()));
}

}
