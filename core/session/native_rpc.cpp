#include "session/native_rpc.hpp"

#include "session/game_session.hpp"
#include <algorithm>
#include <utility>

namespace netcode {
namespace {
constexpr size_t max_data = 4096;
constexpr size_t max_in_flight = 512;
constexpr auto receipt_timeout = std::chrono::seconds(5);

uint32_t writer_epoch(GameSession& game, ObjectId object)
{
    const auto* grant = game.writers().grant(object);
    return grant ? grant->epoch : 0;
}
}

void NativeRpc::reset()
{
    next_receipt_ = 0;
    routes_.clear();
    deliveries_.clear();
    outcomes_.clear();
}

bool NativeRpc::send(GameSession& game, Transport& link, ObjectId object, int32_t target, uint32_t token,
                     const Bytes& data, bool sender_handled)
{
    if (!game.room_properties().ready() || (object && !game.object(object)) ||
        (!object && target == target_owner) || !token ||
        (target < 0 && target != target_owner && target != target_master) ||
        data.empty() || data.size() > max_data) return false;
    const auto epoch = target == target_owner ? writer_epoch(game, object) : 0;
    if (game.host()) {
        const bool accepted = route(game, link, 0, object, game.local(), target, token, epoch, data, sender_handled, Clock::now());
        return accepted || !sender_handled;
    }
    if (game.guest()->spectator()) return false;
    auto request = game_packet(GameMessage::NativeRpcRequest, game.room(), game.round());
    request.u32(object);
    request.u32(uint32_t(target));
    request.u32(token);
    request.u32(epoch);
    request.u8(sender_handled ? 1 : 0);
    request.append(data);
    return link.send(game.guest()->peer(), Lane::Control, request.bytes());
}

bool NativeRpc::route(GameSession& game, Transport& link, Peer caller, ObjectId object, PlayerId sender, int32_t target,
                      uint32_t token, uint32_t epoch, const Bytes& data, bool sender_handled, TimePoint now)
{
    const auto* meta = game.world().identity(object);
    if ((object && (!meta || (caller && !game.authority().known(caller, object)))) ||
        (target < 0 && target != target_master && target != target_owner) ||
        (target == target_owner && (!meta || meta->grant_creator || epoch != writer_epoch(game, object)))) {
        if (caller || !sender_handled) report(game, link, caller, token, Status::Unavailable);
        return false;
    }
    const bool owner = target == target_owner;
    const auto writer = object ? game.writers().writer(object) : game.coordinator();
    const auto recipient = owner ? writer :
        target == target_master ? game.coordinator() : PlayerId(target);
    const auto reachable = [&](const Member& member) {
        return member.peer && game.room_properties().negotiated(member.peer) &&
               (!object || (member.ready && game.authority().known(member.peer, object)));
    };
    std::vector<Peer> destinations;
    if (target == target_all) {
        destinations.push_back(0);
        for (const auto& [_, member] : game.host()->members())
            if (reachable(member)) destinations.push_back(member.peer);
    } else if (recipient == game.local()) {
        destinations.push_back(0);
    } else {
        const auto address = game.host()->members().find(recipient);
        if (address != game.host()->members().end() && reachable(address->second)) destinations.push_back(address->second.peer);
    }
    if (sender_handled && !destinations.empty()) {
        destinations.erase(std::remove(destinations.begin(), destinations.end(), caller), destinations.end());
        if (destinations.empty()) {
            report(game, link, caller, token, Status::Delivered);
            return true;
        }
    }
    if (destinations.empty() || routes_.size() + destinations.size() > max_in_flight ||
        UINT32_MAX - next_receipt_ < destinations.size()) {
        if (caller || !sender_handled) report(game, link, caller, token, Status::Unavailable);
        return false;
    }
    auto request = std::make_shared<Request>(Request{caller, token, destinations.size()});
    for (auto peer : destinations) {
        const auto receipt = ++next_receipt_;
        if (!peer) {
            deliveries_.push_back({receipt, object, sender, writer, target, epoch, data});
        } else {
            auto delivery = game_packet(GameMessage::NativeRpcDelivery, game.room(), game.round());
            delivery.u32(receipt);
            delivery.u32(object);
            delivery.u32(sender);
            delivery.u32(writer);
            delivery.u32(uint32_t(target));
            delivery.u32(epoch);
            delivery.append(data);
            if (!link.send(peer, Lane::Control, delivery.bytes())) {
                settle(game, link, *request, Status::Unavailable);
                continue;
            }
        }
        routes_.emplace(receipt, Route{request, peer, now + receipt_timeout});
    }
    return true;
}

void NativeRpc::settle(GameSession& game, Transport& link, Request& request, Status status)
{
    if (request.result == Status::Delivered) request.result = status;
    if (!--request.remaining) report(game, link, request.caller, request.token, request.result);
}

void NativeRpc::report(GameSession& game, Transport& link, Peer caller, uint32_t token, Status status)
{
    if (!caller) {
        outcomes_.push_back({token, status});
        return;
    }
    auto result = game_packet(GameMessage::NativeRpcResult, game.room(), game.round());
    result.u32(token);
    result.u8(uint8_t(status));
    link.send(caller, Lane::Control, result.bytes());
}

void NativeRpc::finish(GameSession& game, Transport& link, uint32_t receipt, bool delivered)
{
    if (game.host()) {
        auto it = routes_.find(receipt);
        if (it == routes_.end() || it->second.recipient) return;
        settle(game, link, *it->second.request, delivered ? Status::Delivered : Status::Rejected);
        routes_.erase(it);
        return;
    }
    auto ack = game_packet(GameMessage::NativeRpcReceipt, game.room(), game.round());
    ack.u32(receipt);
    ack.u8(delivered);
    link.send(game.guest()->peer(), Lane::Control, ack.bytes());
}

bool NativeRpc::receive(GameSession& game, Transport& link, const Event& event, TimePoint now)
{
    if (event.type != EventType::Message || event.lane != Lane::Control || event.payload.empty() ||
        event.payload.front() < uint8_t(GameMessage::NativeRpcRequest) ||
        event.payload.front() > uint8_t(GameMessage::NativeRpcResult)) return false;
    Reader in(event.payload);
    GameMessage type;
    if (!read_game_header(in, type, game.room(), game.round())) return true;
    // A delivery routed just after Hello can precede the first roster snapshot.
    // Keep it in the bounded delivery queue until the engine sees the snapshot.
    if (!game.room_properties().ready() && (game.host() || type != GameMessage::NativeRpcDelivery)) return true;
    uint32_t object, token, target, sender, writer, epoch;
    uint8_t status;
    Bytes data;
    if (auto* host = game.host()) {
        auto* member = host->member(event.peer);
        if (!member || member->spectator || !game.room_properties().negotiated(event.peer)) return true;
        if (type == GameMessage::NativeRpcRequest && in.u32(object) && in.u32(target) && in.u32(token) && in.u32(epoch) &&
            in.u8(status) && status <= 1 &&
            token && in.remaining() && in.remaining() <= max_data && in.read(in.remaining(), data)) {
            route(game, link, event.peer, object, member->id, int32_t(target), token, epoch, data, status != 0, now);
        } else if (type == GameMessage::NativeRpcReceipt && in.u32(token) && in.u8(status) && status <= 1 && in.done()) {
            auto route = routes_.find(token);
            if (route != routes_.end() && route->second.recipient == event.peer) {
                settle(game, link, *route->second.request, status ? Status::Delivered : Status::Rejected);
                routes_.erase(route);
            }
        }
    } else if (event.peer == game.guest()->peer()) {
        if (type == GameMessage::NativeRpcDelivery && in.u32(token) && in.u32(object) && in.u32(sender) &&
            in.u32(writer) && in.u32(target) && in.u32(epoch) && in.remaining() && in.remaining() <= max_data &&
            in.read(in.remaining(), data) && deliveries_.size() < max_in_flight) {
            deliveries_.push_back({token, object, sender, writer, int32_t(target), epoch, std::move(data)});
        } else if (type == GameMessage::NativeRpcResult && in.u32(token) && in.u8(status) &&
                   status <= uint8_t(Status::Expired) && in.done()) {
            outcomes_.push_back({token, Status(status)});
        }
    }
    return true;
}

void NativeRpc::flush(GameSession& game, Transport& link, TimePoint now)
{
    for (auto it = routes_.begin(); it != routes_.end();) {
        if (now < it->second.deadline) {
            ++it;
        } else {
            settle(game, link, *it->second.request, Status::Expired);
            it = routes_.erase(it);
        }
    }
}

std::deque<NativeRpc::Delivery> NativeRpc::take_deliveries() { return std::exchange(deliveries_, {}); }
std::deque<NativeRpc::Outcome> NativeRpc::take_outcomes() { return std::exchange(outcomes_, {}); }
} // namespace netcode
