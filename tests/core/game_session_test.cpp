#include "session/game_session.hpp"
#include "session/checkpoint.hpp"
#include "session/script_game.hpp"
#include <sstream>
#include "transport/memory.hpp"
#include <cmath>
#include <stdexcept>
#include <iostream>

namespace {
using namespace netcode;
using namespace std::chrono_literals;
void check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
void test_gameplay_methods() {
    std::istringstream a("schema 1 1 Command\nint 1 . amount\n");
    std::istringstream b("schema 1 2 Command\nfloat 1 . amount\n");
    auto server=std::make_unique<MemoryTransport>(), peer=std::make_unique<MemoryTransport>();
    server->link(*peer);
    SessionOptions opts; opts.room=91; opts.host=true; opts.admission={2,true,true};
    ScriptGame host(std::move(server),opts,ReplicationLayout(a));
    opts.host=false;
    ScriptGame guest(std::move(peer),opts,ReplicationLayout(b));
    for (auto* script:{&host,&guest}) {
        check(script->bind_method("Award",33,1,int(Direction::Server),true,false,true),"register typed method");
        check(!script->bind_method("Award",34,1,int(Direction::Server),true,false,true),"duplicate name accepted");
        check(!script->bind_method("Other",33,1,int(Direction::Server),true,false,true),"duplicate method accepted");
    }
    TimePoint now{};
    auto advance=[&] {for(int i=0;i<25;++i){host.pump(now); guest.pump(now); now+=25ms;}};
    advance(); check(host.session().start(now),"typed start"); advance();
    check(guest.call_method("Award",0,{1.f}),"typed guest send");
    advance();
    check(!host.next_typed() && !host.next_rpc(),"mismatched signature dispatched");
    check(!host.call_method("Award",0,{true}) && !host.call_method("Missing",0,{}),"invalid method or type");
    check(host.call_method("Award",0,{int32_t(17)}),"typed local method");
    auto call=host.next_typed();
    check(call && call->values==Values{int32_t(17)} && call->sender==host.session().local(),"typed local delivery");
    host.pop_typed();
}
void test_replica_write_layout() {
    std::istringstream text(
        "schema 1 1 Partial\nint 1 . first\nint 2 . coordinator\nint 3 . last\n"
        "schema 2 1 Full\nint 1 . value\n");
    const ReplicationLayout layout(text);
    auto server = std::make_unique<MemoryTransport>();
    auto peer = std::make_unique<MemoryTransport>();
    server->link(*peer);
    SessionOptions options;
    options.room = 92;
    options.host = true;
    ScriptGame host(std::move(server), options, layout);
    options.host = false;
    ScriptGame guest(std::move(peer), options, layout);
    for (auto* game : {&host, &guest}) {
        check(game->model(1, {1, 3}, ClaimMode::Transaction, 0), "register non-contiguous writable fields");
        check(game->model(2, {1}, ClaimMode::Dynamic, 0), "register fully writable schema");
    }
    guest.set_snapshot_change_events(false);
    TimePoint now{};
    auto advance = [&] {
        for (int i = 0; i < 25; ++i) {
            host.pump(now);
            guest.pump(now);
            now += 25ms;
        }
    };
    advance();
    check(host.session().start(now), "start write layout room");
    const Values initial{std::int32_t(1), std::int32_t(2), std::int32_t(3)};
    const auto partial = host.session().world().spawn(1, host.session().local(), initial);
    const auto full = host.session().world().spawn(2, host.session().local(), {std::int32_t(4)});
    check(partial && full, "spawn both write layouts");
    const auto local_replica = host.receive_native_replica_view(full);
    check(local_replica.role == ScriptGame::ReplicaRole::Writer && !local_replica.values,
          "native replica step identified the local writer");
    Values applied;
    const Values local_sample{std::int32_t(6)};
    check(host.sync_replica(full, {std::int32_t(4)}, applied) == 2, "install host capture baseline");
    check(host.sync_replica(full, local_sample, applied) == 0 && applied.empty(), "capture steady host sample");
    const auto revision = host.session().object(full)->revision;
    check(host.sync_replica(full, {Bytes{1}}, applied) == 0 && host.session().object(full)->revision == revision,
          "invalid steady host sample advanced revision");
    check(host.submit(full, {std::int32_t(4)}) && host.sync_replica(full, local_sample, applied) == 0 &&
          host.session().object(full)->values == Values{std::int32_t(4)}, "unchanged sample overwrote an explicit host write");
    advance();
    const auto guest_id = guest.session().local();
    check(host.session().writers().assign(partial, guest_id, host.session().tick()) &&
          host.session().writers().assign(full, guest_id, host.session().tick()), "delegate both layouts");
    advance();
    check(guest.sync_replica(partial, initial, applied) == 2 && applied == initial,
          "apply partial-layout authority baseline");
    check(guest.sync_replica(full, {std::int32_t(4)}, applied) == 2, "apply fully writable authority baseline");
    const Values expected{std::int32_t(5), std::int32_t(2), std::int32_t(6)};
    check(guest.sync_replica(partial, {std::int32_t(5), std::int32_t(99), std::int32_t(6)}, applied) == 1 &&
          applied == expected, "correct coordinator field and preserve writable slot order");
    check(guest.sync_replica(full, {std::int32_t(7)}, applied) == 0 && applied.empty(),
          "fully writable capture needs no apply");
    const auto sequence = guest.session().writers().grant(full)->sample.sequence;
    check(guest.sync_replica(full, {Bytes{7}}, applied) == 0 &&
          guest.session().writers().grant(full)->sample.sequence == sequence,
          "invalid guest sample did not replace the latest valid submission");
    advance();
    check(host.read(partial, false, applied) && applied == expected, "host received partial layout");
    check(host.read(full, false, applied) && applied == Values{std::int32_t(7)}, "host received full layout");
    check(guest.read(partial, false, applied) && applied == expected, "guest received state without change events");
    for (const auto& event : guest.take_events())
        check(event.kind != ScriptEventKind::Changed, "native scene subscriber did not request change events");

    const auto proxy_baseline = host.receive_native_replica_view(partial);
    check(proxy_baseline.role == ScriptGame::ReplicaRole::Observer && proxy_baseline.baseline &&
          proxy_baseline.mask == 7 && proxy_baseline.values &&
          *proxy_baseline.values == expected, "borrowed proxy baseline in schema order");
    const Values changed{std::int32_t(5), std::int32_t(2), std::int32_t(8)};
    check(guest.sync_replica(partial, changed, applied) == 0, "update the last writable field");
    advance();
    const auto proxy_delta = host.receive_replica_view(partial);
    check(!proxy_delta.baseline && proxy_delta.mask == 4 && proxy_delta.values && *proxy_delta.values == changed,
          "borrowed sparse delta keeps the value at its schema index");

    check(host.session().writers().assign(full, host.session().local(), host.session().tick()), "reclaim full layout");
    advance();
    std::uint64_t mask = 0;
    check(guest.receive_replica(full, applied, mask) == 2 && mask == 1 && applied == Values{std::int32_t(7)},
          "observer sees the reclaimed writer baseline");
    const auto snapshot = guest.session().object(full)->revision;
    check(guest.receive_replica(full, applied, mask) == 0 && mask == 0, "stable observer needs no new application");
    check(guest.session().writers().request(full) && guest.submit(full, {std::int32_t(11)}), "queue predicted claim and write");
    check(guest.session().object(full)->revision == snapshot, "prediction did not change the canonical revision");
    const auto predicted = guest.receive_replica_view(full);
    check(!predicted.baseline && predicted.mask == 1 && predicted.values && *predicted.values == Values{std::int32_t(11)},
          "predicted view remains valid after the presentation temporary is destroyed");
}

void test_time() {
    NetworkClock clock;
    TimePoint start{};
    clock.reset(start, false, .03);
    check(clock.probe_due(start) && clock.sample(clock.sequence(), start + 80ms, 10.04),
          "initial clock probe");
    check(std::abs(clock.rtt() - .08) < .00001 && std::abs(clock.seconds() - 10.08) < .00001,
          "RTT and offset");
    NetworkClock thresholds;
    thresholds.reset(start, false, .03, ClockDiscipline::Threshold);
    check(thresholds.probe_due(start) && thresholds.sample(thresholds.sequence(),start+100ms,10.09),
          "asymmetric 90/10 sample");
    check(std::abs(thresholds.seconds() - 10.14) < 1e-8, "half-RTT bias must be measurable");
    check(thresholds.probe_due(start+1s) && thresholds.sample(thresholds.sequence(),start+1100ms,11.25)
          && std::abs(thresholds.rate()-1.-1./60) < 1e-8, "moderate clock correction");
    check(thresholds.probe_due(start+4s) && thresholds.sample(thresholds.sequence(),start+4100ms,14.8)
          && std::abs(thresholds.seconds()-14.85)<1e-8 && thresholds.rate()==1, "hard clock correction");
    double previous = clock.seconds();
    for (int i = 1; i <= 100; ++i) {
        auto local = start + 80ms + i * 100ms;
        clock.update(local);
        if (clock.probe_due(local)) {
            check(!clock.sample(clock.sequence() - 1, local, 0), "stale echo accepted");
            const double remote = 10.08 + i * .1002 + .04;
            check(clock.sample(clock.sequence(), local + 80ms, remote), "new echo rejected");
        }
        check(clock.seconds() >= previous, "clock ran backwards");
        previous = clock.seconds();
    }
    clock.update(start + 40s);
    check(std::abs(clock.seconds() - 50) < .1 && clock.object_time(11) == .3, "paused clock or object units");
}
}
void test_multiple_models() {
    MemoryTransport a, b;
    SessionOptions opts;
    opts.host = true;
    opts.room = 183;
    Schemas schemas;
    schemas.add({1, 1, {{1, FieldType::Int}}});
    schemas.add({2, 1, {{1, FieldType::Float}, {2, FieldType::Float}}});
    schemas.add({3, 1, {{1, FieldType::Int}}});
    std::map<uint8_t,unsigned> authority_calls, prediction_calls;
    GameSession host(a, opts, schemas);
    opts.host = false;
    GameSession guest(b, opts, schemas);
    for (auto session : {&host, &guest}) {
        auto* calls = session == &host ? &authority_calls : &prediction_calls;
        check(session->prediction().register_model(3,{1,1,{0},[calls](Values& state,const Bytes& input,Tick){
            if (input[0]) { ++(*calls)[input[0]]; std::get<int32_t>(state[0]) += input[0]; }
            return true;
        }}), "trace model");
        check(session->prediction().register_model(1, {1,
                                                       1,
                                                       {0},
                                                       [](Values& state, const Bytes& in, Tick) {
                                                           if (in[0] > 3)
                                                               return false;
                                                           std::get<int32_t>(state[0]) += in[0];
                                                           return true;
                                                       }}),
              "discrete model");
        check(session->prediction().register_model(2, {1,
                                                       1,
                                                       {0},
                                                       [](Values& state, const Bytes& in, Tick) {
                                                           if (in[0] > 2)
                                                               return false;
                                                           auto& speed = std::get<float>(state[1]);
                                                           speed += in[0] == 2 ? -1.f : float(in[0]);
                                                           std::get<float>(state[0]) += speed;
                                                           return true;
                                                       }}),
              "continuous model");
    }
    TimePoint now{};
    std::vector<Event> held;
    auto pump = [&](int frames, bool mute = false) {
        for (int i = 0; i < frames; ++i) {
            now += 30ms;
            host.pump(now);
            for (auto& e : b.poll(now)) {
                if (mute && e.type == EventType::Message && e.lane == Lane::Game)
                    held.push_back(std::move(e));
                else
                    guest.receive(e, now);
            }
            guest.flush(now);
        }
    };
    a.link(b);
    pump(4);
    check(host.start(now), "prediction room start");
    auto piece = host.world().spawn(1, 2, {int32_t(0)});
    auto lift = host.world().spawn(2, 2, {0.f, 0.f});
    check(host.prediction().bind(piece, 2) && host.prediction().bind(lift, 2),
          "bind two models to one player");
    pump(10);
    check(guest.prediction().state(piece) && guest.prediction().state(lift), "model baselines");
    for (int i = 0; i < 6; ++i) {
        check(guest.prediction().advance(piece, {1}) && guest.prediction().advance(lift, {1}),
              "local advance");
        pump(1, true);
    }
    check(std::get<int32_t>(guest.prediction().state(piece)->at(0)) == 6 &&
              std::get<float>(guest.prediction().state(lift)->at(1)) == 6 &&
              guest.prediction().confirmed(piece) == 0,
          "local authority baseline changed under 180ms downlink hold");
    for (auto i = held.rbegin(); i != held.rend(); ++i)
        guest.receive(*i, now);
    check(guest.prediction().replayed(piece) > 0 && guest.prediction().replayed(lift) > 0, "missing replay");
    pump(5);
    check(guest.prediction().confirmed(piece) == 6 && guest.prediction().confirmed(lift) == 6 &&
              *guest.prediction().state(lift) == host.object(lift)->values,
          "models diverged");
    check(host.prediction().bind(lift, host_player), "move authority of only one model");
    pump(3);
    check(!guest.prediction().advance(lift, {2}) && guest.prediction().advance(piece, {2}),
          "old model input stream leaked");
    pump(4);
    check(std::get<int32_t>(host.object(piece)->values[0]) == 8, "surviving stream failed");
    auto traced = host.world().spawn(3,2,{int32_t(0)});
    check(host.prediction().bind(traced,2),"trace owner");
    pump(10);
    held.clear();
    unsigned resets = 0;
    guest.prediction().on_reconciled = [&](ObjectId id,Tick,Tick) { if(id==traced) ++resets; };
    for (uint8_t seq=1;seq<=60;++seq) {
        check(guest.prediction().advance(traced,{seq}),"trace input");
        pump(1,true);
        if (seq%6==0) {
            for (auto it=held.rbegin();it!=held.rend();++it) guest.receive(*it,now);
            held.clear();
        }
    }
    pump(5);
    unsigned executed=0;
    for (const auto& [seq,count] : prediction_calls) executed+=count;
    check(authority_calls.size()==60 && std::all_of(authority_calls.begin(),authority_calls.end(),
        [](const auto& item){return item.second==1;}) && prediction_calls.size()==60 && executed>60 &&
        guest.prediction().confirmed(traced)==60 && std::get<int32_t>(host.object(traced)->values.front())==1830 &&
        *guest.prediction().state(traced)==host.object(traced)->values,"trace lost, duplicated or failed to replay an input");
    std::cout << "trace unique=60 host_callbacks=60 guest_callbacks=" << executed << " resets=" << resets << " confirmed=60 sum=1830\n";
}

