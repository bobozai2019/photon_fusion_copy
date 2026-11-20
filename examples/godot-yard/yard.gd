extends Node2D

const Paint = preload("res://paint.gd")
const Replicator = preload("res://replicator.gd")

enum Template { CAR = 1, GATE, BEACON, BARRIER, LOAD }
enum Phase { CONNECTING, WAITING, PLAYING, ENDED }

var bridge := NetcodeYard.new()
var options: Dictionary
var replicator: Replicator
var world2: Node2D
var world3: Node3D
var status_label: Label
var notice_label: Label
var player_id := 0
var round_id := 0
var visual_logged := false
var capturing := false
var close_at := 0
var barriers: Dictionary = {}

func _ready() -> void:
    set_process(false)
    var error := bridge.configure(OS.get_cmdline_user_args(), FileAccess.get_file_as_string("res://replicators.cfg"))
    if not error.is_empty():
        push_error(error)
        get_tree().quit(1)
        return
    options = bridge.settings()
    get_tree().auto_accept_quit = false
    get_window().title = "Netcode Godot Car Yard - " + ("Host" if options.host else "Guest")
    if options.smoke:
        await RenderingServer.frame_post_draw
        print("Godot scene rendered")
        get_tree().quit()
        return

    make_field()
    replicator = Replicator.new(bridge.bindings(), make_object)
    if options.binding_smoke:
        await RenderingServer.frame_post_draw
        var passed := verify_bindings()
        print("Godot 2D/3D transform, independent fields, child replacement, deletion ",
            "passed" if passed else "failed")
        get_tree().quit(0 if passed else 1)
        return
    set_process(true)

func make_label(text: String, size: int, point: Vector2, color: Color) -> Label:
    var label := Label.new()
    label.position = point
    label.text = text
    label.add_theme_font_size_override("font_size", size)
    label.add_theme_color_override("font_color", color)
    add_child(label)
    return label

func make_field() -> void:
    add_child(Paint.new(Paint.Form.FIELD))
    var screen := SubViewportContainer.new()
    screen.mouse_filter = Control.MOUSE_FILTER_IGNORE
    add_child(screen)
    var viewport := SubViewport.new()
    viewport.size = Vector2i(960, 640)
    viewport.transparent_bg = true
    viewport.own_world_3d = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    screen.add_child(viewport)
    world3 = Node3D.new()
    viewport.add_child(world3)
    var camera := Camera3D.new()
    camera.projection = Camera3D.PROJECTION_ORTHOGONAL
    camera.size = 640.0
    camera.position = Vector3(480, 320, 950)
    world3.add_child(camera)
    camera.make_current()
    var light := DirectionalLight3D.new()
    light.rotation_degrees = Vector3(-47, -31, 0)
    light.light_energy = 1.5
    world3.add_child(light)
    var ambience := WorldEnvironment.new()
    ambience.environment = Environment.new()
    ambience.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
    ambience.environment.ambient_light_color = Color("#7d93aa")
    ambience.environment.ambient_light_energy = 0.65
    world3.add_child(ambience)

    world2 = Node2D.new()
    add_child(world2)
    make_label("CAR YARD", 25, Vector2(25, 12), Color("#e6f0fe"))
    make_label("GODOT", 13, Vector2(163, 22), Color("#8fb8d4"))
    make_label("PARKING", 13, Vector2(69, 116), Color("#b6c8dd"))
    make_label("3D BEACON", 13, Vector2(600, 131), Color("#b6c8dd"))
    make_label("GATE", 13, Vector2(350, 256), Color("#e5c4a1"))
    make_label("WASD / arrows  Drive     E  Gate     B  Place     X  Remove     H  Horn",
        14, Vector2(26, 612), Color("#a7b9cf"))
    status_label = make_label("", 17, Vector2(240, 16), Color("#bbd1e6"))
    notice_label = make_label("", 15, Vector2(26, 49), Color("#ffd28a"))

