#include "state/replication.hpp"
#include "state/layout.hpp"
#include "state/snapshot_buffer.hpp"

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace netcode;
constexpr Peer b = 1, c = 2;
constexpr RoomId room = 73;
constexpr RoundId round_id = 1;
void check(bool valid, const char* reason) { if (!valid) throw std::runtime_error(reason); }

AppliedSnapshot deliver(const Bytes& packet, ReplicaState& replica, AuthorityState* host = nullptr, Peer peer = b)
{
    GameMessage type;
    Reader data(packet);
    AppliedSnapshot result;
    check(read_game_header(data, type, room, round_id) && type == GameMessage::Snapshot &&
          replica.apply(data, room, round_id, result), "invalid snapshot");
    if (host) {
        Reader ack(result.ack);
        check(read_game_header(ack, type, room, round_id) && type == GameMessage::StateAck &&
              host->acknowledge(peer, ack), "invalid acknowledgement");
    }
    return result;
}
}

int main()
{
    std::istringstream config("schema 7 1 Inventory\nint 1 . count\nstring 2 . name 16\nint_array 3 . ids 8\n");
    ReplicationLayout inventory(config);
    check(inventory.schemas().valid(7, {std::int32_t(9), std::string("Bag"), IntArray{11, 12}}) &&
          inventory.bindings(7).at(1).property == "name", "generic config required game-specific schemas");
    SnapshotBuffer history;
    for (const auto tick : {10u, 20u, 14u}) history.observe(Object{1, 7, 1, tick, tick, {float(tick)}});
    check(history.ready() && history.at(12.5)->weight == .25f && history.at(14)->to.sample_tick == 20 &&
          history.at(5)->from.sample_tick == 10 && history.at(50)->weight == 1.f, "sample clock followed packet arrival");
    Schemas schemas;
    const Schema profile{1, 1, {{1, FieldType::Float}, {2, FieldType::Flag}, {3, FieldType::Int},
        {4, FieldType::Vec2}, {5, FieldType::Vec3}, {6, FieldType::Quat}, {7, FieldType::String, 16},
        {8, FieldType::IntList, 4}, {9, FieldType::FloatList, 4}}};
    const Values original{.25f, true, INT32_MIN, Vector2{-3, 2}, Vector3{-1, 4, 7}, Rotation{0, 0, -1, 0},
        std::string(u8"\u73a9\u5bb6\U0001f680"), IntArray{INT32_MAX, -16777219}, FloatArray{-.5f, 3.f}};
    check(schemas.add(profile) && schemas.valid(1, original), "profile schema");
    check(!schemas.add({8, 1, {{1, FieldType::FloatList, 400}}}) &&
          !schemas.add({9, 1, {{1, FieldType::String}}}), "unbounded or oversize schema");
    auto invalid = [&](std::size_t slot, Value value) {
        auto v = original;
        v[slot] = value;
        check(!schemas.valid(1, v), "invalid typed value accepted");
        if (original[slot].index() == value.index()) {
            Writer wire;
            write_value(wire, value);
            Reader read(wire.bytes());
            Value decoded;
            check(!read_value(read, profile.fields[slot], decoded), "invalid wire value accepted");
        }
    };
    invalid(2, true); invalid(3, Vector2{NAN, 0}); invalid(5, Rotation{0, 0, 0, 0});
    invalid(6, std::string("\xc0\x80", 2)); invalid(6, std::string(17, 'a')); invalid(7, IntArray(5));
    invalid(8, FloatArray{INFINITY});

    AuthorityState host(schemas);
    ReplicaState bob(schemas), charlie(schemas), late(schemas);
    check(host.create(1, 1, 1, original, 1, 20) && host.create(2, 1, 2, original, 1, 20, {1}),
          "create scoped objects");
    check(!host.create(3, 1, 1, original, 1, 20, {2, 3}), "missing or cyclic dependency accepted");
    host.attach(b); host.attach(c);
    auto packet = [&](Peer p, Tick t) {
        auto packets = host.packets(p, room, round_id, t);
        check(packets.size() == 1, "expected a single packet");
        return packets.front();
    };
    const auto root = packet(b, 1);
    check(deliver(root, bob, &host).ready == std::vector<ObjectId>{1} && !host.baseline_complete(b), "parent baseline fence");
    const auto child = packet(b, 2);
    check(deliver(child, bob, &host).ready == std::vector<ObjectId>{2} && bob.objects().at(2).values == original &&
          host.baseline_complete(b), "typed or dependency baseline");
    check(deliver(child, late).ready.empty() && late.objects().empty(), "dependent published before its parent");
    check(deliver(root, late).ready == std::vector<ObjectId>{1, 2}, "late parent did not release dependent in order");
    check(late.set_scene_ready(20, false).changed == std::vector<ObjectId>{1, 2} && late.objects().empty(),
          "unloaded local scene still exposed live objects");
    check(deliver(child, late).ready.empty() && late.set_scene_ready(20, true).ready == std::vector<ObjectId>{1, 2},
          "scene completion did not publish its retained baseline in dependency order");
    deliver(packet(c, 2), charlie, &host, c); deliver(packet(c, 2), charlie, &host, c);
    check(host.set_interest(b, 11), "leave scene");
    check(host.packets(c, room, round_id, 3).empty(), "another player's scope resent unchanged fields");
    const auto hide = packet(b, 3);
    check(deliver(hide, bob, &host).hidden == std::vector<ObjectId>{1, 2} && !host.known(b, 2), "hide failed");
    auto changed = original;
    changed[2] = std::int32_t(16777219); changed[6] = std::string(); changed[7] = IntArray{8}; changed[8] = FloatArray{};
    check(host.capture(2, changed, 4) && host.packets(b, room, round_id, 4).empty(), "hidden data sent");
    deliver(packet(c, 4), charlie, &host, c);
    check(charlie.objects().at(2).values == changed, "other peer missed independent changes");
    check(host.set_interest(b, 20, std::set<ObjectId>{2}) && host.packets(b, room, round_id, 5).empty(),
          "missing selected prerequisite");
    check(host.set_interest(b, 20), "return to scene");
    deliver(packet(b, 6), bob, &host);
    const auto reentry = packet(b, 7);
    check(deliver(reentry, bob, &host).ready == std::vector<ObjectId>{2} && bob.objects().at(2).values == changed,
          "return lacked latest full values");
    deliver(hide, bob); deliver(child, bob);
    check(bob.objects().at(2).values == changed, "old hide or live packet replaced reentry");
    check(host.set_interest(b, 11), "second leave");
    deliver(packet(b, 8), bob, &host);
    check(host.destroy(2, 9), "destroy hidden object");
    deliver(packet(b, 9), bob, &host);
    deliver(reentry, bob); deliver(child, bob);
    check(host.set_interest(b, 20), "return after destruction");
    deliver(packet(b, 10), bob, &host);
    check(!bob.objects().count(2) && host.packets(b, room, round_id, 11).empty(), "late packet revived destroyed object");
    check(host.create(3, 1, 1, original, 12, 20, {}, {InterestKind::Area, 9, 4}), "area object");
    check(host.packets(b, room, round_id, 13).empty(), "unsubscribed recipient saw area object");
    check(host.subscribe(b, InterestKind::User, {9}) && host.packets(b, room, round_id, 13).empty(),
          "user and area keys collided");
    check(host.subscribe(b, InterestKind::Area, {9}), "area subscription");
    deliver(packet(b, 13), bob, &host);
    check(bob.objects().count(3) && host.capture(3, changed, 14), "baseline waited for rate divider");
    check(host.packets(b, room, round_id, 14).empty() && host.packets(b, room, round_id, 15).empty(), "rate divider");
    deliver(packet(b, 16), bob, &host);
    check(bob.objects().at(3).values == changed, "slow object lost update");
    check(host.set_distribution(3, {InterestKind::User, 42, 8}), "reclassify");
    check(deliver(packet(b, 17), bob, &host).hidden == std::vector<ObjectId>{3}, "hide delayed by rate");
    check(host.subscribe(b, InterestKind::User, {42}), "team subscription");
    deliver(packet(b, 18), bob, &host);
    check(host.destroy(3, 19) && deliver(packet(b, 19), bob, &host).destroyed == std::vector<ObjectId>{3}, "slow destroy delayed");
    check(host.create(4, 1, 1, original, 20, 20, {}, {InterestKind::Area, 8, 1, {9}}), "multi-key create");
    check(host.subscribe_rates(b, InterestKind::Area, {{8, 8}, {9, 2}}), "overlap rates");
    check(host.subscribe_rates(c, InterestKind::Area, {{8, 8}}), "distant client");
    deliver(packet(b, 20), bob, &host);
    deliver(packet(c, 20), charlie, &host, c);
    check(host.capture(4, changed, 21) && host.packets(b, room, round_id, 21).empty(), "near rate");
    deliver(packet(b, 23), bob, &host);
    check(host.packets(c, room, round_id, 23).empty(), "different rates for same object");
    deliver(packet(c, 25), charlie, &host, c);
    check(bob.objects().at(4).values == charlie.objects().at(4).values, "skipped frame starved far client");
    std::cout << "typed kinds=9 exact_int=16777219 dependency_ready=1,2 hidden_stale_ignored=1\n";
}
