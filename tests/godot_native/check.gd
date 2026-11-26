extends SceneTree

const Orb = preload("res://orb.tscn")
const Other = preload("res://other.tscn")
const Third = preload("res://third.tscn")
const Spawner = preload("res://spawner.gd")
const RosterReceiver = preload("res://rpc_receiver.gd")
var bad := false
var joined_count := 0
var left_count := 0
var failure := ""
var role := ""
var traces: Dictionary = {}
var primary: NetcodeSpawner
var secondary: NetcodeSpawner
var _joined_players := PackedInt64Array()
var _left_players := PackedInt64Array()
var _roster_signal_sizes := PackedInt32Array()
var _roster_rpc: NetcodeRpcResult
var _joining_actors: Array[Node] = []

func _initialize() -> void:
    _run.call_deferred()

func check(ok: bool, label: String) -> void:
    if not ok:
        bad = true
        printerr("FAIL ", label)

func wait_for(predicate: Callable, label: String) -> bool:
    var until := Time.get_ticks_msec() + 15000
    while not predicate.call():
        if Time.get_ticks_msec() >= until:
            check(false, label)
            return false
        await process_frame
    return true

func _run() -> void:
    var args := OS.get_cmdline_user_args()
    role = args[0]
    if role == "nodes":
        _nodes()
        _finish(role)
        return
    var port := int(args[1])
    Netcode.room_joined.connect(func(): joined_count += 1)
    Netcode.room_left.connect(func(): left_count += 1)
    Netcode.connection_failed.connect(func(reason: String): failure = reason)
    Netcode.player_joined.connect(func(player: int, user: String):
        check(user.is_empty(), "TCP join carries numeric identity")
        _joined_players.push_back(player))
    Netcode.player_left.connect(func(player: int, inactive: bool):
        check(not inactive, "native player departure has no reserved seat")
        _left_players.push_back(player))
    if role == "host":
        await _host(port, args[2])
    elif role in ["guest", "late"]:
        await _join(port, role == "late")
    elif role == "occupied":
        check(Netcode.create_room("127.0.0.1", port) == ERR_CANT_CREATE, "occupied listen port")
        check(Netcode.get_room() == null, "no occupied room view")
    elif role == "unreachable":
        check(Netcode.join_room("127.0.0.1", port) == OK, "connection attempt accepted")
        await wait_for(func(): return not failure.is_empty(), "connection_failed")
        check(Netcode.get_room() == null and joined_count == 0, "unreachable room remains closed")
        print("UNREACHABLE ", failure)
    elif role in ["incompatible", "wrong_catalog"]:
        check(Netcode.join_room("127.0.0.1", port) == OK, "incompatible endpoint connected")
        await wait_for(func(): return not failure.is_empty(), "native handshake rejected")
        check("native session v5" in failure and joined_count == 0, "clear incompatible-peer error")
        print("VERSION_MISMATCH ", failure)
    elif role.begins_with("roster_"):
        await _roster_churn(port, args[2])
    _finish(role)

func _roster_churn(port: int, folder: String) -> void:
    var receiver := RosterReceiver.new()
    root.add_child(receiver)
    if role == "roster_host":
        Netcode.player_joined.connect(func(player: int, _user: String):
            if player == 3: _roster_rpc = Netcode.rpc_to_player(player, receiver.room_message, "joined"))
        check(Netcode.create_room("127.0.0.1", port) == OK, "roster room starts")
        if not await wait_for(Netcode.is_in_room, "roster host admitted"): return
        _touch(folder.path_join("roster.host"))
        if not await wait_for(func(): return _roster_rpc != null and _roster_rpc.is_finished(),
            "targeted room RPC from player_joined finishes"): return
        check(_roster_rpc.get_error().is_empty(), "direct room RPC accepts a newly negotiated player")
        _touch(folder.path_join("visitor.rpc"))
        if not await wait_for(func(): return 3 in _left_players, "host observed visitor leave"): return
        _touch(folder.path_join("visitor.left"))
        await wait_for(func(): return FileAccess.file_exists(folder.path_join("observer.done")), "observer finished")
        Netcode.leave_room()
        return
    check(Netcode.join_room("127.0.0.1", port) == OK, "join roster room")
    if not await wait_for(Netcode.is_in_room, "roster guest admitted"): return
    if role == "roster_visitor":
        check(Netcode.get_local_player_id() == 3, "visitor has third seat")
        if not await wait_for(func(): return receiver.messages.size() == 1, "new visitor receives direct room RPC"): return
        check(receiver.messages[0].slice(0, 3) == ["joined", 1, 3], "join-time RPC context")
        await wait_for(func(): return FileAccess.file_exists(folder.path_join("visitor.rpc")), "room RPC receipt reached host")
        Netcode.leave_room()
        return
    Netcode.player_joined.connect(func(_player: int, _user: String): _roster_signal_sizes.push_back(Netcode.get_room().player_count))
    Netcode.player_left.connect(func(_player: int, _inactive: bool): _roster_signal_sizes.push_back(Netcode.get_room().player_count))
    _touch(folder.path_join("observer.paused"))
    var until := Time.get_ticks_msec() + 7000
    while not FileAccess.file_exists(folder.path_join("visitor.left")) and Time.get_ticks_msec() < until:
        OS.delay_msec(10)
    await process_frame
    await process_frame
    check(_joined_players == PackedInt64Array([3]) and _left_players == PackedInt64Array([3]),
        "paused observer receives both roster events")
    check(_roster_signal_sizes == PackedInt32Array([3, 2]), "each signal observes its corresponding roster")
    _touch(folder.path_join("observer.done"))
    Netcode.leave_room()

