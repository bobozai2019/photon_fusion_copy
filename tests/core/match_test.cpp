#include "match.hpp"
#include "transport/memory.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

using namespace netcode;
using namespace yard;
using namespace std::chrono_literals;

void check(bool ok, const char* message)
{
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}

std::size_t count_log(const std::vector<std::string>& log, const std::string& text)
{
    return std::count_if(log.begin(), log.end(), [&](const auto& entry) {
        return entry.find(text) != std::string::npos;
    });
}

ObjectId find(const Match& match, SchemaId schema)
{
    for (const auto id : match.objects()) {
        if (match.object(id)->schema == schema) return id;
    }
    return 0;
}

float field(const Match& match, ObjectId object, std::size_t index)
{
    return std::get<float>(match.object(object)->values.at(index));
}

}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const Layout layout(argv[1]);
    std::vector<std::string> host_log, b_log, c_log;
    auto trace = [](auto& lines) {
        return [&lines](const std::string& text) { lines.push_back(text); };
    };
    auto transport = std::make_unique<MemoryTransport>();
    auto& hub = *transport;
    Match a(layout, std::move(transport), true, 631, {}, trace(host_log));

    auto join = [&](auto& log, std::optional<ResumeTicket> ticket = {}) {
        auto link = std::make_unique<MemoryTransport>();
        hub.link(*link);
        return std::make_unique<Match>(layout, std::move(link), false, a.room(), ticket, trace(log));
    };
    auto b = join(b_log);
    auto c = join(c_log);
    std::unique_ptr<Match> extra;
    TimePoint now{};
    auto advance = [&](int steps) {
        for (int i = 0; i < steps; ++i) {
            now += 10ms;
            a.update(now);
            b->update(now);
            c->update(now);
            if (extra) extra->update(now);
        }
    };
    advance(30);
    check(a.seats() == 3 && a.start(), "Three members must join before first round");
    advance(30);
    check(b->ready() && c->ready() && a.ready_guests() == 2, "Baseline did not cross ready fence");
    const auto player = b->player();
    const auto car = b->car();
    const auto round = a.round();
    const auto gate = find(a, Gate);
    check(car && b->objects() == a.objects(), "Baseline IDs differ");
    b->set_input({0.75f, 0.f});
    advance(20);
    check(field(a, car, 5) == 0.75f && field(*b, car, 5) == 0.75f &&
          count_log(host_log, "input player=2") && count_log(host_log, "authority_write") &&
          count_log(b_log, "net_input_x=0.750000"), "Input-authority-capture-send-apply chain failed");
    std::cout << "input player=" << player << " object=" << car
              << " host=0.75 guest=0.75 sample=" << b->object(car)->sample_tick << '\n';

    check(!b->open_gate(true), "Guest API sent Server RPC on public gate");
    auto malformed = game_packet(GameMessage::Rpc, a.room(), round);
    malformed.u32(gate);
    malformed.u16(TryOpen);
    malformed.u32(1);
    malformed.u32(gate);
    check(b->transport().send(1, Lane::Game, malformed.bytes()), "Failed to inject wrong-owner call");
    advance(10);
    check(!std::get<bool>(a.object(gate)->values[5]), "Public gate accepted wrong-owner receive");
    check(b->open_gate(), "Owned call not queued");
    advance(20);
    check(std::get<bool>(b->object(gate)->values[5]) && std::get<bool>(c->object(gate)->values[5]),
          "Gate approval did not replicate");
    check(b->open_gate(), "Second owned call not queued");
    advance(20);
    check(b->notice() == "The gate is already open" && c->notice() == "The yard gate is open",
          "Business denial not directed to the caller");
    check(c->horn() && c->notice().find("Horn") == 0 && b->notice().find("Horn") == std::string::npos,
          "Guest multicast left its originating client");

    auto& b_link = static_cast<MemoryTransport&>(b->transport());
    b_link.next_game(Delivery::Drop);
    check(b->barrier(true), "Reliable spawn not queued");
    b_link.next_game(Delivery::Duplicate);
    check(b->barrier(false), "Reliable delete not queued");
    advance(30);
    check(count_log(host_log, "barrier_spawn") == 1 && count_log(host_log, "barrier_removed") == 1 &&
          !find(a, Barrier) && !b->rpc_pending(), "Reliable order, retry or deduplication failed");
    std::cout << "door approval + directed denial + wrong-owner rejection + reliable barrier order\n";

    b->set_input({-0.5f, 0.f});
    check(a.barrier(true), "Host barrier not created");
    advance(25);
    const auto old_barrier = find(a, Barrier);
    const auto before = field(a, car, 0);
    const auto preserved = b->ticket();
    b->disconnect();
    advance(10);
    check(a.seats() == 3 && a.ready_guests() == 1 && a.object(car) &&
          field(a, car, 5) == 0.f && std::abs(field(a, car, 0) - before) > 0.01f,
          "Disconnect did not retain and continue simulating the original car");
    check(a.barrier(false) && a.barrier(true), "Host could not modify world during disconnect");
    advance(20);
    std::vector<std::string> extra_log;
    extra = join(extra_log);
    advance(15);
    check(extra->rejected() == Reject::Locked, "New player joined a locked round");
    auto wrong = preserved;
    wrong.key[0] ^= 1;
    extra = join(extra_log, wrong);
    advance(15);
    check(extra->rejected() == Reject::BadResume, "Wrong ticket took over a seat");
    b = join(b_log, preserved);
    advance(30);
    check(b->ready() && b->player() == player && b->car() == car && !b->object(old_barrier) &&
          b->object(find(a, Barrier)) && b->objects() == a.objects(), "Reconnect world or identity is stale");
    extra = join(extra_log, preserved);
    advance(15);
    check(extra->rejected() == Reject::BadResume, "Simultaneous resume replaced current peer");
    b->set_input({-0.5f, 0.15f});
    advance(30);
    check(field(a, car, 5) == -0.5f && field(*b, car, 5) == -0.5f,
          "Resumed player cannot control original car");
    std::cout << "resume player=" << player << " car=" << car << " replaced stale barrier=" << old_barrier << '\n';

    a.end_round();
    advance(10);
    check(a.start() && a.round() != round, "Did not start second round in same room");
    advance(30);
    auto stale = game_packet(GameMessage::Input, a.room(), round);
    stale.u32(40000);
    stale.f32(0.9f);
    stale.f32(0.f);
    check(b->transport().send(1, Lane::Game, stale.bytes()), "Failed to inject previous-round input");
    extra = join(extra_log, preserved);
    advance(20);
    check(extra->rejected() == Reject::BadResume && field(a, b->car(), 5) == 0.f,
          "Previous-round ticket or input was accepted");
    std::cout << "new round discards stale input and resume ticket\n";
}
