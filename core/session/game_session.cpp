#include "session/game_session.hpp"
#include "session/checkpoint.hpp"
#include <stdexcept>

namespace netcode {

GameSession::GameSession(Transport& link, SessionOptions options, Schemas schemas)
    : GameSession(std::make_unique<Outbound>(link), nullptr, options, std::move(schemas)) {}

GameSession::GameSession(Outbound& link, SessionOptions options, Schemas schemas)
    : GameSession({}, &link, options, std::move(schemas)) {}

GameSession::GameSession(std::unique_ptr<Outbound> owned, Outbound* link, SessionOptions options, Schemas schemas)
    : owned_outbound_(std::move(owned)), link_(link ? *link : *owned_outbound_),
      options_(options), schemas_(std::move(schemas)), state_(schemas_), replica_(schemas_),
      writers_(
          link_, schemas_, [this](ObjectId id) { return object(id); },
          [this](ObjectId id, const Values& values, Tick now) { return state_.capture(id, values, now); },
          [this](const ObjectGrant& grant) {
              if (on_grant)
                  on_grant(grant);
          },
          [this](Peer peer, ObjectId id) { return state_.known(peer, id); },
          [this](ObjectId id) {
              const auto* meta = world_.identity(id);
              return meta ? meta->mode : std::nullopt;
          }),
      world_(*this, link_), prediction_(*this, link_),
      rpc_(
          link_, options.host,
          [this](ObjectId id) -> std::optional<PlayerId> {
              auto o = object(id);
              return o ? std::optional{o->owner} : std::nullopt;
          },
          [this](const RpcCall& call) {
              if (on_rpc)
                  on_rpc(call);
          },
          [this](Peer p, ObjectId id) { return state_.known(p, id); },
          [this](ObjectId id) -> std::optional<RpcWriter> {
              if (!object(id)) return std::nullopt;
              const auto* grant = writers_.grant(id);
              return RpcWriter{writers_.writer(id), grant ? grant->epoch : 0};
          }) {
    term_ = options.term;
    if (!term_)
        throw std::invalid_argument("Invalid coordinator term");
    if (options.step.count() <= 0 || options.step.count() > UINT32_MAX)
        throw std::invalid_argument("Session step must be positive");
    if (options.host)
        host_.emplace(link_, options.room, options.admission);
    else
        guest_.emplace(link_, options.room, options.resume, options.spectator, options.extended_join,
                       options.admission.coordinator);
}

RoundId GameSession::round() const {
    if (host_)
        return host_->phase() == RoomPhase::Playing ? host_->round() : 0;
    return guest_->phase() == RoomPhase::Playing ? guest_->ticket().round : 0;
}
PlayerId GameSession::local() const { return host_ ? coordinator() : guest_->ticket().player; }
RoomId GameSession::room() const { return host_ ? host_->id() : guest_->ticket().room; }

const Object* GameSession::object(ObjectId id) const {
    if (host_)
        return state_.get(id);
    auto it = replica_.objects().find(id);
    return it == replica_.objects().end() ? nullptr : &it->second;
}

std::vector<ObjectId> GameSession::objects() const {
    if (host_)
        return state_.active();
    std::vector<ObjectId> result;
    for (const auto& entry : replica_.objects())
        result.push_back(entry.first);
    return result;
}

const SnapshotCodec* GameSession::codec(Peer peer) const {
    auto it = codecs_.find(peer);
    return it == codecs_.end() ? nullptr : &it->second;
}
void GameSession::reset(TimePoint now) {
    origin_ = now;
    tick_ = 1;
    sent_tick_ = 0;
    state_.clear();
    replica_.clear();
    world_.reset();
    resumed_interest_.clear();
    prediction_.reset();
    codecs_.clear();
    decoder_ = {};
    codec_probe_.reset();
    codec_enabled_ = false;
    rpc_.reset(room(), round(), local(), coordinator());
    if (guest_ && round())
        rpc_.attach(guest_->peer(), coordinator());
    clock_.reset(now, host_.has_value(), std::chrono::duration<double>(options_.step).count(), options_.clock_discipline);
    writers_.reset(room(), round(), local(), host_ ? 0 : guest_->peer(), coordinator());
    if (host_ && round())
        for (const auto& [id, member] : host_->members())
            if (member.peer) {
                state_.attach(member.peer);
                world_.attach(member.peer);
            }
}
bool GameSession::advertise(std::unique_ptr<RoomDirectory> directory, RoomListing listing) {
    if (!host_ || !directory)
        return false;
    listing.room = room();
    listing.capacity = options_.admission.capacity;
    listing.occupied = uint8_t(host_->members().size());
    listing.joinable = host_->phase() == RoomPhase::Waiting || (round() && options_.admission.late_join);
    if (!directory->publish(listing))
        return false;
    directory_ = std::move(directory);
    listing_ = std::move(listing);
    return true;
}
bool GameSession::start(TimePoint now) {
    if (!host_ || !host_->start())
        return false;
    reset(now);
    return true;
}
void GameSession::end(TimePoint now) {
    if (host_) {
        host_->end_room();
        link_.poll(now);
        reset(now);
    }
}

void GameSession::apply(const AppliedSnapshot& snapshot) {
    for (const auto id : snapshot.hidden) {
        writers_.unobserve(id);
        prediction_.forget(id);
    }
    for (const auto id : snapshot.destroyed) {
        writers_.forget(id);
        prediction_.forget(id);
    }
    if (on_snapshot)
        on_snapshot(snapshot);
}
AppliedSnapshot GameSession::scene_ready(SceneId scene, bool ready) {
    auto snapshot = replica_.set_scene_ready(scene, ready);
    apply(snapshot);
    return snapshot;
}

void GameSession::ready(Peer peer) {
    auto* m = host_->member(peer);
    if (m && !m->ready && state_.baseline_complete(peer) && host_->ready(peer)) {
        writers_.attach(peer, m->id, !m->spectator);
        rpc_.attach(peer, m->id);
        auto world = world_.peers_.find(peer);
        if (world != world_.peers_.end() && world->second.supported)
            rpc_.allow_cancel(peer);
    }
}

void GameSession::receive(const Event& event, TimePoint now) {
    if (event.type == EventType::Message && !event.payload.empty() &&
        (event.payload.front() == static_cast<unsigned>(GameMessage::RpcSkip) ||
         event.payload.front() == static_cast<unsigned>(GameMessage::RpcAck) ||
         (event.lane == Lane::Game && event.payload.front() == static_cast<unsigned>(GameMessage::Rpc)))) {
        Reader payload(event.payload);
        GameMessage type;
        if (read_game_header(payload, type, room(), round()) &&
            (type != GameMessage::RpcSkip || event.lane == Lane::Control)) {
            auto member = host_ ? host_->member(event.peer) : nullptr;
            if (!(type == GameMessage::Rpc && member && member->spectator))
                rpc_.receive(event.peer, type, payload);
        }
        return;
    }
    if (prediction_.receive(event) || world_.receive(event) || writers_.receive(event, tick_))
        return;
    if (host_) {
        if (event.type == EventType::Closed) {
            if (auto* member = host_->member(event.peer); member && round())
                CheckpointCodec::save_interest(*this,event.peer,member->id);
            writers_.detach(event.peer, tick_);
            state_.detach(event.peer);
            world_.detach(event.peer);
            prediction_.detach(event.peer);
            codecs_.erase(event.peer);
            rpc_.detach(event.peer);
        }
        const bool was_member = host_->member(event.peer) != nullptr;
        expire_members(now);
        host_->receive(event, now);
        if (round() && !was_member && host_->member(event.peer)) {
            state_.attach(event.peer);
            world_.attach(event.peer);
            CheckpointCodec::reattach(*this, event.peer, host_->member(event.peer)->id);
        }
    } else {
        auto previous = round();
        guest_->receive(event);
        if (round() != previous)
            reset(now);
        if (event.peer != guest_->peer())
            return;
    }
    if (event.type != EventType::Message || event.lane != Lane::Game || !round())
        return;
    Reader in(event.payload);
    GameMessage type;
    if (!read_game_header(in, type, room(), round()))
        return;
    if (host_) {
        const auto* m = host_->member(event.peer);
        if (!m)
            return;
        if (type == GameMessage::CodecHello && options_.compact) {
            std::uint16_t version;
            if (in.u16(version) && version == 1 && in.done()) {
                codecs_.try_emplace(event.peer);
                state_.enable_quantization(event.peer, true);
                auto offer = game_packet(GameMessage::CodecAck, room(), round());
                offer.u32(0);
                link_.send(event.peer, Lane::Game, offer.bytes());
            }
        }
        if (type == GameMessage::CodecAck || type == GameMessage::CodecReset) {
            std::uint32_t seq;
            auto it = codecs_.find(event.peer);
            if (it != codecs_.end() && in.u32(seq) && seq && in.done()) {
                if (type == GameMessage::CodecAck)
                    it->second.acknowledge(seq);
                else
                    it->second.discard(seq);
            }
        }
        if (type == GameMessage::StateAck && state_.acknowledge(event.peer, in))
            ready(event.peer);
        if (type == GameMessage::ClockProbe) {
            std::uint32_t seq;
            if (!in.u32(seq) || !seq || !in.done())
                return;
            auto reply = game_packet(GameMessage::ClockSample, room(), round());
            reply.u32(seq);
            reply.u64(std::chrono::duration_cast<std::chrono::microseconds>(now - origin_).count());
            reply.u32(uint32_t(options_.step.count()));
            link_.send(event.peer, Lane::Game, reply.bytes());
        }
    } else if (type == GameMessage::CodecAck && options_.compact) {
        uint32_t n;
        if (in.u32(n) && !n && in.done())
            codec_enabled_ = true;
    } else if (type == GameMessage::Snapshot || (type == GameMessage::CompactSnapshot && options_.compact)) {
        AppliedSnapshot applied;
        Bytes decoded;
        uint32_t seq = 0, missing = 0;
        if (type == GameMessage::CompactSnapshot && !decoder_.decode(in, decoded, seq, missing)) {
            if (missing) {
                auto reset = game_packet(GameMessage::CodecReset, room(), round());
                reset.u32(missing);
                link_.send(event.peer, Lane::Game, reset.bytes());
            }
            return;
        }
        Reader compact(decoded);
        if (replica_.apply(seq ? compact : in, room(), round(), applied)) {
            if (seq) {
                decoder_.remember(seq, std::move(decoded));
                codec_enabled_ = true;
                auto ack = game_packet(GameMessage::CodecAck, room(), round());
                ack.u32(seq);
                link_.send(event.peer, Lane::Game, ack.bytes());
            }
            link_.send(event.peer, Lane::Game, applied.ack);
            apply(applied);
        }
    } else if (type == GameMessage::ClockSample) {
        std::uint32_t seq;
        std::uint64_t us;
        uint32_t step = uint32_t(options_.step.count());
        if (in.u32(seq) && in.u64(us) && (in.done() || in.u32(step)) && step && in.done() &&
            clock_.sample(seq, now, us / 1000000.0))
            clock_.set_step(std::chrono::milliseconds(step));
    }
}
void GameSession::expire_members(TimePoint now) {
    if (!host_) return;
    for (auto player : host_->expire(now)) {
        resumed_interest_.erase(player);
        prediction_.retire_player(player);
    }
}
void GameSession::flush(TimePoint now) {
    expire_members(now);
    if (directory_) {
        listing_.occupied = uint8_t(host_->members().size());
        listing_.joinable = host_->phase() == RoomPhase::Waiting || (round() && options_.admission.late_join);
        if (host_->phase() == RoomPhase::Ended)
            directory_->withdraw();
        else
            directory_->publish(listing_);
        directory_->poll(now);
    }
    if (!round() || now < origin_)
        return;
    clock_.update(now);
    const auto count = (now - origin_) / options_.step;
    tick_ = static_cast<Tick>(count + 1);
    if (tick_ == sent_tick_)
        return;
    sent_tick_ = tick_;
    Outbound::Collect collect(link_);
    prediction_.flush();
    if (host_) {
        // A reliable lane already retries delivery. Keep a slower application retry for snapshots
        // that could not be queued locally, and continue offering every new revision immediately.
        constexpr auto retry_window = std::chrono::milliseconds(200);
        const auto retry_ticks = link_.reliable_ordered(Lane::Game)
            ? static_cast<Tick>((retry_window - std::chrono::milliseconds(1)) / options_.step + 1) : 0;
        for (const auto& [player, member] : host_->members()) {
            if (!member.peer)
                continue;
            auto codec = codecs_.find(member.peer);
            auto packets = state_.packets(member.peer, room(), round(), tick_, retry_ticks);
            if (codec != codecs_.end())
                for (auto& packet : packets)
                    packet = codec->second.encode(packet, room(), round());
            link_.send_messages(member.peer, Lane::Game, std::move(packets));
            ready(member.peer);
        }
    } else if (clock_.probe_due(now)) {
        auto out = game_packet(GameMessage::ClockProbe, room(), round());
        out.u32(clock_.sequence());
        link_.send(guest_->peer(), Lane::Game, out.bytes());
    }
    if (guest_ && options_.compact && !codec_enabled_ && (!codec_probe_ || tick_ - *codec_probe_ >= 30)) {
        auto hello = game_packet(GameMessage::CodecHello, room(), round());
        hello.u16(1);
        link_.send(guest_->peer(), Lane::Game, hello.bytes());
        codec_probe_ = tick_;
    }
    writers_.flush(tick_);
    world_.flush();
    rpc_.update(now);
    link_.flush(now);
}
void GameSession::pump(TimePoint now) {
    for (const auto& e : link_.poll(now))
        receive(e, now);
    flush(now);
}

}
