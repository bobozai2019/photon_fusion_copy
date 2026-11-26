#include "state/value.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace netcode {
namespace {

bool utf8(const std::string& s)
{
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i++]);
        if (c < 0x80) continue;
        if (c < 0xc2 || c > 0xf4) return false;
        const unsigned count = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        if (count > s.size() - i) return false;
        const auto first = static_cast<unsigned char>(s[i]);
        if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
            (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90)) return false;
        for (unsigned j = 0; j < count; ++j)
            if ((static_cast<unsigned char>(s[i++]) & 0xc0) != 0x80) return false;
    }
    return true;
}

template<typename Input> bool integer(Input& in, std::int32_t& value)
{
    std::uint32_t word;
    if (!in.u32(word)) return false;
    std::memcpy(&value, &word, sizeof(value));
    return true;
}

template<typename Input, typename V> bool read_floats(Input& in, V& values)
{
    for (auto& v : values) if (!in.f32(v) || !std::isfinite(v)) return false;
    return true;
}

struct WordWriter {
    std::uint32_t* word;
    unsigned shift = 0;
    void u8(std::uint8_t value) {
        if (!shift) *word = 0;
        *word |= std::uint32_t(value) << shift;
        shift = (shift + 8) % 32;
        if (!shift) ++word;
    }
    void u16(std::uint16_t value) { u32(value); }
    void u32(std::uint32_t value) { *word++ = value; }
    void u64(std::uint64_t value) { u32(std::uint32_t(value)); u32(std::uint32_t(value >> 32)); }
    void f32(float value) { std::uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); u32(bits); }
    void append(const Bytes& bytes) {
        // Byte arrays start on a word boundary after the length word.
        std::size_t offset = 0;
        for (; bytes.size() - offset >= 4; offset += 4) {
            const auto* at = bytes.data() + offset;
            u32(std::uint32_t(at[0]) | (std::uint32_t(at[1]) << 8) |
                (std::uint32_t(at[2]) << 16) | (std::uint32_t(at[3]) << 24));
        }
        for (; offset < bytes.size(); ++offset) u8(bytes[offset]);
    }
};

struct WordReader {
    const std::uint32_t *word, *end;
    unsigned shift = 0;
    std::size_t remaining() const { return 4 * (end - word) - shift / 8; }
    bool u8(std::uint8_t& value) {
        if (word == end) return false;
        value = std::uint8_t(*word >> shift);
        shift = (shift + 8) % 32;
        if (!shift) ++word;
        return true;
    }
    bool u16(std::uint16_t& value) {
        std::uint32_t bits;
        if (!u32(bits) || bits > UINT16_MAX) return false;
        value = std::uint16_t(bits);
        return true;
    }
    bool u32(std::uint32_t& value) {
        if (word == end) return false;
        value = *word++;
        return true;
    }
    bool u64(std::uint64_t& value) {
        std::uint32_t low, high;
        if (!u32(low) || !u32(high)) return false;
        value = low | (std::uint64_t(high) << 32);
        return true;
    }
    bool f32(float& value) {
        std::uint32_t bits;
        if (!u32(bits)) return false;
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }
    bool read(std::size_t size, Bytes& bytes) {
        if (size > remaining()) return false;
        bytes.resize(size);
        for (auto& byte : bytes) u8(byte);
        return true;
    }
    bool padding_is_zero() {
        if (shift && (*word++ >> shift)) return false;
        return std::all_of(word, end, [](auto value) { return value == 0; });
    }
};

}

std::size_t field_capacity(Field f)
{
    std::size_t width = 0;
    switch (f.type) {
    case FieldType::String: case FieldType::ByteArray: width = 1; break;
    case FieldType::IntList: case FieldType::FloatList: case FieldType::LinkedInts: width = 4; break;
    case FieldType::Vec2List: case FieldType::IntDictionary: width = 8; break;
    case FieldType::Vec4: case FieldType::Color4: return f.limit ? 0 : 16;
    case FieldType::Reference: return f.limit ? 0 : 8;
    default: {
        constexpr std::size_t widths[]{4,1,4,8,12,16};
        auto i = static_cast<unsigned>(f.type);
        return !f.limit && i < std::size(widths) ? widths[i] : 0;
    }
    }
    return f.limit ? 2 + width * f.limit : 0;
}

