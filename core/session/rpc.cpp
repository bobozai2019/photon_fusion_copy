#include "session/rpc.hpp"

#include <algorithm>
#include <utility>

namespace netcode {
namespace {

constexpr std::uint32_t window = 64;
constexpr std::size_t max_queued = 256, max_args = 256;
constexpr auto retry_period = std::chrono::milliseconds(100);

}

Rpc::Rpc(Transport& transport, bool host, Owner owner, Handler handle, Known known, WriterLookup writer)
    : transport_(transport), host_(host), owner_(std::move(owner)), writer_(std::move(writer)), handle_(std::move(handle)), known_(std::move(known)) {}

std::deque<RpcOutcome> Rpc::take_outcomes() { return std::exchange(outcomes_, {}); }

void Rpc::record(std::uint64_t token, Peer peer, ObjectId id, MethodId method, RpcStatus status)
{
    if (token) outcomes_.push_back({token, peer ? peers_.at(peer) : local_, id, method, status});
}

std::optional<RpcWriter> Rpc::subject(ObjectId id, const Method& method, PlayerId fallback) const
{
    if (!id) return RpcWriter{fallback, 0};
    if (method.subject == RpcSubject::StateWriter) return writer_(id);
    auto owner = owner_(id);
    return owner ? std::optional{RpcWriter{*owner, 0}} : std::nullopt;
}

bool Rpc::register_method(Method method)
{
    if (method.subject == RpcSubject::StateWriter &&
        (!writer_ || method.room_scope || method.direction == Direction::NetMulticast)) return false;
    if (!method.id || method.argument_bytes > max_args ||
        (method.room_scope && method.direction == Direction::Remote)) return false;
    return methods_.emplace(method.id, method).second;
}

void Rpc::reset(RoomId room, RoundId round, PlayerId local, PlayerId master)
{
    master_=master;
    for (const auto& [channel, stream] : outgoing_)
        for (const auto& [seq, p] : stream.packets)
            record(p.token,channel.first,channel.second,p.method,RpcStatus::RoundEnded);
    cancellable_.clear();
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
    cancellable_.erase(peer);
    for (auto it = outgoing_.begin(); it != outgoing_.end();) {
        if (it->first.first != peer) {
            ++it;
            continue;
        }
        for (const auto& [seq,p] : it->second.packets) record(p.token,peer,it->first.second,p.method,RpcStatus::PeerLeft);
        queued_ -= it->second.packets.size();
        it = outgoing_.erase(it);
    }
    peers_.erase(peer);
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

bool Rpc::invoke(ObjectId object, MethodId id, const Bytes& args, PlayerId target, std::uint64_t token)
{
    const auto found = methods_.find(id);
    if (!round_ || !local_ || found == methods_.end()) return false;
    const auto& method = found->second;
    if (method.room_scope != (object == 0) || (method.variable_size ? args.size() > method.argument_bytes : args.size() != method.argument_bytes)) return false;
    const auto owner = subject(object, method, target);
    if (!owner) { record(token,0,object,id,RpcStatus::ObjectGone); return false; }
    const RpcCall call{local_, object, id, args, owner->epoch};
    switch (method.direction) {
    case Direction::Server:
        if (host_) {
            handle_(call);
        record(token,0,object,id,RpcStatus::LocalSend);
            return true;
        }
        return (method.room_scope || owner->player == local_) && transmit(peer_for(master_), method, call, token);
    case Direction::Client:
    case Direction::Remote: {
        if (!host_ && method.direction == Direction::Remote) {
            return owner->player == local_ && transmit(peer_for(master_), method, call, token);
        }
        const auto peer = host_ ? peer_for(owner->player) : 0;
        if (peer) return transmit(peer, method, call, token);
        if (method.direction == Direction::Remote || (host_ && owner->player != master_)) return false;
        handle_(call);
        record(token,0,object,id,RpcStatus::LocalSend);
        return true;
    }
    case Direction::NetMulticast: {
        bool sent = true;
        if (host_) {
            for (const auto& [peer, remote] : peers_) sent = transmit(peer, method, call, token) && sent;
        }
        handle_(call);
        record(token,0,object,id,RpcStatus::LocalSend);
        return sent;
    }
    }
    return false;
}

bool Rpc::transmit(Peer peer, const Method& method, const RpcCall& call, std::uint64_t token)
{
    if (!peer) return false;
    if (host_ && call.object && known_ && !known_(peer,call.object)) {
        record(token,peer,call.object,call.method,RpcStatus::OutOfScope);
        return method.direction == Direction::NetMulticast;
    }
    std::uint32_t sequence = 0;
    Outgoing* outgoing = nullptr;
    if (method.reliability == Reliability::Reliable) {
        outgoing = &outgoing_[{peer, call.object}];
        if (queued_ >= max_queued || outgoing->packets.size() >= window || outgoing->next == UINT32_MAX) {
            record(token,peer,call.object,call.method,RpcStatus::QueueFull); return false;
        }
        sequence = outgoing->next++;
    }
    auto packet = game_packet(GameMessage::Rpc, room_, round_);
    packet.u32(call.object);
    packet.u16(call.method);
    packet.u32(sequence);
    if (method.subject == RpcSubject::StateWriter) packet.u32(call.writer_epoch);
    packet.append(call.arguments);
    if (!outgoing) {
        const bool sent = transport_.send(peer, Lane::Game, packet.bytes());
        record(token,peer,call.object,call.method,sent ? RpcStatus::LocalSend : RpcStatus::PeerLeft);
        return sent;
    }
    transport_.send(peer, Lane::Game, packet.bytes());
    outgoing->packets.emplace(sequence, Pending{packet.take(),token,call.method});
    ++queued_;
    peak_ = std::max(peak_, queued_);
    return true;
}

void Rpc::deliver(const RpcCall& call, const Method& method)
{
    const auto owner = subject(call.object, method, 0);
    if (!owner || call.writer_epoch != owner->epoch) return;
    if (host_) {
        if ((method.direction == Direction::Server || method.direction == Direction::Remote) &&
            (method.room_scope || owner->player == call.sender)) handle_(call);
    } else if (method.direction == Direction::NetMulticast ||
               ((method.direction == Direction::Client || method.direction == Direction::Remote) &&
                (method.room_scope || owner->player == local_))) {
        handle_(call);
    }
}

void Rpc::receive(Peer peer, GameMessage type, Reader& payload)
{
    const auto found = peers_.find(peer);
    if (found == peers_.end()) return;
    if (type == GameMessage::RpcAck) return rpc_ack(peer, payload);
    if (type == GameMessage::RpcSkip && cancellable_.count(peer)) {
        ObjectId id; std::uint32_t seq;
        if (payload.u32(id) && payload.u32(seq) && seq && payload.done()) ordered(peer,seq,{found->second,id,0,{}});
        return;
    }
    if (type != GameMessage::Rpc) return;
    RpcCall call{found->second, 0, 0, {}};
    std::uint32_t sequence;
    if (!payload.u32(call.object) || !payload.u16(call.method) || !payload.u32(sequence)) return;
    const auto method = methods_.find(call.method);
    if (method == methods_.end()) return;
    const Method& desc = method->second;
    if (desc.subject == RpcSubject::StateWriter && !payload.u32(call.writer_epoch)) return;
    const auto size = payload.remaining();
    if (desc.room_scope != (call.object == 0) || (desc.variable_size ? size > desc.argument_bytes : size != desc.argument_bytes) ||
        (sequence != 0) != (desc.reliability == Reliability::Reliable) ||
        !payload.read(size, call.arguments)) return;
    if (!sequence) {
        if (!host_ || !call.object || !known_ || known_(peer,call.object)) deliver(call,desc);
        return;
    }

    const ObjectId object = call.object;
    if (object && !outgoing_.count({peer, object}) && !incoming_.count({peer, object}) &&
        !owner_(object)) return;
    ordered(peer,sequence,std::move(call));
}

void Rpc::ordered(Peer peer, std::uint32_t sequence, RpcCall call)
{
    const auto object = call.object;
    auto& stream = incoming_[{peer, object}];
    if (sequence >= stream.next && sequence - stream.next < window) {
        if (call.method) stream.calls.emplace(sequence, std::move(call));
        else stream.calls[sequence] = std::move(call);
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
    transport_.send(peer, cancellable_.count(peer) ? Lane::Control : Lane::Game, ack.bytes());
    const auto current_round = round_;
    for (const auto& next : deliveries) {
        if (round_ != current_round || !peers_.count(peer)) break;
        if (next.method && (!host_ || !next.object || !known_ || known_(peer,next.object))) deliver(next, methods_.at(next.method));
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
        record(it->second.token,peer,object,it->second.method,RpcStatus::Received);
        it = packets.erase(it);
        --queued_;
    }
}

void Rpc::update(TimePoint now)
{
    if (now < retry_) return;
    retry_ = now + retry_period;
    for (auto& [channel, stream] : outgoing_) {
        const auto [peer,id] = channel;
        for (auto& [sequence, p] : stream.packets) {
            if (p.cancelled) continue;
            const bool gone = id && !owner_(id);
            const bool hidden = id && host_ && known_ && !known_(peer,id);
            if (cancellable_.count(peer) && (gone || hidden || p.attempts >= 20)) {
                auto skip = game_packet(GameMessage::RpcSkip,room_,round_);
                skip.u32(id); skip.u32(sequence);
                record(p.token,peer,id,p.method,gone ? RpcStatus::ObjectGone : hidden ? RpcStatus::OutOfScope : RpcStatus::Expired);
                p.token = 0; p.cancelled = true;
                // The control stream delivers cancellation in order even on sustained UDP loss.
                if (!transport_.send(peer,Lane::Control,skip.bytes())) transport_.close(peer);
            } else if (!hidden && !gone) {
                if (p.attempts < 20) ++p.attempts;
                transport_.send(peer,Lane::Game,p.bytes);
            }
        }
    }
}

}
