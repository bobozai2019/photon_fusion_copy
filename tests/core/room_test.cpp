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
    {
        MemoryTransport h, a, b;
        RoomHost room(h, 301, {2,true,false});
        check(room.set_player_ttl(100), "ttl configuration");
        RoomGuest first(a,301);
        h.link(a);
        pump(first,a); pump(room,h); pump(first,a);
        check(room.start(), "ttl round");
        pump(first,a);
        auto ticket = first.ticket();
        auto cut = TimePoint{};
        a.close(first.peer());
        for (const auto& e : h.poll(cut)) room.receive(e,cut);
        const auto before = cut + std::chrono::milliseconds(99);
        check(room.expire(before).empty(), "seat expired early");
        MemoryTransport retained_link;
        RoomGuest retained(retained_link,301,ticket);
        h.link(retained_link); pump(retained,retained_link);
        for (const auto& e : h.poll(before)) room.receive(e,before);
        pump(retained,retained_link);
        check(retained.resumed() && retained.ticket().player == ticket.player, "ticket rejected before TTL");
        retained_link.close(retained.peer());
        cut = before;
        for (const auto& e : h.poll(cut)) room.receive(e,cut);
        check(room.expire(cut+std::chrono::milliseconds(100)) == std::vector<PlayerId>{ticket.player},
              "seat did not expire");
        RoomGuest returning(b,301,ticket);
        h.link(b); pump(returning,b); pump(room,h); pump(returning,b);
        check(returning.rejected() == Reject::BadResume, "expired ticket restored");
        MemoryTransport fresh_link;
        RoomGuest fresh(fresh_link,301,std::nullopt,false,true);
        h.link(fresh_link); pump(fresh,fresh_link); pump(room,h); pump(fresh,fresh_link);
        check(fresh.ticket().player > ticket.player && !fresh.resumed() && room.members().size() == 2,
              "expired seat did not admit a fresh player");
    }
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
    check(guest.players() == std::vector<PlayerId>{host_player, guest.ticket().player}, "Guest roster missing");
    check(host.chat("Welcome") && guest.chat("\xe4\xbd\xa0\xe5\xa5\xbd"), "Room chat rejected");
    check(!host.chat(" ") && !guest.chat(std::string(room_chat_limit + 1, 'x')), "Invalid chat accepted");
    pump(host, host_link);
    pump(guest, guest_link);
    check(host.messages().size() == 2 && guest.messages().size() == 2 &&
          guest.messages()[0].sender == host_player && guest.messages()[0].text == "Welcome" &&
          guest.messages()[1].sender == guest.ticket().player && guest.messages()[1].text == "\xe4\xbd\xa0\xe5\xa5\xbd",
          "Chat did not arrive in host order with the correct identities");

    for (int attempt = 0; attempt < 2; ++attempt) {
        RoomGuest visitor(extra_link);
        host_link.link(extra_link);
        pump(visitor, extra_link);
        pump(host, host_link);
        pump(visitor, extra_link);
        pump(guest, guest_link);
        check(visitor.phase() == RoomPhase::Waiting && visitor.messages().size() == 2 &&
              guest.players().size() == 3 && guest.players().back() == visitor.ticket().player,
              "New or returning guest did not receive the room and its chat");
        check(!host.kick(host_player) && host.kick(visitor.ticket().player), "Kick not restricted to other members");
        pump(visitor, extra_link);
        pump(guest, guest_link);
        pump(host, host_link);
        check(visitor.rejected() == Reject::Kicked && visitor.phase() == RoomPhase::Ended &&
              host.members().size() == 2 && guest.players().size() == 2, "Kick did not remove the guest and update the roster");
    }
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
    std::cout << "room isolation, lobby roster, chat, kick and rejoin, roster lock, 2 rounds, stale ticket, host close\n";
}