func _nodes() -> void:
    check(not ClassDB.can_instantiate("NetcodeReplicator"), "abstract replicator")
    check(ClassDB.can_instantiate("NetcodeSharedReplicator"), "native Shared")
    check(ClassDB.can_instantiate("NetcodeSpawner"), "native Spawner")
    var pool := Node.new()
    root.add_child(pool)
    var spawner := Spawner.new()
    pool.add_child(spawner)
    check(spawner.game_script_marker() == 731 and spawner.spawn_path == ^"..", "Spawner script and default")
    check(spawner.add_spawnable_scene(Orb) and spawner.add_spawnable_scene(Orb), "register template")
    check(spawner.spawnable_scenes.size() == 1, "catalog idempotence")
    var first := Orb.instantiate()
    var second := Orb.instantiate()
    pool.add_child(first)
    pool.add_child(second)
    var sync := first.get_node("Sync") as NetcodeSharedReplicator
    check(sync.call("game_script_marker") == 731, "Shared script")
    check(sync.replication_root() == first and sync.root_path == ^"..", "resolve replication root")
    sync.root_path = ^"../.."
    check(sync.replication_root() == pool, "resolve custom relative replication root")
    sync.root_path = ^"..:name"
    check(sync.replication_root() == null, "subproperty cannot be a replication root")
    sync.root_path = ^".."
    check(sync.replication_root() == first, "restore parent replication root")
    var defaults := NetcodeSharedReplicator.new()
    check(defaults.owner_mode == 0 and defaults.root_replication_mode == 0 and not defaults.interest_destroy_on_exit, "Shared defaults")
    defaults.free()
    check(sync.replication_config == second.get_node("Sync").replication_config, "two templates share config")
    check(sync.replication_config.properties == [^":value"], "serialized field path")
    var config := sync.replication_config
    config.smooth_properties = [^":position"]
    check(ResourceSaver.save(config, "user://reopened.tres") == OK, "save shared resource")
    var reopened := ResourceLoader.load("user://reopened.tres", "", ResourceLoader.CACHE_MODE_IGNORE) as NetcodeReplicationConfig
    check(reopened != null and reopened.properties == config.properties and reopened.smooth_properties == config.smooth_properties,
        "reload shared resource")
    var exporter := NetcodeLayoutExporter.new()
    exporter.fields[1] = ^":value"
    check(exporter.build_text(first) == "schema 1 1 Actor\nint 1 . value\n", "native layout exporter emits the legacy field format")
    exporter.free()
    var body := RigidBody3D.new()
    pool.add_child(body)
    var forecast := NetcodeForecastBody3D.new()
    var fields := forecast.sample(body, 1, 2)
    body.position = Vector3(5, 0, 0)
    forecast.apply(body, fields, 0.0)
    check(body.position == Vector3.ZERO and forecast.generation == 1, "native forecast snaps on generation change")
    var island := NetcodePhysicsIsland3D.new()
    check(island.add(1, body) and not island.add(2, body), "native island registers a body once")
    var captured := island.capture(7)
    body.position = Vector3(7, 0, 0)
    check(island.restore(captured) and body.position == Vector3.ZERO, "native island restores the captured transform")
    pool.free()

