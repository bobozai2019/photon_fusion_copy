#include "runner.hpp"

#include <iostream>
#include <thread>

int main(int argc, char** argv)
{
    try {
        auto options = yard::parse_options(argc, argv);
        if (options.host || options.scenario == yard::Scenario::None) {
            std::cerr << "Usage: netcode-guest --join IPV4 --port PORT --scenario stress [--seconds N]\n";
            return 2;
        }
        yard::Runner guest(std::move(options));
        while (!guest.finished()) {
            guest.update(netcode::Clock::now());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return guest.status();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