func make_object(snapshot: Dictionary) -> Node:
    match snapshot.schema:
        Template.CAR:
            var tint := Color("#5ce0bb") if player_id == snapshot.owner else (
                Color("#f9a569") if snapshot.owner == 1 else Color("#7caff6"))
            var car := Paint.new(Paint.Form.CAR, tint)
            var lamp := Paint.new(Paint.Form.LAMP)
            lamp.name = "lamp"
            car.add_child(lamp)
            world2.add_child(car)
            return car
        Template.BEACON:
            var root := Node3D.new()
            var model := MeshInstance3D.new()
            model.name = "model"
            var mesh := BoxMesh.new()
            mesh.size = Vector3(51, 51, 50)
            model.mesh = mesh
            var material := StandardMaterial3D.new()
            material.albedo_color = Color("#aa9bfc")
            material.roughness = 0.52
            model.material_override = material
            root.add_child(model)
            world3.add_child(root)
            return root
        _:
            var form := Paint.Form.LOAD
            if snapshot.schema == Template.GATE:
                form = Paint.Form.GATE
            elif snapshot.schema == Template.BARRIER:
                form = Paint.Form.BARRIER
            var node := Paint.new(form)
            world2.add_child(node)
            return node

func _unhandled_key_input(event: InputEvent) -> void:
    if options.is_empty() or options.scripted or options.smoke or options.binding_smoke:
        return
    var key := event as InputEventKey
    if key == null or not key.pressed or key.echo:
        return
    match key.physical_keycode:
        KEY_F2: bridge.act("start")
        KEY_F3: bridge.act("end_round")
        KEY_F4: bridge.act("disconnect")
        KEY_R: bridge.act("rejoin")
        KEY_E: bridge.act("gate")
        KEY_B: bridge.act("place")
        KEY_X: bridge.act("remove")
        KEY_H: bridge.act("horn")
        KEY_Q: close_room()

func _notification(what: int) -> void:
    if what == NOTIFICATION_WM_CLOSE_REQUEST and not options.is_empty():
        if options.smoke or options.binding_smoke:
            get_tree().quit(1)
        else:
            close_room()

func close_room() -> void:
    bridge.act("end_room")
    close_at = Time.get_ticks_msec() + 350

func _process(delta: float) -> void:
    if not options.scripted:
        if get_window().has_focus():
            var throttle := axis(KEY_W, KEY_UP) - axis(KEY_S, KEY_DOWN)
            var steer := axis(KEY_D, KEY_RIGHT) - axis(KEY_A, KEY_LEFT)
            bridge.drive(throttle, steer)
        else:
            bridge.drive(0.0, 0.0)
    var frame := bridge.advance()
    if frame.has("error"):
        push_error(frame.error)
        get_tree().quit(1)
        return
    if frame.status >= 0 or (close_at > 0 and Time.get_ticks_msec() >= close_at):
        get_tree().quit(maxi(frame.status, 0))
        return
    player_id = frame.player
    if round_id != frame.round:
        replicator.sync([], false)
        barriers.clear()
        round_id = frame.round
    var own := replicator.node_for(frame.car) as Node2D
    var previous_x := own.position.x if own != null else 0.0
    replicator.sync(frame.objects, not options.host)
    replicator.render(delta)
    if options.trace:
        trace_visual(frame, previous_x)
    status_label.text = status_text(frame)
    notice_label.text = frame.notice
    if not capturing and not options.capture.is_empty() and frame.ready and frame.tick > 20:
        capturing = true
        capture_frame()

