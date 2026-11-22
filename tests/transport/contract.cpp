#include "module.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#define CHECK(expression) do { if (!(expression)) throw std::runtime_error(#expression); } while (false)

struct Seen {
    uint32_t type, lane, reason;
    nc_transport_peer peer;
    std::string payload;
};

static void NC_TRANSPORT_CALL receive(void *context, const nc_transport_event *event) {
    auto &events = *static_cast<std::vector<Seen> *>(context);
    const auto &payload = event->payload;
    events.push_back({event->type, event->lane, event->reason, event->peer,
                      payload.size ? std::string(reinterpret_cast<const char *>(payload.data), payload.size) : ""});
}

static nc_transport_bytes bytes(const char *content) {
    return {reinterpret_cast<const uint8_t *>(content), static_cast<uint32_t>(std::strlen(content))};
}

int main(int argc, char **argv) try {
    CHECK(argc == 2);
    Module module(std::filesystem::u8path(argv[1]));
    const auto get_api = module.entry();
    CHECK(get_api);
    struct {
        nc_transport_api_v1 api{};
        uint64_t guard = 123456;
    } output;
    auto &api = output.api;
    CHECK(get_api(77, sizeof api, &api) == NC_TRANSPORT_UNSUPPORTED_ABI);
    CHECK(get_api(1, sizeof api - 1, &api) == NC_TRANSPORT_SHORT_TABLE);
    CHECK(api.abi_version == 0 && output.guard == 123456);
    CHECK(get_api(1, sizeof output, &output) == NC_TRANSPORT_OK);
    CHECK(api.abi_version == 1 && api.struct_size == sizeof api && output.guard == 123456);
    CHECK(api.provider_id.size == 7 && std::memcmp(api.provider_id.data, "fixture", 7) == 0);
    CHECK(api.build_id.size > 0 && api.open && api.send_batch && api.poll && api.close_peer &&
          api.destroy && api.get_caps && api.get_stats);

    nc_transport_instance *instance = nullptr;
    nc_transport_open_args args{NC_TRANSPORT_CONNECT, bytes("invalid"), {}};
    CHECK(api.open(&args, &instance) == NC_TRANSPORT_INVALID_CONFIG && !instance);
    args.endpoint = bytes("echo");
    CHECK(api.open(&args, &instance) == NC_TRANSPORT_OK && instance);
    std::unique_ptr<nc_transport_instance, decltype(api.destroy)> guard(instance, api.destroy);
    nc_transport_caps caps{};
    CHECK(api.get_caps(instance, &caps) == NC_TRANSPORT_OK);
    CHECK(caps.lane[NC_TRANSPORT_CONTROL].flags & NC_TRANSPORT_RELIABLE_ORDERED);
    CHECK(caps.lane[NC_TRANSPORT_CONTROL].max_payload == NC_TRANSPORT_CONTROL_MAX);
    CHECK(caps.lane[NC_TRANSPORT_GAME].max_payload == NC_TRANSPORT_GAME_MAX);

    std::vector<Seen> events;
    const auto poll = [&](uint64_t now, uint32_t budget) {
        CHECK(api.poll(instance, now, budget, receive, &events) == NC_TRANSPORT_OK);
    };
    poll(10, 0);
    CHECK(events.empty());
    poll(11, 1);
    CHECK(events.size() == 1 && events.back().type == NC_TRANSPORT_OPEN && events.back().peer != 0);
    const auto peer = events.back().peer;

    char first[] = "first";
    std::array batch{bytes(first), bytes("second")};
    auto accepted = 99u;
    std::array invalid{batch.front(), nc_transport_bytes{batch.front().data, NC_TRANSPORT_GAME_MAX + 1}};
    CHECK(api.send_batch(instance, peer, NC_TRANSPORT_GAME, invalid.data(), 2, &accepted) == NC_TRANSPORT_TOO_LARGE);
    CHECK(accepted == 0);
    CHECK(api.send_batch(instance, peer, NC_TRANSPORT_GAME, batch.data(), 2, &accepted) == NC_TRANSPORT_WOULD_BLOCK);
    CHECK(accepted == 1);
    first[0] = 'X';
    CHECK(api.send_batch(instance, peer, NC_TRANSPORT_GAME, &batch.back(), 1, &accepted) == NC_TRANSPORT_WOULD_BLOCK);
    CHECK(accepted == 0);
    poll(15, 0);
    nc_transport_stats stats{};
    CHECK(api.get_stats(instance, peer, &stats) == NC_TRANSPORT_OK);
    CHECK(stats.lane[NC_TRANSPORT_GAME].queued_bytes == 5 && stats.lane[NC_TRANSPORT_GAME].oldest_queued_us == 4);
    poll(16, 1);
    CHECK(events.size() == 2 && events.back().payload == "first" && events.back().lane == NC_TRANSPORT_GAME);
    CHECK(api.send_batch(instance, peer, NC_TRANSPORT_GAME, &batch.back(), 1, &accepted) == NC_TRANSPORT_OK);
    CHECK(accepted == 1);
    CHECK(api.close_peer(instance, peer) == NC_TRANSPORT_OK);
    CHECK(api.close_peer(instance, peer) == NC_TRANSPORT_PEER_CLOSED);
    CHECK(api.send_batch(instance, peer, NC_TRANSPORT_GAME, batch.data(), 1, &accepted) == NC_TRANSPORT_PEER_CLOSED);
    CHECK(accepted == 0);
    poll(17, 1);
    CHECK(events.size() == 3 && events.back().payload == "second");
    poll(18, 1);
    CHECK(events.size() == 4 && events.back().type == NC_TRANSPORT_CLOSED && events.back().reason == NC_TRANSPORT_CLOSE_LOCAL);
    poll(19, 99);
    CHECK(events.size() == 4);
    CHECK(api.get_stats(instance, NC_TRANSPORT_ALL_PEERS, &stats) == NC_TRANSPORT_OK);
    const auto &game = stats.lane[NC_TRANSPORT_GAME];
    CHECK(game.sent_bytes == 11 && game.received_bytes == 11 && game.queued_bytes == 0 && game.peak_queued_bytes == 6);
    std::cout << "fixture ABI 1: Open, Game(first), Game(second), Closed(Local); accepted prefix 1/2, bytes 11/11\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