func _host(port: int, folder: String) -> void:
    _setup_spawners()
    check(Netcode.create_room("127.0.0.1", 65536) == ERR_INVALID_PARAMETER, "invalid port")
    check(Netcode.create_room("not-an-ip", port) == ERR_INVALID_PARAMETER, "invalid address")
    var options := NetcodeRoomOptions.new()
    options.max_players = 3
    options.custom_properties = {"lab-progress": 0, "lab-size": 128, "discard": true}
    check(Netcode.create_room("127.0.0.1", port, options) == OK, "creating TCP room")
    if not await wait_for(func(): return joined_count == 1, "host room_joined"): return
    var room: NetcodeRoomView = Netcode.get_room()
    check(room.get_player_count() == 1, "initial host seat")
    check(Netcode.is_in_room() and Netcode.is_master_client() and room.is_master_client, "master room view")
    check(room.players[0].number == room.master_client_id and room.players[0].is_master_client, "initial master player record")
    check(room.get_custom_properties() == options.custom_properties, "host initial properties")
    check(not room.set_properties_cas({"invalid": true}, {"missing": 0}), "host CAS rejects missing expectation")
    check(not room.set_property("invalid", [root]), "room property rejects nested engine objects")
    var node_list: Array[Node] = []
    check(not room.set_property("invalid", node_list), "room property rejects object-typed containers even when empty")
    var bom := String.chr(0xFEFF)
    check(not room.set_properties({"visible": 1, bom + "visible": 2}), "room key must survive UTF-8 round trip")
    check(not room.set_property("invalid", bom + "text"), "host rejects values that cannot survive decoding")
    check(room.set_properties({"nested": {"a": 1, "b": [2]}, "nullable": null}), "portable containers and null are stored")
    check(room.set_properties_cas({"nested_ok": true}, {"nested": {"b": [2], "a": 1}, "nullable": null}),
        "CAS compares dictionary contents independent of key order and matches stored null")
    check(room.remove_properties(PackedStringArray(["nullable"])) and not room.custom_properties.has("nullable") and
        not room.set_properties_cas({"invalid": true}, {"nullable": null}), "deletion and a stored null remain distinct")
    var processor_count := 0
    for child in root.get_children():
        if child is NetcodeClientProcessor: processor_count += 1
    check(processor_count == 1, "one native processor")
    _touch(folder.path_join("host.ready"))
    if not await wait_for(func(): return traces.size() == 3 and traces.values().all(
        func(events: Array): return events.size() >= 5), "three remote templates installed"): return
    check(room.get_player_count() == 2, "host counts negotiated players only")
    check(_joined_players.size() == 1 and _joined_players[0] == room.players[1].number, "only negotiated join emitted")
    for template: String in ["orb", "other", "third"]:
        var events: Array = traces[template]
        var expected := int({"orb": 741, "other": 742, "third": 733}[template])
        check(events.slice(0, 5) == [["enter_tree", -1, false], ["ready", -1, false],
            ["setter", expected, false], ["replicator_spawned", expected, false],
            ["spawner_spawned", expected, false]], "host first field for " + template)
        print("NATIVE trace host ", template, " ", events)
    if not await wait_for(func(): return ["setter", 751, false] in traces.orb, "host receives later guest write"): return
    if not await wait_for(func(): return room.get_custom_properties().get("lab-progress") == 76, "host observes guest patch"): return
    if not await wait_for(func(): return not room.custom_properties.has("discard"), "host commits guest deletion"): return
    check(not room.custom_properties.has("should-not-appear"), "stale CAS rejects the whole patch")
    check(room.set_property("lab-ready", true), "host commits ready")
    _touch(folder.path_join("host.updated"))
    await wait_for(func(): return FileAccess.file_exists(folder.path_join("leave")), "host leave marker")
    await wait_for(func(): return _left_players.size() == 2, "normal and late departures emitted")
    check(_joined_players == _left_players and room.players.size() == 1, "departure roster matches successful joins")
    Netcode.leave_room()
    check(left_count == 1 and Netcode.get_room() == null and not room.set_property("stale", 1), "close invalidates view")
    check(Netcode.create_room("127.0.0.1", port) == OK, "reopen same port")
    await wait_for(func(): return joined_count == 2, "room_joined on reopen")
    Netcode.leave_room()
    check(left_count == 2, "room_left on reopen")

