extends Node

var objects := Node3D.new()
var spawner = null
var scenes := {}
var config: Dictionary
var sending_inputs := false
var serial := 0
var touched_wall := false
var max_step := 0.0
var previous_x := 0.0
var scene_parent := Node3D.new()

func _ready() -> void:
    call_deferred("begin")

func record(event: String, value: Variant) -> void:
    print("CLIENT_EVENT ", JSON.stringify({"event":event, "value":value, "frame":Engine.get_physics_frames()}))

func begin() -> void:
    config = JSON.parse_string(JavaScriptBridge.eval("JSON.stringify(window.fusionProbe)", true))
    objects.name = "Objects"
    add_child(objects)
    spawner = ClassDB.instantiate("FusionSpawner")
    spawner.name = "Spawner"
    spawner.set_spawn_path(^"../Objects")
    add_child(spawner)
    var kinds := ["tracked", "control", "declined", "transfer", "interest"]
    if config.mode == "host":
        kinds = ["predicted"]
    elif config.mode == "spawn":
        kinds = ["attached"]
        spawner.spawned.connect(func(node: Node): record("spawner_spawned", node.score))
    elif config.mode == "session":
        kinds = ["body", "master_item", "attached"]
        add_child(scene_parent)
        Fusion.set_scene_parent(scene_parent)
        add_wall()
        Fusion.scene_ready.connect(func(sequence: int): record("map_ready", sequence))
        Fusion.scene_unloaded.connect(func(sequence: int, _path: String): record("map_unloaded", sequence))
        Fusion.master_client_changed.connect(func(new_id: int, old_id: int): record("master", [new_id, old_id]))
        Fusion.player_left.connect(func(player: int, inactive: bool): record("player_left", [player, inactive]))
    for kind in kinds:
        var scene = load("res://%s.tscn" % kind)
        scenes[kind] = scene
        spawner.add_spawnable_scene(scene)
    Fusion.set_app_id(config.app)
    Fusion.connected_to_photon.connect(func(): Fusion.join_or_create_room(config.room))
    Fusion.connection_failed.connect(func(error: String): record("failed", error))
    Fusion.room_joined.connect(func(): record("joined", [Fusion.get_local_player_id(), Fusion.is_master_client(), Fusion.get_simulation_mode()]))
    Fusion.room_left.connect(func(): record("left", true))
    Fusion.connect_to_photon("client-%s-%s" % [config.room, config.get("identity", config.role)], config.region)

func add_wall() -> void:
    var wall := StaticBody3D.new()
    wall.position.x = 3.0
    var collider := CollisionShape3D.new()
    var box := BoxShape3D.new()
    box.size = Vector3(0.25, 2.0, 2.0)
    collider.shape = box
    wall.add_child(collider)
    add_child(wall)

func find_actor(kind: String):
    for actor in objects.get_children():
        if actor.kind == kind:
            return actor
    return null

func _physics_process(delta: float) -> void:
    if config.get("mode", "") == "session":
        var body = find_actor("body")
        if body != null:
            touched_wall = touched_wall or body.get_contact_count() > 0
            max_step = maxf(max_step, absf(body.position.x - previous_x))
            previous_x = body.position.x
    if config.get("mode", "") != "host":
        return
    var actor = find_actor("predicted")
    if actor == null:
        return
    var sync = actor.get_node("Sync")
    if sending_inputs and sync.has_input_authority() and serial < 60:
        serial += 1
        var payload := PackedByteArray()
        payload.resize(4)
        payload.encode_u32(0, serial)
        sync.queue_input(delta, payload)
    sync.process_input_queue(delta)

