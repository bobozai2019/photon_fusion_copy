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

bool integer(Reader& in, std::int32_t& value)
{
    std::uint32_t word;
    if (!in.u32(word)) return false;
    std::memcpy(&value, &word, sizeof(value));
    return true;
}

template<typename V> bool read_floats(Reader& in, V& values)
{
    for (auto& v : values) if (!in.f32(v) || !std::isfinite(v)) return false;
    return true;
}

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

void write_value(Writer& out, const Value& value)
{
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, float>) out.f32(v);
        else if constexpr (std::is_same_v<T, bool>) out.u8(v);
        else if constexpr (std::is_same_v<T, std::int32_t>) out.u32(static_cast<std::uint32_t>(v));
        else if constexpr (std::is_same_v<T,ObjectReference>) out.u64(v.value);
        else {
            if constexpr (std::is_same_v<T, Bytes> || std::is_same_v<T,Vector2Array> ||
                          std::is_same_v<T,IntMap> || std::is_same_v<T,IntLinkedList> || std::is_same_v<T, std::string> || std::is_same_v<T, IntArray> || std::is_same_v<T, FloatArray>)
                out.u16(static_cast<std::uint16_t>(v.size()));
            for (const auto n : v) {
                if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T,Bytes>) out.u8(static_cast<std::uint8_t>(n));
                else if constexpr (std::is_same_v<T,Vector2Array>) { out.f32(n[0]); out.f32(n[1]); }
                else if constexpr (std::is_same_v<T,IntMap>) { out.u32(n.first); out.u32(n.second); }
                else if constexpr (std::is_same_v<T, IntArray> || std::is_same_v<T,IntLinkedList>) out.u32(static_cast<std::uint32_t>(n));
                else out.f32(n);
            }
        }
    }, value);
}

bool read_value(Reader& in, Field field, Value& value)
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
            Bytes v; if (!in.read(count,v)) return false; value=std::move(v);
        } else if (field.type == FieldType::Vec2List) {
            if (count > in.remaining()/8) return false;
            Vector2Array v(count);
            for (auto& p:v) if (!read_floats(in,p)) return false;
            value=std::move(v);
        } else if (field.type == FieldType::IntDictionary) {
            IntMap v;
            for (unsigned i=0;i<count;++i) {
                int32_t k,n;
                if (!integer(in,k) || !integer(in,n) || !v.emplace(k,n).second) return false;
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

}