func trace_visual(frame: Dictionary, previous_x: float) -> void:
    var current_barriers := {}
    for snapshot: Dictionary in frame.objects:
        if snapshot.schema == Template.BARRIER:
            current_barriers[snapshot.id] = true
            if not barriers.has(snapshot.id) and replicator.node_for(snapshot.id) != null:
                print("visual_created object=", snapshot.id)
        if visual_logged or options.host or not frame.ready or snapshot.id != frame.car or previous_x == 0.0:
            continue
        var node := replicator.node_for(snapshot.id) as Node2D
        var shown := node.position.x
        var authoritative: float = snapshot["values"][0]
        if absf(shown - previous_x) > 0.02 and absf(authoritative - shown) > 0.02 and (
            shown - previous_x) * (authoritative - shown) > 0.0:
            visual_logged = true
            print("visual id=", snapshot.id, " previous_x=", previous_x, " display_x=", shown,
                " authority_x=", authoritative)
    for id: int in barriers.keys():
        if not current_barriers.has(id) and replicator.node_for(id) == null:
            print("visual_removed object=", id)
    barriers = current_barriers

func capture_frame() -> void:
    await RenderingServer.frame_post_draw
    var image := get_viewport().get_texture().get_image()
    var passed := not image.is_empty() and image.save_png(options.capture) == OK
    print("render_capture_ok" if passed else "render_capture_failed")

static func axis(a: Key, b: Key) -> float:
    return 1.0 if Input.is_physical_key_pressed(a) or Input.is_physical_key_pressed(b) else 0.0

func status_text(frame: Dictionary) -> String:
    match frame.phase:
        Phase.CONNECTING: return "Connecting to host"
        Phase.WAITING:
            return str(frame.seats) + "/4 seats    " + ("F2  Start    Q  Close" if options.host else "Waiting for host")
        Phase.PLAYING:
            if not frame.ready:
                return "Applying current world"
            return "Player %s    Round %s    %s" % [
                frame.player, frame.round, "F3  End round    Q  Close" if options.host else "F4  Disconnect"]
        _:
            if options.host:
                return "Room closed"
            if frame.reject != 0:
                return "Join rejected    R  Retry"
            return "Host timed out    R  Reconnect" if frame.loss == 3 else "Disconnected    R  Reconnect"

func verify_bindings() -> bool:
    var car := {"id": 1, "schema": Template.CAR, "owner": 2, "revision": 1,
        "values": [110.0, 125.0, 45.0, 1.3, 1.5, 0.75, 0.5]}
    var beacon := {"id": 2, "schema": Template.BEACON, "owner": 0, "revision": 1,
        "values": [405.0, 340.0, 12.0, 16.0, 32.0, 46.0, 1.1, 1.2, 1.3, 1.0]}
    replicator.sync([car, beacon], false)
    replicator.render(0.0)
    var root := replicator.node_for(1) as Paint
    var lamp := root.get_node("lamp") as Paint
    var model := replicator.node_for(2).get_node("model") as MeshInstance3D
    if root == null or lamp == null or model == null:
        return false
    var passed := root.position.is_equal_approx(Vector2(110, 515)) and (
        root.scale.is_equal_approx(Vector2(1.3, 1.5))) and is_equal_approx(root.rotation_degrees, 45.0) and (
        is_equal_approx(root.net_input_x, 0.75)) and is_equal_approx(lamp.glow, 0.5) and (
        model.position.is_equal_approx(Vector3(405, 340, 12))) and (
        model.rotation_degrees.is_equal_approx(Vector3(16, 32, 46))) and (
        model.scale.is_equal_approx(Vector3(1.1, 1.2, 1.3)))
    car = car.duplicate(true)
    car.revision = 2
    car["values"][5] = -0.5
    replicator.sync([car, beacon], true)
    replicator.render(0.016)
    passed = passed and is_equal_approx(root.net_input_x, -0.5) and is_equal_approx(lamp.glow, 0.5)
    lamp.free()
    replicator.render(0.016)
    var replacement := Paint.new(Paint.Form.LAMP)
    replacement.name = "lamp"
    root.add_child(replacement)
    replicator.render(0.016)
    passed = passed and is_equal_approx(replacement.glow, 0.5)
    replicator.sync([beacon], true)
    return passed and root.get_parent() == null and replicator.node_for(1) == null and (
        replicator.node_for(2) != null)
