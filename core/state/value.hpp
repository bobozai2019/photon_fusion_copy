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
using Words = std::vector<std::uint32_t>;

struct Field {
    FieldId id;
    FieldType type;
    std::uint16_t limit = 0; // UTF-8 bytes or array elements, zero for fixed-width fields.
};

struct WordLayout {
    std::vector<std::size_t> offsets;
    void initialize(const std::vector<Field>& fields);
    void pack(const std::vector<Field>& fields, const Values& values, Words& words) const;
};

struct WordState {
    Words words, shadow;
    std::vector<std::uint32_t> revisions;
    bool pending_scan = false;

    void initialize(const WordLayout& layout, const std::vector<Field>& fields, const Values& values,
                    std::uint32_t revision);
    bool stage(Words& sampled);
    void scan(std::uint32_t revision);
};

std::size_t field_capacity(Field field);
bool valid_value(Field field, const Value& value);
void write_value(Writer& out, const Value& value);
bool read_value(Reader& in, Field field, Value& value);
std::size_t word_capacity(Field field);
void pack_words(Field field, const Value& value, std::uint32_t* words);
bool unpack_words(Field field, const std::uint32_t* words, Value& value);
std::size_t value_size(Field field, const Value& value);
void write_word_varuint(Writer& out, std::uint32_t value);
std::size_t word_varuint_size(std::uint32_t value);
void write_word_change(Writer& out, std::uint32_t skip, std::uint32_t value);
bool read_word_changes(Reader& in, const WordLayout& layout, Words* words, std::uint64_t& changed,
                       bool allow_empty = false);
inline bool valid_value(FieldType type, const Value& value) { return valid_value(Field{0, type}, value); }
inline bool read_value(Reader& in, FieldType type, Value& value) { return read_value(in, Field{0, type}, value); }

}