bool valid_value(Field field, const Value& value)
{
    if (value.index() != static_cast<unsigned>(field.type) || !field_capacity(field)) return false;
    return std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, float>) return std::isfinite(v);
        else if constexpr (std::is_same_v<T, std::string>) return v.size() <= field.limit && utf8(v);
        else if constexpr (std::is_same_v<T, IntArray> || std::is_same_v<T, IntMap> || std::is_same_v<T, Bytes> || std::is_same_v<T, IntLinkedList>) return v.size() <= field.limit;
        else if constexpr (std::is_same_v<T, Vector2Array>) return v.size() <= field.limit &&
            std::all_of(v.begin(),v.end(),[](const Vector2& p){ return std::isfinite(p[0]) && std::isfinite(p[1]); });
        else if constexpr (std::is_same_v<T,ObjectReference> || std::is_arithmetic_v<T>) return true;
        else {
            if constexpr (std::is_same_v<T, FloatArray>) if (v.size() > field.limit) return false;
            if (!std::all_of(v.begin(), v.end(), [](float n) { return std::isfinite(n); })) return false;
            if constexpr (std::is_same_v<T, Rotation>) {
                float norm = 0;
                for (float n : v) norm += n * n;
                return std::abs(norm - 1.f) <= 0.002f;
            }
            return true;
        }
    }, value);
}

template<typename Output> void write_value_to(Output& out, const Value& value)
{
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, float>) out.f32(v);
        else if constexpr (std::is_same_v<T, bool>) out.u8(v);
        else if constexpr (std::is_same_v<T, std::int32_t>) out.u32(static_cast<std::uint32_t>(v));
        else if constexpr (std::is_same_v<T,ObjectReference>) out.u64(v.value);
        else if constexpr (std::is_same_v<T, Bytes>) {
            out.u16(static_cast<std::uint16_t>(v.size()));
            out.append(v);
        }
        else {
            if constexpr (std::is_same_v<T,Vector2Array> ||
                          std::is_same_v<T,IntMap> || std::is_same_v<T,IntLinkedList> || std::is_same_v<T, std::string> || std::is_same_v<T, IntArray> || std::is_same_v<T, FloatArray>)
                out.u16(static_cast<std::uint16_t>(v.size()));
            for (const auto n : v) {
                if constexpr (std::is_same_v<T, std::string>) out.u8(static_cast<std::uint8_t>(n));
                else if constexpr (std::is_same_v<T,Vector2Array>) { out.f32(n[0]); out.f32(n[1]); }
                else if constexpr (std::is_same_v<T,IntMap>) { out.u32(n.first); out.u32(n.second); }
                else if constexpr (std::is_same_v<T, IntArray> || std::is_same_v<T,IntLinkedList>) out.u32(static_cast<std::uint32_t>(n));
                else out.f32(n);
            }
        }
    }, value);
}

template<typename Input> bool read_value_from(Input& in, Field field, Value& value)
{
    switch (field.type) {
    case FieldType::Float: { float v; if (!in.f32(v) || !std::isfinite(v)) return false; value = v; break; }
    case FieldType::Flag: { std::uint8_t v; if (!in.u8(v) || v > 1) return false; value = bool(v); break; }
    case FieldType::Int: { std::int32_t v; if (!integer(in, v)) return false; value = v; break; }
    case FieldType::Vec2: { Vector2 v; if (!read_floats(in, v)) return false; value = v; break; }
    case FieldType::Vec3: { Vector3 v; if (!read_floats(in, v)) return false; value = v; break; }
    case FieldType::Quat: { Rotation v; if (!read_floats(in, v) || !valid_value(field, v)) return false; value = v; break; }
    case FieldType::Vec4: { Vector4 v; if (!read_floats(in,v)) return false; value=v; break; }
    case FieldType::Color4: { Color v; if (!read_floats(in,v)) return false; value=v; break; }
    case FieldType::Reference: { ObjectReference v; if (!in.u64(v.value)) return false; value=v; break; }
    default: {
        std::uint16_t count;
        if (!in.u16(count) || count > field.limit) return false;
        if (field.type == FieldType::String) {
            Bytes bytes;
            if (!in.read(count, bytes)) return false;
            std::string s(bytes.begin(), bytes.end());
            if (!utf8(s)) return false;
            value = std::move(s);
        } else if (field.type == FieldType::IntList) {
            if (count > in.remaining() / 4) return false;
            IntArray v(count);
            for (auto& n : v) if (!integer(in, n)) return false;
            value = std::move(v);
        } else if (field.type == FieldType::FloatList) {
            if (count > in.remaining() / 4) return false;
            FloatArray v(count);
            if (!read_floats(in, v)) return false;
            value = std::move(v);
        } else if (field.type == FieldType::ByteArray) {
            if (auto* bytes = std::get_if<Bytes>(&value)) return in.read(count, *bytes);
            Bytes bytes;
            if (!in.read(count, bytes)) return false;
            value = std::move(bytes);
        } else if (field.type == FieldType::Vec2List) {
            if (count > in.remaining()/8) return false;
            Vector2Array v(count);
            for (auto& p:v) if (!read_floats(in,p)) return false;
            value=std::move(v);
        } else if (field.type == FieldType::IntDictionary) {
            IntMap v;
            for (unsigned i=0;i<count;++i) {
                int32_t k,n;
                if (!integer(in,k) || !integer(in,n)) return false;
                if constexpr (std::is_same_v<Input, WordReader>)
                    if (!v.empty() && k <= v.rbegin()->first) return false;
                if (!v.emplace(k,n).second) return false;
            }
            value=std::move(v);
        } else if (field.type == FieldType::LinkedInts) {
            IntLinkedList v;
            for (unsigned i=0;i<count;++i) { int32_t n; if (!integer(in,n)) return false; v.push_back(n); }
            value=std::move(v);
        } else return false;
    }
    }
    return true;
}

