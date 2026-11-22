#include "state/snapshot_codec.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>

namespace netcode {
namespace {
// Tags 1..127 are literal byte counts. Other tags carry a compact integer,
// a reference into the acknowledged packet, or a reference to earlier literals.
constexpr uint8_t integer_tag = 128, prior_tag = 129, self_tag = 130, zeros_tag = 131;
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
using Index = std::unordered_map<uint32_t, std::vector<Source>>;
void index_at(Index& table, const Bytes& bytes, size_t i, uint8_t tag) {
    if (i + 4 > bytes.size())
        return;
    auto& slots = table[word(bytes, i)];
    if (slots.size() == 8)
        slots.erase(slots.begin());
    slots.push_back({tag, uint16_t(i)});
}
Bytes compress(const Bytes& src, const Bytes& base) {
    Index index;
    for (size_t i = 0; i + 4 <= base.size(); ++i)
        index_at(index, base, i, prior_tag);
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
            auto candidates = index.find(v);
            if (candidates != index.end())
                for (const auto& s : candidates->second) {
                    const auto& from = s.tag == prior_tag ? base : src;
                    const auto end = s.tag == prior_tag ? from.size() : at;
                    size_t n = 0;
                    while (s.position + n < end && at + n < src.size() && src[at + n] == from[s.position + n])
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
            index_at(index, src, i, self_tag);
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
    auto packed = compress(payload, anchor_);
    auto out = game_packet(GameMessage::CompactSnapshot, room, round);
    varint(out, next_);
    varint(out, anchor_id_);
    varint(out, uint32_t(payload.size()));
    out.append(packed);
    original_bytes += legacy.size();
    if (out.bytes().size() >= legacy.size()) {
        encoded_bytes += legacy.size();
        return legacy;
    }
    remember(next_++, std::move(payload));
    encoded_bytes += out.bytes().size();
    return out.take();
}
void SnapshotCodec::remember(uint32_t seq, Bytes payload) {
    if (!seq)
        return;
    history_[seq] = std::move(payload);
    while (history_.size() > 16)
        history_.erase(history_.begin());
}
void SnapshotCodec::acknowledge(uint32_t seq) {
    auto it = history_.find(seq);
    if (it != history_.end() && seq > anchor_id_) {
        anchor_id_ = seq;
        anchor_ = it->second;
    }
}
size_t SnapshotCodec::retained_payload_bytes() const {
    size_t total = anchor_.capacity();
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
    uint32_t seq, anchor, size;
    missing = 0;
    if (!varint(r, seq) || !seq || !varint(r, anchor) || anchor >= seq || !varint(r, size) || !size ||
        size > game_payload_limit - 17)
        return false;
    auto prior = history_.find(anchor);
    if (anchor && prior == history_.end()) {
        missing = anchor;
        return false;
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
                const auto& src = tag == prior_tag ? prior->second : decoded;
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
    result = std::move(decoded);
    return true;
}
}
