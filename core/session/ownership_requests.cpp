#include "session/object_authority.hpp"
#include <algorithm>

namespace netcode {
namespace {
void encode(Writer& out, const Claim& c) {
    out.u32(c.object);
    out.u32(c.requester);
    out.u32(c.epoch);
    out.u32(c.sequence);
}

bool decode(Reader& in, Claim& c) {
    return in.u32(c.object) && c.object && in.u32(c.requester) && c.requester && in.u32(c.epoch) &&
           in.u32(c.sequence) && c.sequence;
}
}

bool ObjectAuthority::allow_requests(SchemaId schema, ClaimPolicy policy) {
    auto model = models_.find(schema);
    if (round_ || model == models_.end() || (model->second.max_recovery && !policy.recovery) ||
        (policy.mode == ClaimMode::PlayerAttached && !policy.destroy))
        return false;
    return policies_.emplace(schema, std::move(policy)).second;
}

Peer ObjectAuthority::peer_for(PlayerId player) const {
    for (const auto& [peer, member] : peers_)
        if (member.player == player)
            return peer;
    return 0;
}

bool ObjectAuthority::request(ObjectId object) { return send_request(object, false); }
bool ObjectAuthority::release(ObjectId object) { return send_request(object, true); }

bool ObjectAuthority::send_request(ObjectId id, bool relinquish) {
    const auto* object = lookup_(id);
    if (local_ == master_ || !round_ || !claim_supported_ || !object || !policies_.count(object->schema) ||
        outgoing_.count(id) || request_sequence_ == UINT32_MAX || (writer(id) == local_) != relinquish)
        return false;
    const auto* current = grant(id);
    const auto& policy = policies_.at(object->schema);
    const auto instance = *mode(id, object->schema);
    if (master_only(instance))
        return false;
    const bool optimistic = !relinquish && instance == ClaimMode::Dynamic;
    const auto last = last_transfer_.find(id);
    if (optimistic && last != last_transfer_.end() && local_step_ - last->second < policy.cooldown)
        return false;
    Claim c{id, local_, current ? current->epoch : 0, ++request_sequence_};
    auto out = game_packet(GameMessage::ClaimRequest, room_, round_);
    encode(out, c);
    out.u8(relinquish);
    if (!link_.send(coordinator_, Lane::Control, out.bytes()))
        return false;
    outgoing_.emplace(id, c);
    if (optimistic)
        predicted_[id] = {c.epoch, {}};
    return true;
}

bool ObjectAuthority::submit_predicted(ObjectId id, Tick step, Values values, Bytes recovery) {
    auto it = predicted_.find(id);
    const auto* object = lookup_(id);
    if (it == predicted_.end() || !object)
        return false;
    const auto& model = models_.at(object->schema);
    if (values.size() != model.fields.size() || recovery.size() > model.max_recovery)
        return false;
    for (std::size_t i = 0; i < values.size(); ++i)
        if (!valid_value(model.fields[i], values[i]))
            return false;
    it->second.pending = AuthoritySample{0, step, std::move(values), std::move(recovery)};
    return true;
}

void ObjectAuthority::resolve_prediction(ObjectId id, bool granted) {
    auto predicted = predicted_.extract(id);
    if (predicted.empty())
        return;
    auto& sample = predicted.mapped().pending;
    if (granted) {
        if (sample) {
            const auto& current = *grant(id);
            const Tick step =
                sample->source_step ? std::max(current.sample.source_step, sample->source_step) : 0;
            submit(id, current.epoch, step, std::move(sample->values), std::move(sample->recovery));
        }
    } else if (prediction_failed_)
        prediction_failed_(id);
}

void ObjectAuthority::finish_claim(const Claim& c, ClaimStatus status) {
    const auto peer = peer_for(c.requester);
    if (!peer || !claim_peers_.count(peer))
        return;
    auto out = game_packet(GameMessage::ClaimResult, room_, round_);
    encode(out, c);
    out.u8(static_cast<std::uint8_t>(status));
    if (!link_.send(peer, Lane::Control, out.bytes()))
        link_.close(peer);
}

bool ObjectAuthority::answer(const Claim& c, bool accept, Tick now) {
    if (writer(c.object) != local_ || !lookup_(c.object))
        return false;
    if (local_ != master_) {
        auto out = game_packet(GameMessage::ClaimAnswer, room_, round_);
        encode(out, c);
        out.u8(accept);
        return link_.send(coordinator_, Lane::Control, out.bytes());
    }
    const auto it = pending_.find(c.object);
    if (it == pending_.end() || it->second.claim.requester != c.requester ||
        it->second.claim.sequence != c.sequence || it->second.claim.epoch != c.epoch)
        return false;
    const auto request = it->second.claim;
    pending_.erase(it);
    const auto* g = grant(request.object);
    ClaimStatus status = ClaimStatus::Denied;
    if ((g ? g->epoch : 0) != request.epoch)
        status = ClaimStatus::Stale;
    else if (accept) {
        const auto& policy = policies_.at(lookup_(request.object)->schema);
        auto state = policy.recovery ? policy.recovery(request.object) : Bytes{};
        status = assign(request.object, request.requester, now, state, now) ? ClaimStatus::Granted
                                                                            : ClaimStatus::Unavailable;
    }
    finish_claim(request, status);
    return true;
}

void ObjectAuthority::receive_claim(Peer peer, GameMessage type, Reader& in, Tick now) {
    if (local_ != master_ && peer != coordinator_)
        return;
    if (local_ == master_ && !peers_.count(peer))
        return;
    if (type == GameMessage::ClaimHello) {
        if (local_ == master_ && !policies_.empty() && in.done()) {
            auto out = game_packet(GameMessage::ClaimSupported, room_, round_);
            if (link_.send(peer, Lane::Control, out.bytes()))
                claim_peers_.insert(peer);
        }
        return;
    }
    if (type == GameMessage::ClaimSupported) {
        if (local_ != master_ && in.done())
            claim_supported_ = true;
        return;
    }
    Claim c{};
    if (!decode(in, c))
        return;
    if (type == GameMessage::ClaimPrompt) {
        const auto* held = grant(c.object);
        if (local_ != master_ && in.done() && held && held->writer == local_ && held->epoch == c.epoch &&
            prompt_)
            prompt_(c);
        return;
    }
    std::uint8_t code;
    if (!in.u8(code) || !in.done())
        return;
    if (local_ != master_) {
        const auto found = outgoing_.find(c.object);
        if (type != GameMessage::ClaimResult || code > static_cast<unsigned>(ClaimStatus::Cooldown) ||
            found == outgoing_.end() || found->second.sequence != c.sequence || c.requester != local_)
            return;
        outgoing_.erase(found);
        if (code != static_cast<unsigned>(ClaimStatus::Granted))
            resolve_prediction(c.object, false);
        if (result_)
            result_(c, static_cast<ClaimStatus>(code));
        return;
    }
    if (!claim_peers_.count(peer) || code > 1)
        return;
    const auto* g = grant(c.object);
    const auto epoch = g ? g->epoch : 0;
    const auto owner = writer(c.object);
    auto& sender = peers_.at(peer);
    if (type == GameMessage::ClaimAnswer) {
        auto it = pending_.find(c.object);
        if (owner != sender.player || c.epoch != epoch || it == pending_.end() ||
            it->second.claim.requester != c.requester || it->second.claim.sequence != c.sequence ||
            it->second.claim.epoch != c.epoch)
            return;
        pending_.erase(it);
        auto status = !code                                ? ClaimStatus::Denied
                      : assign(c.object, c.requester, now) ? ClaimStatus::Granted
                                                           : ClaimStatus::Unavailable;
        finish_claim(c, status);
        return;
    }
    if (type != GameMessage::ClaimRequest || c.requester != sender.player ||
        c.sequence <= sender.last_request)
        return;
    sender.last_request = c.sequence;
    const auto* obj = lookup_(c.object);
    if (!obj || !policies_.count(obj->schema) || !supports(c.requester, obj->schema) ||
        (known_ && !known_(peer, c.object)))
        return finish_claim(c, ClaimStatus::Unavailable);
    const auto instance = *mode(c.object, obj->schema);
    if (master_only(instance))
        return finish_claim(c, ClaimStatus::Denied);
    if (epoch != c.epoch)
        return finish_claim(c, ClaimStatus::Stale);
    if (code) {
        const bool ok = owner == c.requester && assign(c.object, master_, now);
        return finish_claim(c, ok ? ClaimStatus::Granted : ClaimStatus::Denied);
    }
    if (owner == c.requester)
        return finish_claim(c, ClaimStatus::Granted);
    if (pending_.count(c.object))
        return finish_claim(c, ClaimStatus::Busy);
    const auto& policy = policies_.at(obj->schema);
    auto last = last_transfer_.find(c.object);
    if (last != last_transfer_.end() && now - last->second < policy.cooldown)
        return finish_claim(c, ClaimStatus::Cooldown);
    if (instance == ClaimMode::Dynamic) {
        auto state = owner == master_ && policy.recovery ? policy.recovery(c.object) : Bytes{};
        return finish_claim(c, assign(c.object, c.requester, now, state, now) ? ClaimStatus::Granted
                                                                              : ClaimStatus::Unavailable);
    }
    pending_[c.object] = {c, now};
    if (owner == master_ && prompt_) {
        prompt_(c);
        return;
    }
    auto target = peer_for(owner);
    if (target && claim_peers_.count(target)) {
        auto out = game_packet(GameMessage::ClaimPrompt, room_, round_);
        encode(out, c);
        if (link_.send(target, Lane::Control, out.bytes()))
            return;
    }
    pending_.erase(c.object);
    finish_claim(c, ClaimStatus::Unavailable);
}

void ObjectAuthority::maintain_claims(Tick now) {
    if (policies_.empty())
        return;
    if (local_ != master_) {
        if (!claim_supported_ && coordinator_ && (!last_claim_probe_ || now - *last_claim_probe_ >= 30)) {
            last_claim_probe_ = now;
            auto msg = game_packet(GameMessage::ClaimHello, room_, round_);
            link_.send(coordinator_, Lane::Game, msg.bytes());
        }
        return;
    }
    for (auto it = pending_.begin(); it != pending_.end();) {
        const auto c = it->second.claim;
        const auto* g = grant(c.object);
        ClaimStatus status;
        if (!peer_for(c.requester))
            status = ClaimStatus::Unavailable;
        else if ((g ? g->epoch : 0) != c.epoch)
            status = ClaimStatus::Stale;
        else if (now - it->second.since >= 150)
            status = ClaimStatus::Expired;
        else {
            ++it;
            continue;
        }
        it = pending_.erase(it);
        finish_claim(c, status);
    }
}
}
