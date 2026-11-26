#include "session/migration.hpp"
#include "transport/memory.hpp"
#include <iostream>
#include <stdexcept>

using namespace netcode;
using namespace std::chrono_literals;

void partition(bool majority) {
    auto a = std::make_unique<MemoryTransport>(), b = std::make_unique<MemoryTransport>(),
         c = std::make_unique<MemoryTransport>();
    a->link(*b);
    a->link(*c);
    std::map<uint16_t, MemoryTransport*> listeners;
    MigratingSession::Open open = [&](bool listen, const StandbyEndpoint& endpoint) {
        auto& listener = listeners[endpoint.port];
        auto link = std::make_unique<MemoryTransport>();
        if (listen)
            listener = link.get();
        else if (listener)
            listener->link(*link);
        else
            throw std::runtime_error("missing standby");
        return link;
    };
    SessionOptions opts;
    opts.host = true;
    opts.room = 540;
    opts.admission = {4, true, false};
    MigrationOptions policy;
    policy.standbys[2] = {"loopback", 100};
    policy.standbys[3] = {"loopback", 101};
    policy.lease = 360ms;
    Schemas schemas;
    schemas.add({1, 1, {{1, FieldType::Int}}});
    schemas.add({2, 1, {{1, FieldType::Int}}});
    schemas.add({3, 1, {{1, FieldType::Int}}});
    auto configure = [](GameSession& game) {
        game.writers().register_model(1, 1, {1});
        game.writers().allow_requests(1, {ClaimMode::GameGlobal});
        game.writers().register_model(2, 1, {1});
        ClaimPolicy attached{ClaimMode::PlayerAttached};
        attached.destroy = [&game](ObjectId id, Tick) { game.world().destroy(id); };
        game.writers().allow_requests(2, std::move(attached));
    };
    MigratingSession original(std::move(a), opts, schemas, policy, open, configure);
    opts.host = false;
    opts.extended_join = true;
    MigratingSession standby(std::move(b), opts, schemas, policy, open, configure),
        witness(std::move(c), opts, schemas, policy, open, configure);
    TimePoint now{};
    auto step = [&](bool run_original, bool run_witness) {
        now += 30ms;
        if (run_original)
            original.pump(now);
        standby.pump(now);
        if (run_witness)
            witness.pump(now);
    };
    for (int i = 0; i < 8; ++i)
        step(true, true);
    if (!original.active()->start(now))
        throw std::runtime_error("could not start election fixture");
    auto object = original.active()->world().spawn(1, 1, {int32_t(123)}, {0, 431});
    const auto avatar = original.active()->world().spawn(2, 1, {int32_t(1)});
    if (!avatar) throw std::runtime_error("old coordinator avatar missing before migration");
    const auto child = original.active()->world().spawn(3, 1, {int32_t(3)}, {0, 433});
    const auto parent = original.active()->world().spawn(3, 1, {int32_t(4)}, {0, 434});
    if (!child || !parent || !original.active()->world().reparent(child, parent))
        throw std::runtime_error("could not attach earlier child to later parent");
    for (int i = 0; i < 45; ++i)
        step(true, true);
    if (original.active()->world().spawn(1, 1, {int32_t(0)}) ||
        original.active()->writers().assign(object, 2, original.active()->tick()) ||
        witness.active()->writers().request(object))
        throw std::runtime_error("global master policy");
    if (!original.armed() || !standby.saved_tick() || !witness.armed())
        throw std::runtime_error("standby not ready");
    auto image = CheckpointCodec::capture(*original.active());
    auto candidate_options = opts;
    candidate_options.host = true;
    candidate_options.admission.coordinator = 2;
    MemoryTransport dummy;
    GameSession invalid(dummy, candidate_options, schemas);
    image.pop_back();
    if (CheckpointCodec::restore(invalid, image, now) || invalid.round())
        throw std::runtime_error("partial checkpoint published");
    // The old coordinator remains alive, but stops receiving calls for longer than its lease.
    for (int i = 0; i < 90; ++i)
        step(false, majority);
    original.pump(now);
    if (original.active() || original.phase() != MigrationPhase::Fenced)
        throw std::runtime_error("expired original host resumed writes");
    if (majority) {
        auto* host = standby.active();
        auto* guest = witness.active();
        if (!host || !guest || host->term() != 2 || guest->term() != 2 || !guest->guest()->resumed() ||
            !guest->object(object) || std::get<int32_t>(guest->object(object)->values[0]) != 123 ||
            guest->writers().writer(object) != 2 || host->object(avatar) || guest->object(avatar))
            throw std::runtime_error("majority failed to continue");
        if (!guest->object(child) || !guest->object(parent) ||
            !host->authority().set_distribution(parent, {InterestKind::Area, 41}))
            throw std::runtime_error("restored hierarchy did not start visible");
        for (int i = 0; i < 6; ++i) step(false, true);
        if (guest->object(child) || guest->object(parent) || !host->authority().set_distribution(parent, {}))
            throw std::runtime_error("restored parent leaving interest did not hide child");
        for (int i = 0; i < 6; ++i) step(false, true);
        if (!guest->object(child) || !guest->object(parent))
            throw std::runtime_error("restored parent's re-entry did not publish child");
        if (!host->authority().capture(object, {int32_t(456)}, host->tick()) || !standby.handoff() || standby.active())
            throw std::runtime_error("second host did not freeze for graceful handoff");
        for (int i = 0; i < 60; ++i) step(false, true);
        auto* final = witness.active();
        if (!final || !final->host() || final->local() != 3 || final->term() != 3 || final->host()->members().size() != 1 ||
            !final->object(object) || std::get<int32_t>(final->object(object)->values[0]) != 456 ||
            !final->authority().capture(object, {int32_t(789)}, final->tick()) || standby.active())
            throw std::runtime_error("two-player handoff lost the final write or enabled two coordinators");
    } else if (standby.active() || standby.phase() != MigrationPhase::Voting)
        throw std::runtime_error("minority elected itself");
}
int main() try {
    partition(true);
    partition(false);
    std::cout << "quorum and stale-host fencing passed" << std::endl;
} catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
}
