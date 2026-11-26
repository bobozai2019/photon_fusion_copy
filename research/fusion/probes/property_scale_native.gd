extends "res://measure.gd"

const Spawner = preload("res://addons/netcode/scene_spawner.gd")
var folder: String

func _ready() -> void:
    var args := OS.get_cmdline_user_args()
    writer = args[0] == "A"
    count = int(args[1])
    folder = args[3]
    workload = args[4]
    configure()
    spawner = Spawner.new()
    spawner.spawn_path = ^"../Objects"
    spawner.add_spawnable_scene(Scene)
    add_child(spawner)
    spawner.room_joined.connect(func(_id): online = true)
    var error = spawner.open(writer, "127.0.0.1", int(args[2]), 2)
    if error: emit_result("failed", error)
    else: emit_result("opened", true)

func bound(actor: Node) -> bool:
    var sync = actor.get_node("Sync")
    return sync.initialized and (not writer or sync.is_write_ready())

func command() -> String:
    var cmd := ("go" if writer else "go_observer") if prepared else "spawn"
    if not FileAccess.file_exists(folder.path_join(cmd)): return ""
    return "go" if prepared else cmd

func stop_notified() -> bool:
    return FileAccess.file_exists(folder.path_join("writer_done"))
