#include "session/room_properties.hpp"

#include "session/game_session.hpp"
#include "session/game_wire.hpp"
#include <algorithm>
#include <utility>

namespace netcode {
namespace {

constexpr size_t max_entries = 64;
constexpr size_t max_key = 128;
constexpr size_t max_value = 4096;
constexpr size_t max_encoded = 32768;
constexpr auto admission_timeout = std::chrono::seconds(8);

size_t encoded_size(const RoomProperties::Values& values, bool allow_remove = false)
{
    if (values.size() > max_entries) return 0;
    size_t bytes = 2;
    for (const auto& [key, value] : values) {
        if (key.empty() || key.size() > max_key || (!allow_remove && value.empty()) || value.size() > max_value) return 0;
        bytes += 4 + key.size() + value.size();
    }
    return bytes <= max_encoded ? bytes : 0;
}

void write_values(Writer& out, const RoomProperties::Values& values)
{
    out.u16(uint16_t(values.size()));
    for (const auto& [key, value] : values) {
        out.u16(uint16_t(key.size()));
        out.append(Bytes(key.begin(), key.end()));
        out.u16(uint16_t(value.size()));
        out.append(value);
    }
}

bool read_values(Reader& in, RoomProperties::Values& values, bool allow_remove = false)
{
    const auto start = in.remaining();
    uint16_t count;
    if (!in.u16(count) || count > max_entries) return false;
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t key_size, value_size;
        Bytes key, value;
        if (!in.u16(key_size) || !key_size || key_size > max_key || !in.read(key_size, key) ||
            !in.u16(value_size) || (!allow_remove && !value_size) || value_size > max_value || !in.read(value_size, value) ||
            !values.emplace(std::string(key.begin(), key.end()), std::move(value)).second) return false;
    }
    return start - in.remaining() <= max_encoded;
}

bool read_players(Reader& in, std::vector<PlayerId>& players)
{
    uint8_t count;
    if (!in.u8(count) || !count) return false;
    PlayerId previous = 0;
    for (uint16_t i = 0; i < count; ++i) {
        PlayerId player;
        if (!in.u32(player) || player <= previous) return false;
        players.push_back(player);
        previous = player;
    }
    return true;
}
}

void RoomProperties::reset(bool host)
{
    area_rates_supported_ = false;
    hello_sent_ = false;
    peers_.clear();
    pending_.clear();
    if (!host) values_.clear();
    revision_ = host && enabled_ ? 1 : 0;
    players_.clear();
    rosters_.clear();
}

void RoomProperties::apply_roster(std::vector<PlayerId> players)
{
    players_ = std::move(players);
    rosters_.push_back(players_);
}

std::vector<std::vector<PlayerId>> RoomProperties::take_rosters()
{
    return std::exchange(rosters_, {});
}

bool RoomProperties::valid_values(const Values& values) const
{
    for (const auto& [key, value] : values)
        if (validate_ && !validate_(key, value)) return false;
    return true;
}

void RoomProperties::write_checkpoint(Writer& out) const
{
    out.u32(revision_);
    if (!revision_) return;
    out.u64(catalog_);
    write_values(out, values_);
}

bool RoomProperties::read_checkpoint(Reader& in, uint32_t& revision, Values& values) const
{
    uint64_t catalog;
    return in.u32(revision) && (!revision || (enabled_ && in.u64(catalog) && catalog == catalog_ &&
           read_values(in, values) && valid_values(values)));
}

bool RoomProperties::set(GameSession& game, Transport& link, const Values& patch, const Values& expected)
{
    if (!enabled_ || !game.round() || !ready() || patch.empty()) return false;
    const auto patch_size = encoded_size(patch, true);
    const auto expected_size = encoded_size(expected);
    auto update = game_packet(GameMessage::RoomPropertiesUpdate, game.room(), game.round());
    if (!patch_size || !expected_size || update.bytes().size() + patch_size + expected_size > control_payload_limit) return false;
    if (game.host()) {
        if (!valid_values(patch) || !valid_values(expected)) return false;
        for (const auto& [key, value] : expected) {
            const auto it = values_.find(key);
            if (it == values_.end() || (it->second != value && (!equal_ || !equal_(it->second, value)))) return false;
        }
        auto next = values_;
        for (const auto& [key, value] : patch) {
            if (value.empty()) next.erase(key);
            else next.insert_or_assign(key, value);
        }
        if (!encoded_size(next)) return false;
        if (next != values_) {
            values_ = std::move(next);
            if (++revision_ == 0) ++revision_;
        }
        return true;
    }
    if (game.guest()->spectator()) return false;
    write_values(update, patch);
    write_values(update, expected);
    return link.send(game.guest()->peer(), Lane::Control, update.bytes());
}