void test_timed_input_scheduling() {
    MemoryTransport a, b;
    SessionOptions options;
    options.host = true;
    options.room = 185;
    options.step = 16ms;
    Schemas schemas;
    schemas.add({1, 1, {{1, FieldType::Int}}});
    GameSession host(a, options, schemas);
    options.host = false;
    GameSession guest(b, options, schemas);
    auto input = [](float delta, uint8_t payload) {
        Writer packet;
        packet.f32(delta);
        packet.u8(payload);
        return packet.bytes();
    };
    std::vector<uint8_t> callbacks;
    std::vector<Tick> ticks;
    unsigned neutral = 0;
    const auto register_model = [&](GameSession& game, bool authoritative) {
        SimulationModel model;
        model.timed_input = true;
        model.neutral = input(0, 0);
        model.input_bytes = model.neutral.size();
        model.object_step = [&, authoritative](ObjectId, Values& state, const Bytes& data, Tick tick, SimulationStep stage) {
            if (stage == SimulationStep::Neutral) {
                if (authoritative) ++neutral;
                return true;
            }
            if (authoritative) {
                callbacks.push_back(data.back());
                ticks.push_back(tick);
            }
            std::get<int32_t>(state.front()) += data.back();
            return true;
        };
        return game.prediction().register_model(1, std::move(model));
    };
    SimulationModel too_large;
    too_large.input_bytes = SimulationModel::payload_limit + 1;
    too_large.neutral.resize(too_large.input_bytes);
    too_large.step = [](Values&, const Bytes&, Tick) { return true; };
    check(!host.prediction().register_model(1, std::move(too_large)), "legacy input model retained its size limit");
    check(register_model(host, true) && register_model(guest, false), "register delta-driven prediction");
    TimePoint now{};
    auto step = [&] {
        now += 16ms;
        guest.pump(now);
        host.pump(now);
        guest.pump(now);
    };
    a.link(b);
    for (int i = 0; i < 5; ++i) step();
    check(host.start(now), "start delta-driven room");
    const auto id = host.world().spawn(1, 1, {int32_t(0)});
    check(id && host.prediction().bind(id, 2), "bind a remote input player");
    for (int i = 0; i < 20; ++i) step();
    check(guest.prediction().state(id), "remote prediction baseline");
    const auto before = neutral;
    check(!guest.prediction().advance(id, input(2.0f, 9)), "reject a duration that could stall the stream");
    check(guest.prediction().advance(id, input(.048f, 1)) &&
          guest.prediction().advance(id, input(.008f, 2)) &&
          guest.prediction().advance(id, input(.008f, 3)), "queue a long head and two short inputs");
    check(guest.prediction().state(id)->front() == Value(int32_t(6)), "guest prediction stays immediate");
    step();
    check(callbacks.empty() && host.prediction().queued(id) == 3, "first service step fired before the head duration");
    step();
    check(callbacks.empty() && neutral == before, "pending head also ran a neutral step");
    step();
    check(callbacks == std::vector<uint8_t>{1} && host.prediction().confirmed(id) == 1,
          "head command did not execute at its accumulated duration");
    step();
    check(callbacks == std::vector<uint8_t>({1, 2, 3}) && guest.prediction().confirmed(id) == 3 &&
          ticks[0] < ticks[1] && ticks[1] < ticks[2] && host.object(id)->values.front() == Value(int32_t(6)),
          "two short commands did not execute in order in one service step");

    check(guest.prediction().advance(id, input(0.5f, 9)), "queue input from the departing writer");
    step();
    check(host.prediction().queued(id) == 1 && host.prediction().confirmed(id) == 3,
          "old writer's command is still waiting when reassigned");
    check(host.prediction().bind(id, 1) && !host.prediction().queued(id), "move input authority and discard the old queue");
    const auto publish_tick = host.tick();
    check(host.prediction().advance(id, input(.023f, 4)) && host.prediction().advance(id, input(.005f, 5)) &&
          host.tick() == publish_tick && host.prediction().queued(id) == 0 && host.prediction().confirmed(id) == 2 &&
          callbacks == std::vector<uint8_t>({1, 2, 3, 4, 5}) &&
          host.object(id)->values.front() == Value(int32_t(15)), "coordinator input was not committed immediately");
    const auto before_local_flush = neutral;
    step();
    check(callbacks.size() == 5 && neutral == before_local_flush && ticks[3] < ticks[4],
          "local input was processed a second time when the service step advanced");
    step();
    check(neutral == before_local_flush, "the local duration also covers a second service step");
    step();
    check(neutral == before_local_flush + 1 && !guest.prediction().advance(id, input(.016f, 6)),
          "idle simulation or input reassignment regressed");
    for (int i = 0; i < 5; ++i)
        check(host.prediction().advance(id, input(.016f, 1)), "local catch-up input dispatch");
    const auto before_catch_up = neutral;
    now += 5 * 16ms;
    host.pump(now);
    check(neutral == before_catch_up && callbacks.size() == 10, "catch-up service steps duplicated local simulation time");

    for (int i = 0; i < 4; ++i)
        check(host.prediction().advance(id, input(.008f, 1)), "model time advances faster than service time");
    const auto last_model_tick = ticks.back();
    check(host.prediction().bind(id, 2) && host.prediction().bind(id, 1) &&
          host.prediction().advance(id, input(.008f, 1)) && ticks.back() > last_model_tick,
          "model callback tick regressed across an input handover");

    check(host.prediction().advance(id, input(.5f, 1)), "checkpoint captures already committed local time");
    const auto image = CheckpointCodec::capture(host);
    const auto before_restore = ticks.back();
    MemoryTransport moved_transport;
    options.host = true;
    options.admission.coordinator = 2;
    GameSession moved(moved_transport, options, schemas);
    check(register_model(moved, true) && CheckpointCodec::restore(moved, image, now), "restore timed model checkpoint");
    const auto neutral_before_restore = neutral;
    for (int i = 1; i <= 32; ++i) moved.pump(now + i * 16ms);
    check(neutral == neutral_before_restore, "restored host ran neutral during already committed input time");
    check(moved.prediction().advance(id, input(.008f, 1)) && ticks.back() == before_restore + 1,
          "checkpoint restore lost the model's callback tick");
}

