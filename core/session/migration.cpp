#include "session/migration.hpp"
#include <algorithm>
#include <stdexcept>

namespace netcode {
namespace {
using namespace std::chrono_literals;
bool is_migration(const Event &e) {
    return e.type == EventType::Message && !e.payload.empty() &&
           ((e.payload[0] >= uint8_t(GameMessage::MigrationHello) &&
             e.payload[0] <= uint8_t(GameMessage::MigrationCommit)) ||
            e.payload[0] == uint8_t(GameMessage::MigrationYield) ||
            e.payload[0] == uint8_t(GameMessage::MigrationYieldAck));
}
} // namespace
MigratingSession::MigratingSession(std::unique_ptr<Transport> link, SessionOptions options, Schemas schemas,
                                   MigrationOptions migration, Open open, Configure configure)
    : primary_(std::make_unique<Outbound>(std::move(link))), options_(options), schemas_(std::move(schemas)),
      policy_(std::move(migration)), open_(std::move(open)), configure_(std::move(configure)) {
    if ((!policy_.standbys.empty() && !open_) || policy_.lease < 300ms || policy_.lease > 10s)
        throw std::invalid_argument("Invalid migration configuration");
    game_ = make(*primary_, options_);
}
std::unique_ptr<GameSession> MigratingSession::make(Outbound &link, const SessionOptions &opts) {
    auto g = std::make_unique<GameSession>(link, opts, schemas_);
    if (configure_)
        configure_(*g);
    return g;
}
bool MigratingSession::send_permit(Peer peer) {
    auto m = game_->host()->member(peer);
    if (!m)
        return false;
    auto msg = game_packet(GameMessage::MigrationPermit, room_, round_);
    msg.u32(term_);
    msg.u32(successor_);
    msg.u16(uint16_t(electorate_.size()));
    for (auto id : electorate_)
        msg.u32(id);
    return primary_->send(peer, Lane::Control, msg.bytes());
}
bool MigratingSession::protocol(const Event &e, TimePoint now) {
    if (!is_migration(e))
        return false;
    Reader r(e.payload);
    GameMessage type;
    if (!read_game_header(r, type, game_->room(), game_->round()))
        return true;
    if (e.lane != (type == GameMessage::MigrationHello ? Lane::Game : Lane::Control))
        return true;
    if (auto h = game_->host()) {
        if (handed_off_ && type == GameMessage::MigrationVote && e.lane == Lane::Control) {
            uint32_t t;
            PlayerId id;
            Key key{};
            if (!r.u32(t) || t != game_->term() || !r.u32(id))
                return true;
            for (auto &k : key)
                if (!r.u32(k))
                    return true;
            auto m = h->members().find(id);
            if (r.done() && m != h->members().end() && !m->second.peer && id != game_->local() &&
                m->second.key == key) {
                auto msg = game_packet(GameMessage::MigrationCommit, game_->room(), game_->round());
                msg.u32(t);
                msg.u32(game_->local());
                primary_->send(e.peer, Lane::Control, msg.bytes());
            }
            return true;
        }
        auto m = h->member(e.peer);
        if (!m)
            return true;
        if (type == GameMessage::MigrationHello && r.done()) {
            if (optin_.insert(e.peer).second && armed_)
                send_permit(e.peer);
        } else if (type == GameMessage::MigrationEcho) {
            uint32_t t, seq;
            if (r.u32(t) && t == term_ && r.u32(seq) && r.done() && pulses_.count(seq) &&
                electorate_.count(m->id))
                echoes_[m->id] = std::max(echoes_[m->id], pulses_.at(seq));
        }
    } else if (e.peer == game_->guest()->peer()) {
        if (type == GameMessage::MigrationPermit) {
            uint32_t t;
            PlayerId candidate;
            uint16_t n;
            if (!r.u32(t) || t != game_->term() || !r.u32(candidate) || !policy_.standbys.count(candidate) ||
                !r.u16(n) || n < 2 || n > 255 || r.remaining() != n * 4u)
                return true;
            std::set<PlayerId> voters;
            for (unsigned i = 0; i < n; ++i) {
                PlayerId id;
                r.u32(id);
                if (!id || !voters.insert(id).second)
                    return true;
            }
            if (!voters.count(candidate) || !voters.count(game_->coordinator()) ||
                (armed_ && (successor_ != candidate || electorate_ != voters)))
                return true;
            successor_ = candidate;
            electorate_ = std::move(voters);
            armed_ = true;
            room_ = game_->room();
            round_ = game_->round();
            term_ = t;
            ticket_ = game_->guest()->ticket();
            local_ = ticket_.player;
            last_contact_ = now;
        } else if (type == GameMessage::MigrationYield && armed_ && electorate_.size() == 2 &&
                   local_ == successor_) {
            uint32_t term, serial;
            if (r.u32(term) && term == term_ && r.u32(serial) && serial == serial_ && r.done() &&
                !image_.empty()) {
                released_ = true;
            }
        } else if (type == GameMessage::MigrationPulse && armed_) {
            uint32_t t, seq;
            if (!r.u32(t) || t != term_ || !r.u32(seq) || !seq || !r.done())
                return true;
            last_contact_ = now;
            auto echo = game_packet(GameMessage::MigrationEcho, room_, round_);
            echo.u32(t);
            echo.u32(seq);
            primary_->send(e.peer, Lane::Control, echo.bytes());
        } else if (type == GameMessage::MigrationChunk && armed_ && local_ == successor_) {
            uint32_t t, serial, size, at;
            if (!r.u32(t) || t != term_ || !r.u32(serial) || serial <= serial_ || !r.u32(size) || !size ||
                size > CheckpointCodec::max_size || !r.u32(at) || at >= size || !r.remaining() ||
                r.remaining() > size - at)
                return true;
            if (!at) {
                staging_.clear();
                assembling_ = serial;
                image_size_ = size;
            }
            if (assembling_ != serial || image_size_ != size || staging_.size() != at)
                return true;
            Bytes part;
            r.read(r.remaining(), part);
            staging_.insert(staging_.end(), part.begin(), part.end());
            if (staging_.size() == size) {
                Reader blob(staging_);
                CheckpointHeader h;
                if (CheckpointCodec::header(blob, h) && h.room == room_ && h.round == round_ &&
                    h.term == term_ && h.coordinator == game_->coordinator()) {
                    image_ = std::move(staging_);
                    serial_ = serial;
                    saved_tick_ = h.tick;
                }
            }
        }
    }
    return true;
}
void MigratingSession::coordinator(TimePoint now) {
    auto *h = game_->host();
    if (!h || !game_->round())
        return;
    if (!armed_) {
        std::set<PlayerId> voters;
        for (const auto &[id, m] : h->members()) {
            if (m.spectator)
                continue;
            if (id != game_->local() && (!m.peer || !m.ready || !optin_.count(m.peer)))
                return;
            voters.insert(id);
        }
        if (voters.size() < 3)
            return;
        for (const auto &[id, addr] : policy_.standbys)
            if (id != game_->local() && voters.count(id)) {
                successor_ = id;
                break;
            }
        if (!successor_)
            return;
        room_ = game_->room();
        round_ = game_->round();
        term_ = game_->term();
        electorate_ = std::move(voters);
        armed_ = true;
        lease_started_ = now;
        for (auto peer : optin_)
            send_permit(peer);
    }
    if (now >= next_pulse_) {
        next_pulse_ = now + policy_.lease / 4;
        auto msg = game_packet(GameMessage::MigrationPulse, room_, round_);
        msg.u32(term_);
        msg.u32(++sequence_);
        pulses_[sequence_] = now;
        while (pulses_.size() > 8)
            pulses_.erase(pulses_.begin());
        for (const auto &[id, m] : h->members())
            if (m.peer && optin_.count(m.peer))
                primary_->send(m.peer, Lane::Control, msg.bytes());
    }
    publish_image();
}
bool MigratingSession::handoff() {
    if (phase_ != MigrationPhase::Running || armed_ || !game_->host() || !game_->round() ||
        game_->term() == UINT32_MAX)
        return false;
    const auto &members = game_->host()->members();
    if (members.size() != 2)
        return false;
    auto peer = std::find_if(members.begin(), members.end(),
                             [&](const auto &entry) { return entry.first != game_->local(); });
    if (peer == members.end() || !peer->second.ready || peer->second.spectator || !peer->second.peer ||
        !policy_.standbys.count(peer->first) || !optin_.count(peer->second.peer))
        return false;
    auto image = CheckpointCodec::capture(*game_);
    if (image.empty())
        return false;
    room_ = game_->room();
    round_ = game_->round();
    term_ = game_->term();
    successor_ = peer->first;
    electorate_ = {game_->local(), successor_};
    phase_ = MigrationPhase::Retiring;
    sending_ = std::move(image);
    offset_ = 0;
    ++serial_;
    permit_sent_ = false;
    return true;
}
void MigratingSession::retire(TimePoint now) {
    const Peer peer = game_->host()->members().at(successor_).peer;
    for (const auto &e : primary_->poll(now)) {
        if (e.peer != peer)
            continue;
        if (e.type == EventType::Closed) {
            phase_ = MigrationPhase::Fenced;
            return;
        }
        if (e.type != EventType::Message || e.lane != Lane::Control)
            continue;
        Reader in(e.payload);
        GameMessage type;
        uint32_t term, serial;
        if (read_game_header(in, type, room_, round_) && type == GameMessage::MigrationYieldAck &&
            in.u32(term) && term == term_ && in.u32(serial) && serial == serial_ && in.done()) {
            phase_ = MigrationPhase::Ended;
            return;
        }
    }
    if (!permit_sent_)
        permit_sent_ = send_permit(peer);
    if (!permit_sent_)
        return;
    if (!sending_.empty()) {
        publish_image();
        return;
    }
    auto grant = game_packet(GameMessage::MigrationYield, room_, round_);
    grant.u32(term_);
    grant.u32(serial_);
    primary_->send(peer, Lane::Control, grant.bytes());
}
void MigratingSession::reset_election() {
    armed_ = disconnected_ = released_ = permit_sent_ = false;
    successor_ = 0;
    sequence_ = serial_ = assembling_ = image_size_ = saved_tick_ = last_capture_ = 0;
    vote_peer_ = 0;
    offset_ = 0;
    electorate_.clear();
    votes_.clear();
    optin_.clear();
    ballot_peers_.clear();
    echoes_.clear();
    pulses_.clear();
    image_.clear();
    staging_.clear();
    sending_.clear();
    next_join_ = next_pulse_ = {};
}
void MigratingSession::publish_image() {
    auto m = game_->host()->members().find(successor_);
    if (m == game_->host()->members().end() || !m->second.peer || !optin_.count(m->second.peer))
        return;
    if (sending_.empty() && (!last_capture_ || game_->tick() - last_capture_ >= 6)) {
        sending_ = CheckpointCodec::capture(*game_);
        last_capture_ = game_->tick();
        offset_ = 0;
        ++serial_;
    }
    if (sending_.empty())
        return;
    const auto len = std::min<size_t>(16 * 1024, sending_.size() - offset_);
    auto out = game_packet(GameMessage::MigrationChunk, room_, round_);
    out.u32(term_);
    out.u32(serial_);
    out.u32(uint32_t(sending_.size()));
    out.u32(uint32_t(offset_));
    out.append(Bytes(sending_.begin() + offset_, sending_.begin() + offset_ + len));
    if (primary_->send(m->second.peer, Lane::Control, out.bytes())) {
        offset_ += len;
        if (offset_ == sending_.size())
            sending_.clear();
    }
}
void MigratingSession::guest(TimePoint now) {
    auto *guest = game_->guest();
    if (!guest || !game_->round())
        return;
    auto id = guest->ticket().player;
    // A nominated player opens its standby endpoint before advertising readiness.
    if (!guest->spectator() && policy_.standbys.count(id) && !backup_) {
        if (now < next_join_)
            return;
        next_join_ = now + 500ms;
        try {
            if (auto link = open_(true, policy_.standbys.at(id)))
                backup_ = std::make_unique<Outbound>(std::move(link));
        } catch (const std::exception &) {
            return;
        }
        if (!backup_)
            return;
    }
    if (!armed_ && now >= next_pulse_) {
        next_pulse_ = now + 200ms;
        auto hello = game_packet(GameMessage::MigrationHello, game_->room(), game_->round());
        primary_->send(guest->peer(), Lane::Game, hello.bytes());
    }
}
void MigratingSession::fence() {
    phase_ = MigrationPhase::Fenced;
    for (const auto &[id, m] : game_->host()->members())
        if (m.peer)
            primary_->close(m.peer);
}
void MigratingSession::vote(TimePoint now) {
    phase_ = MigrationPhase::Voting;
    if (local_ == successor_) {
        if (!backup_ || image_.empty()) {
            phase_ = MigrationPhase::Fenced;
            return;
        }
        auto opts = options_;
        opts.host = true;
        opts.room = room_;
        opts.admission.coordinator = local_;
        prepared_ = make(*backup_, opts);
        if (!CheckpointCodec::restore(*prepared_, image_, now)) {
            phase_ = MigrationPhase::Fenced;
            return;
        }
        votes_.insert(local_);
    } else {
        // An unused candidate listener must be closed before opening the chosen endpoint as a voter.
        backup_.reset();
    }
    if (!released_ && game_->guest()->peer())
        primary_->close(game_->guest()->peer());
}
void MigratingSession::promote(TimePoint now) {
    auto commit = game_packet(GameMessage::MigrationCommit, room_, round_);
    commit.u32(term_ + 1);
    commit.u32(local_);
    for (const auto &[peer, id] : ballot_peers_) {
        prepared_->receive({EventType::Open, peer}, now);
        backup_->send(peer, Lane::Control, commit.bytes());
    }
    game_.reset();
    primary_ = std::move(backup_);
    game_ = std::move(prepared_);
    options_.host = true;
    options_.admission.coordinator = local_;
    term_ = game_->term();
    phase_ = MigrationPhase::Running;
    reset_election();
    handed_off_ = true;
    last_contact_ = now;
}
void MigratingSession::resume(Peer peer, TimePoint now) {
    auto opts = options_;
    opts.host = false;
    opts.room = room_;
    opts.resume = ticket_;
    opts.term = term_ + 1;
    opts.admission.coordinator = successor_;
    game_.reset();
    primary_ = std::move(backup_);
    game_ = make(*primary_, opts);
    game_->receive({EventType::Open, peer}, now);
    term_ = opts.term;
    phase_ = MigrationPhase::Running;
    options_ = opts;
    reset_election();
    handed_off_ = true;
}
void MigratingSession::standby(TimePoint now) {
    if (phase_ != MigrationPhase::Voting)
        return;
    if (local_ != successor_ && !backup_) {
        if (now < next_join_)
            return;
        next_join_ = now + 300ms;
        try {
            if (auto link = open_(false, policy_.standbys.at(successor_)))
                backup_ = std::make_unique<Outbound>(std::move(link));
        } catch (const std::exception &) {
            return;
        }
    }
    if (!backup_)
        return;
    for (const auto &e : backup_->poll(now)) {
        if (e.type == EventType::Closed) {
            if (local_ != successor_) {
                backup_.reset();
                vote_peer_ = 0;
                return;
            }
            auto it = ballot_peers_.find(e.peer);
            if (it != ballot_peers_.end()) {
                votes_.erase(it->second);
                ballot_peers_.erase(it);
            }
            continue;
        }
        if (local_ != successor_ && e.type == EventType::Open) {
            vote_peer_ = e.peer;
            auto msg = game_packet(GameMessage::MigrationVote, room_, round_);
            msg.u32(term_ + 1);
            msg.u32(local_);
            for (auto k : ticket_.key)
                msg.u32(k);
            backup_->send(e.peer, Lane::Control, msg.bytes());
            continue;
        }
        if (e.type != EventType::Message || e.lane != Lane::Control)
            continue;
        Reader r(e.payload);
        GameMessage type;
        uint32_t t;
        PlayerId id;
        if (!read_game_header(r, type, room_, round_) || !r.u32(t) || t != term_ + 1 || !r.u32(id))
            continue;
        if (local_ == successor_ && type == GameMessage::MigrationVote) {
            Key k{};
            bool valid = true;
            for (auto &word : k)
                valid = r.u32(word) && valid;
            const auto &roster = prepared_->host()->members();
            auto m = roster.find(id);
            if (!valid || !r.done() || m == roster.end() || m->second.key != k || id == local_ ||
                ballot_peers_.count(e.peer))
                continue;
            ballot_peers_[e.peer] = id;
            if (electorate_.count(id))
                votes_.insert(id);
        } else if (local_ != successor_ && e.peer == vote_peer_ && type == GameMessage::MigrationCommit &&
                   id == successor_ && r.done()) {
            resume(e.peer, now);
            return;
        }
    }
    if (local_ == successor_ && votes_.size() > electorate_.size() / 2)
        promote(now);
}
bool MigratingSession::add_standby(PlayerId player, StandbyEndpoint endpoint) {
    if (!open_ || !active() || game_->round() || !player || !endpoint.valid())
        return false;
    for (const auto& [id, existing] : policy_.standbys)
        if (existing.address == endpoint.address && existing.port == endpoint.port &&
            existing.provider_endpoint == endpoint.provider_endpoint) return false;
    return policy_.standbys.emplace(player,std::move(endpoint)).second;
}
void MigratingSession::pump(TimePoint now) {
    if (draining_) {
        draining_->poll(now);
        if (!draining_->traffic().queued_bytes || now >= drain_deadline_) draining_.reset();
    }
    if (policy_.standbys.empty()) { game_->pump(now); return; }
    if (phase_ == MigrationPhase::Retiring) {
        retire(now);
        return;
    }
    if (phase_ == MigrationPhase::Voting) {
        standby(now);
        return;
    }
    if (phase_ != MigrationPhase::Running)
        return;
    if (armed_ && game_->host() && now - lease_started_ >= policy_.lease) {
        size_t live = 1;
        for (const auto &[id, stamp] : echoes_)
            if (electorate_.count(id) && now - stamp < policy_.lease)
                ++live;
        if (live <= electorate_.size() / 2) {
            fence();
            return;
        }
    }
    for (const auto &e : primary_->poll(now)) {
        if (protocol(e, now))
            continue;
        if (handed_off_ && game_->host() && e.type == EventType::Message && !e.payload.empty() &&
            e.payload[0] == uint8_t(GameMessage::MigrationVote))
            continue;
        if (armed_ && game_->guest() && e.type == EventType::Closed && e.peer == game_->guest()->peer()) {
            if (electorate_.size() == 2 && !released_) {
                phase_ = MigrationPhase::Fenced;
                return;
            }
            disconnected_ = true;
            continue;
        }
        game_->receive(e, now);
    }
    if (released_ && game_->guest()) {
        auto peer = game_->guest()->peer();
        vote(now);
        if (prepared_ && phase_ == MigrationPhase::Voting) {
            // Only acknowledge a restored image. Reliable sends can remain queued until poll.
            auto ack = game_packet(GameMessage::MigrationYieldAck, room_, round_);
            ack.u32(term_);
            ack.u32(serial_);
            primary_->send(peer, Lane::Control, ack.bytes());
            primary_->close(peer);
            primary_->poll(now);
            draining_ = std::move(primary_);
            drain_deadline_ = now + policy_.lease;
            promote(now);
        }
        return;
    }
    if (armed_ && electorate_.size() >= 3 && game_->guest() && now - last_contact_ > policy_.lease * 2) {
        vote(now);
        standby(now);
        return;
    }
    if (disconnected_)
        return;
    if (!game_->round() && armed_) {
        armed_ = false;
        image_.clear();
        electorate_.clear();
        if (game_->guest() && game_->guest()->phase() == RoomPhase::Ended) {
            phase_ = MigrationPhase::Ended;
            return;
        }
    }
    game_->flush(now);
    if (game_->host())
        coordinator(now);
    else
        guest(now);
}
} // namespace netcode
