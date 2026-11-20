#include "match.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace yard {
namespace {

using namespace netcode;
using namespace std::chrono_literals;
constexpr auto simulation_period = 33333333ns;
constexpr auto send_period = 50ms;
constexpr float dt = 1.0f / 30.0f;
constexpr float rad = 0.01745329252f;

float number(const Object& object, std::size_t index) { return std::get<float>(object.values.at(index)); }

bool valid_input(float value) { return std::isfinite(value) && value >= -1.f && value <= 1.f; }

}

Match::Match(const Layout& layout, std::unique_ptr<Transport> transport, bool host,
             RoomId room, std::optional<ResumeTicket> ticket, Log trace)
    : link_(std::move(transport)), authority_(layout.schemas()), replica_(layout.schemas()),
      rpc_(*link_, host, [this](ObjectId id) -> std::optional<PlayerId> {
          if (const auto* found = object(id)) return found->owner;
          return std::nullopt;
      }, [this](const RpcCall& call) { receive_rpc(call); }),
      trace_(std::move(trace))
{
    if (host) host_.emplace(*link_, room);
    else guest_.emplace(*link_, room, ticket);
    rpc_.register_method({TryOpen, false, Direction::Server, Reliability::Reliable, 4});
    rpc_.register_method({OpenDenied, false, Direction::Client, Reliability::Reliable, 1});
    rpc_.register_method({PlaceBarrier, false, Direction::Server, Reliability::Reliable, 0});
    rpc_.register_method({RemoveBarrier, false, Direction::Server, Reliability::Reliable, 0});
    rpc_.register_method({RoomNotice, true, Direction::NetMulticast, Reliability::Reliable, 1});
    rpc_.register_method({Horn, false, Direction::NetMulticast, Reliability::BestEffort, 0});
}

RoomPhase Match::phase() const { return host_ ? host_->phase() : guest_->phase(); }
RoomId Match::room() const { return host_ ? host_->id() : guest_->ticket().room; }
RoundId Match::round() const
{
    if (phase() != RoomPhase::Playing) return 0;
    return host_ ? host_->round() : guest_->ticket().round;
}
PlayerId Match::player() const { return host_ ? host_player : guest_->ticket().player; }
ResumeTicket Match::ticket() const { return guest_ ? guest_->ticket() : ResumeTicket{}; }
Reject Match::rejected() const { return guest_ ? guest_->rejected() : Reject::None; }
Disconnect Match::loss() const { return guest_ ? guest_->disconnected() : Disconnect::None; }
std::size_t Match::seats() const { return host_ ? host_->members().size() : guest_->seats(); }
bool Match::ready() const { return host_ ? phase() == RoomPhase::Playing : guest_->ready(); }
ObjectId Match::car() const { return find(Car, player()); }
std::size_t Match::ready_guests() const
{
    if (!host_) return 0;
    return std::count_if(host_->members().begin(), host_->members().end(), [](const auto& member) {
        return member.second.peer && member.second.ready;
    });
}

std::vector<ObjectId> Match::objects() const
{
    if (host_) return authority_.active();
    std::vector<ObjectId> result;
    for (const auto& [id, obj] : replica_.objects()) result.push_back(id);
    return result;
}

const Object* Match::object(ObjectId id) const
{
    if (host_) return authority_.get(id);
    const auto found = replica_.objects().find(id);
    return found == replica_.objects().end() ? nullptr : &found->second;
}

