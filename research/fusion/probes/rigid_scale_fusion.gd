extends "res://scale.gd"

var spawner = null
var ball := preload("res://body.tscn")
var spawn_requested := false
var made := 0

func teleport_body(body: RigidBody3D, destination: Vector3) -> void:
    body.get_node("Sync").teleport_3d(destination, Vector3.ZERO)

func _ready() -> void:
    call_deferred("begin")

func begin() -> void:
    var config: Dictionary = JSON.parse_string(JavaScriptBridge.eval("JSON.stringify(window.fusionProbe)",true))
    count = int(config.count)
    writer = config.role == "A"
    pairs = config.pairs
    actors.name = "Objects"
    add_child(actors)
    spawner = ClassDB.instantiate("FusionSpawner")
    spawner.name = "Spawner"
    spawner.set_spawn_path(^"../Objects")
    add_child(spawner)
    spawner.add_spawnable_scene(ball)
    Fusion.set_app_id(config.app)
    Fusion.connected_to_photon.connect(func(): report("connected", true); Fusion.join_or_create_room(config.room))
    Fusion.connection_failed.connect(func(reason): report("failed", reason))
    Fusion.room_joined.connect(func(): report("joined",Fusion.get_local_player_id()))
    Fusion.connect_to_photon("scale-%s-%s" % [config.room,config.role],config.region)

func _process(_dt: float) -> void:
    if not writer: return
    var cmd = JavaScriptBridge.eval("window.scaleCommand || ''",true)
    if cmd == "spawn": spawn_requested = true
    if cmd == "go" and prepared and not final_sent and not running: start_measurement()

func _physics_process(dt: float) -> void:
    if spawn_requested and made < count:
        for i in mini(4, count-made):
            var body = spawner.spawn(ball)
            if body == null:
                report("failed","spawn returned null")
                spawn_requested = false
                break
            made += 1
    super._physics_process(dt)