void write_value(Writer& out, const Value& value) { write_value_to(out, value); }
bool read_value(Reader& in, Field field, Value& value) { return read_value_from(in, field, value); }

std::size_t word_capacity(Field field)
{
    const auto bytes = field_capacity(field);
    return field.limit && bytes ? 1 + (bytes - 2 + 3) / 4 : (bytes + 3) / 4;
}

void pack_words(Field field, const Value& value, std::uint32_t* words)
{
    WordWriter out{words};
    write_value_to(out, value);
    if (field.limit) std::fill(out.word + (out.shift != 0), words + word_capacity(field), 0);
}

bool unpack_words(Field field, const std::uint32_t* words, Value& value)
{
    WordReader in{words, words + word_capacity(field)};
    return read_value_from(in, field, value) && in.padding_is_zero();
}

std::size_t value_size(Field field, const Value& value)
{
    const auto bytes = field_capacity(field);
    if (!field.limit) return bytes;
    return std::visit([&](const auto& v) -> std::size_t {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, IntArray> ||
                      std::is_same_v<T, FloatArray> || std::is_same_v<T, Bytes> ||
                      std::is_same_v<T, Vector2Array> || std::is_same_v<T, IntMap> ||
                      std::is_same_v<T, IntLinkedList>) return 2 + v.size() * ((bytes - 2) / field.limit);
        return bytes;
    }, value);
}

void WordLayout::initialize(const std::vector<Field>& fields)
{
    offsets.assign(1, 0);
    offsets.reserve(fields.size() + 1);
    for (const auto field : fields) offsets.push_back(offsets.back() + word_capacity(field));
}

void WordLayout::pack(const std::vector<Field>& fields, const Values& values, Words& words) const
{
    words.resize(offsets.back());
    for (std::size_t i = 0; i < fields.size(); ++i)
        pack_words(fields[i], values[i], words.data() + offsets[i]);
}

void WordState::initialize(const WordLayout& layout, const std::vector<Field>& fields, const Values& values,
                           std::uint32_t revision)
{
    layout.pack(fields, values, words);
    shadow = words;
    revisions.assign(words.size(), revision);
    pending_scan = false;
}

bool WordState::stage(Words& sampled)
{
    if (sampled == words) return false;
    words.swap(sampled);
    pending_scan = true;
    return true;
}

void WordState::scan(std::uint32_t revision)
{
    if (!pending_scan) return;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (words[i] == shadow[i]) continue;
        shadow[i] = words[i];
        revisions[i] = revision;
    }
    pending_scan = false;
}

void write_word_varuint(Writer& out, std::uint32_t value)
{
    while (value >= 128) {
        out.u8(std::uint8_t(value) | 0x80);
        value >>= 7;
    }
    out.u8(std::uint8_t(value));
}

std::size_t word_varuint_size(std::uint32_t value)
{
    std::size_t size = 1;
    while (value >= 128) {
        value >>= 7;
        ++size;
    }
    return size;
}

void write_word_change(Writer& out, std::uint32_t skip, std::uint32_t value)
{
    write_word_varuint(out, skip);
    write_word_varuint(out, (value << 1) ^ (0u - (value >> 31)));
}

static bool read_word_varuint(Reader& in, std::uint32_t& value)
{
    value = 0;
    for (unsigned shift = 0; shift <= 28; shift += 7) {
        std::uint8_t byte;
        if (!in.u8(byte) || (shift == 28 && byte > 15)) return false;
        value |= std::uint32_t(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return !shift || byte != 0;
    }
    return false;
}

bool read_word_changes(Reader& in, const WordLayout& layout, Words* words, std::uint64_t& changed,
                       bool allow_empty)
{
    return read_word_changes_into(in, layout, words ? words->data() : nullptr, changed, allow_empty);
}

bool read_word_changes_into(Reader& in, const WordLayout& layout, std::uint32_t* words, std::uint64_t& changed,
                            bool allow_empty)
{
    const auto& offsets = layout.offsets;
    std::uint32_t count;
    if (!read_word_varuint(in, count) || (!count && !allow_empty) || count > offsets.back() ||
        count > in.remaining() / 2) return false;
    std::size_t position = 0, field = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t skip, encoded;
        if (!read_word_varuint(in, skip) || skip >= offsets.back() - position ||
            !read_word_varuint(in, encoded)) return false;
        position += skip;
        const auto value = (encoded >> 1) ^ (0u - (encoded & 1));
        if (words && words[position] != value) {
            words[position] = value;
            while (position >= offsets[field + 1]) ++field;
            changed |= std::uint64_t{1} << field;
        }
        ++position;
    }
    return true;
}

}
