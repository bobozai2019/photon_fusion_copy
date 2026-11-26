#include "session/object_authority.hpp"
#include "transport/memory.hpp"

#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>

namespace {

using namespace netcode;
constexpr RoomId room = 42;
constexpr RoundId round_id = 1;
constexpr ObjectId piece = 1, motion = 2;

void check(bool valid, const char* description)
{
    if (valid) return;
    std::cerr << description << '\n';
    std::exit(1);
}

class Node {
public:
    explicit Node(PlayerId player, std::uint16_t motion_version = 1)
        : writers(link, schemas, [this](ObjectId id) -> const Object* {
              const auto it = objects.find(id);
              return it == objects.end() ? nullptr : &it->second;
          }, [this](ObjectId id, const Values& values, Tick) {
              if (!commit_allowed) return false;
              ++commits;
              auto& object = objects.at(id);
              ++object.revision;
              object.values = values;
              return true;
          }, [this](const ObjectGrant&) { ++notifications; })
    {
        check(schemas.add({1, 1, {{10, FieldType::Float}, {20, FieldType::Flag}}}), "piece schema");
        check(schemas.add({2, 1, {{10, FieldType::Float}, {20, FieldType::Float}}}), "motion schema");
        check(writers.register_model(1, 1, {10}, 0,
            [this](const Object&, const AuthoritySample&, const AuthoritySample& next, Tick) {
                if (on_piece_validate) on_piece_validate(next);
                const auto cell = std::get<float>(next.values[0]);
                return cell >= 0 && cell <= 15 && cell == std::trunc(cell);
            }), "piece model");
        check(writers.register_model(2, motion_version, {10, 20}, 4), "motion model");
        check(schemas.add({3, 1, {{10, FieldType::Int}, {20, FieldType::String, 32}, {30, FieldType::IntList, 4}}}) &&
              writers.register_model(3, 1, {10, 20, 30}), "inventory model");
        writers.reset(room, round_id, player, player == host_player ? 0 : 1);
    }

    void receive()
    {
        for (const auto& event : link.poll({})) {
            if (writers.receive(event, 40) && event.lane == Lane::Control &&
                event.payload.front() == static_cast<std::uint8_t>(GameMessage::AuthorityGrant))
                grants.push_back(event);
        }
    }

    MemoryTransport link;
    Schemas schemas;
    std::map<ObjectId, Object> objects;
    unsigned commits = 0, notifications = 0;
    bool commit_allowed = true;
    std::function<void(const AuthoritySample&)> on_piece_validate;
    ObjectAuthority writers;
    std::vector<Event> grants;
};

Bytes state(RoundId round_id, ObjectId id, unsigned epoch, Tick sequence, float cell)
{
    auto packet = game_packet(GameMessage::AuthorityState, room, round_id);
    packet.u32(id);
    packet.u32(epoch);
    packet.u32(sequence);
    packet.u32(0);
    packet.f32(cell);
    packet.u16(0);
    return packet.take();
}

void test_nested_and_retried_samples()
{
    Node host(1), guest(2);
    host.link.link(guest.link);
    host.writers.attach(1, 2);
    host.objects = {{piece, {piece, 1, 1, 1, 1, {2.f, true}}},
                    {motion, {motion, 1, 1, 1, 1, {3.f, false}}}};
    guest.objects = host.objects;
    host.writers.flush();
    guest.receive();
    guest.writers.flush();
    host.receive();
    check(host.writers.assign(piece, 2, 30) && host.writers.assign(motion, 2, 30),
          "two compatible grants for nested receive");
    host.on_piece_validate = [&](const AuthoritySample& next) {
        if (next.values != Values{5.f}) return;
        check(host.writers.receive({EventType::Message, 1, Lane::Game, state(round_id, motion, 1, 1, 9.f)}, 40),
              "nested authority message handled");
        check(next.values == Values{5.f}, "nested receive changed the sample being validated");
    };
    check(host.writers.receive({EventType::Message, 1, Lane::Game, state(round_id, piece, 1, 1, 5.f)}, 40) &&
          host.commits == 2 && host.objects.at(piece).values == Values{5.f, true} &&
          host.objects.at(motion).values == Values{9.f, false}, "nested samples retain independent fields");

    const Event update{EventType::Message, 1, Lane::Game, state(round_id, piece, 1, 2, 6.f)};
    host.commit_allowed = false;
    check(host.writers.receive(update, 41) && host.writers.grant(piece)->confirmed == 1 &&
          host.writers.grant(piece)->sample.values == Values{5.f} && host.commits == 2,
          "rejected commit altered the accepted sample");
    host.commit_allowed = true;
    check(host.writers.receive(update, 42) && host.writers.grant(piece)->confirmed == 2 &&
          host.objects.at(piece).values == Values{6.f, true} && host.commits == 3,
          "valid retry used a rejected or nested sample");
}

}

