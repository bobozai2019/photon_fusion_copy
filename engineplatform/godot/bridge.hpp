#pragma once

#include "runner.hpp"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

class NetcodeYard final : public godot::RefCounted {
    GDCLASS(NetcodeYard, godot::RefCounted)

public:
    godot::String configure(const godot::PackedStringArray& arguments, const godot::String& config);
    godot::Dictionary settings() const { return settings_; }
    godot::Dictionary bindings() const { return bindings_; }
    godot::Dictionary advance();
    void drive(double throttle, double steer);
    bool act(const godot::String& action);

protected:
    static void _bind_methods();

private:
    std::unique_ptr<yard::Runner> runner_;
    godot::Dictionary settings_, bindings_;
};