void test_rpc_outcomes() {
    MemoryTransport left, right;
    left.link(right);
    left.poll({});
    right.poll({});
    bool visible = true, exists = true;
    std::vector<uint8_t> received;
    auto owner = [&](ObjectId) -> std::optional<PlayerId> {
        return exists ? std::optional<PlayerId>{2} : std::nullopt;
    };
    Rpc host(left, true, owner, [](const RpcCall&) {}, [&](Peer, ObjectId) { return visible; });
    Rpc guest(right, false, owner, [&](const RpcCall& c) { received.push_back(c.arguments.front()); });
    for (auto r : {&host, &guest})
        r->register_method({1, false, Direction::Client, Reliability::Reliable, 1});
    host.reset(123, 1, 1);
    guest.reset(123, 1, 2);
    host.attach(1, 2);
    guest.attach(1, 1);
    host.allow_cancel(1);
    guest.allow_cancel(1);
    auto poll = [](MemoryTransport& transport, Rpc& rpc) {
        for (auto& e : transport.poll({})) {
            Reader in(e.payload);
            GameMessage type;
            if (read_game_header(in, type, 123, 1))
                rpc.receive(e.peer, type, in);
        }
    };
    auto outcome = [&](uint64_t token, RpcStatus status) {
        auto results = host.take_outcomes();
        check(results.size() == 1 && results.front().token == token && results.front().player == 2 &&
                  results.front().status == status,
              "RPC result correlation");
    };
    visible = false;
    auto prior = left.traffic();
    check(!host.invoke(5, 1, {1}, 0, 1) && left.traffic().udp_sent == prior.udp_sent, "hidden RPC sent data");
    outcome(1, RpcStatus::OutOfScope);
    visible = true;
    left.next_game(Delivery::Drop);
    check(host.invoke(5, 1, {2}, 0, 2), "queue RPC");
    TimePoint now{};
    for (int i = 0; i < 22; ++i) {
        left.next_game(Delivery::Drop);
        host.update(now);
        now += 120ms;
        poll(right, guest);
        poll(left, host);
    }
    outcome(2, RpcStatus::Expired);
    check(!host.queued() && received.empty(), "retry exhaustion blocked stream");
    left.next_game(Delivery::Normal);
    check(host.invoke(5, 1, {3}, 0, 3), "next RPC");
    poll(right, guest);
    poll(left, host);
    outcome(3, RpcStatus::Received);
    check(received == std::vector<uint8_t>{3}, "later event was blocked by expiry");
    left.next_game(Delivery::Drop);
    host.invoke(5, 1, {4}, 0, 4);
    exists = false;
    host.update(now);
    poll(right, guest);
    poll(left, host);
    outcome(4, RpcStatus::ObjectGone);
    exists = true;
    host.invoke(5, 1, {5}, 0, 5);
    host.detach(1);
    outcome(5, RpcStatus::PeerLeft);
}

