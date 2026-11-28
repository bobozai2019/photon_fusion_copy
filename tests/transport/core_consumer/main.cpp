#include "session/game_session.hpp"
#include "session/outbound.hpp"
#include "transport/memory.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace netcode;
using namespace std::chrono_literals;

void check(bool result, const char* detail) {
    if (!result) throw std::runtime_error(detail);
}

class Limited final : public Transport {
public:
    bool supports_backpressure() const override { return true; }
    bool reliable_ordered(Lane) const override { return true; }
    bool send(Peer, Lane, const Bytes&) override { throw std::logic_error("Expected one batch call"); }
    BatchResult send_batch(Peer peer, Lane lane, const Bytes* const* messages, size_t count) override {
        check(peer == 4, "Wrong recipient");
        attempts.push_back({lane, count});
        size_t accepted = std::min(count, slots);
        for (size_t i = 0; i < accepted; ++i) sent.emplace_back(lane, *messages[i]);
        slots -= accepted;
        return {accepted == count ? SendStatus::Accepted : SendStatus::Blocked, accepted};
    }
    std::vector<Event> poll(TimePoint) override { return std::exchange(events, {}); }
    void close(Peer peer) override { check(peer == 4, "Wrong close"); closed = true; }
    Traffic traffic() const override { return {}; }
    Traffic peer_traffic(Peer peer) const override { return peer == 4 ? slow_peer : Traffic{}; }

    Traffic slow_peer{};
    bool closed = false;
    size_t slots = 0;
    std::vector<Event> events{{EventType::Open, 4}};
    std::vector<std::pair<Lane, size_t>> attempts;
    std::vector<std::pair<Lane, Bytes>> sent;
};

void test_outbound() {
    Limited link;
    Outbound out(link);
    out.poll(TimePoint{} + 1ms);
    {
        Outbound::Collect collecting(out);
        check(out.send(4, Lane::Game, {2, 9}), "Stage RPC");
        out.send_messages(4, Lane::Game, {{4, 5}, {6, 7}});
        check(out.send(4, Lane::Control, {11}), "Stage control");
        check(out.snapshot_packet_budget(4), "Collecting messages without transport backpressure");
        check(link.attempts.empty(), "Batch escaped before flush");
        link.slots = 1;
        out.flush(TimePoint{} + 2ms);
    }
    check(out.traffic().queued_bytes == 5 && out.traffic().oldest_queued_us == 1000, "Pending statistics");
    check(link.attempts == decltype(link.attempts){{Lane::Game, 3}}, "Grouped Game messages");
    check(!out.snapshot_packet_budget(4) && out.traffic().snapshot_backpressure_skips == 1,
          "Partial acceptance blocks another snapshot offer");
    out.close(4);
    check(!link.closed && !out.send(4, Lane::Control, {12}), "Close must wait behind queue");
    link.slots = 32;
    out.flush(TimePoint{} + 3ms);
    const decltype(link.sent) expected{{Lane::Game, {2, 9}}, {Lane::Game, {4, 5}},
                                      {Lane::Game, {6, 7}}, {Lane::Control, {11}}};
    check(link.closed && link.sent == expected && out.traffic().queued_bytes == 0,
          "Only unsent suffix retried, preserving message boundaries and order before close");
    std::cout << "Batch trace peer=4 Game[02 09][04 05][06 07] Control[0b], partial 1/3, drained before close\n";
}

void test_memory_contract() {
    MemoryTransport a, b;
    a.link(b);
    const auto a_open = a.poll(TimePoint{});
    const auto b_open = b.poll(TimePoint{});
    check(a_open.size() == 1 && b_open.size() == 1 &&
          a_open.front().type == EventType::Open && b_open.front().type == EventType::Open, "Memory open events");
    const auto sender = a_open.front().peer;
    const auto recipient = b_open.front().peer;

    const Bytes first{0x00, 0xff}, second{0xa1}, game{0x1a, 0x00, 0x01}, empty;
    auto input = first;
    const Bytes* batch[]{&input, &empty, &second};
    auto result = a.send_batch(sender, Lane::Control, batch, 3);
    check(result.status == SendStatus::Blocked && result.accepted == 1, "Memory batch acceptance prefix");
    input.assign(4, 0x55);
    result = a.send_batch(sender, Lane::Control, batch + 2, 1);
    check(result.status == SendStatus::Accepted && result.accepted == 1, "Memory retry suffix");
    check(a.send(sender, Lane::Game, game), "Memory game submit");
    a.close(sender);
    check(!a.send(sender, Lane::Control, second), "Memory send after close");

    const auto received = b.poll(TimePoint{});
    check(received.size() == 4, "Memory receives three messages and close");
    const auto message = [&](size_t i, Lane lane, const Bytes& expected) {
        auto& e = received[i];
        return e.type == EventType::Message && e.peer == recipient && e.lane == lane && e.payload == expected;
    };
    check(message(0, Lane::Control, first) && message(1, Lane::Control, second) &&
          message(2, Lane::Game, game), "Memory message order and owned payloads");
    const auto closed = a.poll(TimePoint{});
    check(received.back().type == EventType::Closed && received.back().reason == Disconnect::Remote &&
          closed.size() == 1 && closed[0].type == EventType::Closed && closed[0].reason == Disconnect::Local,
          "Memory close reasons");
    check(a.traffic().tcp_sent == 3 && b.traffic().tcp_received == 3 &&
          a.traffic().udp_sent == 3 && b.traffic().udp_received == 3, "Memory business bytes");
    a.link(b);
    const auto reconnected = b.poll(TimePoint{});
    check(reconnected.size() == 1 && reconnected[0].type == EventType::Open &&
          reconnected[0].peer != recipient, "Memory reconnect peer");
    std::cout << "Memory contract: Open, Control[00ff][a1], Game[1a0001], Remote Closed, new peer on reconnect\n";
}

