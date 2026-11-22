#include "state/replication.hpp"
#include "transport/memory.hpp"

#include <cstdlib>
#include <iostream>

namespace {

using namespace netcode;

void check(bool ok, const char* message)
{
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}

}

int main()
{
    using namespace netcode;
    Values rich{Vector4{{1,2,3,4}},Color{{.25f,0,1,1}},ObjectReference{0xfedcba9876543210ull},
                Bytes{255,0},Vector2Array{{1,-2}},IntMap{{-1,42},{7,-6}},IntLinkedList{2,2,-1}};
    for (const auto& v:rich) {
        auto kind=FieldType(v.index());
        Field f{1,kind,static_cast<uint16_t>(field_capacity({1,kind}) ? 0 : 3)};
        check(valid_value(f,v),"new field rejected");
        Writer w; write_value(w,v); Reader r(w.bytes()); Value decoded;
        check(read_value(r,f,decoded) && r.done() && v==decoded,"new field did not round trip");
        auto short_data=w.bytes(); short_data.pop_back(); Reader short_read(short_data);
        check(!read_value(short_read,f,decoded),"truncated field accepted");
    }
    check(!valid_value({1,FieldType::ByteArray,2},Bytes{1,2,3}),"capacity ignored");
    Writer duplicate; duplicate.u16(2);
    for(int i=0;i<2;++i){duplicate.u32(1); duplicate.u32(7);}
    Reader map_read(duplicate.bytes()); Value bad_map;
    check(!read_value(map_read,{1,FieldType::IntDictionary,2},bad_map),"duplicate map key accepted");
    constexpr RoomId room = 10;
    constexpr RoundId round = 2;
    Schemas schemas;
    check(schemas.add({1, 1, {{10, FieldType::Float}, {20, FieldType::Float}, {30, FieldType::Flag}}}),
          "Schema rejected");
    check(!schemas.add({2, 1, {{10, FieldType::Float}, {10, FieldType::Flag}}}), "Duplicate field allowed");
    AuthorityState a(schemas);
    ReplicaState b(schemas);
    MemoryTransport host, guest;
    host.link(guest);
    const auto to_guest = host.poll({}).front().peer;
    const auto to_host = guest.poll({}).front().peer;

    auto send = [&](Tick tick) {
        for (const auto& packet : a.packets(to_guest, room, round, tick)) {
            check(host.send(to_guest, Lane::Game, packet), "Snapshot send failed");
        }
    };
    auto applied = [&](bool discard_ack = false) {
        unsigned changes = 0;
        for (const auto& event : guest.poll({})) {
            Reader reader(event.payload);
            GameMessage type;
            check(read_game_header(reader, type, room, round) && type == GameMessage::Snapshot, "Bad header");
            AppliedSnapshot result;
            check(b.apply(reader, room, round, result), "Snapshot application failed");
            changes += static_cast<unsigned>(result.changed.size());
            if (!discard_ack) check(guest.send(to_host, Lane::Game, result.ack), "Ack send failed");
        }
        return changes;
    };
    auto acknowledge = [&] {
        for (const auto& event : host.poll({})) {
            Reader reader(event.payload);
            GameMessage type;
            check(read_game_header(reader, type, room, round) && type == GameMessage::StateAck &&
                  a.acknowledge(to_guest, reader), "Ack failed");
        }
    };

    check(a.create(1, 1, 2, {0.0f, 3.0f, false}, 1), "Create failed");
    a.attach(to_guest);
    send(1);
    check(applied() == 1, "First snapshot missing");
    acknowledge();
    check(a.baseline_complete(to_guest), "Baseline fence incomplete");

    Values source{0.75f, 3.0f, false};
    check(a.capture(1, source, 2), "Capture failed");
    source[0] = -0.5f;
    check(std::get<float>(a.get(1)->values[0]) == 0.75f, "Source write affected collected snapshot");
    host.next_game(Delivery::Hold);
    send(3);
    source[2] = true;
    check(a.capture(1, source, 4), "Capture of final value failed");
    host.next_game(Delivery::Drop);
    send(6);
    check(applied() == 0, "Dropped update applied");
    host.next_game(Delivery::Duplicate);
    send(7);
    check(applied(true) == 1, "Duplicate final update applied more than once");
    check(b.objects().at(1).values == source && b.objects().at(1).sample_tick == 4,
          "Last update did not converge or independent field changed");
    host.release_held();
    check(applied() == 0 && b.objects().at(1).values == source, "Late snapshot rolled back state");
    acknowledge();
    send(8);
    check(applied() == 0 && b.objects().at(1).values == source, "Unacked final snapshot retry changed state");
    acknowledge();
    check(a.packets(to_guest, room, round, 9).empty(), "Acknowledged state sent again");

    check(a.destroy(1, 10) && !a.create(1, 1, 2, source, 11), "Can reuse locally destroyed ID");
    send(10);
    check(applied() == 1 && b.objects().empty(), "Tombstone did not invalidate instance");
    acknowledge();
    check(a.active().empty() && a.packets(to_guest, room, round, 11).empty(), "Tombstone not retired");
    std::cout << "0 -> 0.75 observed at capture tick 2 -> -0.5 applied from capture tick 4"
                 " after drop at send tick 6 and retry at 7; duplicate, reorder and deletion verified\n";
}