void test_compact() {
    Schemas schemas;
    schemas.add({1,
                 1,
                 {{1, FieldType::Int},
                  {2, FieldType::Float},
                  {3, FieldType::IntList, 90},
                  {4, FieldType::String, 128}}});
    AuthorityState source(schemas);
    ReplicaState compressed(schemas), legacy(schemas);
    check(source.set_quantum(1, 2, .25f), "field quantization");
    IntArray array;
    for (int i = 0; i < 80; ++i)
        array.push_back(25813 * i + 17);
    Values fields{int32_t(-2), 3.14159f, array, std::string(90, 'X')};
    source.create(1, 1, 1, fields, 1);
    source.attach(1);
    source.attach(2, true);
    source.enable_quantization(1, true);
    SnapshotCodec sender, receiver;
    uint32_t seq = 0, missing = 0;
    GameMessage type;
    auto deliver = [&](Peer peer, Tick tick) {
        auto packets = source.packets(peer, 333, 1, tick);
        check(packets.size() == 1, "compact packet count");
        auto bytes = peer == 1 ? sender.encode(packets[0], 333, 1) : packets[0];
        Reader r(bytes);
        check(read_game_header(r, type, 333, 1), "compact header");
        Bytes payload;
        AppliedSnapshot result;
        if (type == GameMessage::CompactSnapshot) {
            check(receiver.decode(r, payload, seq, missing), "compact decode");
            Reader body(payload);
            check(compressed.apply(body, 333, 1, result), "translated compact state");
            receiver.remember(seq, payload);
            sender.acknowledge(seq);
        } else
            check(legacy.apply(r, 333, 1, result), "legacy packet");
        Reader ack(result.ack);
        check(read_game_header(ack, type, 333, 1) && source.acknowledge(peer, ack), "original state ACK");
        return bytes;
    };
    deliver(1, 1);
    deliver(2, 1);
    check(std::abs(std::get<float>(compressed.objects().at(1).values[1]) - 3.14159f) < .125f &&
              legacy.objects().at(1).values == fields && seq != 0,
          "legacy precision or anchor");
    const auto original = sender.original_bytes, encoded = sender.encoded_bytes;
    auto& a = std::get<IntArray>(fields[2]);
    a[44] = -7;
    a.resize(76);
    fields[3] = std::string(89, 'X') + "Y";
    source.capture(1, fields, 2);
    const auto delta = deliver(1, 2);
    deliver(2, 2);
    check(std::get<IntArray>(compressed.objects().at(1).values[2]) == a &&
              compressed.objects().at(1).values[3] == fields[3] &&
              (sender.encoded_bytes - encoded) * 2 < sender.original_bytes - original,
          "sparse array/string reuse");
    SnapshotCodec restarted;
    Reader lost(delta);
    read_game_header(lost, type, 333, 1);
    Bytes expanded;
    check(!restarted.decode(lost, expanded, seq, missing) && missing != 0, "unknown anchor was accepted");
    std::cout << "compact_delta_bytes=" << sender.encoded_bytes - encoded
              << " legacy_delta_bytes=" << sender.original_bytes - original << std::endl;
}

void test_anchor_recovery() {
    MemoryTransport a, b;
    Schemas types;
    types.add({1, 1, {{1, FieldType::String, 180}}});
    SessionOptions opts;
    opts.room = 660;
    opts.host = true;
    GameSession host(a, opts, types);
    opts.host = false;
    GameSession guest(b, opts, types);
    TimePoint now{};
    unsigned acks = 0, resets = 0;
    uint32_t lost_dictionary = 0;
    bool cut = false, lose_next_dictionary = false;
    auto frame = [&] {
        now += 30ms;
        for (auto& e : a.poll(now)) {
            if (e.type == EventType::Message && !e.payload.empty()) {
                if (e.payload[0] == uint8_t(GameMessage::CodecAck)) {
                    ++acks;
                    if (cut)
                        continue;
                }
                if (e.payload[0] == uint8_t(GameMessage::CodecReset))
                    ++resets;
            }
            host.receive(e, now);
        }
        host.flush(now);
        for (const auto& e : b.poll(now)) {
            if (lose_next_dictionary && e.type == EventType::Message && e.lane == Lane::Game) {
                Reader in(e.payload);
                GameMessage type;
                if (read_game_header(in, type, host.room(), host.round()) && type == GameMessage::CompactSnapshot) {
                    uint8_t seq;
                    check(in.u8(seq) && seq > 0 && seq < 128, "unexpected compact sequence in fixture");
                    lost_dictionary = seq;
                    lose_next_dictionary = false;
                    // Let the sender use a dictionary the receiver never installed.
                    auto ack = game_packet(GameMessage::CodecAck, host.room(), host.round());
                    ack.u32(lost_dictionary);
                    check(b.send(e.peer, Lane::Game, ack.bytes()), "inject dictionary ACK");
                    continue;
                }
            }
            guest.receive(e, now);
        }
        guest.flush(now);
    };
    a.link(b);
    for (int i = 0; i < 4; ++i)
        frame();
    check(host.start(now), "anchor room");
    auto value = [](unsigned n) { return std::string(120, 'x') + std::to_string(n); };
    auto id = host.world().spawn(1, 1, {value(0)});
    for (unsigned i = 1; i <= 50; ++i) {
        host.authority().capture(id, {value(i)}, host.tick());
        frame();
        if (i == 31)
            check(cut && resets == 0 && guest.object(id) &&
                      std::get<std::string>(guest.object(id)->values[0]) == value(i),
                  "confirmed dictionary was evicted during an ACK gap");
        if (acks >= 2 && i < 32)
            cut = true;
        else if (i >= 32)
            cut = false;
    }
    lose_next_dictionary = true;
    for (unsigned i = 51; i <= 54; ++i) {
        host.authority().capture(id, {value(i)}, host.tick());
        frame();
    }
    for (unsigned i = 0; i < 6; ++i)
        frame();
    auto codec = host.codec(1);
    check(lost_dictionary && resets > 0 && codec && codec->encoded_bytes < codec->original_bytes &&
              guest.object(id) && std::get<std::string>(guest.object(id)->values[0]) == value(54),
          "lost dictionary failed to recover on the wire");
}

void test_native_rpc_before_roster() {
    MemoryTransport h, a, b;
    SessionOptions options;
    options.room = 684;
    options.admission = {3, true, false};
    options.host = true;
    Schemas schemas;
    GameSession host(h, options, schemas);
    options.host = false;
    options.extended_join = true;
    GameSession sender(a, options, schemas), joiner(b, options, schemas);
    for (auto* game : {&host, &sender, &joiner}) game->room_properties().enable(11, {}, {});

    auto now = Clock::now();
    h.link(a);
    check(host.start(now), "native RPC negotiation test starts");
    for (int frame = 0; frame < 12 && !sender.room_properties().ready(); ++frame) {
        now += 30ms;
        host.pump(now);
        sender.pump(now);
    }
    check(sender.room_properties().ready(), "sender finished native negotiation");
    h.link(b);
    for (int frame = 0; frame < 12 && !joiner.round(); ++frame) {
        now += 30ms;
        host.pump(now);
        joiner.pump(now);
    }
    check(joiner.round() && !joiner.room_properties().ready(), "joiner's Hello is pending before the snapshot");
    check(sender.send_native_rpc(0, joiner.local(), 1, {42}), "sender queues room RPC immediately after Hello");
    host.pump(now);
    bool first_roster = false, early_rpc = false;
    for (const auto& event : b.poll(now)) {
        if (event.type == EventType::Message && event.lane == Lane::Control && !event.payload.empty()) {
            if (event.payload[0] == uint8_t(GameMessage::RoomPropertiesSnapshot)) first_roster = true;
            if (event.payload[0] == uint8_t(GameMessage::NativeRpcDelivery)) early_rpc = !first_roster;
        }
        joiner.receive(event, now);
    }
    const auto calls = joiner.native_rpc().take_deliveries();
    check(early_rpc && first_roster && joiner.room_properties().ready() && calls.size() == 1 &&
          calls.front().sender == sender.local() && calls.front().target == int32_t(joiner.local()) &&
          calls.front().data == Bytes{42}, "room RPC queued before the first snapshot was lost");
    joiner.finish_native_rpc(calls.front().receipt, true);
    host.pump(now);
    sender.pump(now);
    const auto results = sender.native_rpc().take_outcomes();
    check(results.size() == 1 && results.front().status == NativeRpc::Status::Delivered,
          "early room RPC did not complete after negotiation");

    for (uint32_t token = 1; token <= 256; ++token)
        check(host.send_native_rpc(0, NativeRpc::target_all, token, {42}, true), "room broadcast was admitted");
    check(!host.send_native_rpc(0, NativeRpc::target_all, 257, {42}, true),
          "host reported a route rejected for capacity as admitted");
    check(host.native_rpc().take_deliveries().empty(), "sender-handled broadcasts were echoed back to the sender");
}

