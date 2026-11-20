#include "session/room.hpp"

#include <iostream>
#include <string_view>
#include <thread>

namespace {

using namespace netcode;
using namespace std::chrono_literals;

int host_process(const DirectConfig& config, bool stall)
{
    auto link = listen_direct(config);
    RoomHost room(*link, 410);
    std::cout << "listening port=" << config.port << std::endl;
    const auto started = Clock::now();
    TimePoint next{};
    for (;;) {
        const auto now = Clock::now();
        for (const auto& event : link->poll(now)) room.receive(event);
        if (now - started > 14s) return 1;
        if (!room.round() && room.members().size() == 2) {
            room.start();
            std::cout << "round=1 seats=2" << std::endl;
            next = now + 1500ms;
            if (stall) {
                // Deliver the start before stopping the host event loop.
                for (int i = 0; i < 5; ++i) {
                    link->poll(Clock::now());
                    std::this_thread::sleep_for(10ms);
                }
                std::cout << "stalled" << std::endl;
                std::this_thread::sleep_for(2600ms);
                return 0;
            }
        } else if (room.round() && now >= next) {
            if (room.round() == 1 && room.phase() == RoomPhase::Playing) {
                room.end_round();
                std::cout << "waiting" << std::endl;
                next = now + 300ms;
            } else if (room.round() == 1) {
                room.start();
                std::cout << "round=2 seats=2" << std::endl;
                next = now + 350ms;
            } else if (room.phase() == RoomPhase::Playing) {
                room.end_room();
                next = now + 300ms;
            } else if (room.phase() == RoomPhase::Ended) {
                return 0;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
}

int guest_process(const DirectConfig& config, std::string_view expectation)
{
    auto link = connect_direct(config);
    RoomGuest room(*link, expectation == "wrong" ? 411 : 410);
    int rounds = 0;
    RoundId latest = 0;
    PlayerId player = 0;
    const auto deadline = Clock::now() + 10s;
    while (Clock::now() < deadline) {
        for (const auto& event : link->poll(Clock::now())) {
            room.receive(event);
            if (room.phase() == RoomPhase::Playing && room.ticket().round > latest) {
                latest = room.ticket().round;
                if (player && room.ticket().player != player) return 1;
                player = room.ticket().player;
                ++rounds;
                std::cout << "entered round=" << latest << " player=" << player << std::endl;
            }
        }
        if (room.phase() == RoomPhase::Ended) {
            std::cout << "ended rounds=" << rounds
                      << " reject=" << static_cast<int>(room.rejected())
                      << " disconnect=" << static_cast<int>(room.disconnected()) << std::endl;
            if (expectation == "wrong") return room.rejected() == Reject::WrongRoom ? 0 : 1;
            if (expectation == "locked") return room.rejected() == Reject::Locked ? 0 : 1;
            if (expectation == "timeout") {
                return rounds == 1 && room.disconnected() == Disconnect::Timeout ? 0 : 1;
            }
            return rounds == 2 && room.disconnected() == Disconnect::Remote ? 0 : 1;
        }
        std::this_thread::sleep_for(5ms);
    }
    return 1;
}

}

int main(int argc, char** argv)
{
    if (argc != 4) return 2;
    try {
        const auto port = std::stoul(argv[2]);
        if (!port || port > 65535) return 2;
        const DirectConfig config{"127.0.0.1", static_cast<std::uint16_t>(port), 1200ms};
        const std::string_view role(argv[1]), mode(argv[3]);
        if (role == "host") return host_process(config, mode == "timeout");
        if (role == "guest") return guest_process(config, mode);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 2;
}
