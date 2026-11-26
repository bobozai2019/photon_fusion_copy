#include "game.hpp"
#include "forecast_body.hpp"
#include "native_nodes.hpp"
#include "native_session.hpp"
#include "scene_sampler.hpp"
#include "scene_editor.hpp"
#include "scene_tools.hpp"

#include <godot_cpp/classes/editor_plugin_registration.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace {

void initialize(godot::ModuleInitializationLevel level)
{
    if (level == godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
        godot::ClassDB::register_class<NetcodeGame>();
        godot::ClassDB::register_abstract_class<NetcodeForecastBody>();
        godot::ClassDB::register_class<NetcodeForecastBody2D>();
        godot::ClassDB::register_class<NetcodeForecastBody3D>();
        godot::ClassDB::register_class<NetcodePhysicsIsland3D>();
        godot::ClassDB::register_class<NetcodeLayoutExporter>();
        godot::ClassDB::register_class<NetcodeSceneSampler>();
        godot::ClassDB::register_class<NetcodeReplicationConfig>();
        godot::ClassDB::register_abstract_class<NetcodeReplicator>();
        godot::ClassDB::register_class<NetcodeSharedReplicator>();
        godot::ClassDB::register_class<NetcodeServerReplicator>();
        godot::ClassDB::register_class<NetcodeInterestArea>();
        godot::ClassDB::register_class<NetcodeSpawner>();
        godot::ClassDB::register_class<NetcodeRoomOptions>();
        godot::ClassDB::register_class<NetcodePlayer>();
        godot::ClassDB::register_class<NetcodeRoomView>();
        godot::ClassDB::register_class<NetcodeRpcResult>();
        godot::ClassDB::register_class<NetcodeRpcInfo>();
        godot::ClassDB::register_class<NetcodeClientProcessor>();
        godot::ClassDB::register_abstract_class<NetcodeSession>();
        NetcodeSession::install();
    } else if (level == godot::MODULE_INITIALIZATION_LEVEL_EDITOR) {
        godot::ClassDB::register_class<NetcodeFieldPicker>();
        godot::ClassDB::register_class<NetcodeFieldPanel>();
        godot::ClassDB::register_class<NetcodeEditorPlugin>();
        godot::EditorPlugins::add_by_type<NetcodeEditorPlugin>();
    }
}

void uninitialize(godot::ModuleInitializationLevel level)
{
    if (level == godot::MODULE_INITIALIZATION_LEVEL_SCENE) NetcodeSession::uninstall();
}

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
