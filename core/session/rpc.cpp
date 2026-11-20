#include "session/rpc.hpp"

#include <algorithm>

namespace netcode {
namespace {

constexpr std::uint32_t window = 64;
constexpr std::size_t max_queued = 256, max_args = 256;
constexpr auto retry_period = std::chrono::milliseconds(100);

}

Rpc::Rpc(Transport& transport, bool host, Owner owner, Handler handle)
    : transport_(transport), host_(host), owner_(std::move(owner)), handle_(std::move(handle)) {}

bool Rpc::register_method(Method method)
{
    if (!method.id || method.argument_bytes > max_args ||
        (method.room_scope && method.direction == Direction::Remote)) return false;
    return methods_.emplace(method.id, method).second;
}

void Rpc::reset(RoomId room, RoundId round, PlayerId local)
{
    room_ = room;
    round_ = round;
    local_ = local;
    peers_.clear();
    incoming_.clear();
    outgoing_.clear();
    queued_ = 0;
    retry_ = {};
}

void Rpc::attach(Peer peer, PlayerId player)
{
    if (peer && player && player != local_) peers_.emplace(peer, player);
}

void Rpc::detach(Peer peer)
{
    peers_.erase(peer);
    for (auto it = outgoing_.begin(); it != outgoing_.end();) {
        if (it->first.first != peer) {
            ++it;
            continue;
        }
        queued_ -= it->second.packets.size();
        it = outgoing_.erase(it);
    }
    for (auto it = incoming_.begin(); it != incoming_.end();) {
        if (it->first.first == peer) it = incoming_.erase(it);
        else ++it;
    }
}

Peer Rpc::peer_for(PlayerId player) const
{
    for (const auto& [peer, remote] : peers_) {
        if (remote == player) return peer;
    }
    return 0;
}

bool Rpc::invoke(ObjectId object, MethodId id, const Bytes& args, PlayerId target)
{
    const auto found = methods_.find(id);
    if (!round_ || !local_ || found == methods_.end()) return false;
    const auto& method = found->second;
    if (method.room_scope != (object == 0) || method.argument_bytes != args.size()) return false;
    const auto owner = object ? owner_(object) : std::optional<PlayerId>{target};
    if (!owner) return false;
    const RpcCall call{local_, object, id, args};
    switch (method.direction) {
    case Direction::Server:
        if (host_) {
            handle_(call);
            return true;
        }
        return (method.room_scope || *owner == local_) && transmit(peer_for(host_player), method, call);
    case Direction::Client:
    case Direction::Remote: {
        if (!host_ && method.direction == Direction::Remote) {
            return *owner == local_ && transmit(peer_for(host_player), method, call);
        }
        const auto peer = host_ ? peer_for(*owner) : 0;
        if (peer) return transmit(peer, method, call);
        if (method.direction == Direction::Remote || (host_ && *owner > host_player)) return false;
        handle_(call);
        return true;
    }
    case Direction::NetMulticast: {
        bool sent = true;
        if (host_) {
            for (const auto& [peer, remote] : peers_) sent = transmit(peer, method, call) && sent;
        }
        handle_(call);
        return sent;
    }
    }
    return false;
}

bool Rpc::transmit(Peer peer, const Method& method, const RpcCall& call)
{
    if (!peer) return false;
    std::uint32_t sequence = 0;
    Outgoing* outgoing = nullptr;
    if (method.reliability == Reliability::Reliable) {
        if (queued_ >= max_queued) return false;
        outgoing = &outgoing_[{peer, call.object}];
        if (outgoing->packets.size() >= window) return false;
        sequence = outgoing->next++;
    }
    auto packet = game_packet(GameMessage::Rpc, room_, round_);
    packet.u32(call.object);
    packet.u16(call.method);
    packet.u32(sequence);
    packet.append(call.arguments);
    if (!outgoing) return transport_.send(peer, Lane::Game, packet.bytes());
    transport_.send(peer, Lane::Game, packet.bytes());
    outgoing->packets.emplace(sequence, packet.take());
    ++queued_;
    peak_ = std::max(peak_, queued_);
    return true;
}

void Rpc::deliver(const RpcCall& call, const Method& method)
{
    const auto owner = call.object ? owner_(call.object) : std::optional<PlayerId>{0};
    if (!owner) return;
    if (host_) {
        if ((method.direction == Direction::Server || method.direction == Direction::Remote) &&
            (method.room_scope || *owner == call.sender)) handle_(call);
    } else if (method.direction == Direction::NetMulticast ||
               ((method.direction == Direction::Client || method.direction == Direction::Remote) &&
                (method.room_scope || *owner == local_))) {
        handle_(call);
    }
}

void Rpc::receive(Peer peer, GameMessage type, Reader& payload)
{
    const auto found = peers_.find(peer);
    if (found == peers_.end()) return;
    if (type == GameMessage::RpcAck) return rpc_ack(peer, payload);
    if (type != GameMessage::Rpc) return;
    RpcCall call{found->second, 0, 0, {}};
    std::uint32_t sequence;
    if (!payload.u32(call.object) || !payload.u16(call.method) || !payload.u32(sequence)) return;
    const auto method = methods_.find(call.method);
    if (method == methods_.end()) return;
    const Method& desc = method->second;
    if (desc.room_scope != (call.object == 0) || payload.remaining() != desc.argument_bytes ||
        (sequence != 0) != (desc.reliability == Reliability::Reliable) ||
        !payload.read(desc.argument_bytes, call.arguments)) return;
    if (!sequence) return deliver(call, desc);

    const ObjectId object = call.object;
    if (object && !outgoing_.count({peer, object}) && !incoming_.count({peer, object}) &&
        !owner_(object)) return;
    auto& stream = incoming_[{peer, object}];
    if (sequence >= stream.next && sequence - stream.next < window) {
        stream.calls.emplace(sequence, std::move(call));
    }
    std::vector<RpcCall> deliveries;
    for (auto it = stream.calls.find(stream.next); it != stream.calls.end(); it = stream.calls.find(stream.next)) {
        deliveries.push_back(std::move(it->second));
        stream.calls.erase(it);
        ++stream.next;
    }
    auto ack = game_packet(GameMessage::RpcAck, room_, round_);
    ack.u32(object);
    ack.u32(stream.next - 1);
    transport_.send(peer, Lane::Game, ack.bytes());
    const auto current_round = round_;
    for (const auto& next : deliveries) {
        if (round_ != current_round || !peers_.count(peer)) break;
        deliver(next, methods_.at(next.method));
    }
}

void Rpc::rpc_ack(Peer peer, Reader& payload)
{
    ObjectId object;
    std::uint32_t through;
    if (!payload.u32(object) || !payload.u32(through) || !payload.done()) return;
    const auto found = outgoing_.find({peer, object});
    if (found == outgoing_.end() || through >= found->second.next) return;
    auto& packets = found->second.packets;
    for (auto it = packets.begin(); it != packets.end() && it->first <= through;) {
        it = packets.erase(it);
        --queued_;
    }
}

void Rpc::update(TimePoint now)
{
    if (now < retry_) return;
    retry_ = now + retry_period;
    for (const auto& [channel, stream] : outgoing_) {
        for (const auto& [sequence, packet] : stream.packets) {
            transport_.send(channel.first, Lane::Game, packet);
        }
    }
}

}
