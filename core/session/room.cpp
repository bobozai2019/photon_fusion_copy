#include "session/room.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>

namespace netcode {
namespace {

constexpr std::uint8_t version = 1;
enum class Control : std::uint8_t {
    Join = 1, Resume, Entered, Seats, RoundEnded, RoomEnded, Rejected, Roster, ChatInput, ChatMessage, JoinExtended
};

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

bool valid_chat(std::string_view text)
{
    return !text.empty() && text.size() <= room_chat_limit &&
        std::any_of(text.begin(), text.end(), [](char byte) { return byte != ' '; }) &&
        std::none_of(text.begin(), text.end(), [](unsigned char byte) { return byte < 32 || byte == 127; });
}

bool read_chat(Reader& r, std::string& text)
{
    Bytes bytes;
    if (r.remaining() == 0 || r.remaining() > room_chat_limit || !r.read(r.remaining(), bytes)) return false;
    text.assign(bytes.begin(), bytes.end());
    return valid_chat(text);
}

Writer chat_message(RoomId room, const RoomMessage& message)
{
    auto w = control(Control::ChatMessage);
    w.u64(room);
    w.u32(message.sender);
    w.append(Bytes(message.text.begin(), message.text.end()));
    return w;
}

void remember(std::deque<RoomMessage>& messages, RoomMessage message)
{
    constexpr std::size_t history_limit = 32;
    if (messages.size() == history_limit) messages.pop_front();
    messages.push_back(std::move(message));
}

}

RoomHost::RoomHost(Transport& transport, RoomId room, RoomOptions options)
    : transport_(transport), room_(room ? room : new_room()), options_(options) {
    if (!options.coordinator) throw std::invalid_argument("Room coordinator must have a player ID");
    members_.emplace(options.coordinator,Member{options.coordinator,0,true});
    next_player_=options.coordinator+1;
    if (!options.capacity) throw std::invalid_argument("Room capacity must include the host");
}

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

bool RoomHost::set_player_ttl(std::optional<uint32_t> ms)
{
    if (phase_ != RoomPhase::Waiting) return false;
    options_.player_ttl_ms = ms;
    return true;
}

std::vector<PlayerId> RoomHost::expire(TimePoint now)
{
    std::vector<PlayerId> removed;
    if (!options_.player_ttl_ms) return removed;
    for (auto it = members_.begin(); it != members_.end();) {
        const auto& m = it->second;
        if (!m.peer && m.reserved_until && now >= *m.reserved_until) {
            removed.push_back(m.id);
            it = members_.erase(it);
        } else ++it;
    }
    if (!removed.empty()) publish();
    return removed;
}

void RoomHost::receive(const Event& event, TimePoint now)
{
    expire(now);
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
            if (options_.player_ttl_ms) member.reserved_until = now + std::chrono::milliseconds(*options_.player_ttl_ms);
        }
        return;
    }
    if (event.type != EventType::Message || event.lane != Lane::Control) return;
    Reader r(event.payload);
    Control type;
    if (!read_control(r, type)) {
        reject(event.peer, Reject::Protocol);
        return;
    }
    if (const auto* joined = member(event.peer)) {
        if (type != Control::ChatInput) return reject(event.peer, Reject::Protocol);
        std::string text;
        if (phase_ == RoomPhase::Waiting && read_chat(r, text)) broadcast_chat(joined->id, std::move(text));
        return;
    }
    switch (type) {
    case Control::Join: join(event.peer, r, false); break;
    case Control::JoinExtended: join(event.peer, r, true); break;
    case Control::Resume: resume(event.peer, r); break;
    default: reject(event.peer, Reject::Protocol); break;
    }
}

void RoomHost::join(Peer peer, Reader& reader, bool extended)
{
    RoomId requested;
    std::uint8_t spectator = 0;
    if (!reader.u64(requested) || (extended && (!reader.u8(spectator) || spectator > 1)) || !reader.done()) return reject(peer, Reject::Protocol);
    if (requested && requested != room_) return reject(peer, Reject::WrongRoom);
    if (!extended && options_.capacity > room_capacity) return reject(peer, Reject::Protocol);
    if ((spectator && !options_.spectators) ||
        (phase_ != RoomPhase::Waiting && !(extended && phase_ == RoomPhase::Playing && options_.late_join)))
        return reject(peer, Reject::Locked);
    if (members_.size() >= options_.capacity) return reject(peer, Reject::Full);
    const auto id = next_player_++;
    const auto it = members_.emplace(id, Member{id, peer, false, phase_ == RoomPhase::Playing ? random_key() : Key{}, spectator != 0}).first;
    entered(it->second, false);
    publish();
    if (phase_ != RoomPhase::Waiting) return;
    for (const auto& message : messages_) {
        transport_.send(peer, Lane::Control, chat_message(room_, message).bytes());
    }
}

