#pragma once
#include "state/value.hpp"
#include <godot_cpp/variant/variant.hpp>
#include <type_traits>
#include <godot_cpp/variant/typed_array.hpp>

namespace netcode_godot {
using namespace godot;
inline Variant field_value(const netcode::Value& value)
{
    return std::visit([](const auto& v) -> Variant {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, netcode::Vector2>) return Vector2(v[0], v[1]);
        else if constexpr (std::is_same_v<T, netcode::Vector3>) return Vector3(v[0], v[1], v[2]);
        else if constexpr (std::is_same_v<T, netcode::Rotation>) return Quaternion(v[0], v[1], v[2], v[3]);
        else if constexpr (std::is_same_v<T, std::string>) return String::utf8(v.data(), v.size());
        else if constexpr (std::is_same_v<T, netcode::IntArray>) {
            PackedInt32Array a; for (auto n : v) a.push_back(n); return a;
        } else if constexpr (std::is_same_v<T, netcode::FloatArray>) {
            PackedFloat32Array a; for (auto n : v) a.push_back(n); return a;
        } else if constexpr (std::is_same_v<T,netcode::Vector4>) return Vector4(v[0],v[1],v[2],v[3]);
        else if constexpr (std::is_same_v<T,netcode::Color>) return Color(v[0],v[1],v[2],v[3]);
        else if constexpr (std::is_same_v<T,netcode::ObjectReference>) return int64_t(v.value);
        else if constexpr (std::is_same_v<T,netcode::Bytes>) {
            PackedByteArray a; for (auto n:v) a.push_back(n); return a;
        } else if constexpr (std::is_same_v<T,netcode::Vector2Array>) {
            PackedVector2Array a; for (auto p:v) a.push_back(Vector2(p[0],p[1])); return a;
        } else if constexpr (std::is_same_v<T,netcode::IntMap>) {
            Dictionary a; a.set_typed(Variant::INT,StringName(),Variant(),Variant::INT,StringName(),Variant()); for (auto [k,n]:v) a[k]=n; return a;
        } else if constexpr (std::is_same_v<T,netcode::IntLinkedList>) {
            godot::TypedArray<int64_t> a; for (auto n:v) a.push_back(n); return a;
        } else return v;
    }, value);
}
}
