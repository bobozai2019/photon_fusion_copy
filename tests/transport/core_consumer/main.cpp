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
        check(link.attempts.empty(), "Batch escaped before flush");
        link.slots = 1;
        out.flush(TimePoint{} + 2ms);
    }
    check(out.traffic().queued_bytes == 5 && out.traffic().oldest_queued_us == 1000, "Pending statistics");
    check(link.attempts == decltype(link.attempts){{Lane::Game, 3}}, "Grouped Game messages");
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
    test_memory_session();
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
