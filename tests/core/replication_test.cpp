#include "state/replication.hpp"
#include "transport/memory.hpp"

#include <cstdlib>
#include <iostream>

namespace {

using namespace netcode;

void check(bool ok, const char* message)
{
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}

void test_word_snapshots()
{
    constexpr RoomId room = 20;
    constexpr RoundId round = 1;
    constexpr Peer first = 1, second = 2, legacy = 3;
    constexpr std::size_t kind_offset = 22 + 12, body_offset = 22 + 25;
    Schemas schemas;
    check(schemas.add({1, 1, {{1, FieldType::Vec2}, {2, FieldType::ByteArray, 12}}}), "word schema");
    AuthorityState authority(schemas);
    ReplicaState replica(schemas), delayed(schemas), old(schemas);
    const Values original{Vector2{1, 2}, Bytes{1, 2, 3, 4, 5}};
    check(authority.create(1, 1, 1, original, 1), "word object");
    const auto apply = [&](const Bytes& packet, ReplicaState& state, Peer peer, bool ack = true) {
        Reader in(packet);
        GameMessage type;
        AppliedSnapshot result;
        if (!read_game_header(in, type, room, round) || type != GameMessage::Snapshot ||
            !state.apply(in, room, round, result)) return false;
        if (!ack) return true;
        Reader reply(result.ack);
        return read_game_header(reply, type, room, round) && type == GameMessage::StateAck &&
               authority.acknowledge(peer, reply);
    };
    authority.attach(first, true);
    authority.attach(second, true);
    authority.attach(legacy);
    check(apply(authority.packets(first, room, round, 1).at(0), replica, first) &&
          apply(authority.packets(second, room, round, 1).at(0), delayed, second) &&
          apply(authority.packets(legacy, room, round, 1).at(0), old, legacy), "word baselines");
    check(authority.capture(1, {Vector2{3, 2}, Bytes{1, 2, 3, 4, 5}}, 2) &&
          authority.capture(1, original, 2) && authority.packets(first, room, round, 2).empty(),
          "transient change survived the shadow scan");
    const Values x_changed{Vector2{3, 2}, Bytes{1, 2, 3, 4, 5}};
    check(authority.capture(1, x_changed, 3), "capture vector component");
    const auto x_delta = authority.packets(first, room, round, 3).at(0);
    check(x_delta[kind_offset] == 5 && x_delta[body_offset] == 1 && x_delta[body_offset + 1] == 0 &&
          x_delta.size() == body_offset + 7, "one component did not emit one indexed word");
    auto invalid = x_delta;
    invalid[body_offset + 1] = 97;
    check(!apply(invalid, replica, first) && replica.objects().at(1).values == original,
          "invalid word position committed");
    check(apply(x_delta, replica, first) && replica.objects().at(1).values == x_changed, "word vector apply");
    const Values final{Vector2{3, 6}, Bytes{1, 2, 3}};
    check(authority.capture(1, final, 4), "capture shrunk vector and bytes");
    const auto next = authority.packets(first, room, round, 4).at(0);
    const auto both = authority.packets(second, room, round, 4).at(0);
    const auto fallback = authority.packets(legacy, room, round, 4).at(0);
    check(next[kind_offset] == 5 && both[kind_offset] == 5 && both[body_offset] == next[body_offset] + 1 &&
          fallback[kind_offset] == 1, "delta did not follow per-peer ACK and legacy capability");
    check(apply(next, replica, first) && apply(both, delayed, second) && apply(x_delta, delayed, second) &&
          apply(fallback, old, legacy) && replica.objects().at(1).values == final &&
          delayed.objects().at(1).values == final && old.objects().at(1).values == final,
          "word streams did not converge after a gap, shrink, or stale update");
}

void test_projection_baseline()
{
    constexpr RoomId room = 21;
    constexpr RoundId round = 1;
    Schemas schemas;
    check(schemas.add({1, 1, {{1, FieldType::Vec2}, {2, FieldType::Float}}}), "projection schema");
    AuthorityState host(schemas);
    ReplicaState guest(schemas), other(schemas);
    const Values raw{Vector2{.14f, .24f}, .37f};
    const Values quantized{Vector2{.1f, .2f}, .4f};
    check(host.set_quantum(1, 1, .1f) && host.set_quantum(1, 2, .1f) &&
          host.create(1, 1, 1, raw, 1), "projection object");
    host.attach(1, true);
    host.attach(2, true);
    host.enable_quantization(1, true);
    auto deliver = [&](Peer peer, ReplicaState& replica, const Bytes& packet) {
        Reader in(packet);
        GameMessage type;
        AppliedSnapshot result;
        check(read_game_header(in, type, room, round) && replica.apply(in, room, round, result), "apply projection");
        Reader ack(result.ack);
        check(read_game_header(ack, type, room, round) && host.acknowledge(peer, ack), "projection ACK");
    };
    deliver(1, guest, host.packets(1, room, round, 1).at(0));
    deliver(2, other, host.packets(2, room, round, 1).at(0));
    check(guest.objects().at(1).values == quantized, "first projection");
    host.enable_quantization(1, false);
    check(host.known(1, 1) && host.packets(2, room, round, 2).empty(), "projection affected another peer or visibility");
    deliver(1, guest, host.packets(1, room, round, 2).at(0));
    check(guest.objects().at(1).values == raw, "switch without capture did not clear all quantized fields");
    host.enable_quantization(1, true);
    const auto late = host.packets(1, room, round, 3).at(0);
    host.enable_quantization(1, false);
    const Values latest{Vector2{.16f, .24f}, .37f};
    check(host.capture(1, latest, 4), "capture one unquantized component");
    const auto full = host.packets(1, room, round, 4).at(0);
    check(full[34] == 1, "switch must send a full record while old projection is unacknowledged");
    deliver(1, guest, full);
    deliver(1, guest, late);
    check(guest.objects().at(1).values == latest && host.packets(1, room, round, 5).empty(),
          "late quantized projection rolled back the new baseline");
}

void test_snapshot_staging()
{
    constexpr RoomId room = 22;
    constexpr RoundId round = 1;
    Schemas schemas;
    check(schemas.add({1, 1, {{1, FieldType::Float}, {2, FieldType::ByteArray, 8}}}), "staging schema");
    const auto& schema = *schemas.get(1);
    ReplicaState guest(schemas);
    AppliedSnapshot applied;
    const auto header = [&](ObjectId id, Tick revision, std::uint8_t kind) {
        Writer out;
        out.u32(id);
        out.u32(revision);
        out.u32(revision);
        out.u8(kind);
        out.u16(schema.id);
        out.u16(schema.version);
        out.u32(schema_fingerprint(schema));
        out.u32(1);
        return out;
    };
    const auto record = [&](ObjectId id, Tick revision, std::uint64_t mask, const Values& values) {
        auto out = header(id, revision, 1); // Live record with a typed field mask.
        out.u64(mask);
        for (const auto& value : values) write_value(out, value);
        return out.take();
    };
    const auto packet = [](Tick tick, const Bytes& first, const Bytes& second) {
        Writer out;
        out.u32(tick);
        out.u8(2);
        out.append(first);
        out.append(second);
        return out.take();
    };
    const auto apply = [&](const Bytes& bytes) {
        Reader in(bytes);
        return guest.apply(in, room, round, applied);
    };
    const Values first{1.f, Bytes{1, 2, 3}}, second{2.f, Bytes{4, 5, 6}};
    const auto baseline = packet(1, record(1, 1, 3, first), record(2, 1, 3, second));
    check(apply(baseline), "staging baseline");
    auto incompatible = schema;
    incompatible.fields.back().limit = 7;
    Schemas other_schemas;
    check(other_schemas.add(incompatible), "register a modified copy of the schema");
    ReplicaState other(other_schemas);
    AppliedSnapshot rejected;
    Reader old_signature(baseline);
    check(!other.apply(old_signature, room, round, rejected) && other.objects().empty(),
          "a re-registered layout accepted the signature from its original schema");
    const auto update_first = record(1, 2, 1, {4.f});
    const auto update_second = record(2, 2, 2, {Bytes{7, 8}});
    auto truncated = packet(2, update_first, update_second);
    truncated.pop_back();
    check(!apply(truncated) && !apply(packet(2, update_first, update_first)) &&
          guest.objects().at(1).values == first && guest.objects().at(2).values == second &&
          guest.objects().at(1).revision == 1 && applied.send_tick == 1,
          "invalid packet replaced staged fields or reported an ACK");
    check(apply(packet(2, update_second, update_first)) && applied.changed == std::vector<ObjectId>{1, 2} &&
          guest.objects().at(1).values == Values{4.f, Bytes{1, 2, 3}} &&
          guest.objects().at(2).values == Values{2.f, Bytes{7, 8}},
          "reverse-order sparse updates overwrote unrelated fields");

    const auto word_record = [&](ObjectId id, float value) {
        auto out = header(id, 3, 5); // Live record with one changed word.
        std::uint32_t word;
        pack_words(schema.fields[0], value, &word);
        write_word_varuint(out, 1);
        write_word_change(out, 0, word);
        return out.take();
    };
    const auto word_first = word_record(1, 8.f);
    const auto word_second = word_record(2, 9.f);
    auto invalid_words = packet(3, word_first, word_second);
    invalid_words.pop_back();
    check(!apply(invalid_words) && guest.objects().at(1).revision == 2 &&
          guest.objects().at(1).values == Values{4.f, Bytes{1, 2, 3}} && applied.send_tick == 2,
          "a malformed later word record committed an earlier record");
    check(apply(packet(3, word_second, word_first)) && applied.changed == std::vector<ObjectId>{1, 2} &&
          guest.objects().at(1).values == Values{8.f, Bytes{1, 2, 3}} &&
          guest.objects().at(2).values == Values{9.f, Bytes{7, 8}},
          "retry used words from the rejected packet or from the other object");
}

}

