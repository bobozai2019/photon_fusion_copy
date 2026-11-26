extends SceneTree

class Fields extends Node3D:
    var reads := 0
    var score: float = 1.0:
        get:
            reads += 1
            return score
    var flag := false
    var lives := 3
    var label := "short"
    var offset := Vector3.ONE
    var facing := Quaternion.IDENTITY
    var samples: Array[float] = []

const ROOT_WORDS := {
    "Node": [1, 2], "Node2D": [1, 5], "Node3D": [1, 8],
    "CharacterBody2D": [1, 7], "CharacterBody3D": [1, 11],
    "RigidBody2D": [3, 9], "RigidBody3D": [3, 15],
}
const FIELD_WORDS := {
    ^".:score": 1, ^".:flag": 1, ^".:lives": 2,
    ^".:label": 2, ^".:offset": 3, ^".:facing": 4,
}
var failures := 0

func _initialize() -> void:
    call_deferred("run")

func measure(label: String, actual: Variant, expected: Variant) -> void:
    print(label, " = ", actual)
    if actual != expected:
        failures += 1
        push_error("%s expected %s, got %s" % [label, expected, actual])

func measure_racing_vehicle() -> void:
    var modes := {}
    for name in ClassDB.class_get_enum_constants("FusionReplicator", "PhysicsCorrectionMode"):
        modes[name] = ClassDB.class_get_integer_constant("FusionReplicator", name)
    measure("native PhysicsCorrectionMode", modes,
        {"CORRECTION_VELOCITY": 0, "CORRECTION_FORCE": 1, "CORRECTION_BLEND": 2})
    var scene := load("res://scenes/vehicle.tscn") as PackedScene
    var car := scene.instantiate()
    var sync = car.get_node("FusionSharedReplicator")
    measure("Racing [owner, root replication, root interpolation]",
        [sync.get_owner_mode(), sync.get_root_replication_mode(), sync.get_root_interpolation_mode()], [1, 1, 2])
    measure("Racing [teleport distance, angle, forecast gravity]",
        [sync.get_root_teleport_threshold(), sync.get_root_rotation_teleport_threshold(), sync.get_root_forecast_gravity()],
        [10.0, 90.0, false])
    measure("Racing [correction, stiffness, damping, max forecast time]",
        [sync.get_root_correction_mode(), sync.get_root_spring_stiffness(),
         sync.get_root_spring_damping(), sync.get_root_max_forecast_time()], [2, 50.0, 10.0, 0.25])
    var fresh = ClassDB.instantiate("FusionSharedReplicator")
    measure("fresh SharedReplicator correction mode", fresh.get_root_correction_mode(), 2)
    fresh.free()
    car.free()

func run() -> void:
    if not Engine.has_singleton("Fusion") or not ClassDB.class_exists("FusionSharedReplicator"):
        push_error("Run in the imported Racing project with its native editor extension.")
        quit(1)
        return
    var fusion = Engine.get_singleton("Fusion")
    var classes := PackedStringArray()
    for name in ClassDB.get_class_list():
        if str(name).begins_with("Fusion"):
            classes.append(name)
    classes.sort()
    print("Godot ", Engine.get_version_info().string, " / ", classes)
    measure("registered Fusion classes", classes.size(), 21)
    measure("in room", fusion.is_in_room(), false)
    measure("create_singleplayer_room registered", fusion.has_method("create_singleplayer_room"), false)
    measure("float compression setting", ProjectSettings.get_setting("fusion/serialization/float_compression"), 0)
    measure_racing_vehicle()

    for name in ROOT_WORDS:
        var actor = ClassDB.instantiate(name)
        root.add_child(actor)
        var replicator = ClassDB.instantiate("FusionSharedReplicator")
        actor.add_child(replicator)
        var sizes := []
        for mode in [0, 1]:
            replicator.set_root_replication_mode(mode)
            sizes.append(replicator.compute_word_count(actor))
        measure(name + " [None, AUTO]", sizes, ROOT_WORDS[name])
        actor.free()

    var actor := Fields.new()
    root.add_child(actor)
    var replicator = ClassDB.instantiate("FusionSharedReplicator")
    actor.add_child(replicator)
    replicator.set_root_replication_mode(0)
    var config = ClassDB.instantiate("FusionReplicationConfig")
    replicator.set_replication_config(config)
    var base: int = replicator.compute_word_count(actor)
    measure("Node3D without custom fields", base, 1)
    for path in FIELD_WORDS:
        config.clear_properties()
        config.add_property(path)
        measure(str(path) + " additional words", int(replicator.compute_word_count(actor)) - base, FIELD_WORDS[path])
    config.clear_properties()
    config.add_property(^".:label")
    var short_words: int = replicator.compute_word_count(actor)
    actor.label = "x".repeat(256)
    measure("string [5, 256 characters]", [short_words, replicator.compute_word_count(actor)], [3, 3])

    for capacity in [2, 4, 8]:
        config.clear_properties()
        config.add_array_property(^".:samples", capacity)
        config.set_property_element_type(0, TYPE_FLOAT)
        actor.samples.clear()
        var empty_words: int = replicator.compute_word_count(actor)
        actor.samples.assign([3.0, 4.0])
        measure("float array capacity %d [empty, two elements]" % capacity,
            [empty_words, replicator.compute_word_count(actor)], [base + 1 + capacity, base + 1 + capacity])

    config.clear_properties()
    var before := actor.reads
    for i in range(5):
        replicator.compute_word_count(actor)
    measure("getter calls during five layouts, field absent", actor.reads - before, 0)
    config.add_property(^".:score")
    before = actor.reads
    for i in range(5):
        replicator.compute_word_count(actor)
    measure("getter calls during five layouts, field present", actor.reads - before, 5)
    before = actor.reads
    var start_frame := Engine.get_physics_frames()
    for i in range(30):
        await physics_frame
    print("offline physics frame delta = ", Engine.get_physics_frames() - start_frame)
    measure("getter calls during 30 offline physics signals", actor.reads - before, 0)
    actor.free()
    quit(int(failures > 0))