func _join(port: int, late: bool) -> void:
    _setup_spawners()
    if not late: Netcode.room_joined.connect(_spawn_joining_guest, CONNECT_ONE_SHOT)
    check(Netcode.join_room("127.0.0.1", port) == OK, "join request")
    if not await wait_for(func(): return joined_count == 1, "guest room_joined"): return
    var room: NetcodeRoomView = Netcode.get_room()
    if not await wait_for(func(): return room.get_player_count() == 2, "guest receives negotiated player count"): return
    check(Netcode.is_in_room() and not Netcode.is_master_client() and not room.is_master_client, "guest room view")
    check(room.players[0].number == room.master_client_id and room.players[0].is_master_client and
        room.players[1].number == Netcode.get_local_player_id(), "guest roster reflects native identities")
    check(room.get_custom_properties().get("lab-size") == 128, "guest initial baseline")
    if not late:
        if not await wait_for(func(): return bad or _joining_actors.all(
            func(actor): return is_instance_valid(actor) and actor.get_node("Sync").is_write_ready()),
            "three guest objects obtain accepted authority"): return
        if bad: return
        _joining_actors[0].value = 751
        check(room.set_properties_cas({"lab-progress": 76, "lab-area": 1}, {"lab-progress": 0}), "guest submits CAS")
        check(room.set_properties_cas({"lab-progress": 901, "should-not-appear": true}, {"lab-progress": 0}),
            "stale CAS submitted for coordinator validation")
        check(room.remove_properties(PackedStringArray(["discard"])), "guest requests deletion")
        await wait_for(func(): return room.get_custom_properties().get("lab-ready") == true, "guest sees commit")
    check(room.get_custom_properties().get("lab-progress") == 76, "guest committed progress")
    check(room.get_custom_properties().get("lab-area") == 1, "guest committed area")
    check(room.get_custom_properties().get("lab-ready") == true, "guest committed ready")
    check(not room.custom_properties.has("discard") and not room.custom_properties.has("should-not-appear"),
        "successful and late views share the committed property set")
    Netcode.leave_room()
    check(left_count == 1, "guest room_left")

func _spawn_joining_guest() -> void:
    check(not Netcode.is_simulation_ready(), "room_joined precedes the simulation clock")
    var canceled_in_ready := primary.spawn(null, func(_sync: NetcodeReplicator, actor: Node):
        actor.set_meta("cancel_in_ready", primary))
    check(canceled_in_ready == null, "cancellation from _ready prevents later spawn signals")
    check(not traces.orb.any(func(event: Array): return event[0].ends_with("spawned")),
        "cancellation from _ready suppresses both spawn signals")
    var canceled := primary.spawn(null, _init_spawn.bind(999))
    check(is_instance_valid(canceled) and primary.despawn(canceled) and canceled.is_queued_for_deletion(),
        "cancel a pre-handshake spawn before sending")
    traces.clear()
    _joining_actors.assign([primary.spawn(null, _init_spawn.bind(731)), primary.spawn(Other),
        secondary.spawn(Third, _init_spawn.bind(733))])
    for i in _joining_actors.size():
        var actor := _joining_actors[i]
        if not is_instance_valid(actor):
            check(false, "synchronous spawn return inside room_joined")
            return
        var sync: NetcodeSharedReplicator = actor.get_node("Sync")
        check(not sync.is_write_ready(), "room_joined instance has no confirmed writer baseline")
        record_order(actor, "spawn_return", actor.value, sync.has_authority())
        var events: Array = traces[["orb", "other", "third"][i]]
        var expected := -1 if i == 1 else 731 + i
        var opening := [] if i == 1 else [["pre_spawn", -1, true], ["setter", expected, true]]
        check(events == opening + [["enter_tree", expected, true], ["ready", expected, true],
            ["replicator_spawned", expected, true], ["spawner_spawned", expected, true],
            ["spawn_return", expected, true]], "guest spawn order " + actor.name)
        print("NATIVE trace joining guest ", actor.name, " ", events)
    _joining_actors[0].value = 741
    _joining_actors[1].value = 742

func _setup_spawners() -> void:
    var pool := Node.new()
    root.add_child(pool)
    primary = NetcodeSpawner.new()
    secondary = NetcodeSpawner.new()
    for spawner in [primary, secondary]:
        pool.add_child(spawner)
        spawner.spawned.connect(func(actor: Node):
            record_order(actor, "spawner_spawned", actor.value, actor.get_node("Sync").has_authority()))
        spawner.spawn_failed.connect(func(_actor: Node, reason: String): check(false, reason))
    primary.add_spawnable_scene(Orb)
    primary.add_spawnable_scene(Other)
    secondary.add_spawnable_scene(Third)

func _init_spawn(sync: NetcodeSharedReplicator, actor: Node, value: int) -> void:
    record_order(actor, "pre_spawn", actor.value, sync.has_authority())
    actor.value = value

func record_order(node: Node, event: String, value: int, writes: bool) -> void:
    if role not in ["host", "guest"]: return
    var key := node.scene_file_path.get_file().get_basename()
    if not traces.has(key): traces[key] = []
    traces[key].append([event, value, writes])

func _touch(path: String) -> void:
    var handle := FileAccess.open(path, FileAccess.WRITE)
    if handle: handle.store_string("ready")

func _finish(role: String) -> void:
    print("NATIVE ", role, " ", "FAIL" if bad else "PASS", " ", NetcodeGame.sdk_build_id())
    quit(int(bad))
