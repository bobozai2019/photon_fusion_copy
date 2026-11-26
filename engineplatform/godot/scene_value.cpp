#include "scene_value.hpp"

#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>

namespace netcode_godot {
using namespace godot;
namespace {
constexpr int max_depth = 12;

bool leaf(int type)
{
    switch (type) {
    case Variant::NIL: case Variant::BOOL: case Variant::INT: case Variant::FLOAT: case Variant::STRING:
    case Variant::VECTOR2: case Variant::VECTOR3: case Variant::VECTOR4: case Variant::QUATERNION:
    case Variant::COLOR: case Variant::PACKED_BYTE_ARRAY: case Variant::PACKED_INT32_ARRAY:
    case Variant::PACKED_FLOAT32_ARRAY: case Variant::PACKED_VECTOR2_ARRAY:
        return true;
    default:
        return false;
    }
}

bool plain(int type)
{
    return leaf(type) || type == Variant::ARRAY || type == Variant::DICTIONARY;
}

bool put(PackedByteArray& bytes, const Variant& value, int limit, int depth)
{
    const int type = value.get_type();
    if (depth > max_depth || bytes.size() >= limit || !plain(type)) return false;
    bytes.append(type);
    if (leaf(type)) {
        const auto data = UtilityFunctions::var_to_bytes(value);
        if (data.size() > limit - bytes.size()) return false;
        bytes.append_array(data);
        return true;
    }
    const bool dictionary = type == Variant::DICTIONARY;
    const auto map = dictionary ? Dictionary(value) : Dictionary();
    const auto items = dictionary ? map.keys() : Array(value);
    const auto first = dictionary ? map.get_typed_key_builtin() : items.get_typed_builtin();
    const auto second = dictionary ? map.get_typed_value_builtin() : Variant::NIL;
    if (items.size() > std::min(65535, limit) || !plain(first) || !plain(second) ||
        bytes.size() + (dictionary ? 4 : 3) > limit) return false;
    bytes.append(first);
    if (dictionary) bytes.append(second);
    bytes.append(items.size() & 255);
    bytes.append(items.size() >> 8);
    for (int64_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        if (!put(bytes, item, limit, depth + 1) ||
            (dictionary && !put(bytes, map[item], limit, depth + 1))) return false;
    }
    return true;
}

bool take(const PackedByteArray& bytes, int64_t& cursor, Variant& value, int depth)
{
    if (cursor >= bytes.size() || depth > max_depth) return false;
    const int type = bytes[cursor++];
    if (leaf(type)) {
        if (bytes.size() - cursor < 4 || bytes.decode_u16(cursor) != type) return false;
        const auto length = bytes.decode_var_size(cursor, false);
        if (length < 4 || length > bytes.size() - cursor) return false;
        value = bytes.decode_var(cursor, false);
        cursor += length;
        return true;
    }
    const bool dictionary = type == Variant::DICTIONARY;
    if ((!dictionary && type != Variant::ARRAY) || bytes.size() - cursor < (dictionary ? 4 : 3)) return false;
    const int first = bytes[cursor++];
    const int second = dictionary ? bytes[cursor++] : Variant::NIL;
    if (!plain(first) || !plain(second)) return false;
    const auto count = bytes.decode_u16(cursor);
    cursor += 2;
    if (count > bytes.size() - cursor) return false;
    Array array;
    Dictionary map;
    if (dictionary) map.set_typed(first, {}, {}, second, {}, {});
    else array.set_typed(first, {}, {});
    for (unsigned i = 0; i < count; ++i) {
        Variant key;
        if (!take(bytes, cursor, key, depth + 1) || (first && key.get_type() != first)) return false;
        if (dictionary) {
            Variant item;
            if (!take(bytes, cursor, item, depth + 1) || (second && item.get_type() != second) ||
                map.has(key)) return false;
            map[key] = item;
        } else {
            array.push_back(key);
        }
    }
    value = dictionary ? Variant(map) : Variant(array);
    return true;
}

int builtin(const String& hint)
{
    if (hint.is_empty() || hint == "Variant") return Variant::NIL;
    const auto prefix = hint.get_slice(":", 0);
    if (prefix.is_valid_int()) return prefix.to_int();
    for (int type = 0; type < Variant::VARIANT_MAX; ++type) {
        if (UtilityFunctions::type_string(type) == hint) return type;
    }
    return -1;
}
}

PackedByteArray encode_integer(int64_t value)
{
    PackedByteArray bytes;
    bytes.resize(8);
    bytes.encode_s64(0, value);
    return bytes;
}

Variant encode_collection(const Variant& value, int capacity)
{
    PackedByteArray bytes;
    return put(bytes, value, capacity, 0) ? Variant(bytes) : Variant();
}

Variant decode_collection(const PackedByteArray& bytes)
{
    Variant value;
    int64_t cursor = 0;
    return take(bytes, cursor, value, 0) && cursor == bytes.size() ? value : Variant();
}

Variant type_collection(const Variant& value, const String& hint)
{
    if (hint.is_empty()) return value;
    const auto types = hint.split(";", true);
    const auto first = builtin(types[0]);
    if (first < 0 || first >= Variant::VARIANT_MAX || first == Variant::OBJECT) return {};
    if (value.get_type() == Variant::ARRAY && types.size() == 1) {
        const Array array = value;
        if (array.get_typed_builtin() == first) return value;
        for (int64_t i = 0; first && i < array.size(); ++i) {
            if (array[i].get_type() != first) return {};
        }
        return Array(array, first, {}, {});
    }
    if (value.get_type() != Variant::DICTIONARY || types.size() != 2) return {};
    const auto second = builtin(types[1]);
    if (second < 0 || second >= Variant::VARIANT_MAX || second == Variant::OBJECT) return {};
    const Dictionary map = value;
    if (map.get_typed_key_builtin() == first && map.get_typed_value_builtin() == second) return value;
    const auto keys = map.keys();
    Dictionary typed;
    typed.set_typed(first, {}, {}, second, {}, {});
    for (int64_t i = 0; i < keys.size(); ++i) {
        const auto& key = keys[i];
        const auto& item = map[key];
        if ((first && key.get_type() != first) || (second && item.get_type() != second)) return {};
        typed[key] = item;
    }
    return typed;
}
}
