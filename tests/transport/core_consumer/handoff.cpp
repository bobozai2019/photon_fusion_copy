#include "host/game.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
using namespace netcode;
using namespace std::chrono_literals;

void check(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}

void run(const ProviderConfig& provider, const char* main_endpoint, const char* standby_endpoint) {
    std::istringstream definition("schema 1 1 Board\nint 1 . score\n");
    const ReplicationLayout layout(definition);
    SessionOptions options;
    options.host = true;
    options.room = 416;
    options.admission = {2, true, false};
    auto host = open_provider_game(provider, main_endpoint, options, layout, nullptr);
    options.host = false;
    options.extended_join = true;
    auto standby = open_provider_game(provider, main_endpoint, options, layout, nullptr);
    const StandbyEndpoint endpoint{{}, 0, standby_endpoint};
    check(host.game->migration().add_standby(2, endpoint) && standby.game->migration().add_standby(2, endpoint),
          "Provider endpoints were not accepted for migration");
    auto advance = [&](auto predicate, const char* stage) {
        const auto deadline = Clock::now() + 6s;
        while (!predicate() && Clock::now() < deadline) {
            const auto now = Clock::now();
            host.game->pump(now);
            standby.game->pump(now);
            std::this_thread::sleep_for(4ms);
        }
        check(predicate(), stage);
    };
    advance([&] { return host.game->session().host()->members().size() == 2; }, "Join timed out");
    check(host.game->session().start(Clock::now()), "Cannot open round");
    auto object = host.game->session().world().spawn(1, 1, {int32_t(18)});
    check(object != 0, "Cannot spawn object");
    advance([&] { return standby.game->session().object(object) != nullptr; }, "Initial state timed out");
    auto& current = host.game->session();
    check(current.authority().capture(object, {int32_t(42)}, current.tick()), "Cannot capture final state");
    bool started = false;
    advance([&] { return started || (started = host.game->migration().handoff()); }, "Handoff refused");
    advance([&] {
        auto* winner = standby.game->active();
        return winner && winner->host() && winner->term() == 2 && !host.game->active();
    }, "Provider promotion timed out");
    const auto* restored = standby.game->session().object(object);
    check(standby.game->session().local() == 2 && restored && restored->values == Values{int32_t(42)},
          "Successor did not restore the final state");
    std::cout << "HANDOFF provider=" << standby.provider.label() << " owner=2 term=2 value=42" << std::endl;
}
}

int main(int argc, char** argv) try {
    check(argc == 5, "Usage: provider_handoff provider-id module primary-endpoint standby-endpoint");
    run({argv[1], std::filesystem::absolute(std::filesystem::u8path(argv[2])), {}}, argv[3], argv[4]);
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
