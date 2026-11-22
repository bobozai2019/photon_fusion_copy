#include "session/script_game.hpp"
#include "session/game_wire.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace {
using namespace netcode;
using namespace std::chrono_literals;

void check(bool value, const char* detail) {
    if (!value) throw std::runtime_error(detail);
}

class Trace final : public Transport {
public:
    explicit Trace(std::unique_ptr<Transport> link) : link_(std::move(link)) {}
    bool send(Peer peer, Lane lane, const Bytes& payload) override {
        const bool accepted = link_->send(peer, lane, payload);
        if (accepted && !payload.empty()) {
            auto data = payload;
            if (lane == Lane::Control && data.size() == 40 && data[0] == 1 && data[1] == 3) {
                std::fill(data.begin() + 22, data.begin() + 38, 0); // Random resume key.
            } else if (lane == Lane::Game && data.size() > 34 && data[0] == uint8_t(GameMessage::Snapshot) && data[21] == 1) {
                std::fill(data.begin() + 17, data.begin() + 21, 0); // Packet tick.
                std::fill(data.begin() + 26, data.begin() + 34, 0); // Object revision and sample tick.
            } else if (lane != Lane::Game || data[0] != uint8_t(GameMessage::Rpc)) {
                return accepted;
            }
            if (seen_.emplace(peer, lane, data).second) {
                const char* hex = "0123456789abcdef";
                std::cout << "TRACE " << (lane == Lane::Control ? "Control " : "Game ");
                for (const auto byte : data) std::cout << hex[byte >> 4] << hex[byte & 15];
                std::cout << std::endl;
            }
        }
        return accepted;
    }
    std::vector<Event> poll(TimePoint now) override { return link_->poll(now); }
    void close(Peer peer) override { link_->close(peer); }
    Traffic traffic() const override { return link_->traffic(); }
private:
    std::unique_ptr<Transport> link_;
    std::set<std::tuple<Peer, Lane, Bytes>> seen_;
};

void save_ticket(const char* path, const ResumeTicket& ticket) {
    Writer packet;
    packet.u64(ticket.room);
    packet.u64(ticket.round);
    packet.u32(ticket.player);
    for (const auto word : ticket.key) packet.u32(word);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const auto& bytes = packet.bytes();
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    check(bool(file), "Cannot save ticket");
}

ResumeTicket read_ticket(const char* path) {
    std::ifstream file(path, std::ios::binary);
    const Bytes bytes(std::istreambuf_iterator<char>(file), {});
    Reader packet(bytes);
    ResumeTicket result{};
    check(packet.u64(result.room) && packet.u64(result.round) && packet.u32(result.player), "Cannot read ticket");
    for (auto& word : result.key) check(packet.u32(word), "Cannot read ticket key");
    return result;
}

int run(bool host, bool resume, uint16_t port, const char* ticket_path) {
    DirectConfig config{"127.0.0.1", port};
    SessionOptions options;
    options.host = host;
    options.room = 812;
    options.compact = false;
    options.extended_join = true;
    options.admission = {2, false, false};
    if (resume) options.resume = read_ticket(ticket_path);
    std::istringstream text("schema 1 1 Board\nint 1 . score\n");
    const ReplicationLayout layout(text);
    auto link = host ? listen_direct(config) : connect_direct(config);
    ScriptGame game(std::make_unique<Trace>(std::move(link)), options, layout);
    check(game.model(1, {1}, ClaimMode::Transaction, 0), "Cannot register writer");
    check(game.register_rpc(41, int(Direction::Server), true, 1, false, true), "Cannot register Proof");
    check(game.register_rpc(42, int(Direction::Client), true, 1, false, true), "Cannot register Reply");
    std::cout << "LISTEN " << (host ? "host" : "guest") << std::endl;

    int proofs = 0, replies = 0;
    bool requested = false, sent = false;
    std::optional<TimePoint> exit_at;
    const auto expected_phase = resume ? 2 : 1;
    const auto deadline = Clock::now() + 15s;
    while (Clock::now() < deadline) {
        const auto now = Clock::now();
        game.pump(now);
        auto& session = game.session();
        for (const auto& event : game.take_events()) {
            if (host && event.kind == ScriptEventKind::Request)
                check(game.answer({event.object, event.player, event.epoch, event.sequence}, true), "Cannot grant write authority");
        }
        if (host && !session.round() && session.host()->members().size() == 2) {
            check(session.start(now) && session.world().spawn(1, 1, {int32_t(7)}) == 1, "Cannot start");
        }
        Values current;
        const bool readable = game.read(1, false, current);
        if (!host && session.round() && session.local() == 2 && readable) {
            if (!resume && !requested && current == Values{int32_t(7)}) requested = session.writers().request(1);
            if (!resume && session.writers().writer(1) == 2 && current != Values{int32_t(42)})
                check(game.submit(1, {int32_t(42)}), "Cannot submit held field");
            if (!sent && current == Values{int32_t(42)})
                sent = session.rpc().invoke(0, 41, {static_cast<uint8_t>(expected_phase)});
        }
        while (const auto* call = game.next_rpc()) {
            if (host) {
                check(call->method == 41 && call->sender == 2 && ++proofs <= 2 &&
                      call->arguments == Bytes{static_cast<uint8_t>(proofs)}, "Unexpected or duplicate proof");
            } else {
                check(call->method == 42 && call->sender == 1 && call->arguments == Bytes{42} && ++replies == 1,
                      "Unexpected or duplicate reply");
                if (!resume) save_ticket(ticket_path, session.guest()->ticket());
                std::cout << "GUEST_OK phase=" << expected_phase << " player=2 value=42" << std::endl;
                exit_at = now + 180ms;
            }
            game.pop_rpc();
        }
        if (host && proofs > replies && readable && current == Values{int32_t(42)}) {
            check(session.rpc().invoke(0, 42, {42}, 2), "Cannot send reply");
            std::cout << "HOST_OK phase=" << ++replies << " value=42" << std::endl;
            if (proofs == 2) exit_at = now + 600ms;
        }
        if (exit_at && now >= *exit_at) return 0;
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("Direct compatibility scenario timed out");
}
}

int main(int argc, char** argv) try {
    check(argc == 4, "Usage: compat_probe host|join|resume port ticket");
    const std::string_view role(argv[1]);
    return run(role == "host", role == "resume", static_cast<uint16_t>(std::stoul(argv[2])), argv[3]);
} catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
}
