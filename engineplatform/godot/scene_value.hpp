#pragma once

#include <godot_cpp/variant/variant.hpp>

namespace netcode_godot {

godot::PackedByteArray encode_integer(int64_t value);
godot::Variant encode_collection(const godot::Variant& value, int capacity);
godot::Variant decode_collection(const godot::PackedByteArray& bytes);
godot::Variant type_collection(const godot::Variant& value, const godot::String& hint);

}
