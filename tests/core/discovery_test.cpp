#include "session/game_session.hpp"
#include "transport/memory.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace netcode;
using namespace std::chrono_literals;
int main() try {
    const LanDiscoveryOptions options{"127.0.0.1", 43886};
    auto browser = lan_directory(options);
    SessionOptions config;
    config.host = true;
    config.room = 120;
    config.admission = {2, true, false};
    DirectConfig red_port{"127.0.0.1", 43887}, blue_port{"127.0.0.1", 43888};
    auto a = listen_direct(red_port);
    auto b = listen_direct(blue_port);
    GameSession red(*a, config);
    config.room = 121;
    GameSession blue(*b, config);
    auto populate = [&](GameSession& game, const char* name, const char* region, const char* mode,
                        uint16_t port) {
        RoomListing listing;
        listing.name = name;
        listing.build = "1.0";
        listing.region = region;
        listing.port = port;
        listing.properties["mode"] = mode;
        if (!game.advertise(lan_directory(options), listing))
            throw std::runtime_error("advertisement failed");
    };
    populate(red, "Red", "east", "puzzle", red_port.port);
    populate(blue, "Blue", "west", "coop", blue_port.port);
    std::unique_ptr<Transport> client_link, racing_link;
    std::unique_ptr<GameSession> guest, contender;
    auto until = [&](auto test) {
        auto deadline = Clock::now() + 4s;
        do {
            auto now = Clock::now();
            red.pump(now);
            blue.pump(now);
            if (guest)
                guest->pump(now);
            if (contender)
                contender->pump(now);
            browser->poll(now);
            if (test())
                return;
            std::this_thread::sleep_for(5ms);
        } while (Clock::now() < deadline);
        throw std::runtime_error("directory expectation timed out");
    };
    RoomFilter query{"east", "1.0", {{"mode", "puzzle"}}};
    until([&] { return browser->search({}).size() == 2; });
    auto hits = browser->search(query);
    if (hits.size() != 1 || hits[0].room != 120 || hits[0].endpoint != "127.0.0.1" ||
        hits[0].port != red_port.port)
        throw std::runtime_error("discovery filters or endpoint");
    config.host = false;
    config.room = hits[0].room;
    config.extended_join = true;
    std::optional<RoomListing> chosen;
    if (browser->join_random({"absent","1.0"}, [](auto){}) ||
        !browser->join_random(query, [&](auto listing) { chosen = std::move(listing); }) ||
        !chosen || chosen->room != hits[0].room)
        throw std::runtime_error("room join selection");
    client_link = connect_direct({chosen->endpoint, chosen->port});
    guest = std::make_unique<GameSession>(*client_link, config);
    until([&] { return guest->local() > 1; });
    racing_link = connect_direct({hits[0].endpoint, hits[0].port});
    contender = std::make_unique<GameSession>(*racing_link, config);
    until([&] { return contender->guest()->rejected() == Reject::Full && browser->search(query).empty(); });
    if (!red.start(Clock::now())) throw std::runtime_error("could not start discovered room");
    until([&] { return guest->round() != 0; });
    const auto ticket = guest->guest()->ticket();
    client_link->close(guest->guest()->peer());
    guest.reset();
    client_link.reset();
    until([&] { return red.host()->members().at(ticket.player).peer == 0; });

    // A reservation already occupies its seat; a returning player searches even full rooms.
    auto resume_filter = query;
    resume_filter.require_space = false;
    const auto reservations = browser->search(resume_filter);
    if (reservations.size() != 1 || reservations.front().room != ticket.room)
        throw std::runtime_error("reserved room missing in browser");
    const auto& reservation = reservations.front();
    config.resume = ticket;
    client_link = connect_direct({reservation.endpoint, reservation.port});
    guest = std::make_unique<GameSession>(*client_link, config);
    until([&] { return guest->guest()->resumed() && guest->round() == ticket.round; });
    if (guest->local() != ticket.player) throw std::runtime_error("reservation consumed another seat");
    blue.end(Clock::now());
    until([&] { return browser->search({}).empty(); });
    std::cout << "two LAN rooms, filter, real join, stale-list capacity race, resume reserved seat, expired advertisement passed\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
}