void test_snapshot_admission() {
    Limited link;
    Outbound out(link);
    const auto start = TimePoint{} + 1ms;
    out.poll(start);
    check(out.snapshot_packet_budget(4), "New peer has no pending backpressure");
    check(out.send(4, Lane::Game, {42}) && !out.snapshot_packet_budget(4), "Immediate block stages a retry");
    link.slots = 1;
    out.poll(start + 1ms);
    check(out.snapshot_packet_budget(4) && out.traffic().queued_bytes == 0 &&
          link.sent == decltype(link.sent){{Lane::Game, {42}}}, "Snapshot offers resume after ordered drain");

    link.events.push_back({EventType::Open, 7});
    out.poll(start + 2ms);
    const auto healthy_budget = out.snapshot_packet_budget(7);
    link.slow_peer.queued_bytes = 60 * 1024;
    check(out.snapshot_packet_budget(4) > 0 && out.snapshot_packet_budget(4) < healthy_budget,
          "Provider backlog shrinks the budget before it blocks");
    link.slow_peer.oldest_queued_us = 60'000;
    check(!out.snapshot_packet_budget(4) && out.snapshot_packet_budget(7) == healthy_budget,
          "An old provider queue defers only that peer before hard backpressure");
    check(out.traffic().snapshot_soft_skips == 1, "Provider pressure is separate from hard blocking");
    link.slots = 1;
    check(out.send(4, Lane::Control, {9}), "A deferred snapshot leaves Control admissible");
    link.slow_peer = {};
    check(out.snapshot_packet_budget(4) == healthy_budget, "Provider drain restores snapshot budget");
}

void test_snapshot_budget_rotation() {
    Schemas schemas;
    schemas.add({1, 1, {{1, FieldType::Int}}});
    AuthorityState state(schemas);
    state.attach(4);
    for (ObjectId id = 1; id <= 110; ++id)
        check(state.create(id, 1, 1, {int32_t(id)}, 1), "Create budget fixture");

    ReplicaState replica(schemas);
    const auto receive = [&](Tick tick) {
        bool yielded = false;
        auto packets = state.packets(4, 1, 1, tick, 200, 1, &yielded);
        check(packets.size() == 1, "Snapshot respects one-packet budget");
        Reader in(packets.front());
        GameMessage type;
        AppliedSnapshot applied;
        check(read_game_header(in, type, 1, 1) && type == GameMessage::Snapshot, "Read budgeted snapshot header");
        check(replica.apply(in, 1, 1, applied), "Receive budgeted snapshot");
        return yielded;
    };
    check(receive(1) && !replica.objects().count(110), "First packet yields before the tail");
    check(state.capture(1, {int32_t(999)}, 2), "Change an earlier entry");
    check(state.capture(110, {int32_t(222)}, 2), "Change the deferred entry");
    for (Tick tick = 2; tick != 12 && !replica.objects().count(110); ++tick) receive(tick);
    check(replica.objects().count(110) && replica.objects().at(110).values == Values{int32_t(222)},
          "Deferred object gets the latest revision even while earlier entries change");
    receive(12);
    check(replica.objects().at(1).values == Values{int32_t(999)}, "The cursor wraps to the updated prefix");
}

void test_memory_session() {
    MemoryTransport a, b;
    a.link(b);
    SessionOptions options;
    options.host = true;
    options.room = 146;
    Schemas schemas;
    schemas.add({1, 1, {{1, FieldType::Int}}});
    GameSession host(a, options, schemas);
    options.host = false;
    GameSession guest(b, options, schemas);
    auto now = TimePoint{};
    const auto advance = [&] {
        for (int i = 0; i < 20; ++i) {
            host.pump(now);
            guest.pump(now);
            now += 25ms;
        }
    };
    advance();
    check(host.host()->members().size() == 2 && host.start(now), "In-memory room");
    const auto object = host.world().spawn(1, 1, {int32_t(42)});
    advance();
    check(object && guest.object(object) && guest.object(object)->values == Values{int32_t(42)},
          "In-memory snapshot convergence");
    std::cout << "Relocated Netcode::Core memory room: seats=2, score=42\n";
}
}

int main() try {
    test_outbound();
    test_snapshot_admission();
    test_snapshot_budget_rotation();
    test_memory_contract();
    test_memory_session();
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
