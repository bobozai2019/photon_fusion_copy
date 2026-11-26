extends "res://scale.gd"

const Spawner = preload("res://addons/netcode/scene_spawner.gd")
var spawner := Spawner.new()
var ball := preload("res://body.tscn")
var folder: String
var joined := false
var made := 0

func teleport_body(body: RigidBody3D, destination: Vector3) -> void:
    super.teleport_body(body, destination)
    body.get_node("Sync").teleport_generation += 1

func _ready() -> void:
    var args := OS.get_cmdline_user_args()
    writer = args[0] == "A"
    count = int(args[1])
    folder = args[3]
    pairs = args[4] == "pairs"
    actors.name = "Objects"
    add_child(actors)
    spawner.spawn_path = ^"../Objects"
    spawner.add_spawnable_scene(ball)
    add_child(spawner)
    spawner.room_joined.connect(func(_id): joined = true)
    var error := spawner.open(writer,"127.0.0.1",int(args[2]),2)
    if error: report("failed", error)
    else: report("opened", true)

func available() -> int:
    var total := 0
    for body in actors.get_children():
        var sync = body.get_node("Sync")
        if sync.initialized:
            sync.body_adapter.mode = sync.body_adapter.Correction.BLEND
            total += 1
    return total

func _physics_process(dt: float) -> void:
    if not joined: return
    if writer and made < count and FileAccess.file_exists(folder.path_join("spawn")):
        for i in mini(4, count-made):
            spawner.spawn(ball)
            made += 1
    if writer and prepared and not running and not final_sent and FileAccess.file_exists(folder.path_join("go")):
        start_measurement()
    super._physics_process(dt)
