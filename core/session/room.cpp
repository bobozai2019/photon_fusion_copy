#include "session/room.hpp"

#include <algorithm>
#include <random>

namespace netcode {
namespace {

constexpr std::uint8_t version = 1;
enum class Control : std::uint8_t { Join = 1, Resume, Entered, Seats, RoundEnded, RoomEnded, Rejected };

Writer control(Control type)
{
    Writer w;
    w.u8(version);
    w.u8(static_cast<std::uint8_t>(type));
    return w;
}

bool read_control(Reader& reader, Control& type)
{
    std::uint8_t v, byte;
    if (!reader.u8(v) || v != version || !reader.u8(byte)) return false;
    type = static_cast<Control>(byte);
    return true;
}

Key random_key()
{
    std::random_device source;
    return {source(), source(), source(), source()};
}

RoomId new_room()
{
    const auto key = random_key();
    const auto room = (static_cast<RoomId>(key[0]) << 32) | key[1];
    return room ? room : 1;
}

void write_ticket(Writer& w, const ResumeTicket& ticket)
{
    w.u64(ticket.room);
    w.u64(ticket.round);
    w.u32(ticket.player);
    for (const auto word : ticket.key) w.u32(word);
}

bool read_ticket(Reader& r, ResumeTicket& ticket)
{
    if (!r.u64(ticket.room) || !r.u64(ticket.round) || !r.u32(ticket.player)) return false;
    for (auto& word : ticket.key) {
        if (!r.u32(word)) return false;
    }
    return true;
}

}

RoomHost::RoomHost(Transport& transport, RoomId room)
    : transport_(transport), room_(room ? room : new_room()) {}

const Member* RoomHost::member(Peer peer) const
{
    if (!peer) return nullptr;
    const auto it = std::find_if(members_.begin(), members_.end(),
        [peer](const auto& entry) { return entry.second.peer == peer; });
    return it == members_.end() ? nullptr : &it->second;
}

bool RoomHost::ready(Peer peer)
{
    const auto* joined = member(peer);
    if (!joined || phase_ != RoomPhase::Playing) return false;
    members_.at(joined->id).ready = true;
    return true;
}

void RoomHost::receive(const Event& event)
{
    if (event.type == EventType::Closed) {
        const auto* joined = member(event.peer);
        if (!joined) return;
        const auto id = joined->id;
        if (phase_ == RoomPhase::Waiting) {
            members_.erase(id);
            publish();
        } else {
            auto& member = members_.at(id);
            member.peer = 0;
            member.ready = false;
        }
        return;
    }
    if (event.type != EventType::Message || event.lane != Lane::Control) return;
    Reader r(event.payload);
    Control type;
    if (!read_control(r, type) || member(event.peer)) {
        reject(event.peer, Reject::Protocol);
        return;
    }
    switch (type) {
    case Control::Join: join(event.peer, r); break;
    case Control::Resume: resume(event.peer, r); break;
    default: reject(event.peer, Reject::Protocol); break;
    }
}

void RoomHost::join(Peer peer, Reader& reader)
{
    RoomId requested;
    if (!reader.u64(requested) || !reader.done()) return reject(peer, Reject::Protocol);
    if (requested && requested != room_) return reject(peer, Reject::WrongRoom);
    if (phase_ != RoomPhase::Waiting) return reject(peer, Reject::Locked);
    if (members_.size() >= room_capacity) return reject(peer, Reject::Full);
    const auto id = next_player_++;
    const auto it = members_.emplace(id, Member{id, peer, false}).first;
    entered(it->second, false);
    publish();
}

void RoomHost::resume(Peer peer, Reader& reader)
{
    ResumeTicket ticket;
    if (!read_ticket(reader, ticket) || !reader.done()) return reject(peer, Reject::Protocol);
    const auto it = members_.find(ticket.player);
    if (phase_ != RoomPhase::Playing || ticket.room != room_ || ticket.round != round_ ||
        ticket.player == host_player || it == members_.end() || it->second.peer ||
        it->second.key != ticket.key) {
        return reject(peer, Reject::BadResume);
    }
    auto& joined = it->second;
    joined.peer = peer;
    joined.ready = false;
    entered(joined, true);
}

void RoomHost::reject(Peer peer, Reject reason)
{
    auto w = control(Control::Rejected);
    w.u8(static_cast<std::uint8_t>(reason));
    transport_.send(peer, Lane::Control, w.bytes());
    transport_.close(peer);
}

void RoomHost::publish()
{
    auto w = control(Control::Seats);
    w.u64(room_);
    w.u8(static_cast<std::uint8_t>(members_.size()));
    for (const auto& [id, member] : members_) {
        if (member.peer) transport_.send(member.peer, Lane::Control, w.bytes());
    }
}

void RoomHost::entered(const Member& member, bool resumed)
{
    auto w = control(Control::Entered);
    write_ticket(w, {room_, phase_ == RoomPhase::Playing ? round_ : 0, member.id, member.key});
    w.u8(static_cast<std::uint8_t>(members_.size()));
    w.u8(resumed ? 1 : 0);
    transport_.send(member.peer, Lane::Control, w.bytes());
}

bool RoomHost::start()
{
    if (phase_ != RoomPhase::Waiting) return false;
    phase_ = RoomPhase::Playing;
    ++round_;
    for (auto& [id, member] : members_) {
        if (!member.peer) continue;
        member.ready = false;
        member.key = random_key();
        entered(member, false);
    }
    return true;
}

bool RoomHost::end_round()
{
    if (phase_ != RoomPhase::Playing) return false;
    auto w = control(Control::RoundEnded);
    w.u64(room_);
    w.u64(round_);
    phase_ = RoomPhase::Waiting;
    for (auto it = members_.begin(); it != members_.end();) {
        auto& member = it->second;
        if (member.id != host_player && !member.peer) {
            it = members_.erase(it);
            continue;
        }
        if (member.peer) {
            member.ready = false;
            member.key = {};
            transport_.send(member.peer, Lane::Control, w.bytes());
        }
        ++it;
    }
    publish();
    return true;
}

void RoomHost::end_room()
{
    if (phase_ == RoomPhase::Ended) return;
    phase_ = RoomPhase::Ended;
    auto w = control(Control::RoomEnded);
    w.u64(room_);
    for (const auto& [id, member] : members_) {
        if (!member.peer) continue;
        transport_.send(member.peer, Lane::Control, w.bytes());
        transport_.close(member.peer);
    }
}

RoomGuest::RoomGuest(Transport& transport, RoomId requested, std::optional<ResumeTicket> resume)
    : transport_(transport), requested_(requested), resume_(resume),
      ticket_(resume.value_or(ResumeTicket{})) {}

void RoomGuest::receive(const Event& event)
{
    if (event.type == EventType::Closed && (event.peer == peer_ || !peer_)) {
        disconnected_ = event.reason;
        phase_ = RoomPhase::Ended;
        ready_ = false;
        return;
    }
    if (phase_ == RoomPhase::Ended) return;
    if (event.type == EventType::Open && phase_ == RoomPhase::Connecting && !peer_) {
        peer_ = event.peer;
        auto w = control(resume_ ? Control::Resume : Control::Join);
        if (resume_) write_ticket(w, *resume_);
        else w.u64(requested_);
        transport_.send(peer_, Lane::Control, w.bytes());
        return;
    }
    if (event.type != EventType::Message || event.lane != Lane::Control || event.peer != peer_) return;
    Reader r(event.payload);
    Control type;
    if (!read_control(r, type)) return;
    switch (type) {
    case Control::Entered: {
        ResumeTicket ticket;
        std::uint8_t seats, resumed;
        if (!read_ticket(r, ticket) || !r.u8(seats) || !r.u8(resumed) || !r.done() ||
            !ticket.room || (requested_ && ticket.room != requested_) ||
            seats < 2 || seats > room_capacity || resumed > 1 ||
            (ticket_.player && (ticket.player != ticket_.player || ticket.room != ticket_.room))) {
            return;
        }
        ticket_ = ticket;
        seats_ = seats;
        phase_ = ticket.round ? RoomPhase::Playing : RoomPhase::Waiting;
        ready_ = false;
        resumed_ = resumed != 0;
        break;
    }
    case Control::Seats: {
        RoomId room;
        std::uint8_t seats;
        if (r.u64(room) && r.u8(seats) && r.done() && room == ticket_.room &&
            seats >= 2 && seats <= room_capacity) seats_ = seats;
        break;
    }
    case Control::RoundEnded: {
        RoomId room;
        RoundId round;
        if (r.u64(room) && r.u64(round) && r.done() && room == ticket_.room &&
            round == ticket_.round && phase_ == RoomPhase::Playing) {
            ticket_.round = 0;
            ticket_.key = {};
            ready_ = resumed_ = false;
            phase_ = RoomPhase::Waiting;
        }
        break;
    }
    case Control::RoomEnded: {
        RoomId room;
        if (r.u64(room) && r.done() && room == ticket_.room) {
            phase_ = RoomPhase::Ended;
            disconnected_ = Disconnect::Remote;
            ready_ = false;
            ticket_.round = 0;
        }
        break;
    }
    case Control::Rejected: {
        std::uint8_t reason;
        if (r.u8(reason) && r.done() && reason > 0 && reason <= static_cast<std::uint8_t>(Reject::Protocol)) {
            rejected_ = static_cast<Reject>(reason);
            phase_ = RoomPhase::Ended;
        }
        break;
    }
    default: break;
    }
}

}
