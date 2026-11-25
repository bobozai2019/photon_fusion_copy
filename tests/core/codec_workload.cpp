#include "state/replication.hpp"
#include "state/snapshot_codec.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using namespace netcode;

double process_ms() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        throw std::runtime_error("process times");
    auto ticks = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) / 10000.;
#else
    return 1000. * std::clock() / CLOCKS_PER_SEC;
#endif
}

int main() try {
    Schemas schemas;
    schemas.add({1, 1, {{1, FieldType::Int}, {2, FieldType::String, 128}, {3, FieldType::IntList, 64}}});
    AuthorityState host(schemas);
    ReplicaState guest(schemas);
    SnapshotCodec sender, receiver;
    const std::string label(80, 'Q');
    IntArray inventory(40);
    for (int i = 0; i < 40; ++i)
        inventory[i] = i * 7937;
    host.create(1, 1, 1, {int32_t(0), label, inventory}, 1);
    host.attach(1, true);
    size_t sent = 0, delivered = 0, retained = 0, raw = 0, encoded = 0;
    auto start = std::chrono::steady_clock::now();
    auto cpu = process_ms();
    for (int tick = 1; tick <= 600; ++tick) {
        inventory[tick % 40] = tick;
        host.capture(1, {int32_t(tick), label, inventory}, tick);
        for (const auto& packet : host.packets(1, 1, 1, tick)) {
            auto data = sender.encode(packet, 1, 1);
            raw += packet.size();
            encoded += data.size();
            ++sent;
            if (tick % 10 == 0 && tick != 600)
                continue;
            Reader r(data);
            GameMessage type;
            if (!read_game_header(r, type, 1, 1))
                throw std::runtime_error("header");
            AppliedSnapshot changes;
            if (type == GameMessage::CompactSnapshot) {
                uint32_t sequence, missing;
                Bytes plain;
                if (!receiver.decode(r, plain, sequence, missing) ||
                    !std::equal(plain.begin(), plain.end(), packet.begin() + 17, packet.end()))
                    throw std::runtime_error("changed payload");
                Reader decoded(plain);
                if (!guest.apply(decoded, 1, 1, changes))
                    throw std::runtime_error("compressed application");
                receiver.remember(sequence, std::move(plain));
                sender.acknowledge(sequence);
            } else if (!guest.apply(r, 1, 1, changes))
                throw std::runtime_error("raw application");
            Reader ack(changes.ack);
            if (!read_game_header(ack, type, 1, 1) || !host.acknowledge(1, ack))
                throw std::runtime_error("ack");
            retained =
                std::max(retained, sender.retained_payload_bytes() + receiver.retained_payload_bytes());
            ++delivered;
        }
    }
    if (guest.objects().at(1).values != host.get(1)->values || encoded >= raw || !retained)
        throw std::runtime_error("codec convergence or compression");
    const auto us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
            .count();
    const auto cpu_ms = process_ms() - cpu;
    const auto& final = guest.objects().at(1).values;
    uint32_t digest = 2166136261u;
    auto mix = [&](uint32_t value) { digest = (digest ^ value) * 16777619u; };
    mix(std::get<int32_t>(final[0]));
    for (unsigned char byte : std::get<std::string>(final[1])) mix(byte);
    for (auto value : std::get<IntArray>(final[2])) mix(value);
    if (digest != 1124070639u) throw std::runtime_error("workload final digest");
    std::cout << "final_digest=" << digest << " frames=" << sent << " delivered=" << delivered << " raw_bytes=" << raw
              << " encoded_bytes=" << encoded << " retained_payload_peak_bytes=" << retained
              << " elapsed_us=" << us << " process_cpu_ms=" << cpu_ms << "\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
}