bool RoomProperties::receive(GameSession& game, Transport& link, const Event& event)
{
    if (event.type != EventType::Message || event.lane != Lane::Control || event.payload.empty()) return false;
    const auto code = event.payload.front();
    if (code != uint8_t(GameMessage::RoomInterestKeys) && code != uint8_t(GameMessage::RoomAreaRatesSupported) &&
        code != uint8_t(GameMessage::RoomAreaRates) &&
        (code < uint8_t(GameMessage::RoomPropertiesHello) || code > uint8_t(GameMessage::RoomPropertiesSnapshot))) return false;
    Reader in(event.payload);
    GameMessage type;
    if (!enabled_ || !read_game_header(in, type, game.room(), game.round())) return true;
    if (auto* host = game.host()) {
        auto* member = host->member(event.peer);
        if (!member) return true;
        if (type == GameMessage::RoomPropertiesHello) {
            uint16_t requested;
            uint64_t catalog;
            if (!in.u16(requested) || !in.u64(catalog) || !in.done()) return true;
            if (requested != version || catalog != catalog_) {
                link.close(event.peer);
                return true;
            }
            pending_.erase(event.peer);
            peers_.insert_or_assign(event.peer, 0);
        } else if (type == GameMessage::RoomPropertiesUpdate && peers_.count(event.peer) && !member->spectator) {
            Values patch, expected;
            if (read_values(in, patch, true) && read_values(in, expected) && in.done()) set(game, link, patch, expected);
        } else if ((type == GameMessage::RoomInterestKeys || type == GameMessage::RoomAreaRates) &&
                   peers_.count(event.peer)) {
            const bool has_rates = type == GameMessage::RoomAreaRates;
            uint8_t kind = uint8_t(InterestKind::Area), count;
            if ((!has_rates && !in.u8(kind)) ||
                (kind != uint8_t(InterestKind::User) && kind != uint8_t(InterestKind::Area)) ||
                !in.u8(count) || count > max_interest_keys) return true;
            AreaRates keys;
            for (uint8_t i = 0; i < count; ++i) {
                uint64_t key;
                uint16_t divisor = 1;
                if (!in.u64(key) || (has_rates && (!in.u16(divisor) || !divisor)) ||
                    !keys.emplace(key, divisor).second) return true;
            }
            if (in.done()) game.authority().subscribe_rates(event.peer, InterestKind(kind), std::move(keys));
        }
    } else if (event.peer == game.guest()->peer() && type == GameMessage::RoomAreaRatesSupported) {
        if (in.done()) area_rates_supported_ = true;
    } else if (event.peer == game.guest()->peer() && type == GameMessage::RoomPropertiesSnapshot) {
        uint32_t revision;
        std::vector<PlayerId> players;
        Values values;
        if (in.u32(revision) && revision && read_players(in, players) && read_values(in, values) && in.done() &&
            valid_values(values)) {
            revision_ = revision;
            if (players_ != players) apply_roster(std::move(players));
            values_ = std::move(values);
        }
    }
    return true;
}

bool RoomProperties::subscribe(GameSession& game, Transport& link, uint8_t kind, const std::set<uint64_t>& keys)
{
    if (!enabled_ || !game.round() || !ready() || !game.guest() || keys.size() > max_interest_keys ||
        (kind != uint8_t(InterestKind::User) && kind != uint8_t(InterestKind::Area))) return false;
    auto out = game_packet(GameMessage::RoomInterestKeys, game.room(), game.round());
    out.u8(kind);
    out.u8(uint8_t(keys.size()));
    for (auto key : keys) out.u64(key);
    return link.send(game.guest()->peer(), Lane::Control, out.bytes());
}

bool RoomProperties::subscribe_area_rates(GameSession& game, Transport& link, const AreaRates& rates)
{
    if (!enabled_ || !game.round() || !ready() || !game.guest() || rates.size() > max_interest_keys) return false;
    const bool include_rates = area_rates_supported_ && std::any_of(rates.begin(), rates.end(),
        [](const auto& cell) { return cell.second != 1; });
    auto out = game_packet(include_rates ? GameMessage::RoomAreaRates : GameMessage::RoomInterestKeys,
                           game.room(), game.round());
    if (!include_rates) out.u8(uint8_t(InterestKind::Area));
    out.u8(uint8_t(rates.size()));
    for (const auto& [key, divisor] : rates) {
        if (!divisor) return false;
        out.u64(key);
        if (include_rates) out.u16(divisor);
    }
    return link.send(game.guest()->peer(), Lane::Control, out.bytes());
}

void RoomProperties::flush(GameSession& game, Transport& link)
{
    if (!enabled_ || !game.round()) return;
    if (game.host()) {
        const auto now = Clock::now();
        std::vector<PlayerId> players;
        for (const auto& [player, member] : game.host()->members()) {
            if (player == game.coordinator() || (member.peer && negotiated(member.peer))) {
                players.push_back(player);
                continue;
            }
            if (!member.peer) continue;
            const auto deadline = pending_.try_emplace(member.peer, now + admission_timeout).first;
            if (now >= deadline->second) link.close(member.peer);
        }
        if (players_ != players) {
            apply_roster(std::move(players));
            for (auto& [_, sent] : peers_) sent = 0;
        }
        if (peers_.empty()) return;
        std::optional<Writer> area_support;
        std::optional<Writer> snapshot;
        for (auto& [peer, sent] : peers_) {
            if (sent == revision_) continue;
            if (!sent) {
                if (!area_support) area_support = game_packet(GameMessage::RoomAreaRatesSupported, game.room(), game.round());
                if (!link.send(peer, Lane::Control, area_support->bytes())) continue;
            }
            if (!snapshot) {
                snapshot = game_packet(GameMessage::RoomPropertiesSnapshot, game.room(), game.round());
                snapshot->u32(revision_);
                snapshot->u8(uint8_t(players_.size()));
                for (const auto player : players_) snapshot->u32(player);
                write_values(*snapshot, values_);
            }
            if (link.send(peer, Lane::Control, snapshot->bytes())) sent = revision_;
        }
    } else if (!hello_sent_) {
        auto hello = game_packet(GameMessage::RoomPropertiesHello, game.room(), game.round());
        hello.u16(version);
        hello.u64(catalog_);
        hello_sent_ = link.send(game.guest()->peer(), Lane::Control, hello.bytes());
    }
}

} // namespace netcode
