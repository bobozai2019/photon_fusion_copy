#include "state/snapshot_codec.hpp"
#include <algorithm>
#include <array>

namespace netcode {
namespace {
// Tags 1..127 are literal byte counts. Other tags carry a compact integer,
// a reference into the acknowledged packet, or a reference to earlier literals.
constexpr uint8_t integer_tag = 128, prior_tag = 129, self_tag = 130, zeros_tag = 131;
constexpr size_t max_payload = game_payload_limit - 17;
void varint(Writer& w, uint32_t n) {
    while (n >= 128) {
        w.u8(uint8_t(n) | 128);
        n >>= 7;
    }
    w.u8(uint8_t(n));
}
bool varint(Reader& r, uint32_t& n) {
    n = 0;
    for (unsigned shift = 0; shift <= 28; shift += 7) {
        uint8_t b;
        if (!r.u8(b) || (shift == 28 && b > 15))
            return false;
        n |= uint32_t(b & 127) << shift;
        if (!(b & 128))
            return !shift || b != 0;
    }
    return false;
}
uint32_t word(const Bytes& data, size_t pos) {
    return uint32_t(data[pos]) | uint32_t(data[pos + 1]) << 8 | uint32_t(data[pos + 2]) << 16 |
           uint32_t(data[pos + 3]) << 24;
}
struct Source {
    uint8_t tag;
    uint16_t position;
};
struct Matches {
    uint32_t key;
    std::array<Source, 8> sources{};
    size_t first = 0, size = 0;

    const Source& operator[](size_t i) const { return sources[(first + i) % sources.size()]; }
    void append(Source source) {
        sources[(first + size) % sources.size()] = source;
        if (size < sources.size()) ++size;
        else first = (first + 1) % sources.size();
    }
};

// A packet and its anchor contribute at most one window per byte each.
// The flat index retains the eight newest positions in their original search order.
class MatchIndex {
    static constexpr size_t index_bits = 12;
    static constexpr size_t table_size = size_t{1} << index_bits;
    static_assert(2 * max_payload < UINT16_MAX && 2 * max_payload < table_size);
    std::array<uint16_t, table_size> slots_{};
    std::vector<Matches> matches_;

    uint16_t& slot(uint32_t key) {
        const uint32_t hash = (key ^ (key >> 16)) * 0x9e3779b1u;
        size_t i = hash >> (32 - index_bits);
        while (slots_[i] && matches_[slots_[i] - 1].key != key)
            i = (i + 1) & (table_size - 1);
        return slots_[i];
    }

public:
    explicit MatchIndex(size_t input_bytes) { matches_.reserve(input_bytes); }

    void add_anchor(const Bytes& base) {
        for (size_t i = 0; i + 4 <= base.size(); ++i) add(base, i, prior_tag);
    }

    void add(const Bytes& bytes, size_t i, uint8_t tag) {
        if (i + 4 > bytes.size()) return;
        const auto key = word(bytes, i);
        auto& id = slot(key);
        if (!id) {
            matches_.push_back({key});
            id = uint16_t(matches_.size());
        }
        matches_[id - 1].append({tag, uint16_t(i)});
    }