void test_word_authority() {
    MemoryTransport server, client;
    SessionOptions options;
    options.room = 402;
    options.admission = {2, true, true};
    options.host = true;
    Schemas schemas;
    check(schemas.add({1, 1, {{1, FieldType::Vec2}, {2, FieldType::Int}, {3, FieldType::Int}}}),
          "writer word schema");
    GameSession host(server, options, schemas);
    options.host = false;
    GameSession guest(client, options, schemas);
    for (auto* game : {&host, &guest})
        check(game->writers().register_model(1, 1, {1, 2, 3}, 3), "writer word model");
    TimePoint now{};
    Bytes dropped;
    std::size_t word_packets = 0;
    auto pump = [&](int frames, bool drop = false) {
        for (int i = 0; i < frames; ++i) {
            now += 30ms;
            for (const auto& event : server.poll(now)) {
                if (event.type == EventType::Message && !event.payload.empty() &&
                    event.payload.front() == uint8_t(GameMessage::AuthorityStateWords)) {
                    ++word_packets;
                    if (drop) {
                        dropped = event.payload;
                        continue;
                    }
                }
                host.receive(event, now);
            }
            host.flush(now);
            guest.pump(now);
        }
    };
    server.link(client);
    pump(8);
    check(host.start(now), "start word authority session");
    const Values initial{Vector2{1, 2}, int32_t(7), int32_t(-9)};
    const auto id = host.world().spawn(1, host.local(), initial);
    pump(15);
    check(id && host.writers().assign(id, guest.local(), host.tick()), "grant word writer");
    pump(10);
    const auto* grant = guest.writers().grant(id);
    check(grant && grant->writer == guest.local(), "guest did not receive word grant");
    const auto epoch = grant->epoch;
    check(guest.writers().submit(id, epoch, 10, {Vector2{3, 2}, int32_t(7), int32_t(-9)}, {1}),
          "submit first word sample");
    pump(2, true);
    check(!dropped.empty() && host.object(id)->values == initial, "first word sample was not sent and dropped");
    const Values final{Vector2{3, 6}, int32_t(7), int32_t(-9)};
    check(guest.writers().submit(id, epoch, 11, final, {2, 3}), "submit cumulative word sample");
    pump(8);
    check(host.object(id)->values == final && host.writers().grant(id)->sample.recovery == Bytes{2, 3} &&
          guest.writers().grant(id)->confirmed == 2, "cumulative word state or recovery did not converge");
    check(client.send(guest.guest()->peer(), Lane::Game, dropped), "replay old word sample");
    pump(3);
    check(host.object(id)->values == final && word_packets >= 3, "stale word sample rolled back state");
    check(guest.writers().submit(id, epoch, 12, final, {9}), "submit recovery-only sample");
    pump(5);
    check(host.writers().grant(id)->sample.source_step == 12 &&
          host.writers().grant(id)->sample.recovery == Bytes{9} && guest.writers().grant(id)->confirmed == 3,
          "zero-word sample did not advance recovery");

    const Values replacement{Vector2{8, 12}, int32_t(15), int32_t(-9)};
    check(guest.writers().submit(id, epoch, 13, replacement), "submit full replacement");
    auto full = game_packet(GameMessage::AuthorityState, host.room(), host.round());
    full.u32(id);
    full.u32(epoch);
    full.u32(4);
    full.u32(13);
    for (const auto& value : replacement) write_value(full, value);
    full.u16(0);
    check(client.send(guest.guest()->peer(), Lane::Game, full.bytes()), "send full replacement");
    pump(3);
    check(host.object(id)->values == replacement && guest.writers().grant(id)->confirmed == 4,
          "full sample did not replace the word baseline");

    auto malformed = game_packet(GameMessage::AuthorityStateWords, host.room(), host.round());
    malformed.u32(id);
    malformed.u32(epoch);
    malformed.u32(5);
    malformed.u32(14);
    write_word_varuint(malformed, 1);
    write_word_change(malformed, 0, 0);
    malformed.u16(4);
    check(client.send(guest.guest()->peer(), Lane::Game, malformed.bytes()), "send truncated word sample");
    pump(2);
    check(host.object(id)->values == replacement && host.writers().grant(id)->confirmed == 4,
          "invalid sample advanced accepted words");
    const Values after_full{Vector2{8, 12}, int32_t(16), int32_t(-9)};
    check(guest.writers().submit(id, epoch, 14, after_full), "submit delta on replacement baseline");
    pump(4);
    check(host.object(id)->values == after_full && guest.writers().grant(id)->confirmed == 5,
          "word delta lost unmodified fields from the full replacement");
    check(guest.object(id)->values == after_full && guest.object(id)->revision == host.object(id)->revision,
          "canonical snapshot did not relay the accepted word state");
}

void test_spatial_writer_interest(bool retain) {
    MemoryTransport server, client, observer;
    SessionOptions options;
    options.room = 410;
    options.admission = {3, true, true};
    options.host = true;
    options.retain_spatial_writer_interest = retain;
    Schemas schemas;
    check(schemas.add({1, 1, {{1, FieldType::Int}}}), "spatial writer schema");
    GameSession host(server, options, schemas);
    options.host = false;
    GameSession guest(client, options, schemas), other(observer, options, schemas);
    for (auto* game : {&host, &guest, &other})
        check(game->writers().register_model(1, 1, {1}), "spatial writer model");
    TimePoint now{};
    const auto pump = [&] {
        for (int frame = 0; frame < 12; ++frame) {
            now += 30ms;
            host.pump(now);
            guest.pump(now);
            other.pump(now);
        }
    };
    server.link(client);
    server.link(observer);
    pump();
    check(host.start(now), "spatial writer start");
    pump();
    const auto writer_peer = host.host()->members().at(guest.local()).peer;
    const auto observer_peer = host.host()->members().at(other.local()).peer;
    check(host.authority().subscribe(writer_peer, InterestKind::Area, {1}) &&
          host.authority().subscribe(observer_peer, InterestKind::Area, {1}), "spatial observers subscribe");
    const auto id = host.world().spawn(1, host.local(), {int32_t(11)});
    check(id && host.authority().set_distribution(id, {InterestKind::Area, 1}), "spatial writer spawn");
    pump();
    check(host.writers().assign(id, guest.local(), host.tick()), "spatial writer grant");
    pump();
    const auto* grant = guest.writers().grant(id);
    check(grant != nullptr, "guest received spatial writer grant");
    const auto epoch = grant->epoch;
    check(host.authority().set_distribution(id, {InterestKind::Area, 2}), "move spatial writer outside of view");
    pump();
    check(!other.object(id), "unsubscribed observer still sees spatial writer");
    if (retain) {
        check(guest.object(id) && guest.writers().writer(id) == guest.local() &&
              guest.writers().submit(id, epoch, 1, {int32_t(19)}), "out-of-area writer lost readiness");
        pump();
        check(host.object(id)->values == Values{int32_t(19)} && host.writers().grant(id)->confirmed == 1,
              "out-of-area writer sample did not apply");
        check(host.writers().assign(id, host.local(), host.tick()), "reclaim spatial writer");
        pump();
    }
    check(!guest.object(id) && host.writers().writer(id) == host.local(), "released spatial writer was not culled");
    check(host.authority().subscribe(writer_peer, InterestKind::Area, {2}), "spatial writer re-enter");
    pump();
    check(guest.object(id) && guest.object(id)->values == Values{int32_t(retain ? 19 : 11)},
          "re-entered spatial object did not receive the latest baseline");
    if (retain) {
        check(host.writers().assign(id, guest.local(), host.tick()), "re-assign spatial writer");
        pump();
        check(host.authority().set_interest(writer_peer, 0, std::set<ObjectId>{}), "explicit visibility selection");
        pump();
        check(!guest.object(id) && host.writers().writer(id) == host.local(), "writer visibility selection was bypassed");
    }
}