std::uint64_t Match::digest() const
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto id : objects()) {
        const auto& obj = *object(id);
        Writer w;
        w.u32(id);
        w.u16(obj.schema);
        w.u32(obj.owner);
        for (const auto& value : obj.values) {
            if (const auto* f = std::get_if<float>(&value)) w.f32(*f);
            else w.u8(std::get<bool>(value));
        }
        for (const auto byte : w.bytes()) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

void Match::log(const std::string& message) const { if (trace_) trace_(message); }

ObjectId Match::find(SchemaId schema, PlayerId owner) const
{
    for (const auto id : objects()) {
        const auto* obj = object(id);
        if (obj->schema == schema && (!owner || obj->owner == owner)) return id;
    }
    return 0;
}

ObjectId Match::spawn(SchemaId schema, PlayerId owner, const Values& values)
{
    const auto id = next_object_++;
    if (!authority_.create(id, schema, owner, values, tick_)) {
        throw std::runtime_error("Invalid template spawn for schema " + std::to_string(schema));
    }
    return id;
}

void Match::clear_world()
{
    authority_.clear();
    replica_.clear();
    drivers_.clear();
    attached_.clear();
    rpc_.reset(room(), round(), player());
    next_object_ = 1;
    gate_ = beacon_ = input_sequence_ = tick_ = send_tick_ = 0;
    sample_count_ = 0;
    local_ = {};
    notice_.clear();
    frozen_ = false;
}

void Match::begin(std::size_t count)
{
    clear_world();
    tick_ = 1;
    std::size_t slot = 0;
    for (const auto& [id, member] : host_->members()) {
        const auto car_id = spawn(Car, id, {205.f + 32.f * float(slot), 210.f + 53.f * float(slot),
                                           0.f, 1.f, 1.f, 0.f, 0.f});
        drivers_.emplace(id, Driver{car_id});
        ++slot;
    }
    gate_ = spawn(Gate, host_player, {345.f, 290.f, 0.f, 1.f, 1.f, false});
    beacon_ = spawn(Beacon, 0, {690.f, 420.f, 0.f, 22.f, 20.f, 0.f, 1.f, 1.f, 1.f, 0.f});
    for (std::size_t i = objects().size(); i < count; ++i) {
        spawn(Load, 0, {80.f + float(i % 16) * 49.f, 76.f + float(i / 16) * 72.f,
                        0.f, 1.f, 1.f, float(i)});
    }
    log("round_started round=" + std::to_string(round()) + " objects=" + std::to_string(objects().size()));
}

bool Match::start(std::size_t count)
{
    if (!host_ || count > 100 || !host_->start()) return false;
    begin(count);
    return true;
}

void Match::end_round()
{
    if (host_ && host_->end_round()) clear_world();
}

void Match::end_room()
{
    if (host_) {
        host_->end_room();
        clear_world();
    }
}

void Match::disconnect()
{
    if (guest_ && guest_->peer()) link_->close(guest_->peer());
}

void Match::set_input(Input input)
{
    if (valid_input(input.throttle) && valid_input(input.steer)) local_ = input;
}

bool Match::open_gate(bool public_object)
{
    const auto door = host_ ? gate_ : find(Gate);
    if (!ready() || !door) return false;
    Writer arg;
    arg.u32(door);
    return rpc_.invoke(public_object ? door : car(), TryOpen, arg.bytes());
}

bool Match::barrier(bool create)
{
    return ready() && rpc_.invoke(car(), create ? PlaceBarrier : RemoveBarrier);
}

bool Match::horn() { return ready() && rpc_.invoke(car(), Horn); }

void Match::receive(const Event& event)
{
    if (event.type == EventType::Closed && host_) {
        if (const auto* member = host_->member(event.peer)) {
            const auto driver = drivers_.find(member->id);
            if (driver != drivers_.end()) {
                driver->second.input = {};
                driver->second.sequence = 0;
                driver->second.last_input = 0;
            }
            log("peer_lost player=" + std::to_string(member->id));
        }
        authority_.detach(event.peer);
        rpc_.detach(event.peer);
        attached_.erase(event.peer);
    }
    const auto previous_round = round();
    if (host_) host_->receive(event);
    else guest_->receive(event);
    if (guest_ && round() != previous_round) clear_world();
    if (phase() != RoomPhase::Playing || event.type != EventType::Message || event.lane != Lane::Game) return;
    Reader reader(event.payload);
    GameMessage type;
    if (!read_game_header(reader, type, room(), round())) return;
    if (host_) host_message(event, type, reader);
    else if (event.peer == guest_->peer()) guest_message(event, type, reader);
}

void Match::host_message(const Event& event, GameMessage type, Reader& reader)
{
    const auto* member = host_->member(event.peer);
    if (!member || !attached_.count(event.peer)) return;
    if (type == GameMessage::StateAck) {
        authority_.acknowledge(event.peer, reader);
        if (!member->ready && authority_.baseline_complete(event.peer)) {
            host_->ready(event.peer);
            rpc_.attach(event.peer, member->id);
            log("baseline_ack player=" + std::to_string(member->id));
        }
        return;
    }
    if (!member->ready) return;
    if (type == GameMessage::Input) {
        std::uint32_t sequence;
        Input input;
        auto& driver = drivers_.at(member->id);
        if (!reader.u32(sequence) || !reader.f32(input.throttle) || !reader.f32(input.steer) ||
            !reader.done() || sequence <= driver.sequence ||
            !valid_input(input.throttle) || !valid_input(input.steer)) return;
        if (input.throttle != driver.input.throttle || input.steer != driver.input.steer) {
            log("input player=" + std::to_string(member->id) + " seq=" + std::to_string(sequence) +
                " throttle=" + std::to_string(input.throttle));
        }
        driver.sequence = sequence;
        driver.input = input;
        driver.last_input = tick_;
    } else {
        rpc_.receive(event.peer, type, reader);
    }
}

void Match::guest_message(const Event& event, GameMessage type, Reader& reader)
{
    if (type == GameMessage::Snapshot) {
        AppliedSnapshot applied;
        if (replica_.apply(reader, room(), round(), applied)) {
            tick_ = std::max(tick_, applied.send_tick);
            link_->send(event.peer, Lane::Game, applied.ack);
            for (const auto id : applied.changed) {
                if (!object(id)) log("replica_removed object=" + std::to_string(id));
            }
            const auto own_car = car();
            if (own_car && std::find(applied.changed.begin(), applied.changed.end(), own_car) !=
                applied.changed.end()) {
                const auto& obj = *object(own_car);
                log("applied object=" + std::to_string(own_car) + " sample=" +
                    std::to_string(obj.sample_tick) + " send=" + std::to_string(applied.send_tick) +
                    " net_input_x=" + std::to_string(number(obj, 5)));
            }
        }
    } else if (type == GameMessage::Ready && reader.done() && !guest_->ready() && car()) {
        guest_->mark_ready();
        rpc_.attach(event.peer, host_player);
        log("ready player=" + std::to_string(player()) + " car=" + std::to_string(car()) +
            " resumed=" + std::to_string(guest_->resumed()));
    } else if (guest_->ready()) {
        rpc_.receive(event.peer, type, reader);
    }
}

void Match::network_step()
{
    ++send_tick_;
    if (host_) {
        for (const auto& [id, member] : host_->members()) {
            if (!member.peer) continue;
            if (attached_.insert(member.peer).second) authority_.attach(member.peer);
            for (const auto& bytes : authority_.packets(member.peer, room(), round(), send_tick_)) {
                link_->send(member.peer, Lane::Game, bytes);
            }
            if (member.ready && !drivers_.at(id).sequence) {
                link_->send(member.peer, Lane::Game, game_packet(GameMessage::Ready, room(), round()).bytes());
            }
        }
    } else if (guest_->ready()) {
        auto input = game_packet(GameMessage::Input, room(), round());
        input.u32(++input_sequence_);
        input.f32(local_.throttle);
        input.f32(local_.steer);
        link_->send(guest_->peer(), Lane::Game, input.bytes());
    }
}

void Match::simulate()
{
    if (frozen_) return;
    ++tick_;
    std::vector<const Object*> blockers;
    if (const auto* gate = object(gate_); gate && !std::get<bool>(gate->values[5])) blockers.push_back(gate);
    const auto active = objects();
    sample_count_ += active.size();
    for (const auto id : active) {
        const auto* obj = object(id);
        if (obj->schema == Barrier) blockers.push_back(obj);
    }
    for (auto& [id, driver] : drivers_) {
        const auto& car = *object(driver.object);
        const Input input = id == host_player ? local_ :
            (tick_ - driver.last_input <= 8 ? driver.input : Input{});
        driver.speed += (input.throttle * 155.f - driver.speed) * 0.09f;
        if (std::abs(driver.speed) < 0.015f) driver.speed = 0;
        const auto angle = std::remainder(number(car, 2) + input.steer * 125.f * dt, 360.f);
        auto x = std::clamp(number(car, 0) + std::cos(angle * rad) * driver.speed * dt, 38.f, 922.f);
        auto y = std::clamp(number(car, 1) - std::sin(angle * rad) * driver.speed * dt, 40.f, 550.f);
        for (const auto* blocker : blockers) {
            const float width = blocker->schema == Gate ? 18.f : 22.f;
            const float height = blocker->schema == Gate ? 75.f : 20.f;
            if (std::abs(x - number(*blocker, 0)) < width && std::abs(y - number(*blocker, 1)) < height) {
                x = number(car, 0);
                y = number(car, 1);
                driver.speed = 0;
                break;
            }
        }
        if (input.throttle != number(car, 5)) {
            log("authority_write object=" + std::to_string(car.id) + " tick=" + std::to_string(tick_) +
                " net_input_x=" + std::to_string(input.throttle));
        }
        authority_.capture(car.id, {x, y, angle, number(car, 3), number(car, 4), input.throttle,
                                   std::abs(input.throttle)}, tick_);
    }
    if (tick_ % 3 == 0) {
        const float t = float(tick_) * dt;
        const float scale = 1.f + 0.15f * std::sin(t);
        authority_.capture(beacon_, {690.f + 25.f * std::sin(t), 420.f + 10.f * std::cos(t),
                                      6.f * std::sin(t), 22.f, std::remainder(t * 75.f, 360.f),
                                      12.f * std::sin(t), scale, scale, scale, t}, tick_);
        for (const auto id : active) {
            const auto* obj = object(id);
            if (obj->schema != Load || id % 4 != (tick_ / 3) % 4) continue;
            auto values = obj->values;
            values.back() = float(tick_) + float(id);
            values[2] = std::remainder(t * 30.f, 360.f);
            values[3] = 1.f + 0.15f * std::sin(t + float(id));
            values[4] = 1.f + 0.15f * std::cos(t + float(id));
            authority_.capture(id, values, tick_);
        }
    }
}

void Match::update(TimePoint now)
{
    for (const auto& event : link_->poll(now)) receive(event);
    if (phase() != RoomPhase::Playing) {
        next_simulate_ = next_network_ = now;
        return;
    }
    if (host_) {
        for (int catch_up = 0; now >= next_simulate_ && catch_up < 5; ++catch_up) {
            simulate();
            next_simulate_ += simulation_period;
        }
        if (next_simulate_ < now) next_simulate_ = now + simulation_period;
    }
    if (now >= next_network_) {
        network_step();
        next_network_ += send_period;
        if (now >= next_network_) next_network_ = now + send_period;
    }
    rpc_.update(now);
}

void Match::receive_rpc(const RpcCall& call)
{
    const auto id = call.method;
    if (id == Horn || id == RoomNotice || id == OpenDenied) {
        if (id == Horn) notice_ = "Horn from player " + std::to_string(call.sender);
        else if (id == RoomNotice) notice_ = "The yard gate is open";
        else notice_ = call.arguments.front() == 1 ? "Drive closer to the gate" : "The gate is already open";
        log("rpc method=" + std::to_string(id) + " sender=" + std::to_string(call.sender) + " " + notice_);
        return;
    }
    if (!host_) return;
    const auto* car = object(call.object);
    if (!car || car->schema != Car || car->owner != call.sender) return;
    if (id == TryOpen) {
        Reader r(call.arguments);
        ObjectId door;
        if (!r.u32(door) || door != gate_) return;
        const auto& gate = *object(gate_);
        if (std::get<bool>(gate.values[5])) {
            rpc_.invoke(car->id, OpenDenied, {2});
        } else if (std::hypot(number(gate, 0) - number(*car, 0), number(gate, 1) - number(*car, 1)) > 155.f) {
            rpc_.invoke(car->id, OpenDenied, {1});
        } else {
            auto values = gate.values;
            values[2] = 72.f;
            values[5] = true;
            authority_.capture(gate_, values, tick_);
            rpc_.invoke(0, RoomNotice, {1});
            log("door_open player=" + std::to_string(call.sender));
        }
    } else if (id == PlaceBarrier) {
        if (find(Barrier, call.sender)) return;
        const float x = std::clamp(number(*car, 0) + std::cos(number(*car, 2) * rad) * 55.f, 54.f, 906.f);
        const float y = std::clamp(number(*car, 1) - std::sin(number(*car, 2) * rad) * 55.f, 54.f, 530.f);
        const auto barrier = spawn(Barrier, call.sender, {x, y, 0.f, 1.f, 1.f, float(call.sender)});
        log("barrier_spawn object=" + std::to_string(barrier));
    } else if (id == RemoveBarrier) {
        const auto id = find(Barrier, call.sender);
        if (id && authority_.destroy(id, tick_)) log("barrier_removed object=" + std::to_string(id));
    }
}

}