void RoomHost::resume(Peer peer, Reader& reader)
{
    ResumeTicket ticket;
    if (!read_ticket(reader, ticket) || !reader.done()) return reject(peer, Reject::Protocol);
    const auto it = members_.find(ticket.player);
    if (phase_ != RoomPhase::Playing || ticket.room != room_ || ticket.round != round_ ||
        ticket.player == options_.coordinator || it == members_.end() || it->second.peer ||
        it->second.key != ticket.key) {
        return reject(peer, Reject::BadResume);
    }
    auto& joined = it->second;
    joined.peer = peer;
    joined.reserved_until.reset();
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
    auto roster = control(Control::Roster);
    roster.u64(room_);
    roster.u8(static_cast<std::uint8_t>(members_.size()));
    for (const auto& [id, member] : members_) roster.u32(id);
    for (const auto& [id, member] : members_) {
        if (!member.peer) continue;
        transport_.send(member.peer, Lane::Control, w.bytes());
        transport_.send(member.peer, Lane::Control, roster.bytes());
    }
}

bool RoomHost::kick(PlayerId player)
{
    if (phase_ != RoomPhase::Waiting || player == options_.coordinator) return false;
    const auto it = members_.find(player);
    if (it == members_.end()) return false;
    reject(it->second.peer, Reject::Kicked);
    members_.erase(it);
    publish();
    return true;
}

bool RoomHost::chat(std::string_view text)
{
    if (phase_ != RoomPhase::Waiting || !valid_chat(text)) return false;
    broadcast_chat(options_.coordinator, std::string(text));
    return true;
}

void RoomHost::broadcast_chat(PlayerId sender, std::string text)
{
    remember(messages_, {sender, std::move(text)});
    const auto w = chat_message(room_, messages_.back());
    for (const auto& [id, member] : members_) {
        if (member.peer) transport_.send(member.peer, Lane::Control, w.bytes());
    }
}

void RoomHost::entered(const Member& member, bool resumed)
{
    auto w = control(Control::Entered);
    write_ticket(w, {room_, phase_ == RoomPhase::Playing ? round_ : 0, member.id, member.key});
    w.u8(static_cast<std::uint8_t>(members_.size()));
    w.u8((resumed ? 1 : 0) | (member.spectator ? 2 : 0));
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
        if (member.id != options_.coordinator && !member.peer) {
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

RoomGuest::RoomGuest(Transport& transport, RoomId requested, std::optional<ResumeTicket> resume, bool spectator, bool extended, PlayerId coordinator)
    : transport_(transport), requested_(requested), resume_(resume),
      ticket_(resume.value_or(ResumeTicket{})), spectator_(spectator), extended_(extended || spectator), coordinator_(coordinator) {}

bool RoomGuest::chat(std::string_view text)
{
    if (phase_ != RoomPhase::Waiting || !valid_chat(text)) return false;
    auto w = control(Control::ChatInput);
    w.append(Bytes(text.begin(), text.end()));
    return transport_.send(peer_, Lane::Control, w.bytes());
}

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
        auto w = control(resume_ ? Control::Resume : extended_ ? Control::JoinExtended : Control::Join);
        if (resume_) write_ticket(w, *resume_);
        else {
            w.u64(requested_);
            if (extended_) w.u8(spectator_);
        }
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
            seats < 2 || resumed > 3 ||
            (ticket_.player && (ticket.player != ticket_.player || ticket.room != ticket_.room))) {
            return;
        }
        ticket_ = ticket;
        seats_ = seats;
        phase_ = ticket.round ? RoomPhase::Playing : RoomPhase::Waiting;
        ready_ = false;
        resumed_ = (resumed & 1) != 0;
        spectator_ = (resumed & 2) != 0;
        break;
    }
    case Control::Seats: {
        RoomId room;
        std::uint8_t seats;
        if (r.u64(room) && r.u8(seats) && r.done() && room == ticket_.room &&
            seats >= 2) seats_ = seats;
        break;
    }
    case Control::Roster: {
        RoomId room;
        std::uint8_t count;
        if (!r.u64(room) || room != ticket_.room || !r.u8(count) || count < 2) break;
        std::vector<PlayerId> players(count);
        for (auto& id : players) {
            if (!r.u32(id)) return;
        }
        if (!r.done() || std::find(players.begin(),players.end(),coordinator_)==players.end() || !std::is_sorted(players.begin(), players.end()) ||
            std::adjacent_find(players.begin(), players.end()) != players.end() ||
            !std::binary_search(players.begin(), players.end(), ticket_.player)) break;
        players_ = std::move(players);
        break;
    }
    case Control::ChatMessage: {
        RoomId room;
        RoomMessage message;
        if (phase_ == RoomPhase::Waiting && r.u64(room) && room == ticket_.room &&
            r.u32(message.sender) && message.sender && read_chat(r, message.text)) {
            remember(messages_, std::move(message));
        }
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
        if (r.u8(reason) && r.done() && reason > 0 && reason <= static_cast<std::uint8_t>(Reject::Kicked)) {
            rejected_ = static_cast<Reject>(reason);
            phase_ = RoomPhase::Ended;
        }
        break;
    }
    default: break;
    }
}

}
