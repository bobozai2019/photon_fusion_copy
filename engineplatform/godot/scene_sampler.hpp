#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <vector>

class NetcodeSceneSampler final : public godot::RefCounted {
    GDCLASS(NetcodeSceneSampler, godot::RefCounted)

public:
    bool configure(const godot::TypedArray<godot::StringName>& members,
                   const godot::PackedByteArray& kinds, const godot::PackedInt32Array& caps);
    godot::Array sample(godot::Object* root, const godot::Callable& encode_collection);

protected:
    static void _bind_methods();

private:
    enum Kind { Raw, Integer, Array, Dictionary };
    struct Field {
        godot::StringName name;
        Kind kind;
        int32_t capacity;
        godot::Variant previous;
        godot::PackedByteArray encoded;
        godot::Array keys;
    };

    static godot::Variant sample_array(Field& field, const godot::Variant& value,
                                       const godot::Callable& encode_collection);
    static godot::Variant sample_dictionary(Field& field, const godot::Variant& value,
                                            const godot::Callable& encode_collection);
    std::vector<Field> fields_;
};
