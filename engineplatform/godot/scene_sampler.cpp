#include "scene_sampler.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {
bool append_leaf(PackedByteArray& bytes, const Variant& value, int32_t limit) {
    const auto data = UtilityFunctions::var_to_bytes(value);
    if (bytes.size() + 1 + data.size() > limit) return false;
    bytes.append(value.get_type());
    bytes.append_array(data);
    return true;
}

bool header(PackedByteArray& bytes, Variant::Type type, int64_t key, int64_t item,
            int64_t count, int32_t limit) {
    const bool dictionary = type == Variant::DICTIONARY;
    if (count > UINT16_MAX || count > limit || limit < (dictionary ? 5 : 4)) return false;
    bytes.append(type);
    bytes.append(key);
    if (dictionary) bytes.append(item);
    bytes.append(count & 255);
    bytes.append(count >> 8);
    return true;
}
}

void NetcodeSceneSampler::_bind_methods() {
    ClassDB::bind_method(D_METHOD("configure", "members", "kinds", "capacities"), &NetcodeSceneSampler::configure);
    ClassDB::bind_method(D_METHOD("sample", "root", "encode_collection"), &NetcodeSceneSampler::sample);
}

bool NetcodeSceneSampler::configure(const TypedArray<StringName>& members, const PackedByteArray& kinds,
                                    const PackedInt32Array& caps) {
    fields_.clear();
    if (members.size() != kinds.size() || members.size() != caps.size()) return false;
    fields_.reserve(members.size());
    for (int64_t i = 0; i < members.size(); ++i) {
        if (kinds[i] > Dictionary || caps[i] < 0 || caps[i] > UINT16_MAX) {
            fields_.clear();
            return false;
        }
        fields_.push_back({members[i], Kind(kinds[i]), caps[i], {}, {}, {}});
    }
    return true;
}

godot::Array NetcodeSceneSampler::sample(Object* root, const Callable& encode_collection) {
    godot::Array sample;
    if (!root) return sample;
    sample.resize(fields_.size());
    for (size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        const Variant value = root->get(field.name);
        switch (field.kind) {
        case Raw:
            sample[i] = value;
            break;
        case Integer:
            if (value.get_type() != Variant::INT) return {};
            if (field.previous.get_type() != Variant::INT || int64_t(field.previous) != int64_t(value)) {
                PackedByteArray bytes;
                bytes.resize(8);
                bytes.encode_s64(0, int64_t(value));
                field.previous = value;
                field.encoded = bytes;
            }
            sample[i] = field.encoded;
            break;
        case Array:
            sample[i] = sample_array(field, value, encode_collection);
            break;
        case Dictionary:
            sample[i] = sample_dictionary(field, value, encode_collection);
            break;
        }
    }
    return sample;
}

Variant NetcodeSceneSampler::sample_array(Field& field, const Variant& value, const Callable& fallback) {
    if (value.get_type() != Variant::ARRAY) return {};
    const godot::Array array = value;
    if (array.get_typed_builtin() != Variant::INT) return fallback.call(value, "array_value", field.capacity);
    if (field.previous.get_type() == Variant::ARRAY && array == godot::Array(field.previous)) return field.encoded;
    PackedByteArray bytes;
    if (!header(bytes, Variant::ARRAY, Variant::INT, Variant::NIL, array.size(), field.capacity)) return {};
    for (int64_t i = 0; i < array.size(); ++i)
        if (!append_leaf(bytes, array[i], field.capacity)) return {};
    // Store a detached copy so in-place writes can still be detected.
    field.previous = array.duplicate();
    field.encoded = bytes;
    return bytes;
}

Variant NetcodeSceneSampler::sample_dictionary(Field& field, const Variant& value, const Callable& fallback) {
    if (value.get_type() != Variant::DICTIONARY) return {};
    const godot::Dictionary dictionary = value;
    const auto keys = dictionary.keys();
    const auto key_type = dictionary.get_typed_key_builtin();
    const auto item_type = dictionary.get_typed_value_builtin();
    if ((key_type != Variant::NIL && key_type != Variant::STRING) ||
        (item_type != Variant::NIL && item_type != Variant::INT))
        return fallback.call(value, "dictionary_value", field.capacity);

    const bool cached = field.previous.get_type() == Variant::DICTIONARY;
    const godot::Dictionary previous = cached ? godot::Dictionary(field.previous) : godot::Dictionary();
    bool equal = cached && dictionary.is_same_typed(previous) && keys.size() == field.keys.size();
    for (int64_t i = 0; i < keys.size(); ++i) {
        const Variant key = keys[i];
        const Variant item = dictionary[key];
        if (key.get_type() != Variant::STRING || item.get_type() != Variant::INT)
            return fallback.call(value, "dictionary_value", field.capacity);
        // Godot dictionaries compare without insertion order. The wire keeps it.
        equal = equal && key == field.keys[i] && item == previous[key];
    }
    if (equal) return field.encoded;
    PackedByteArray bytes;
    if (!header(bytes, Variant::DICTIONARY, key_type, item_type, keys.size(), field.capacity)) return {};
    for (int64_t i = 0; i < keys.size(); ++i) {
        const Variant key = keys[i];
        if (!append_leaf(bytes, key, field.capacity) || !append_leaf(bytes, dictionary[key], field.capacity)) return {};
    }
    field.previous = dictionary.duplicate();
    field.keys = keys;
    field.encoded = bytes;
    return bytes;
}
