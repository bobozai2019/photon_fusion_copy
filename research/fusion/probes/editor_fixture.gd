extends SceneTree

class Target extends Node:
    var reads := 0
    var value: Variant:
        get:
            reads += 1
            return value

var errors := 0
var joined := false

func check(label: String, value: Variant, expected: Variant) -> void:
    print("FIXTURE ", label, " ", value)
    if value != expected:
        errors += 1
        push_error("%s expected %s" % [label, expected])

func _initialize() -> void:
    call_deferred("run")

func run() -> void:
    var mode := OS.get_cmdline_user_args()[0]
    if mode == "editor":
        await editor_export()
    elif mode == "single":
        await single_room()
    else:
        layout()
    print("FIXTURE_FINISH ",errors)
    quit(int(errors > 0))

func editor_export() -> void:
    for i in range(60): await process_frame
    var plugins := root.find_children("*", "FusionEditorPlugin", true, false)
    check("editor plugin instances", plugins.size(), 1)
    if plugins.is_empty(): return
    var folder := ProjectSettings.globalize_path("res://")
    ProjectSettings.set_setting("fusion/schema/dll_output_path", folder)
    plugins[0].call("_export_schema_json")
    plugins[0].call("_schema_dir_selected", folder)
    check("generated JSON", FileAccess.file_exists("res://FusionCustomPlugin.Generated.g.json"), true)
    plugins[0].call("_export_schema_cs")
    plugins[0].call("_schema_dir_selected", folder)
    for i in range(3): await process_frame

func layout() -> void:
    for row in [["actor", 9], ["auto", 16]]:
        var scene = load("res://%s.tscn" % row[0]).instantiate()
        root.add_child(scene)
        var sync = scene.get_node("Sync")
        check(str(row[0]) + " runtime words", sync.compute_word_count(scene), row[1])
        var cfg = sync.get_replication_config()
        check("config-only words", cfg.get_total_word_count(), 0)
        var persisted := []
        for prop in cfg.get_property_list():
            if str(prop.name).begins_with("properties/"): persisted.append(prop.name)
        check("three rows serializable entries", persisted.size(), 18)
        var clone = ClassDB.instantiate("FusionReplicationConfig")
        for i in range(cfg.get_property_count()):
            clone.insert_full_property(i, cfg.get_property_data(i))
        sync.set_replication_config(clone)
        check("copied config runtime words", sync.compute_word_count(scene), row[1])
        scene.free()
    var actor := Node3D.new()
    root.add_child(actor)
    var original := Target.new()
    original.name = "Target"
    original.value = 1.0
    actor.add_child(original)
    var sync = ClassDB.instantiate("FusionSharedReplicator")
    sync.set_root_replication_mode(0)
    var cfg = ClassDB.instantiate("FusionReplicationConfig")
    cfg.add_property(^"Target:value")
    sync.set_replication_config(cfg)
    actor.add_child(sync)
    check("original float size", sync.compute_word_count(actor), 2)
    actor.remove_child(original)
    var fresh := Target.new()
    fresh.name = "Target"
    fresh.value = Vector3.ONE
    actor.add_child(fresh)
    check("new Vector3 before old free", sync.compute_word_count(actor), 4)
    check("getter calls on old and replacement", [original.reads, fresh.reads], [1, 1])
    original.free()
    check("after old free", sync.compute_word_count(actor), 4)
    fresh.free()
    check("missing target", sync.compute_word_count(actor), 1)
    actor.free()

func single_room() -> void:
    var fusion = Engine.get_singleton("Fusion")
    fusion.room_joined.connect(func(): joined = true)
    fusion.create_singleplayer_room()
    for i in range(20):
        if joined: break
        await physics_frame
    check("single room", [joined, fusion.is_in_room(), fusion.is_master_client(), fusion.get_local_player_id()],
        [true, true, true, 1])
    if not joined: return
    var stage := Node3D.new()
    root.add_child(stage)
    var objects := Node3D.new()
    objects.name = "Objects"
    stage.add_child(objects)
    var spawner = ClassDB.instantiate("FusionSpawner")
    spawner.set_spawn_path(^"../Objects")
    stage.add_child(spawner)
    var prefab: PackedScene = load("res://actor.tscn")
    spawner.add_spawnable_scene(prefab)
    var actor = spawner.spawn(prefab)
    check("spawn count", objects.get_child_count(), 1)
    if actor == null: return
    var sync = actor.get_node("Sync")
    check("local authority", [sync.has_authority(), sync.get_owner_id()], [true, 1])
    for i in range(10): await physics_frame
    var reads: int = actor.reads
    var first := Engine.get_physics_frames()
    for i in range(60): await physics_frame
    check("frames, getter delta, has shadow", [Engine.get_physics_frames()-first, actor.reads-reads, sync.has_shadow_data()],
        [60, 0, false])
    await codec_workload(spawner)
    spawner.despawn(actor)
    for i in range(3): await physics_frame
    check("despawn count", objects.get_child_count(), 0)
    fusion.leave_room()
    for i in range(3): await physics_frame
    check("in room after leave", fusion.is_in_room(), false)
    stage.free()


# Match the scalar, UTF-8 string and integer-list mutations in tests/core/codec_workload.cpp.
func codec_workload(spawner: Node) -> void:
    var prefab: PackedScene = load("res://workload.tscn")
    spawner.add_spawnable_scene(prefab)
    var actor = spawner.spawn(prefab)
    if actor == null:
        check("workload spawned", false, true)
        return
    var sync = actor.get_node("Sync")
    actor.label = "Q".repeat(80)
    for n in range(40): actor.marks.append(n * 7937)
    var words: int = sync.compute_word_count(actor)
    var reads: int = actor.reads
    var first := Engine.get_physics_frames()
    for tick in range(1,601):
        actor.score = tick
        actor.marks[tick % 40] = tick
        await physics_frame
    var sampled: int = actor.reads - reads
    var digest: int = ((2166136261 ^ actor.score) * 16777619) & 0xffffffff
    for byte in actor.label.to_utf8_buffer(): digest = ((digest ^ byte) * 16777619) & 0xffffffff
    for value in actor.marks: digest = ((digest ^ value) * 16777619) & 0xffffffff
    check("workload final digest", digest, 1124070639)
    check("workload physics frames", Engine.get_physics_frames() - first, 600)
    print("FIXTURE workload words=", words, " scalar_reads=", sampled, " shadow=", sync.has_shadow_data(),
        " remote_peers=0 encoded_bytes=unobserved codec_cpu_ms=unobserved")
    spawner.despawn(actor)