void test_spatial_subscription_rates(bool legacy_host) {
    MemoryTransport server, client;
    SessionOptions options;
    options.room = 411;
    options.admission = {2, true, true};
    options.host = true;
    Schemas schemas;
    check(schemas.add({1, 1, {{1, FieldType::Int}}}), "spatial rate schema");
    GameSession host(server, options, schemas);
    options.host = false;
    GameSession guest(client, options, schemas);
    for (auto* game : {&host, &guest}) game->room_properties().enable(12, {}, {});
    TimePoint now{};
    const auto pump = [&](unsigned frames) {
        for (unsigned frame = 0; frame < frames; ++frame) {
            now += 30ms;
            host.pump(now);
            for (const auto& event : client.poll(now)) {
                if (legacy_host && event.type == EventType::Message && !event.payload.empty() &&
                    event.payload.front() == uint8_t(GameMessage::RoomAreaRatesSupported)) continue;
                guest.receive(event, now);
            }
            guest.flush(now);
        }
    };
    server.link(client);
    pump(12);
    check(host.start(now), "spatial rate session start");
    pump(12);
    check(guest.room_properties().ready() && guest.room_properties().supports_area_rates() != legacy_host,
          "spatial subscription rate negotiation");
    check(guest.subscribe_local_area_rates({{1, 1}, {2, 4}}), "request spatial rates");
    const auto near = host.world().spawn(1, host.local(), {int32_t(0)});
    const auto far = host.world().spawn(1, host.local(), {int32_t(0)});
    check(near && far && host.authority().set_distribution(near, {InterestKind::Area, 1}) &&
          host.authority().set_distribution(far, {InterestKind::Area, 2}), "set spatial rate groups");
    pump(12);
    check(guest.object(near) && guest.object(far), "spatial subscriptions delivered both baselines");
    unsigned near_updates = 0, far_updates = 0, enters = 0;
    guest.on_snapshot = [&](const AppliedSnapshot& snapshot) {
        near_updates += std::count(snapshot.changed.begin(), snapshot.changed.end(), near);
        far_updates += std::count(snapshot.changed.begin(), snapshot.changed.end(), far);
        enters += std::count(snapshot.ready.begin(), snapshot.ready.end(), far);
    };
    for (int32_t value = 1; value <= 16; ++value) {
        check(host.authority().capture(near, {value}, host.tick()) &&
              host.authority().capture(far, {value}, host.tick()), "publish spatial-rate values");
        pump(1);
    }
    pump(6);
    check(near_updates >= 14 && (legacy_host ? far_updates == near_updates : far_updates >= 4 && far_updates <= 6),
          "spatial rates did not change delivery cadence as negotiated");
    check(guest.object(far)->values == Values{int32_t(16)}, "far rate did not reach the final value");
    check(guest.subscribe_local_area_rates({{1, 1}, {2, 1}}), "raise the far spatial rate");
    pump(3);
    check(host.authority().capture(far, {int32_t(17)}, host.tick()), "publish a fast spatial update");
    pump(1);
    check(guest.object(far)->values == Values{int32_t(17)} && !enters, "rate change disrupted visibility or the next delivery");
}

void test_authority_batch(bool legacy) {
    class OrderedTransport final : public Transport {
      public:
        std::vector<Event> poll(TimePoint now) override { return memory.poll(now); }
        bool send(Peer peer, Lane lane, const Bytes& payload) override { return memory.send(peer, lane, payload); }
        void close(Peer peer) override { memory.close(peer); }
        bool reliable_ordered(Lane) const override { return true; }
        Traffic traffic() const override { return memory.traffic(); }
        MemoryTransport memory;
    } server, client;
    SessionOptions options;
    options.room = 409;
    options.host = true;
    options.admission = {2, true, true};
    Schemas schemas;
    check(schemas.add({1, 1, {{1, FieldType::Vec2}, {2, FieldType::Int}}}) &&
          schemas.add({2, 1, {{1, FieldType::Flag}}}), "batch schemas");
    GameSession host(server, options, schemas);
    options.host = false;
    GameSession guest(client, options, schemas);
    for (auto* game : {&host, &guest})
        check(game->writers().register_model(1, 1, {1, 2}) && game->writers().register_model(2, 1, {1}),
              "batch models");
    TimePoint now{};
    const auto kind = [](const Event& e) {
        return e.type == EventType::Message && !e.payload.empty() ? GameMessage(e.payload[0]) : GameMessage{};
    };
    const auto pump = [&](int frames) {
        for (int step = 0; step < frames; ++step) {
            now += 30ms;
            for (const auto& event : server.poll(now))
                if (!legacy || kind(event) != GameMessage::AuthorityBatchHello) host.receive(event, now);
            host.flush(now);
            guest.pump(now);
        }
    };
    server.memory.link(client.memory);
    pump(8);
    check(host.start(now), "start batch room");
    constexpr ObjectId count = 128;
    const auto initial = [](ObjectId id) -> Values { return id % 2 ? Values{Vector2{1, 2}, int32_t(9)} : Values{false}; };
    const auto moved = [](ObjectId id) -> Values { return id % 2 ? Values{Vector2{3, 2}, int32_t(9)} : Values{true}; };
    for (ObjectId id = 1; id <= count; ++id) check(host.world().spawn(id % 2 ? 1 : 2, host.local(), initial(id)) == id, "spawn batch object");
    pump(15);
    for (ObjectId id = 1; id <= count; ++id) check(host.writers().assign(id, guest.local(), host.tick()), "assign batch object");
    pump(15);
    for (ObjectId id = 1; id <= count; ++id) {
        const auto* grant = guest.writers().grant(id);
        check(grant && guest.writers().submit(id, grant->epoch, 1, moved(id)), "submit batch object");
    }
    now += 30ms;
    guest.flush(now);
    std::vector<Event> states;
    for (auto& event : server.poll(now)) {
        const auto type = kind(event);
        if (type == GameMessage::AuthorityState || type == GameMessage::AuthorityStateWords || type == GameMessage::AuthorityStateBatch)
            states.push_back(std::move(event));
    }
    check(legacy ? states.size() == count : states.size() > 1 && states.size() < count / 4, "batch negotiation or splitting");
    if (!legacy) {
        check(kind(states.back()) == GameMessage::AuthorityStateBatch && states.back().payload.size() + 1 <= game_payload_limit,
              "batch framing fixture");
        auto invalid = states.back();
        invalid.payload.push_back(0);
        host.receive(invalid, now);
        check(host.writers().acceptances() == 0, "truncated batch committed its prefix");
    }
    for (const auto& event : states) {
        check(event.payload.size() <= game_payload_limit, "batch exceeded game frame limit");
        host.receive(event, now);
    }
    check(host.writers().acceptances() == count, "batch did not commit every sample");
    const Values latest{Vector2{7, 5}, int32_t(9)};
    if (!legacy) {
        const auto* grant = guest.writers().grant(1);
        check(grant && guest.writers().submit(1, grant->epoch, 2, latest), "submit newer state before ACK flush");
        now += 30ms;
        guest.flush(now);
        for (const auto& event : server.poll(now)) host.receive(event, now);
        check(host.writers().acceptances() == count + 1 && host.traffic().authority_ack_coalesces >= 1,
              "Newer sample replaces the queued receipt");
    }
    host.flush(now);
    std::size_t receipts = 0;
    for (const auto& event : client.poll(now)) {
        const auto type = kind(event);
        if (type == GameMessage::AuthorityAck || type == GameMessage::AuthorityAckBatch) {
            check(type == (legacy ? GameMessage::AuthorityAck : GameMessage::AuthorityAckBatch), "wrong ACK form");
            if (!legacy && !receipts) {
                auto invalid = event;
                invalid.payload.push_back(0);
                guest.receive(invalid, now);
                for (ObjectId id = 1; id <= count; ++id)
                    check(guest.writers().grant(id)->confirmed == 0, "truncated ACK advanced its prefix");
            }
            ++receipts;
        }
        guest.receive(event, now);
    }
    check(legacy ? receipts == states.size() : receipts > 0 && receipts < states.size(),
          "Negotiated receipts are packed separately from state messages");
    for (ObjectId id = 1; id <= count; ++id) {
        const bool updated = !legacy && id == 1;
        check(host.object(id)->values == (updated ? latest : moved(id)) &&
              guest.writers().grant(id)->confirmed == (updated ? 2u : 1u),
              "Latest state and cumulative ACK converge");
    }
}