    const Matches* find(uint32_t key) {
        const auto id = slot(key);
        return id ? &matches_[id - 1] : nullptr;
    }
};

Bytes compress(const Bytes& src, const Bytes& base, MatchIndex& index) {
    Writer out;
    Bytes literal;
    auto flush = [&] {
        if (!literal.empty()) {
            out.u8(uint8_t(literal.size()));
            out.append(literal);
            literal.clear();
        }
    };
    for (size_t at = 0; at < src.size();) {
        size_t best = 0, pos = 0;
        uint8_t tag = 0;
        uint32_t small = 0;
        while (at + best < src.size() && !src[at + best])
            ++best;
        if (best >= 3)
            tag = zeros_tag;
        else
            best = 0;
        if (at + 4 <= src.size()) {
            auto v = word(src, at);
            auto zig = (v << 1) ^ (0u - (v >> 31));
            if (best < 4 && zig < 128) {
                best = 4;
                tag = integer_tag;
                small = zig;
            }
            if (const auto* candidates = index.find(v))
                for (size_t i = 0; i < candidates->size; ++i) {
                    const auto& s = (*candidates)[i];
                    const auto& from = s.tag == prior_tag ? base : src;
                    const auto end = s.tag == prior_tag ? from.size() : at;
                    const auto limit = std::min(end - s.position, src.size() - at);
                    if (limit < 5 || limit <= best || (best >= 4 && src[at + best] != from[s.position + best]))
                        continue;
                    // The four-byte index key already matches.
                    size_t n = 4;
                    while (n < limit && src[at + n] == from[s.position + n])
                        ++n;
                    if (n >= 5 && n > best) {
                        best = n;
                        pos = s.position;
                        tag = s.tag;
                    }
                }
        }
        if (!tag) {
            literal.push_back(src[at]);
            if (literal.size() == 127)
                flush();
            best = 1;
        } else {
            flush();
            out.u8(tag);
            if (tag == integer_tag)
                varint(out, small);
            else {
                if (tag != zeros_tag)
                    varint(out, uint32_t(pos));
                varint(out, uint32_t(best));
            }
        }
        for (auto i = at; i < at + best; ++i)
            index.add(src, i, self_tag);
        at += best;
    }
    flush();
    return out.take();
}
}
Bytes SnapshotCodec::encode(const Bytes& legacy, RoomId room, RoundId round) {
    if (legacy.size() <= 17 || legacy.size() > game_payload_limit || !next_)
        return legacy;
    Bytes payload(legacy.begin() + 17, legacy.end());
    MatchIndex index(payload.size() + anchor_.size());
    index.add_anchor(anchor_);
    const auto packed = compress(payload, anchor_, index);
    auto encoded = wrap(std::move(payload), packed, room, round);
    if (!encoded.empty()) return encoded;
    return legacy;
}

void SnapshotCodec::encode_batch(std::vector<Bytes>& packets, RoomId room, RoundId round) {
    if (packets.empty() || !next_) return;
    if (packets.size() == 1) {
        packets.front() = encode(packets.front(), room, round);
        return;
    }
    MatchIndex anchor_index(anchor_.size());
    anchor_index.add_anchor(anchor_);
    MatchIndex working(2 * max_payload);
    for (auto& packet : packets) {
        if (!next_) break;
        if (packet.size() <= 17 || packet.size() > game_payload_limit) continue;
        // Each packet starts with the same acknowledged dictionary. Its own
        // match positions must stay local to that packet.
        working = anchor_index;
        Bytes payload(packet.begin() + 17, packet.end());
        const auto packed = compress(payload, anchor_, working);
        auto encoded = wrap(std::move(payload), packed, room, round);
        if (!encoded.empty()) packet = std::move(encoded);
    }
}

Bytes SnapshotCodec::wrap(Bytes payload, const Bytes& packed, RoomId room, RoundId round) {
    const auto original_size = payload.size() + 17;
    auto out = game_packet(GameMessage::CompactSnapshot, room, round);
    varint(out, next_);
    varint(out, anchor_id_);
    varint(out, uint32_t(payload.size()));
    out.append(packed);
    original_bytes += original_size;
    if (out.bytes().size() >= original_size) {
        encoded_bytes += original_size;
        return {};
    }
    remember(next_++, std::move(payload));
    encoded_bytes += out.bytes().size();
    return out.take();
}
void SnapshotCodec::remember(uint32_t seq, Bytes payload) {
    if (!seq || payload.size() > max_payload)
        return;
    history_[seq] = std::move(payload);
    while (history_.size() > 16)
        history_.erase(history_.begin());
}

void SnapshotCodec::remember_received(uint32_t seq, uint32_t anchor, Bytes payload) {
    if (!seq || anchor >= seq || payload.size() > max_payload) return;
    if (seq > received_seq_) {
        // A burst can use the same dictionary after the recent packet cache
        // rolls over. Keep the dictionary of the newest accepted packet.
        if (!anchor) {
            received_anchor_.clear();
            received_anchor_id_ = 0;
        } else if (anchor != received_anchor_id_) {
            const auto it = history_.find(anchor);
            if (it != history_.end()) {
                received_anchor_ = it->second;
                received_anchor_id_ = anchor;
            }
        }
        received_seq_ = seq;
    }
    remember(seq, std::move(payload));
}
void SnapshotCodec::acknowledge(uint32_t seq) {
    auto it = history_.find(seq);
    if (it != history_.end() && seq > anchor_id_) {
        anchor_id_ = seq;
        anchor_ = it->second;
    }
}
size_t SnapshotCodec::retained_payload_bytes() const {
    size_t total = anchor_.capacity() + received_anchor_.capacity();
    for (const auto& [seq, bytes] : history_) total += bytes.capacity();
    return total;
}
void SnapshotCodec::discard(uint32_t anchor) {
    if (anchor_id_ == anchor) {
        anchor_id_ = 0;
        anchor_.clear();
    }
}
bool SnapshotCodec::decode(Reader& r, Bytes& result, uint32_t& sequence, uint32_t& missing) const {
    uint32_t anchor;
    return decode_with_anchor(r, result, sequence, missing, anchor);
}

bool SnapshotCodec::decode_with_anchor(Reader& r, Bytes& result, uint32_t& sequence, uint32_t& missing,
                                       uint32_t& used_anchor) const {
    uint32_t seq, anchor, size;
    missing = 0;
    if (!varint(r, seq) || !seq || !varint(r, anchor) || anchor >= seq || !varint(r, size) || !size ||
        size > max_payload)
        return false;
    const Bytes* prior = nullptr;
    if (anchor) {
        if (anchor == received_anchor_id_) prior = &received_anchor_;
        else if (const auto it = history_.find(anchor); it != history_.end()) prior = &it->second;
        else {
            missing = anchor;
            return false;
        }
    }
    Bytes decoded;
    decoded.reserve(size);
    while (!r.done() && decoded.size() < size) {
        uint8_t tag;
        r.u8(tag);
        if (tag == 0)
            return false;
        if (tag < 128) {
            Bytes chunk;
            if (tag > size - decoded.size() || !r.read(tag, chunk))
                return false;
            decoded.insert(decoded.end(), chunk.begin(), chunk.end());
        } else if (tag == integer_tag) {
            uint32_t z;
            if (!varint(r, z) || z >= 128 || size - decoded.size() < 4)
                return false;
            uint32_t word = (z >> 1) ^ (0u - (z & 1));
            for (int i = 0; i < 4; ++i)
                decoded.push_back(uint8_t(word >> (i * 8)));
        } else {
            uint32_t pos = 0, n;
            if (tag > zeros_tag || (tag != zeros_tag && !varint(r, pos)) || !varint(r, n) || !n ||
                n > size - decoded.size())
                return false;
            if (tag == zeros_tag)
                decoded.insert(decoded.end(), n, 0);
            else {
                if (tag == prior_tag && !anchor)
                    return false;
                const auto& src = tag == prior_tag ? *prior : decoded;
                if (pos > src.size() || n > src.size() - pos)
                    return false;
                // Reserve above keeps self references valid across push_back.
                for (uint32_t i = 0; i < n; ++i)
                    decoded.push_back(src[pos + i]);
            }
        }
    }
    if (!r.done() || decoded.size() != size)
        return false;
    sequence = seq;
    used_anchor = anchor;
    result = std::move(decoded);
    return true;
}
}
