#include "game.hpp"

#include <godot_cpp/godot.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace {

void initialize(godot::ModuleInitializationLevel level)
{
    if (level == godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
        godot::ClassDB::register_class<NetcodeGame>();
    }
}

void uninitialize(godot::ModuleInitializationLevel) {}

}

extern "C" GDExtensionBool GDE_EXPORT netcode_init(
    GDExtensionInterfaceGetProcAddress functions, GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization)
{
    godot::GDExtensionBinding::InitObject binding(functions, library, initialization);
    binding.register_initializer(initialize);
    binding.register_terminator(uninitialize);
    binding.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);
    return binding.init();
}
