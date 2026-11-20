extends SceneTree

const VEHICLE := preload("res://scenes/vehicle.tscn")
const MARKERS := [0.75, -0.5]

var role := ""
var room_name := ""
var trigger := ""
var vehicles: Node3D
var spawner: FusionSpawner
var authority: Node
var joined := false
var next_mark := 0
var first_mark_ms := 0
var previous := 0.0

func _initialize() -> void:
    call_deferred("boot")

func boot() -> void:
    var args := OS.get_cmdline_user_args()
    if args.size() != 3 or args[0] not in ["A", "B"]:
        quit(2)
        return
    role = args[0]
    room_name = args[1]
    trigger = args[2]

    var stage := Node3D.new()
    root.add_child(stage)
    vehicles = Node3D.new()
    vehicles.name = "Vehicles"
    stage.add_child(vehicles)
    spawner = FusionSpawner.new()
    spawner.set("spawn_path", ^"../Vehicles")
    stage.add_child(spawner)
    spawner.add_spawnable_scene(VEHICLE)
    vehicles.child_entered_tree.connect(freeze_vehicle)
    Fusion.connected_to_photon.connect(func(): Fusion.join_or_create_room(room_name))
    Fusion.room_joined.connect(on_joined)
    Fusion.connection_failed.connect(func(error: String):
        print("PROBE_FAILED ", error)
        quit(3)
    )
    create_timer(24.0).timeout.connect(func():
        print("PROBE_TIMEOUT role=", role, " joined=", joined, " vehicles=", vehicles.get_child_count(),
            " marks=", next_mark)
        quit(4)
    )
    process_frame.connect(sample)
    Fusion.connect_to_photon("netcode-" + room_name + "-" + role)

func freeze_vehicle(vehicle: Node) -> void:
    vehicle.set_physics_process(false)
    vehicle.set_process(false)
    vehicle.set_deferred("freeze", true)

func on_joined() -> void:
    joined = true
    if role == "A":
        authority = spawner.spawn(VEHICLE)
        freeze_vehicle(authority)
    print("PROBE_JOINED role=", role, " player=", Fusion.get_local_player_id())

func sample() -> void:
    if not joined:
        return
    if role == "A":
        if next_mark == MARKERS.size():
            if Time.get_ticks_msec() - first_mark_ms > 2700:
                quit(0)
            return
        if not FileAccess.file_exists(trigger) or not is_instance_valid(authority):
            return
        var replicator: FusionSharedReplicator = authority.get_node("FusionSharedReplicator")
        if not replicator.has_authority():
            return
        if next_mark == 0:
            first_mark_ms = Time.get_ticks_msec()
        elif Time.get_ticks_msec() - first_mark_ms < 700:
            return
        authority.set("net_input_x", MARKERS[next_mark])
        print("PROBE_WRITE marker=", MARKERS[next_mark], " physics_frame=", Engine.get_physics_frames())
        next_mark += 1
    elif vehicles.get_child_count() == 1:
        var vehicle := vehicles.get_child(0)
        var replicator: FusionSharedReplicator = vehicle.get_node("FusionSharedReplicator")
        if replicator.has_authority():
            return
        var value := float(vehicle.get("net_input_x"))
        if value != previous:
            previous = value
            print("PROBE_READ marker=", value, " physics_frame=", Engine.get_physics_frames())
            if is_equal_approx(value, MARKERS[next_mark]):
                next_mark += 1
                if next_mark == MARKERS.size():
                    quit(0)
