extends Node

const Actor = preload("res://actor.gd")
const SCENES = [preload("res://area0.tscn"), preload("res://area1.tscn"), preload("res://area2.tscn")]
var objects := Node3D.new()
var spawner
var count := 0
var player := 0
var online := false
var prepared := false
var running := false
var done := false
var area := 1
var switched := false
var owned_ids := PackedInt32Array()
var owned: Array[Actor] = []
var steps := 0
var last_step := 0
var start_us := 0
var end_us := 0
var wave := 0
var gaps := PackedInt32Array()

func emit(event: String, value: Variant = true) -> void:
    print("CLIENT_EVENT ", JSON.stringify({"event": event, "value": value}))

func flag(name_: String) -> bool:
    return JavaScriptBridge.eval("Boolean(window.mixedFlags?.[%s])" % JSON.stringify(name_), true)

func _ready() -> void:
    call_deferred("connect_room")

func connect_room() -> void:
    var cfg: Dictionary = JSON.parse_string(JavaScriptBridge.eval("JSON.stringify(window.fusionProbe)", true))
    player = "ABCDEFGH".find(cfg.role) + 1
    Actor.test_player = player
    count = int(cfg.count)
    for i in count:
        if Actor.owner_for(i) == player: owned_ids.append(i)
    objects.name = "Objects"
    add_child(objects)
    spawner = ClassDB.instantiate("FusionSpawner")
    spawner.set_spawn_path(^"../Objects")
    add_child(spawner)
    for scene in SCENES: spawner.add_spawnable_scene(scene)
    Fusion.set_app_id(cfg.app)
    Fusion.connected_to_photon.connect(func(): Fusion.join_or_create_room(cfg.room))
    Fusion.connection_failed.connect(func(reason): emit("failed", reason))
    Fusion.room_joined.connect(func():
        Fusion.add_user_key(1)
        if player == 1: Fusion.add_user_key(2)
        online = true
        emit("joined", Fusion.get_local_player_id()))
    Fusion.connect_to_photon("mixed-%s-%s" % [cfg.room, cfg.role], cfg.region)

func expected(index: int, key: int) -> bool:
    return player == 1 or key == 0 or index < Actor.DELEGATED or 1 + index % 2 == key

func visible(key: int) -> bool:
    var seen := {}
    for actor: Actor in objects.get_children():
        if actor.index < 0 or actor.index >= count or seen.has(actor.index): return false
        seen[actor.index] = true
    for i in count:
        if seen.has(i) != expected(i, key): return false
    return true

func prepare() -> void:
    for _i in mini(4, owned_ids.size() - owned.size()):
        var index := owned_ids[owned.size()]
        var group := 0 if index < Actor.DELEGATED else 1 + index % 2
        var actor: Actor = spawner.spawn(SCENES[group])
        if not actor:
            emit("failed", "spawn %d for role %d" % [index, player])
            done = true
            return
        actor.number = index + 1
        owned.append(actor)
    if not visible(1): return
    if not owned.all(func(actor: Actor): return actor.get_node("Sync").has_authority()): return
    prepared = true
    emit("baseline", count)

func _process(_dt: float) -> void:
    if not online or done: return
    if not prepared:
        if flag("spawn"): prepare()
        return
    if not running and flag("go"):
        running = true
    if player != 1 and not switched and flag("switch_area"):
        if area == 1:
            Fusion.remove_user_key(1)
            Fusion.add_user_key(2)
            area = 2
        if visible(2):
            switched = true
            emit("switched")
    if end_us and flag("final_area"):
        if area != 0 and player != 1:
            Fusion.add_user_key(1)
        area = 0
        if visible(0) and objects.get_children().all(func(actor: Actor): return actor.terminal()):
            done = true
            emit("final", {"checked": count * 8, "player": player, "writes": owned.size() * wave * 8,
                "step_hz": steps * 1000000.0 / (end_us - start_us), "step_gap_p95_ms": percentile(gaps) / 1000.0,
                "age_p95_ms": percentile(Actor.received_ages), "applied_samples": Actor.received_ages.size(),
                "seconds": (end_us - start_us) / 1000000.0, "errors": Actor.version_errors})

func _physics_process(_dt: float) -> void:
    if not running or end_us: return
    var now := Time.get_ticks_usec()
    if not start_us:
        start_us = now
        emit("run_started")
    if last_step: gaps.append(now - last_step)
    last_step = now
    steps += 1
    if now - start_us < (wave + 1) * 100000: return
    wave += 1
    var wall := roundi(Time.get_unix_time_from_system() * 1000.0)
    for actor in owned: actor.write(wave, wall)
    if wave == Actor.WAVES / 2: emit("halfway")
    if wave == Actor.WAVES:
        end_us = Time.get_ticks_usec()
        emit("written", owned.size() * wave)

func percentile(values: PackedInt32Array) -> float:
    if values.is_empty(): return 0.0
    values.sort()
    return values[ceili(values.size() * 0.95) - 1]