int main()
{
    using namespace netcode;
    test_word_snapshots();
    test_projection_baseline();
    test_snapshot_staging();
    const Field bytes_field{1, FieldType::ByteArray, 13};
    const Value bytes = Bytes{255, 128, 17, 34, 99};
    Words padded_bytes(word_capacity(bytes_field), UINT32_MAX);
    pack_words(bytes_field, bytes, padded_bytes.data());
    Writer byte_packet;
    write_value(byte_packet, bytes);
    check(padded_bytes == Words{5, 0x221180ff, 99, 0, 0} && byte_packet.bytes() == Bytes{5, 0, 255, 128, 17, 34, 99},
          "byte array packing changed little-endian content or tail padding");
    Values rich{1.25f, true, std::int32_t(-17), Vector2{1, 2}, Vector3{1, 2, 3}, Rotation{0, 0, 0, 1},
                std::string("abc"), IntArray{-1, 7}, FloatArray{-10.5f, 0},
                Vector4{{1,2,3,4}},Color{{.25f,0,1,1}},ObjectReference{0xfedcba9876543210ull},
                Bytes{255,0},Vector2Array{{1,-2}},IntMap{{-1,42},{7,-6}},IntLinkedList{2,2,-1}};
    for (const auto& v:rich) {
        auto kind=FieldType(v.index());
        Field f{1,kind,static_cast<uint16_t>(field_capacity({1,kind}) ? 0 : 3)};
        check(valid_value(f,v),"new field rejected");
        Writer w; write_value(w,v); Reader r(w.bytes()); Value decoded;
        check(read_value(r,f,decoded) && r.done() && v==decoded,"new field did not round trip");
        Words words(word_capacity(f), UINT32_MAX);
        pack_words(f, v, words.data());
        check(unpack_words(f, words.data(), decoded) && v == decoded, "word field did not round trip");
        auto short_data=w.bytes(); short_data.pop_back(); Reader short_read(short_data);
        check(!read_value(short_read,f,decoded),"truncated field accepted");
    }
    check(!valid_value({1,FieldType::ByteArray,2},Bytes{1,2,3}),"capacity ignored");
    Writer duplicate; duplicate.u16(2);
    for(int i=0;i<2;++i){duplicate.u32(1); duplicate.u32(7);}
    Reader map_read(duplicate.bytes()); Value bad_map;
    check(!read_value(map_read,{1,FieldType::IntDictionary,2},bad_map),"duplicate map key accepted");
    constexpr RoomId room = 10;
    constexpr RoundId round = 2;
    Schemas schemas;
    check(schemas.add({1, 1, {{10, FieldType::Float}, {20, FieldType::Float}, {30, FieldType::Flag}}}),
          "Schema rejected");
    check(!schemas.add({2, 1, {{10, FieldType::Float}, {10, FieldType::Flag}}}), "Duplicate field allowed");
    AuthorityState a(schemas);
    ReplicaState b(schemas);
    MemoryTransport host, guest;
    host.link(guest);
    const auto to_guest = host.poll({}).front().peer;
    const auto to_host = guest.poll({}).front().peer;

    auto send = [&](Tick tick) {
        for (const auto& packet : a.packets(to_guest, room, round, tick)) {
            check(host.send(to_guest, Lane::Game, packet), "Snapshot send failed");
        }
    };
    auto applied = [&](bool discard_ack = false) {
        unsigned changes = 0;
        for (const auto& event : guest.poll({})) {
            Reader reader(event.payload);
            GameMessage type;
            check(read_game_header(reader, type, room, round) && type == GameMessage::Snapshot, "Bad header");
            AppliedSnapshot result;
            check(b.apply(reader, room, round, result), "Snapshot application failed");
            changes += static_cast<unsigned>(result.changed.size());
            if (!discard_ack) check(guest.send(to_host, Lane::Game, result.ack), "Ack send failed");
        }
        return changes;
    };
    auto acknowledge = [&] {
        for (const auto& event : host.poll({})) {
            Reader reader(event.payload);
            GameMessage type;
            check(read_game_header(reader, type, room, round) && type == GameMessage::StateAck &&
                  a.acknowledge(to_guest, reader), "Ack failed");
        }
    };

    check(a.create(1, 1, 2, {0.0f, 3.0f, false}, 1), "Create failed");
    a.attach(to_guest);
    send(1);
    check(applied() == 1, "First snapshot missing");
    acknowledge();
    check(a.baseline_complete(to_guest), "Baseline fence incomplete");

    Values source{0.75f, 3.0f, false};
    check(a.capture(1, source, 2), "Capture failed");
    source[0] = -0.5f;
    check(std::get<float>(a.get(1)->values[0]) == 0.75f, "Source write affected collected snapshot");
    host.next_game(Delivery::Hold);
    send(3);
    source[2] = true;
    check(a.capture(1, source, 4), "Capture of final value failed");
    host.next_game(Delivery::Drop);
    send(6);
    check(applied() == 0, "Dropped update applied");
    host.next_game(Delivery::Duplicate);
    send(7);
    check(applied(true) == 1, "Duplicate final update applied more than once");
    check(b.objects().at(1).values == source && b.objects().at(1).sample_tick == 4,
          "Last update did not converge or independent field changed");
    host.release_held();
    check(applied() == 0 && b.objects().at(1).values == source, "Late snapshot rolled back state");
    acknowledge();
    send(8);
    check(applied() == 0 && b.objects().at(1).values == source, "Unacked final snapshot retry changed state");
    acknowledge();
    check(a.packets(to_guest, room, round, 9).empty(), "Acknowledged state sent again");

    check(a.destroy(1, 10) && !a.create(1, 1, 2, source, 11), "Can reuse locally destroyed ID");
    send(10);
    check(applied() == 1 && b.objects().empty(), "Tombstone did not invalidate instance");
    acknowledge();
    check(a.active().empty() && a.packets(to_guest, room, round, 11).empty(), "Tombstone not retired");

    AuthorityState paced(schemas);
    check(paced.create(1, 1, 2, {0.0f, 3.0f, false}, 1), "Create paced snapshot");
    paced.attach(to_guest);
    constexpr Tick retry_ticks = 7;
    check(!paced.packets(to_guest, room, round, 1, retry_ticks).empty(), "First paced snapshot missing");
    check(paced.packets(to_guest, room, round, 2, retry_ticks).empty(), "Repeated paced snapshot too early");
    check(paced.capture(1, source, 3), "Capture fresh revision for reliable lane");
    check(!paced.packets(to_guest, room, round, 3, retry_ticks).empty(), "Fresh revision waited for retry");
    check(paced.packets(to_guest, room, round, 9, retry_ticks).empty(), "Retry window ended early");
    check(!paced.packets(to_guest, room, round, 10, retry_ticks).empty(), "Unacked snapshot was lost");
    std::cout << "0 -> 0.75 observed at capture tick 2 -> -0.5 applied from capture tick 4"
                 " after drop at send tick 6 and retry at 7; duplicate, reorder and deletion verified\n";
}
