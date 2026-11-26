#pragma once

#include "state/replication.hpp"

#include <functional>
#include <tuple>

namespace netcode {

using MethodId = std::uint16_t;
enum class Direction { Server, Client, NetMulticast, Remote };
enum class Reliability { Reliable, BestEffort };

enum class RpcSubject { BusinessOwner, StateWriter };
struct RpcWriter { PlayerId player; Tick epoch; };

struct RpcCall {
    PlayerId sender;
    ObjectId object;
    MethodId method;
    Bytes arguments;
    Tick writer_epoch = 0;
};

struct Method {
    MethodId id;
    bool room_scope;
    Direction direction;
    Reliability reliability;
    std::size_t argument_bytes;
    RpcSubject subject = RpcSubject::BusinessOwner;
    bool variable_size = false;
};

enum class RpcStatus { Received, OutOfScope, PeerLeft, Expired, ObjectGone, RoundEnded, LocalSend, QueueFull };
struct RpcOutcome {
    std::uint64_t token;
    PlayerId player;
    ObjectId object;
    MethodId method;
    RpcStatus status;
};

class Rpc {
public:
    using Owner = std::function<std::optional<PlayerId>(ObjectId)>;
    using Handler = std::function<void(const RpcCall&)>;

    using Known = std::function<bool(Peer, ObjectId)>;
    using WriterLookup = std::function<std::optional<RpcWriter>(ObjectId)>;
    using Destroyed = std::function<bool(ObjectId)>;
    Rpc(Transport& transport, bool host, Owner owner, Handler handle, Known known = {}, WriterLookup writer = {},
        Destroyed destroyed = {});
    void allow_cancel(Peer peer) { if (peers_.count(peer)) cancellable_.insert(peer); }
    std::deque<RpcOutcome> take_outcomes();
    bool register_method(Method method);
    void reset(RoomId room, RoundId round, PlayerId local, PlayerId master = host_player);
    void attach(Peer peer, PlayerId player);
    void detach(Peer peer);
    void forget(ObjectId object);
    bool invoke(ObjectId object, MethodId method, const Bytes& args = {}, PlayerId target = 0, std::uint64_t token = 0);
    void receive(Peer peer, GameMessage type, Reader& payload);
    void update(TimePoint now);
    std::size_t queued() const { return queued_; }
    std::size_t peak_queued() const { return peak_; }

private:
    using Channel = std::pair<ObjectId, Peer>;
    struct Pending { Bytes bytes; std::uint64_t token; MethodId method; unsigned attempts = 0; bool cancelled = false; };
    struct Outgoing {
        std::uint32_t next = 1;
        std::map<std::uint32_t, Pending> packets;
    };
    struct Incoming {
        std::uint32_t next = 1;
        std::map<std::uint32_t, RpcCall> calls;
    };
    bool transmit(Peer peer, const Method& method, const RpcCall& call, std::uint64_t token);
    void record(std::uint64_t token, Peer peer, ObjectId object, MethodId method, RpcStatus status);
    void ordered(Peer peer, std::uint32_t sequence, RpcCall call);
    void rpc_ack(Peer peer, Reader& payload);
    void deliver(const RpcCall& call, const Method& method);
    Peer peer_for(PlayerId player) const;

    Transport& transport_;
    bool host_;
    PlayerId master_ = host_player;
    std::optional<RpcWriter> subject(ObjectId object, const Method& method, PlayerId fallback) const;
    Owner owner_;
    WriterLookup writer_;
    Handler handle_;
    Known known_;
    Destroyed destroyed_;
    std::set<Peer> cancellable_;
    std::deque<RpcOutcome> outcomes_;
    RoomId room_ = 0;
    RoundId round_ = 0;
    PlayerId local_ = 0;
    TimePoint retry_{};
    std::map<MethodId, Method> methods_;
    std::map<Peer, PlayerId> peers_;
    std::map<Channel, Outgoing> outgoing_;
    std::map<Channel, Incoming> incoming_;
    std::size_t queued_ = 0, peak_ = 0;
};

}
