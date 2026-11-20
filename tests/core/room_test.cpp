#include "session/room.hpp"
#include "transport/memory.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void check(bool ok, const char* message)
{
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}

template<class Room>
void pump(Room& room, netcode::Transport& transport)
{
    for (const auto& event : transport.poll({})) room.receive(event);
}

}

int main()
{
    using namespace netcode;
    MemoryTransport host_link, guest_link, extra_link, wrong_link;
    RoomHost host(host_link, 410);
    RoomGuest guest(guest_link, 410), extra(extra_link, 410), wrong(wrong_link, 411);
    host_link.link(wrong_link);
    pump(wrong, wrong_link);
    pump(host, host_link);
    pump(wrong, wrong_link);
    check(wrong.rejected() == Reject::WrongRoom && host.members().size() == 1, "Room isolation");

    host_link.link(guest_link);
    pump(guest, guest_link);
    pump(host, host_link);
    pump(guest, guest_link);
    check(guest.phase() == RoomPhase::Waiting && guest.seats() == 2, "Join failed");
    check(host.start() && !host.start(), "Start must be legal only in Waiting");
    pump(guest, guest_link);
    const auto ticket = guest.ticket();
    check(ticket.round == 1 && guest.phase() == RoomPhase::Playing, "First round rejected");

    host_link.link(extra_link);
    pump(extra, extra_link);
    pump(host, host_link);
    pump(extra, extra_link);
    check(extra.rejected() == Reject::Locked && host.members().size() == 2, "Roster lock failed");
    check(host.end_round(), "First round did not end");
    pump(guest, guest_link);
    check(guest.phase() == RoomPhase::Waiting && !guest.ticket().round, "Round identity survived end");

    RoomGuest replay(extra_link, 410, ticket);
    host_link.link(extra_link);
    pump(replay, extra_link);
    check(host.start(), "Second round did not start");
    pump(host, host_link);
    pump(replay, extra_link);
    pump(guest, guest_link);
    check(replay.rejected() == Reject::BadResume, "Old round ticket accepted");
    check(guest.ticket().player == ticket.player && guest.ticket().round == ticket.round + 1,
          "Room did not retain player or advance round");

    host.end_room();
    pump(guest, guest_link);
    check(guest.phase() == RoomPhase::Ended && guest.disconnected() == Disconnect::Remote,
          "Host close did not reach guest");
    std::cout << "room isolation, roster lock, 2 rounds, stale ticket, host close\n";
}
