#include "transport/memory.hpp"

#include <utility>

namespace netcode {

MemoryTransport::~MemoryTransport()
{
    // Destructors are also used for fixtures that simulate an abruptly ended peer.
    while (!peers_.empty()) {
        const auto link = peers_.begin()->second;
        link.remote->peers_.erase(link.remote_peer);
        peers_.erase(peers_.begin());
    }
}

void MemoryTransport::link(MemoryTransport& remote)
{
    const auto peer = next_peer_++;
    const auto other = remote.next_peer_++;
    peers_.emplace(peer, Link{&remote, other});
    remote.peers_.emplace(other, Link{this, peer});
    incoming_.push_back({EventType::Open, peer});
    remote.incoming_.push_back({EventType::Open, other});
}

std::vector<Event> MemoryTransport::poll(TimePoint)
{
    return std::exchange(incoming_, {});
}

bool MemoryTransport::send(Peer peer, Lane lane, const Bytes& payload)
{
    const auto found = peers_.find(peer);
    if (found == peers_.end() || payload.empty() ||
        payload.size() > (lane == Lane::Game ? game_payload_limit : control_payload_limit)) {
        return false;
    }
    auto& remote = *found->second.remote;
    auto& sent = lane == Lane::Game ? traffic_.udp_sent : traffic_.tcp_sent;
    auto& received = lane == Lane::Game ? remote.traffic_.udp_received : remote.traffic_.tcp_received;
    sent += payload.size();
    auto delivery = lane == Lane::Game ? std::exchange(next_, Delivery::Normal) : Delivery::Normal;
    if (delivery == Delivery::Drop) {
        ++traffic_.udp_dropped;
        return true;
    }
    if (delivery == Delivery::Hold) {
        held_.emplace_back(peer, payload);
        return true;
    }
    const Event event{EventType::Message, found->second.remote_peer, lane, payload};
    remote.incoming_.push_back(event);
    received += payload.size();
    if (delivery == Delivery::Duplicate) {
        remote.incoming_.push_back(event);
        received += payload.size();
    }
    return true;
}

void MemoryTransport::release_held()
{
    auto held = std::exchange(held_, {});
    for (const auto& [peer, data] : held) send(peer, Lane::Game, data);
}

void MemoryTransport::close(Peer peer)
{
    auto found = peers_.find(peer);
    if (found == peers_.end()) return;
    const auto link = found->second;
    peers_.erase(found);
    link.remote->peers_.erase(link.remote_peer);
    incoming_.push_back({EventType::Closed, peer, Lane::Control, {}, Disconnect::Local});
    link.remote->incoming_.push_back(
        {EventType::Closed, link.remote_peer, Lane::Control, {}, Disconnect::Remote});
}

}
