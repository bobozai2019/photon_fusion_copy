#pragma once

#include "transport/wire.hpp"

#include <array>
#include <map>
#include <list>
#include <string>
#include <variant>

namespace netcode {

using FieldId = std::uint16_t;
using Vector2 = std::array<float, 2>;
using Vector3 = std::array<float, 3>;
using Rotation = std::array<float, 4>; // Unit quaternion in x, y, z, w order.
using IntArray = std::vector<std::int32_t>;
using FloatArray = std::vector<float>;
struct Vector4 : std::array<float,4> {};
struct Color : std::array<float,4> {};
struct ObjectReference {
    std::uint64_t value = 0;
    bool operator==(ObjectReference rhs) const { return value == rhs.value; }
    bool operator!=(ObjectReference rhs) const { return value != rhs.value; }
};
using Vector2Array = std::vector<Vector2>;
using IntMap = std::map<std::int32_t,std::int32_t>;
using IntLinkedList = std::list<std::int32_t>;
enum class FieldType : std::uint8_t { Float, Flag, Int, Vec2, Vec3, Quat, String, IntList, FloatList,
    Vec4, Color4, Reference, ByteArray, Vec2List, IntDictionary, LinkedInts };
using Value = std::variant<float, bool, std::int32_t, Vector2, Vector3, Rotation, std::string, IntArray, FloatArray,
    Vector4, Color, ObjectReference, Bytes, Vector2Array, IntMap, IntLinkedList>;
using Values = std::vector<Value>;

struct Field {
    FieldId id;
    FieldType type;
    std::uint16_t limit = 0; // UTF-8 bytes or array elements, zero for fixed-width fields.
};

std::size_t field_capacity(Field field);
bool valid_value(Field field, const Value& value);
void write_value(Writer& out, const Value& value);
bool read_value(Reader& in, Field field, Value& value);
inline bool valid_value(FieldType type, const Value& value) { return valid_value(Field{0, type}, value); }
inline bool read_value(Reader& in, FieldType type, Value& value) { return read_value(in, Field{0, type}, value); }

}