func _process(_delta: float) -> void:
    if config.is_empty():
        return
    var text = JavaScriptBridge.eval("window.fusionCommand ? JSON.stringify(window.fusionCommand) : ''", true)
    if not text is String or text.is_empty():
        return
    JavaScriptBridge.eval("window.fusionCommand = null", true)
    var cmd: Dictionary = JSON.parse_string(text)
    var kind := str(cmd.get("kind", "tracked"))
    var actor = find_actor(kind)
    match str(cmd.op):
        "spawn":
            actor = spawner.spawn(scenes[kind])
        "spawn_order":
            var initialize := Callable()
            if cmd.pre:
                initialize = func(_sync: Node, node: Node): node.score = 0.25
            actor = spawner.spawn(scenes[kind], initialize)
            record("spawn_return", actor.score)
            actor.score = 0.75
        "despawn":
            if actor != null:
                spawner.despawn(actor)
        "state":
            if actor != null:
                actor.score = float(cmd.score)
                actor.label = cmd.label
                actor.marks.assign(cmd.marks)
        "watch_score":
            actor.watch_score = bool(cmd.yes)
            if actor.watch_score:
                actor.observed_scores.clear()
        "write_three":
            for value in [0.125, 0.375, 0.875]:
                actor.score = value
                record("write", [Engine.get_physics_frames(), value])
        "score_window":
            record("scores", actor.observed_scores.duplicate())
        "float_setting":
            record("float_setting", ProjectSettings.get_setting("fusion/serialization/float_compression"))
        "inspect":
            if actor != null:
                var sync = actor.get_node("Sync")
                record("state", [kind, actor.last_score, actor.label, actor.marks, sync.has_authority(), sync.get_owner_id(), actor.get_instance_id()])
        "members":
            var names := []
            for child in objects.get_children():
                names.append(child.kind)
            names.sort()
            record("members", names)
        "measure":
            measure(int(cmd.ticks), int(cmd.hz))
        "rpc":
            if actor != null:
                for sequence in range(4):
                    Fusion.rpc_to_player(int(cmd.player), Callable(actor, "reliable_mark"), sequence)
        "mixed_rpc":
            var target := int(cmd.player)
            var ctl = find_actor("control")
            for sequence in range(2):
                Fusion.rpc_to_player(target, Callable(actor, "reliable_mark"), 10 + sequence)
                Fusion.rpc_to_player(target, Callable(ctl, "reliable_mark"), 20 + sequence)
                Fusion.rpc_to_player(target, Callable(actor, "unreliable_mark"), sequence)
        "swap_child":
            var old = actor.get_node("Part")
            old.value = 11.0
            actor.remove_child(old)
            actor.detached = old
            var fresh = preload("res://part.gd").new()
            fresh.name = "Part"
            fresh.value = 22.0
            actor.add_child(fresh)
        "free_old":
            if is_instance_valid(actor.detached):
                actor.detached.free()
                actor.detached = null
        "child_state":
            var part = actor.get_node("Part")
            record("child", [part.last_value, part.reads, actor.detached.reads if is_instance_valid(actor.detached) else -1])
        "allow":
            if actor != null: actor.allow_transfer = cmd.yes
        "want":
            if actor != null: actor.get_node("Sync").want_authority(cmd.yes)
        "subscribe":
            if cmd.yes: Fusion.add_user_key(42)
            else: Fusion.remove_user_key(42)
        "input_owner":
            if actor != null: actor.get_node("Sync").set_input_authority(int(cmd.player))
        "input_start":
            sending_inputs = true
        "launch_body":
            actor.sleeping = false
            actor.linear_velocity = Vector3(3, 0, 0)
        "warp_body":
            actor.get_node("Sync").teleport_3d(Vector3(25, 0, 0), Vector3.ZERO)
            actor.linear_velocity = Vector3.ZERO
        "sleep_body":
            actor.linear_velocity = Vector3.ZERO
            actor.sleeping = true
        "wake_body":
            actor.sleeping = false
            actor.linear_velocity = Vector3(0.5, 0, 0)
        "body_state":
            if actor != null:
                record("body", [actor.position.x, actor.linear_velocity.x, actor.sleeping, touched_wall, max_step])
        "load_map":
            record("map_seq", Fusion.load_scene(load("res://map.tscn")))
        "maps":
            record("maps", [Fusion.get_loaded_scenes().size(), scene_parent.get_child_count()])
        "unload_map":
            Fusion.unload_scene(int(cmd.sequence))
        "session":
            record("room", [Fusion.get_local_player_id(), Fusion.is_master_client(), Fusion.get_room().get_player_count()])
        "inputs":
            if actor != null:
                var sync = actor.get_node("Sync")
                record("inputs", [sync.get_input_authority(), sync.has_input_authority(), serial,
                    actor.input_calls, actor.seen_inputs.size(), actor.replay_calls, actor.resets, actor.highest_payload, sync.get_input_queue_count()])
        "leave":
            Fusion.leave_room()
    record("command", int(cmd.id))

func measure(ticks: int, hz: int) -> void:
    Engine.physics_ticks_per_second = hz
    for i in range(4): await get_tree().physics_frame
    var tracked = find_actor("tracked")
    var control = find_actor("control")
    if tracked == null or control == null:
        record("sample", [])
        return
    var start := [Engine.get_physics_frames(), Engine.get_process_frames(), tracked.reads, control.reads, tracked.sets, Time.get_ticks_msec()]
    for i in range(ticks): await get_tree().physics_frame
    record("sample", [hz, Engine.get_physics_frames()-start[0], Engine.get_process_frames()-start[1],
        tracked.reads-start[2], control.reads-start[3], tracked.sets-start[4], Time.get_ticks_msec()-start[5]])