void test_grant_replication_baseline() {
    MemoryTransport server, writer_link, observer_link, late_link;
    SessionOptions options;
    options.room = 403;
    options.admission = {4, true, true};
    options.host = true;
    Schemas schemas;
    check(schemas.add({1, 1, {{1, FieldType::Float}}}), "grant baseline schema");
    GameSession host(server, options, schemas);
    options.host = false;
    GameSession writer(writer_link, options, schemas), observer(observer_link, options, schemas),
                late(late_link, options, schemas);
    for (auto* session : {&host, &writer, &observer, &late})
        check(session->writers().register_model(1, 1, {1}), "grant baseline model");
    check(host.authority().set_quantum(1, 1, .1f), "grant baseline quantization");
    TimePoint now{};
    std::optional<Event> deferred_model;
    Peer late_peer = 0;
    auto pump = [&](int frames) {
        while (frames-- > 0) {
            now += 30ms;
            for (const auto& event : server.poll(now)) {
                if (late_peer && event.peer == late_peer && event.type == EventType::Message &&
                    event.lane == Lane::Control && !event.payload.empty() &&
                    event.payload.front() == uint8_t(GameMessage::AuthorityModels)) {
                    deferred_model = event;
                    continue;
                }
                host.receive(event, now);
            }
            host.flush(now);
            writer.pump(now);
            observer.pump(now);
            late.pump(now);
        }
    };
    server.link(writer_link);
    server.link(observer_link);
    server.link(late_link);
    pump(8);
    late_peer = host.host()->members().at(late.local()).peer;
    check(host.start(now), "start grant baseline session");
    pump(8);
    const Distribution slow{InterestKind::Global, 0, UINT16_MAX};
    const auto first = host.world().spawn(1, host.local(), {.14f});
    const auto second = host.world().spawn(1, host.local(), {.14f});
    const auto delayed = host.world().spawn(1, host.local(), {.14f});
    for (const auto id : {first, second, delayed})
        check(host.authority().set_distribution(id, slow), "set grant test send rate");
    pump(12);
    check(first && second && delayed && deferred_model && observer.object(first)->values == Values{.1f},
          "quantized observer initial state");
    const auto deliver_baseline = [&](GameSession& guest, ObjectId id) {
        const auto* grant = guest.writers().grant(id);
        const auto* object = guest.object(id);
        return grant && object && object->revision >= grant->baseline_revision &&
               guest.writers().presentation(*object).values == Values{.1f};
    };
    check(host.authority().capture(first, {.27f}, host.tick()) &&
          host.authority().capture(first, {.14f}, host.tick()) &&
          host.writers().assign(first, writer.local(), host.tick()), "capture then grant");
    check(host.authority().capture(second, {.27f}, host.tick()) &&
          host.writers().assign(second, writer.local(), host.tick()) &&
          host.authority().capture(second, {.14f}, host.tick()), "grant then return to shadow");
    pump(12);
    check(deliver_baseline(observer, first) && deliver_baseline(observer, second), "grant revision stayed unpublished");

    check(host.writers().assign(delayed, writer.local(), host.tick()), "grant delayed delivery case");
    pump(8);
    const auto* grant = writer.writers().grant(delayed);
    check(grant && !late.writers().grant(delayed), "late model got grant prematurely");
    for (const auto& [sequence, value] : {std::pair{1u, .27f}, std::pair{2u, .14f}}) {
        auto packet = game_packet(GameMessage::AuthorityState, host.room(), host.round());
        packet.u32(delayed);
        packet.u32(grant->epoch);
        packet.u32(sequence);
        packet.u32(1);
        packet.f32(value);
        packet.u16(0);
        check(writer_link.send(writer.guest()->peer(), Lane::Game, packet.bytes()), "send two accepted samples");
    }
    pump(1);
    check(host.writers().grant(delayed)->confirmed == 2 &&
          late.object(delayed)->revision < host.writers().grant(delayed)->baseline_revision,
          "accepted samples did not coalesce before late grant");
    host.receive(*deferred_model, now);
    pump(12);
    check(deliver_baseline(late, delayed) &&
          observer.object(delayed)->revision < late.writers().grant(delayed)->baseline_revision &&
          deliver_baseline(observer, delayed), "late grant did not refresh exactly its receiver's baseline");
}

