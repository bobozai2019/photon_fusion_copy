extends "res://measure.gd"

func _ready() -> void:
    call_deferred("begin")

func begin() -> void:
    var config: Dictionary = JSON.parse_string(JavaScriptBridge.eval("JSON.stringify(window.fusionProbe)", true))
    writer = config.role == "A"
    count = int(config.count)
    workload = config.workload
    configure()
    spawner = ClassDB.instantiate("FusionSpawner")
    spawner.name = "Spawner"
    spawner.set_spawn_path(^"../Objects")
    add_child(spawner)
    spawner.add_spawnable_scene(Scene)
    Fusion.set_app_id(config.app)
    Fusion.connected_to_photon.connect(func(): Fusion.join_or_create_room(config.room))
    Fusion.connection_failed.connect(func(reason): emit_result("failed", reason))
    Fusion.room_joined.connect(joined)
    Fusion.connect_to_photon("properties-%s-%s" % [config.room, config.role], config.region)

func joined() -> void:
    online = true
    emit_result("joined", Fusion.get_local_player_id())

func command() -> String:
    var op: String = JavaScriptBridge.eval("window.scaleCommand || ''", true)
    return "go" if op == "go_observer" else op

func stop_notified() -> bool:
    return JavaScriptBridge.eval("window.scaleCommand === 'writer_done'", true)
