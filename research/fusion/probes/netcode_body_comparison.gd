extends SceneTree

# Run twice against the same local room, passing role, port, shared scratch directory
# and "default" or "blend". Files synchronize observations, never the physics state.
const Room = preload("res://addons/netcode/scene_room.gd")
const Sync = preload("res://addons/netcode/scene_replicator.gd")
var room := Room.new()
var world := Node3D.new()
var body := RigidBody3D.new()
var sync := Sync.new()
var host: bool
var folder: String
var blend: bool
var stage := 0
var touched := false
var started := false
var configured := false
var deadline := 0

func shape(parent: Node3D, resource: Shape3D) -> void:
    var collider := CollisionShape3D.new()
    collider.shape = resource
    parent.add_child(collider)

func _initialize() -> void:
    setup.call_deferred()

func setup() -> void:
    var args := OS.get_cmdline_user_args()
    host = args[0] == "host"
    folder = args[2]
    blend = args[3] == "blend"
    get_root().add_child(world)
    var wall := StaticBody3D.new()
    var box := BoxShape3D.new()
    box.size = Vector3(0.25, 2, 2)
    shape(wall, box)
    wall.position.x = 3
    world.add_child(wall)
    var ball := SphereShape3D.new()
    ball.radius = 0.25
    body.name = "Ball"
    body.gravity_scale = 0
    body.linear_damp_mode = RigidBody3D.DAMP_MODE_REPLACE
    body.linear_damp = 0
    body.contact_monitor = true
    body.max_contacts_reported = 4
    shape(body, ball)
    body.add_child(sync)
    world.add_child(body)
    sync.teleport_distance = 10
    sync.teleport_angle_degrees = 90
    var error := room.open(world, host, "127.0.0.1", int(args[1]), 2)
    if not error.is_empty():
        push_error(error)
        quit(1)
        return
    deadline = Time.get_ticks_msec() + 20000
    print("comparison_open")

func marker(label: String) -> String:
    return folder.path_join(label)

func observe(label: String) -> void:
    print("observed ", label, " x=", body.position.x, " vx=", body.linear_velocity.x,
        " sleeping=", body.sleeping, " contact=", touched, " frame=", Engine.get_physics_frames())
    FileAccess.open(marker(label), FileAccess.WRITE).close()

func _physics_process(_dt: float) -> bool:
    if not deadline: return false
    if Time.get_ticks_msec() > deadline:
        push_error("Timed out at stage %s, x=%s" % [stage, body.position.x])
        quit(1)
        return false
    var frame := room.advance()
    if host and not started and frame.get("seats", 0) == 2 and room.game.start():
        started = room.spawn(sync) != 0
    if not sync.initialized: return false
    if not configured:
        if blend: sync.body_adapter.mode = sync.body_adapter.Correction.BLEND
        print("adapter mode=", sync.body_adapter.mode, " forecast_s=", sync.body_adapter.max_forecast)
        configured = true
        if not host: observe("ready")
    touched = touched or body.get_contact_count() > 0
    if host:
        if stage == 0 and FileAccess.file_exists(marker("ready")):
            body.sleeping = false
            body.linear_velocity = Vector3(3, 0, 0)
            stage = 1
        if stage == 1 and touched and FileAccess.file_exists(marker("proxy_wall")):
            observe("owner_wall")
            body.position = Vector3(25, 0, 0)
            body.linear_velocity = Vector3.ZERO
            sync.teleport_generation += 1
            stage = 2
        if stage == 2 and FileAccess.file_exists(marker("warp")):
            body.sleeping = false
            body.linear_velocity.x = 0.5
            stage = 3
        if stage == 3 and FileAccess.file_exists(marker("awake")):
            body.linear_velocity = Vector3.ZERO
            body.sleeping = true
            stage = 4
        if stage == 4 and FileAccess.file_exists(marker("asleep")): quit()
    else:
        if stage == 0 and body.position.x > 0.1 and body.linear_velocity.x > 0.1:
            observe("moving")
            stage = 1
        if stage == 1 and touched and body.position.x > 2.3:
            observe("proxy_wall")
            stage = 2
        if stage == 2 and body.position.x > 24:
            observe("warp")
            stage = 3
        if stage == 3 and not body.sleeping and body.linear_velocity.x > 0.1:
            observe("awake")
            stage = 4
        if stage == 4 and body.sleeping:
            observe("asleep")
            quit()
    return false
