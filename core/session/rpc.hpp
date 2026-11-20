#pragma once

#include "state/replication.hpp"

#include <functional>
#include <tuple>

namespace netcode {

using MethodId = std::uint16_t;
enum class Direction { Server, Client, NetMulticast, Remote };
enum class Reliability { Reliable, BestEffort };

struct RpcCall {
    PlayerId sender;
    ObjectId object;
    MethodId method;
    Bytes arguments;
};

struct Method {
    MethodId id;
    bool room_scope;
    Direction direction;
    Reliability reliability;
    std::size_t argument_bytes;
};

class Rpc {
public:
    using Owner = std::function<std::optional<PlayerId>(ObjectId)>;
    using Handler = std::function<void(const RpcCall&)>;

    Rpc(Transport& transport, bool host, Owner owner, Handler handle);
    bool register_method(Method method);
    void reset(RoomId room, RoundId round, PlayerId local);
    void attach(Peer peer, PlayerId player);
    void detach(Peer peer);
    bool invoke(ObjectId object, MethodId method, const Bytes& args = {}, PlayerId target = 0);
    void receive(Peer peer, GameMessage type, Reader& payload);
    void update(TimePoint now);
    std::size_t queued() const { return queued_; }
    std::size_t peak_queued() const { return peak_; }

private:
    using Channel = std::pair<Peer, ObjectId>;
    struct Outgoing {
        std::uint32_t next = 1;
        std::map<std::uint32_t, Bytes> packets;
    };
    struct Incoming {
        std::uint32_t next = 1;
        std::map<std::uint32_t, RpcCall> calls;
    };
    bool transmit(Peer peer, const Method& method, const RpcCall& call);
    void rpc_ack(Peer peer, Reader& payload);
    void deliver(const RpcCall& call, const Method& method);
    Peer peer_for(PlayerId player) const;

    Transport& transport_;
    bool host_;
    Owner owner_;
    Handler handle_;
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
