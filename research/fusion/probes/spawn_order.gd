extends SceneTree

const Orb = preload("res://orb.tscn")
const INITIAL := 731
const AFTER_RETURN := 741
var side := ""
var fusion: Object
var spawner: Node
var sequence := 0
var joined := false
var tried_spawn := false
var ready_file := ""
var room_name := ""
var deadline := 0
var write_after_return := false

func _initialize() -> void:
    call_deferred("_run")

func _run() -> void:
    var args := OS.get_cmdline_user_args()
    side = args[0]
    if side == "defaults":
        for kind in ["FusionSharedReplicator", "FusionSpawner"]:
            var node: Node = ClassDB.instantiate(kind)
            var names := ["root_path", "root_replication_mode", "owner_mode", "interest_destroy_on_exit"] if kind.ends_with("Replicator") else ["spawn_path"]
            for key in names:
                record(kind + "." + key, node.get(key))
            node.free()
        quit()
        return
    room_name = args[1]
    ready_file = args[2]
    write_after_return = "--after-return" in args
    fusion = Engine.get_singleton("Fusion")
    spawner = ClassDB.instantiate("FusionSpawner")
    root.add_child(spawner)
    spawner.add_spawnable_scene(Orb)
    spawner.spawned.connect(func(node: Node): record("spawner_spawned", node.value, node.get_node("Sync").has_authority()))
    fusion.connected_to_photon.connect(_connected)
    fusion.room_joined.connect(func():
        joined = true
        record("room_joined", fusion.get_local_player_id())
        if side == "host":
            var ready := FileAccess.open(ready_file, FileAccess.WRITE)
            ready.store_string("ready"))
    fusion.connection_failed.connect(func(reason: String):
        record("connection_failed", reason)
        quit(2))
    deadline = Time.get_ticks_msec() + 25000
    process_frame.connect(_observe)
    fusion.connect_to_photon("spawn-order-%s-%s" % [room_name, side])

func _connected() -> void:
    record("connected")
    if side == "host":
        var options: Object = ClassDB.instantiate("FusionRoomOptions")
        options.set_max_players(2)
        options.set_is_visible(false)
        fusion.join_or_create_room(room_name, options)
    else:
        fusion.join_room(room_name)

func _observe() -> void:
    if Time.get_ticks_msec() >= deadline:
        record("timeout")
        quit(3)
    if not joined or tried_spawn or float(fusion.get_network_time()) <= 0.0:
        return
    tried_spawn = true
    record("clock_ready")
    if side == "host":
        return
    record("spawn_call")
    var node: Node = spawner.spawn(Orb, _initialize_orb)
    if not is_instance_valid(node):
        record("spawn_failed")
        quit(4)
        return
    record("spawn_return", node.value, node.get_node("Sync").has_authority())
    if write_after_return:
        node.value = AFTER_RETURN
    create_timer(4.0).timeout.connect(func(): quit())

func _initialize_orb(...args: Array) -> void:
    var types := PackedStringArray()
    for arg in args:
        types.append(arg.get_class() if arg is Object else type_string(typeof(arg)))
    record("pre_spawn_arguments", ",".join(types))
    for arg in args:
        if arg is Node2D and arg.has_node("Sync"):
            record("pre_spawn", arg.value, arg.get_node("Sync").has_authority())
            arg.value = INITIAL
            return

func record(event: String, value: Variant = null, authority: Variant = null) -> void:
    sequence += 1
    print("SPAWN_ORDER ", JSON.stringify({"side": side, "n": sequence, "event": event,
        "value": str(value), "authority": authority, "us": Time.get_ticks_usec(),
        "clock": fusion.get_network_time() if is_instance_valid(fusion) else 0.0}))
    if side == "host" and event == "setter" and value == (AFTER_RETURN if write_after_return else INITIAL):
        create_timer(2.0).timeout.connect(func(): quit())
