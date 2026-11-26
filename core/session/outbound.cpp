#include "session/outbound.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace netcode {
namespace {
constexpr std::size_t game_budget = 256 * 1024;
constexpr std::size_t control_reserve = 128 * 1024;
constexpr std::size_t game_message_budget = 2048;
constexpr std::size_t control_message_reserve = 128;
constexpr std::size_t max_batch = 64;
constexpr auto close_grace = std::chrono::seconds(3);

Transport& require_link(const std::unique_ptr<Transport>& link) {
    if (!link) throw std::invalid_argument("Session requires a transport");
    return *link;
}
}

Outbound::Outbound(Transport& link) : link_(link), backpressure_(link.supports_backpressure()) {}
Outbound::Outbound(std::unique_ptr<Transport> link)
    : owned_(std::move(link)), link_(require_link(owned_)), backpressure_(link_.supports_backpressure()) {}

bool Outbound::can_queue(const Stream& stream, Lane lane, std::size_t size) const {
    const bool control = lane == Lane::Control;
    return stream.queued.size() < game_message_budget + (control ? control_message_reserve : 0) &&
           stream.bytes + size <= game_budget + (control ? control_reserve : 0);
}

void Outbound::queue(Stream& stream, Lane lane, Bytes payload) {
    const auto size = payload.size();
    stream.queued.push_back({lane, std::move(payload), now_});
    stream.bytes += size;
    bytes_ += size;
    peak_ = std::max(peak_, bytes_);
}

bool Outbound::send(Peer peer, Lane lane, const Bytes& payload) {
    if (!backpressure_) return link_.send(peer, lane, payload);
    const auto max = lane == Lane::Control ? control_payload_limit : game_payload_limit;
    if (payload.size() > max) return false;
    const auto found = peers_.find(peer);
    if (found == peers_.end() || found->second.close_started) return false;
    auto& stream = found->second;
    const bool immediate = stream.queued.empty() && !collecting_;
    if (immediate) {
        const Bytes* message = &payload;
        const auto result = link_.send_batch(peer, lane, &message, 1);
        if (result.status != SendStatus::Blocked) return result.accepted == 1;
        stream.blocked = true;
    }
    if (!can_queue(stream, lane, payload.size())) return false;
    queue(stream, lane, payload);
    if (!immediate && !collecting_) submit(peer, stream);
    return true;
}

void Outbound::send_messages(Peer peer, Lane lane, std::vector<Bytes> messages) {
    if (backpressure_) {
        const auto found = peers_.find(peer);
        if (found == peers_.end() || found->second.close_started) return;
        auto& stream = found->second;
        for (auto& message : messages) {
            const auto limit = lane == Lane::Control ? control_payload_limit : game_payload_limit;
            if (message.size() <= limit && can_queue(stream, lane, message.size()))
                queue(stream, lane, std::move(message));
        }
        if (!collecting_) submit(peer, stream);
        return;
    }
    // The old driver tries each datagram once even after a failed send.
    std::array<const Bytes*, max_batch> views;
    for (size_t first = 0; first < messages.size();) {
        const auto count = std::min(views.size(), messages.size() - first);
        for (size_t i = 0; i < count; ++i) views[i] = &messages[first + i];
        const auto result = link_.send_batch(peer, lane, views.data(), count);
        first += result.accepted < count ? result.accepted + 1 : count;
    }
}

bool Outbound::admit_snapshot(Peer peer) {
    if (!backpressure_ || !reliable_ordered(Lane::Game)) return true;
    const auto found = peers_.find(peer);
    if (found == peers_.end() || found->second.close_started) return false;
    if (!found->second.blocked) return true;
    ++snapshot_backpressure_skips_;
    return false;
}

void Outbound::discard(Stream& stream) {
    bytes_ -= stream.bytes;
    stream.bytes = 0;
    stream.queued.clear();
    stream.blocked = false;
}

void Outbound::submit(Peer peer, Stream& stream) {
    std::array<const Bytes*, max_batch> views;
    while (!stream.queued.empty()) {
        const auto lane = stream.queued.front().lane;
        size_t count = 0;
        for (const auto& message : stream.queued) {
            if (message.lane != lane || count == views.size()) break;
            views[count++] = &message.payload;
        }
        const auto result = link_.send_batch(peer, lane, views.data(), count);
        for (size_t i = 0; i < result.accepted; ++i) {
            const auto size = stream.queued.front().payload.size();
            bytes_ -= size;
            stream.bytes -= size;
            stream.queued.pop_front();
        }
        stream.blocked = result.status == SendStatus::Blocked && !stream.queued.empty();
        if (result.status == SendStatus::Blocked) return;
        if (result.status != SendStatus::Accepted) {
            discard(stream);
            stream.close_started = now_;
            link_.close(peer);
            return;
        }
    }
    if (stream.close_started) link_.close(peer);
}

void Outbound::close(Peer peer) {
    if (const auto found = peers_.find(peer); found != peers_.end()) {
        if (!found->second.close_started) {
            found->second.close_started = now_;
            submit(peer, found->second);
        }
    } else {
        link_.close(peer);
    }
}

std::vector<Event> Outbound::poll(TimePoint now) {
    flush(now);
    auto events = link_.poll(now);
    if (backpressure_) {
        for (const auto& event : events) {
            if (event.type == EventType::Open) peers_.try_emplace(event.peer);
            if (event.type == EventType::Closed) {
                if (const auto found = peers_.find(event.peer); found != peers_.end()) {
                    discard(found->second);
                    peers_.erase(found);
                }
            }
        }
        if (bytes_) flush(now);
    }
    return events;
}

void Outbound::flush(TimePoint now) {
    now_ = std::max(now_, now);
    for (auto& [peer, stream] : peers_) {
        if (stream.queued.empty()) continue;
        if (stream.close_started && now_ - *stream.close_started >= close_grace) {
            discard(stream);
            link_.close(peer);
        } else {
            submit(peer, stream);
        }
    }
}

Traffic Outbound::traffic() const {
    auto stats = link_.traffic();
    stats.queued_bytes += bytes_;
    stats.core_peak_queued_bytes = peak_;
    stats.snapshot_backpressure_skips = snapshot_backpressure_skips_;
    for (const auto& [peer, stream] : peers_) {
        if (stream.queued.empty()) continue;
        const auto age = std::chrono::duration_cast<std::chrono::microseconds>(now_ - stream.queued.front().queued_at);
        stats.oldest_queued_us = std::max(stats.oldest_queued_us, static_cast<uint64_t>(age.count()));
    }
    return stats;
}
}
