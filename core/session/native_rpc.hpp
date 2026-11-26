#pragma once

#include "state/replication.hpp"
#include <deque>

namespace netcode {

class GameSession;

class NativeRpc {
public:
    enum class Status : uint8_t { Delivered, Unavailable, Rejected, Expired };
    struct Delivery {
        uint32_t receipt;
        ObjectId object;
        PlayerId sender;
        PlayerId writer;
        int32_t target;
        uint32_t epoch;
        Bytes data;
    };
    struct Outcome { uint32_t token; Status status; };
    static constexpr int32_t target_owner = -3;
    static constexpr int32_t target_master = -1;
    static constexpr int32_t target_all = 0;

    void reset();
    bool send(GameSession& game, Transport& link, ObjectId object, int32_t target, uint32_t token,
              const Bytes& data, bool sender_handled = false);
    bool receive(GameSession& game, Transport& link, const Event& event, TimePoint now);
    void finish(GameSession& game, Transport& link, uint32_t receipt, bool delivered);
    void flush(GameSession& game, Transport& link, TimePoint now);
    std::deque<Delivery> take_deliveries();
    std::deque<Outcome> take_outcomes();

private:
    struct Request { Peer caller; uint32_t token; size_t remaining; Status result = Status::Delivered; };
    struct Route { std::shared_ptr<Request> request; Peer recipient; TimePoint deadline; };
    bool route(GameSession& game, Transport& link, Peer caller, ObjectId object, PlayerId sender, int32_t target,
               uint32_t token, uint32_t epoch, const Bytes& data, bool sender_handled, TimePoint now);
    void report(GameSession& game, Transport& link, Peer caller, uint32_t token, Status status);
    void settle(GameSession& game, Transport& link, Request& request, Status status);
    uint32_t next_receipt_ = 0;
    std::map<uint32_t, Route> routes_;
    std::deque<Delivery> deliveries_;
    std::deque<Outcome> outcomes_;
};

} // namespace netcode