int main() try {
    test_gameplay_methods();
    test_replica_write_layout();
    test_time();
    test_multiple_models();
    test_timed_input_scheduling();
    test_rpc_outcomes();
    test_compact();
    test_anchor_recovery();
    test_native_rpc_before_roster();
    test_word_authority();
    test_spatial_writer_interest(false);
    test_spatial_writer_interest(true);
    test_spatial_subscription_rates(false);
    test_spatial_subscription_rates(true);
    test_authority_batch(false);
    test_authority_batch(true);
    test_grant_replication_baseline();
    MemoryTransport h, a, b, late;
    SessionOptions options;
    options.room = 105;
    options.admission = {6, true, true};
    options.host = true;
    Schemas types;
    types.add({1, 1, {{1, FieldType::Float}}});
    types.add({2, 1, {{1, FieldType::Int}}});
    GameSession host(h, options, types);
    options.host = false;
    options.extended_join = true;
    GameSession alice(a, options, types), bob(b, options, types);
    std::unique_ptr<GameSession> rejoin;
    for (auto game : {&host, &alice, &bob}) {
        game->writers().register_model(1, 1, {1});
        game->writers().allow_requests(1, {ClaimMode::Dynamic, 3});
        game->writers().register_model(2, 1, {1});
        game->writers().allow_requests(
            2, {ClaimMode::PlayerAttached, 0, {}, [game](ObjectId id, Tick) { game->world().destroy(id); }});
    }
    TimePoint now{};
    auto pump = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            now += 30ms;
            host.pump(now);
            alice.pump(now);
            bob.pump(now);
            if (rejoin)
                rejoin->pump(now);
        }
    };
    h.link(a);
    h.link(b);
    pump(5);
    check(host.start(now), "start");
    check(host.world().load_map(8, "courtyard") && host.world().load_map(9, "tower"), "load maps");
    auto root = host.world().spawn(1, 1, {4.f}, {8, 100});
    auto door = host.world().spawn(1, 1, {3.f}, {9, 100});
    auto orb = host.world().spawn(1, 1, {1.f}, {0, 33});
    check(root && door && orb && !host.world().spawn(1, 1, {9.f}, {0, 33}), "stable identity collision");
    pump(40);
    check(alice.world().maps().size() == 2 && alice.object(orb) && !alice.object(root),
          "scene leaked before ready");
    check(alice.world().loaded(8, 1) && bob.world().loaded(9, 2), "load acknowledgement");
    pump(5);
    check(alice.world().find(8, 100) == root && !alice.object(door) && !bob.object(root) &&
              bob.world().find(9, 100) == door,
          "independent scene gates");
    check(alice.writers().request(orb) && bob.writers().request(orb), "concurrent dynamic claims");
    bool lost = false;
    bob.writers().on_prediction_failed([&](ObjectId id) { lost = id == orb; });
    check(alice.writers().submit_predicted(orb, 10, {8.f}) && bob.writers().submit_predicted(orb, 10, {6.f}),
          "predicted actions before host reply");
    check(std::get<float>(bob.writers().presentation(*bob.object(orb)).values[0]) == 6 &&
              std::get<float>(host.object(orb)->values[0]) == 1,
          "local prediction touched canonical data");
    pump(8);
    check(lost && bob.writers().presentation_writer(orb) == alice.local() &&
              std::get<float>(bob.object(orb)->values[0]) == 8,
          "optimistic loser failed to converge");
    host.world().allow_spawn(1, [](PlayerId player, SceneId) { return Values{float(player)}; });
    ObjectId spawned = 0;
    alice.world().on_spawn_result = [&](std::uint32_t, ObjectId id) { spawned = id; };
    auto request = alice.world().request_spawn(1, 8);
    check(request != 0, "spawn request");
    auto duplicate = game_packet(GameMessage::SpawnRequest, host.room(), host.round());
    duplicate.u32(request);
    duplicate.u16(1);
    duplicate.u32(8);
    a.send(alice.guest()->peer(), Lane::Control, duplicate.bytes());
    pump(6);
    check(spawned && host.authority().active().size() == 4 && alice.object(spawned)->owner == alice.local(),
          "dedup spawn");
    check(host.world().reparent(spawned, root) && !host.world().reparent(root, spawned) &&
              !host.world().reparent(spawned, door),
          "hierarchy validation");
    pump(3);
    check(alice.world().identity(spawned)->parent == root && host.world().reparent(spawned, 0),
          "attachment failed");
    pump(2);
    check(alice.world().identity(spawned)->parent == 0 && host.world().reparent(spawned, root),
          "detach failed");
    auto peer = host.host()->members().at(alice.local()).peer;
    host.authority().set_interest(peer, 0, std::set<ObjectId>{spawned});
    pump(6);
    check(!alice.object(spawned) && !alice.world().identity(spawned),
          "out-of-scope parent leaked child state");
    host.authority().set_interest(peer, 0);
    pump(6);
    check(alice.object(spawned) && alice.world().identity(spawned)->parent == root,
          "child re-entry missing parent");
    host.authority().set_distribution(spawned, {InterestKind::User, 99, 1});
    pump(5);
    check(alice.object(root) && !alice.object(spawned), "independent attachment filter");
    host.authority().set_distribution(spawned, {InterestKind::Inherit});
    pump(5);
    check(alice.object(spawned), "attachment did not inherit visible root");
    host.authority().set_distribution(root, {InterestKind::Area, 8, 3});
    pump(5);
    check(!alice.object(root) && !alice.object(spawned), "inherited hidden root");
    host.authority().subscribe(peer,InterestKind::Area,{8});
    pump(6);
    check(alice.object(root) && alice.object(spawned), "inherited re-entry");
    host.authority().set_distribution(root, {});
    pump(5);
    auto locked = host.world().spawn(1, host.local(), {0.f}, {0, 300, 0, ClaimMode::MasterClient});
    auto movable = host.world().spawn(1, host.local(), {0.f}, {0, 301, 0, ClaimMode::Transaction});
    pump(8);
    check(locked && movable && !alice.writers().request(locked) &&
          !host.writers().assign(locked, alice.local(), host.tick()), "locked instance accepted a writer");
    check(alice.world().identity(movable)->mode == ClaimMode::Transaction, "missing per-instance metadata");
    check(host.writers().assign(movable, alice.local(), host.tick()), "first console writer");
    pump(4);
    for (auto* g : {&host, &alice, &bob}) {
        check(g->rpc().register_method({31, false, Direction::Server, Reliability::Reliable, 8,
              RpcSubject::StateWriter, true}), "register writer command");
        check(g->rpc().register_method({32, false, Direction::Server, Reliability::Reliable, 1}),
              "register business owner command");
    }
    std::vector<PlayerId> commands;
    host.on_rpc = [&](const RpcCall& call) { commands.push_back(call.sender); };
    check(alice.rpc().invoke(movable, 31, {1,2}) && !alice.rpc().invoke(movable, 32, {1}),
          "writer and business owner permissions differ");
    pump(3);
    check(commands == std::vector<PlayerId>{alice.local()}, "writer command missing");
    check(alice.rpc().invoke(movable, 31, {5}) && host.writers().assign(movable, bob.local(), host.tick()) &&
          host.writers().assign(movable, alice.local(), host.tick()), "return console to same player");
    pump(4);
    check(commands.size() == 1, "old writer command executed after epoch rollover");
    check(host.writers().assign(movable, bob.local(), host.tick()), "transfer console");
    pump(4);
    check(!alice.rpc().invoke(movable, 31, {}) && bob.rpc().invoke(movable, 31, {7}), "new writer permission");
    pump(4);
    check(commands == std::vector<PlayerId>{alice.local(), bob.local()}, "handoff RPC dispatch");
    auto badge = host.world().spawn(2, 1, {int32_t(5)});
    pump(5);
    bool expired = false;
    alice.writers().on_result([&](const Claim& c, ClaimStatus status) {
        if (c.object == badge && status == ClaimStatus::Expired)
            expired = true;
    });
    host.writers().on_request([](const Claim&) {});
    check(alice.writers().request(badge), "transaction request for player attachment");
    pump(153);
    check(expired && host.writers().assign(badge, alice.local(), host.tick()),
          "request timeout or attachment grant");
    pump(4);
    alice.world().on_spawn_result = {};
    host.world().allow_spawn(1, [](PlayerId p, SceneId){ return Values{float(p)}; }, true);
    auto alice_ref = alice.world().request_reference(1, 0);
    auto bob_ref = bob.world().request_reference(1, 0);
    auto denied_ref = bob.world().request_reference(2,0);
    check(alice_ref && bob_ref && denied_ref && (alice_ref >> 32) != (bob_ref >> 32) &&
          !host.world().resolve(alice_ref), "could not reference a creation before the host materialized it");
    pump(12);
    auto alice_item = host.world().resolve(alice_ref), bob_item = host.world().resolve(bob_ref);
    check(alice_item && bob_item && alice_item != bob_item && alice.world().resolve(bob_ref) == bob_item &&
          bob.world().resolve(alice_ref) == alice_item && !host.world().resolve(denied_ref) &&
          alice.writers().writer(alice_item) == alice.local() && bob.writers().writer(bob_item) == bob.local(),
          "creator identity, automatic writer, or denial failed");
    check(host.world().field_reference(alice_item).value == alice_ref &&
          bob.world().resolve_field({alice_ref}) == alice_item && bob.world().resolve_field({denied_ref}) == 0,
          "field references did not resolve through the current peer namespace");
    const auto badge_reference = host.world().field_reference(badge);
    check(badge_reference.value && bob.world().resolve_field(badge_reference) == badge, "ordinary object reference");
    auto ticket = alice.guest()->ticket();
    a.close(alice.guest()->peer());
    pump(3);
    check(!bob.world().resolve_field(badge_reference), "destroyed field reference still resolved");
    check(!host.object(badge) && !bob.object(badge), "attached object survived owning player disconnect");
    options.resume = ticket;
    rejoin = std::make_unique<GameSession>(late, options, types);
    h.link(late);
    pump(40);
    check(rejoin->guest()->resumed() && rejoin->local() == ticket.player &&
              rejoin->world().maps().size() == 2 && rejoin->world().loaded(8, 1),
          "resume with map table");
    pump(6);
    auto new_ref = rejoin->world().request_reference(1,0);
    check(new_ref && (new_ref >> 32) != (alice_ref >> 32) && rejoin->world().resolve(alice_ref) == alice_item,
          "rejoin reused an issuance namespace or lost a live reference");
    check(rejoin->world().find(8, 100) == root && rejoin->world().identity(spawned)->parent == root,
          "resumed hierarchy");
    check(host.world().unload_map(8) && host.world().load_map(8, "courtyard"), "map reload");
    auto replacement = host.world().spawn(1, 1, {9.f}, {8, 100});
    pump(5);
    check(!rejoin->object(spawned) && !rejoin->object(root) && bob.object(door) &&
              !rejoin->object(replacement) && !rejoin->world().loaded(8, 1) && rejoin->world().loaded(8, 4),
          "map generation isolation");
    pump(5);
    check(rejoin->world().find(8, 100) == replacement && replacement != root && rejoin->object(orb),
          "stable map entity");
    check(!host.world().change_map(2, "") && host.world().maps().size() == 2 && host.object(replacement), "invalid replacement mutated world");
    check(host.world().change_map(8,"next"), "whole-world replacement");
    pump(5);
    check(rejoin->world().maps().size() == 1 && rejoin->world().maps().at(8).asset == "next" &&
          !rejoin->world().loaded(8,4) && rejoin->world().loaded(8,5) && !host.object(replacement) &&
          !host.object(door) && host.object(orb), "replacement leaked scene state or destroyed persistent object");
    std::cout << "clock, dynamic contention, idempotent spawn, stable keys, hierarchy, maps, resume passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
}
