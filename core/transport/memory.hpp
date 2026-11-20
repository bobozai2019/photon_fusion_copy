#pragma once

#include "transport/transport.hpp"

#include <deque>
#include <map>

namespace netcode {

enum class Delivery { Normal, Drop, Duplicate, Hold };

class MemoryTransport final : public Transport {
public:
    ~MemoryTransport() override;
    void link(MemoryTransport& remote);
    void next_game(Delivery delivery) { next_ = delivery; }
    void release_held();
    std::vector<Event> poll(TimePoint now) override;
    bool send(Peer peer, Lane lane, const Bytes& payload) override;
    void close(Peer peer) override;
    Traffic traffic() const override { return traffic_; }

private:
    struct Link { MemoryTransport* remote; Peer remote_peer; };
    Peer next_peer_ = 1;
    std::map<Peer, Link> peers_;
    std::vector<Event> incoming_;
    std::deque<std::pair<Peer, Bytes>> held_;
    Delivery next_ = Delivery::Normal;
    Traffic traffic_;
};

}