int main()
{
    test_nested_and_retried_samples();
    Node host(1), b(2), c(3), legacy(4, 9);
    host.link.link(b.link);
    host.link.link(c.link);
    host.link.link(legacy.link);
    host.writers.attach(1, 2);
    host.writers.attach(2, 3);
    host.writers.attach(3, 4);
    host.objects = {{piece, {piece, 1, 3, 1, 1, {2.f, false}}},
                    {motion, {motion, 2, 2, 1, 1, {10.f, 20.f}}}};
    c.objects = legacy.objects = host.objects;
    b.objects.emplace(motion, host.objects.at(motion));
    check(host.writers.writer(piece) == host_player, "default writer");
    check(!host.writers.assign(piece, 2, 30), "grant before compatibility");
    host.writers.flush();
    for (auto* guest : {&b, &c, &legacy}) {
        guest->receive();
        guest->writers.flush();
    }
    host.receive();
    check(!host.writers.assign(motion, 4, 30), "different model version");
    check(host.writers.assign(piece, 2, 30), "piece grant");
    check(host.writers.assign(motion, 3, 30, {1, 2, 3, 4}, 12), "motion grant");
    host.writers.flush();
    b.receive();
    c.receive();
    check(b.notifications == 1 && !b.writers.submit(piece, 1, 0, {4.f}),
          "grant arrived before object and enabled submission prematurely");
    b.objects.emplace(piece, host.objects.at(piece));
    b.writers.flush();
    check(b.notifications == 2 && c.notifications == 2, "both object/grant arrival orders");
    check(!b.writers.submit(piece, 1, 0, {true}), "wrong field type");
    check(!b.writers.submit(piece, 1, 0, {4.f, true}), "protected field submission");
    check(!b.writers.submit(piece, 1, 0, {INFINITY}), "nonfinite value");
    check(b.writers.submit(piece, 1, 0, {5.f}), "first piece submission");
    check(b.writers.presentation(b.objects.at(piece)).values == Values{5.f, false}, "local presentation");
    check(b.writers.receive(b.grants.front(), 40) && b.notifications == 2 &&
          b.writers.grant(piece)->sample.values == Values{5.f}, "duplicate grant rolled local state back");
    c.link.send(1, Lane::Game, state(round_id, piece, 1, 50, 9.f));
    b.link.send(1, Lane::Game, state(round_id + 1, piece, 1, 50, 9.f));
    b.link.send(1, Lane::Control, state(round_id, piece, 1, 50, 9.f));
    host.receive();
    check(!host.commits, "wrong player, round, or lane published a state");
    host.objects.at(piece).values[1] = true;
    b.link.next_game(Delivery::Duplicate);
    b.writers.flush();
    host.receive();
    check(host.commits == 1 && host.objects.at(piece).values == Values{5.f, true} &&
          host.objects.at(piece).owner == 3, "duplicate accepted or protected fields lost");
    check(b.writers.submit(piece, 1, 0, {6.f}), "second submission");
    b.link.next_game(Delivery::Hold);
    b.writers.flush();
    check(b.writers.submit(piece, 1, 0, {7.f}), "third submission");
    b.writers.flush();
    b.link.release_held();
    host.receive();
    check(host.commits == 2 && host.objects.at(piece).values == Values{7.f, true},
          "out-of-order submission accepted");
    b.receive();
    check(b.writers.grant(piece)->confirmed == 3 &&
          b.writers.grant(piece)->sample.values == Values{7.f}, "acknowledgment lost latest local state");
    check(c.writers.submit(motion, 1, 19, {11.f, 20.f}, {4, 3, 2, 1}), "motion submission");
    c.writers.flush();
    host.receive();
    check(host.writers.assign(motion, 2, 40) &&
          host.writers.grant(motion)->sample.source_step == 19 &&
          host.writers.grant(motion)->sample.recovery == Bytes{4, 3, 2, 1}, "motion handoff recovery");
    b.link.send(1, Lane::Game, state(round_id, piece, 1, 4, 7.5f));
    host.receive();
    check(host.commits == 3 && host.objects.at(piece).values == Values{7.f, true}, "invalid cell published");
    check(host.writers.assign(piece, 3, 40), "transfer piece");
    b.link.send(1, Lane::Game, state(round_id, piece, 1, 10, 9.f));
    host.receive();
    check(host.commits == 3, "old epoch accepted");
    host.writers.flush();
    b.receive();
    c.receive();
    c.objects.erase(piece);
    c.writers.forget(piece);
    check(c.writers.receive(c.grants[c.grants.size() - 2], 40) && !c.writers.grant(piece),
          "grant revived destroyed object");
    host.writers.detach(1, 40);
    check(host.writers.writer(motion) == host_player && host.writers.grant(motion)->epoch == 3,
          "disconnect did not revoke");
    host.writers.attach(1, 2);
    check(!host.writers.assign(motion, 2, 40), "reattach inherited capability from old link");
    const Object inventory{3, 3, 1, 1, 1, {std::int32_t(1), std::string("Bag"), IntArray{1}}};
    host.objects[3] = c.objects[3] = inventory;
    check(host.writers.assign(3, 3, 41), "inventory grant");
    host.writers.flush(); c.receive();
    const Values contents{std::int32_t(16777219), std::string("Bag B"), IntArray{-2, INT32_MAX}};
    check(c.writers.submit(3, 1, 0, contents), "typed inventory submission");
    c.writers.flush(); host.receive();
    check(host.objects.at(3).values == contents, "typed grant did not preserve inventory");
    host.writers.reset(room, round_id + 1, host_player);
    check(!host.writers.grant(piece) && !host.writers.grant(motion), "round retained grants");
}
