extends Node

const Room = preload("res://addons/netcode/scene_room.gd")
const Replica = preload("res://addons/netcode/scene_replicator.gd")
const Values = preload("res://addons/netcode/scene_values.gd")
const PLAYERS := 8
const DELEGATED := 24
const WAVES := 60
const AREA := 1

class Actor extends Node3D:
    var index := 0
    var player := 0
    var errors := 0
    var last_wave := 0
    var ages := PackedInt32Array()
    var number := 0
    var scalar := 0.0
    var enabled := false
    var label := ""
    var vector := Vector3.ZERO
    var list: Array[int] = []
    var dictionary: Dictionary = {}
    var sample := 0:
        set(value):
            if value != sample and value != 0 and owner_for(index) != player:
                var wave := value % 100
                var sent := value / 100
                var now := Time.get_unix_time_from_system() * 1000.0
                if wave <= last_wave or wave > WAVES: errors += 1
                last_wave = wave
                ages.append(maxi(0, roundi(now - sent)))
            sample = value

    static func owner_for(id: int) -> int:
        return 1 + id % PLAYERS if id < DELEGATED else 1

    func write(wave: int, wall: int) -> void:
        number = (wave << 34) + index
        scalar = wave / 8.0
        enabled = (wave + index) % 2 == 0
        label = "%d:%d" % [index, wave]
        vector = Vector3(index, wave * 0.5, -wave)
        list = [index, wave, -wave]
        dictionary = {"index": index, "wave": wave}
        sample = wall * 100 + wave

    func terminal() -> bool:
        return number == (WAVES << 34) + index and scalar == WAVES / 8.0 \
            and enabled == ((WAVES + index) % 2 == 0) and label == "%d:%d" % [index, WAVES] \
            and vector == Vector3(index, WAVES * 0.5, -WAVES) and list == [index, WAVES, -WAVES] \
            and dictionary == {"index": index, "wave": WAVES} and sample % 100 == WAVES

class Predicted extends Node:
    var total := 0

var room := Room.new()
var portable := false
var actors: Array[Actor] = []
var syncs: Array[Replica] = []
var owned := PackedInt32Array()
var claimed := {}
var prediction_sync: Replica
var prediction_steps := 0
var prediction_inputs := 0
var folder := ""
var role := ""
var player := 0
var started := false
var prepared := false
var switched := false
var area := 1
var failed := false
var done := false
var start_us := 0
var write_end_us := 0
var wave := 0
var step_us := 0
var steps := 0
var gaps := PackedInt32Array()
var queue_peak := 0
var queue_age := 0
var write_queue_peak := 0
var write_queue_age := 0
var skipped_snapshots := 0
var skipped_before := 0
var advance_us := 0
var advance_calls := 0
var write_us := 0

func emit(event: String, value: Variant = true) -> void:
    print("CLIENT_EVENT ", JSON.stringify({"event": event, "value": value}))

func check(condition: bool, message: String) -> bool:
    if not condition and not failed:
        failed = true
        emit("failed", message)
    return condition

func flag(name_: String) -> bool:
    return FileAccess.file_exists(folder.path_join(name_))

func area_of(index: int) -> int:
    return 1 + (index % 2)

func attach(actor: Node, layout: String, paths: Array[NodePath], delegated: bool) -> Replica:
    var sync := Replica.new()
    sync.root_mode = Replica.RootMode.NONE
    sync.layout_key = layout
    sync.properties = paths
    if delegated: sync.client_writable = paths
    actor.add_child(sync)
    return sync

func _ready() -> void:
    var args := OS.get_cmdline_user_args()
    role = args[0]
    folder = args[3]
    portable = args[4] == "portable"
    for i in int(args[1]):
        var actor := Actor.new()
        actor.name = "Actor%d" % i
        actor.index = i
        if portable: actor.number = i + 1
        add_child(actor)
        actors.append(actor)
        syncs.append(attach(actor, "mixed-load",
            [^":number", ^":scalar", ^":enabled", ^":label", ^":vector", ^":list", ^":dictionary", ^":sample"], true))
    if not portable:
        var prediction := Predicted.new()
        prediction.name = "Prediction"
        add_child(prediction)
        prediction_sync = attach(prediction, "mixed-prediction", [^":total"], false)
        prediction_sync.set_physics_process(false)
    var port := int(args[2]) if role == "A" else int(FileAccess.get_file_as_string(folder.path_join("relay_port")))
    var error := room.open_with_provider(self, role == "A", "127.0.0.1:%d" % port, PLAYERS)
    if not check(error.is_empty(), error): return
    if not portable and not check(room.game.register_prediction(prediction_sync.schema_id, 1, PackedByteArray([0]), predict_step),
            "register prediction"): return
    emit("opened")

func predict_step(state: Array, input: PackedByteArray, _tick: int) -> Array:
    prediction_steps += 1
    return [Values.encode_integer(state[0].decode_s64(0) + input[0])]

func configure_areas(first: int, second := 0) -> bool:
    var keys := PackedInt64Array([first])
    var rates := PackedInt32Array([1])
    if second:
        keys.append(second)
        rates.append(1)
    for guest in range(2, PLAYERS + 1):
        if not room.game.subscribe(guest, AREA, keys, rates): return false
    return true

func spawn_objects() -> bool:
    for i in syncs.size():
        var id := room.spawn(syncs[i])
        if not id: return false
        if i >= DELEGATED and not room.game.distribute(id, AREA, PackedInt64Array([area_of(i)]), 1): return false
    if not portable:
        var predicted := room.spawn(prediction_sync)
        if not predicted or not room.game.bind_prediction(predicted, 2): return false
    return configure_areas(1)

func check_visibility(current: int) -> bool:
    for i in syncs.size():
        var expected := player == 1 or current == 0 or i < DELEGATED or area_of(i) == current
        if syncs[i].initialized != expected: return false
    return true

func prepare() -> void:
    if not check_visibility(1): return
    if not portable and player == 2 and room.game.predicted_fields(prediction_sync.object_id).is_empty(): return
    for i in owned:
        var sync := syncs[i]
        if sync.is_write_ready(): continue
        if player != 1 and not claimed.has(i) and room.game.claim(sync.object_id): claimed[i] = true
        # Try all of this player's claims on the same frame.
    for i in owned:
        if not syncs[i].is_write_ready(): return
    prepared = true
    emit("baseline", player)

func _process(_dt: float) -> void:
    if failed: return
    var measure := start_us > 0 and write_end_us == 0
    var before := Time.get_ticks_usec() if measure else 0
    var frame := room.advance()
    if measure:
        advance_us += Time.get_ticks_usec() - before
        advance_calls += 1
    if done or frame.is_empty(): return
    if player == 0 and frame.player:
        player = frame.player
        for actor in actors:
            actor.player = player
            if Actor.owner_for(actor.index) == player: owned.append(actor.index)
    for event in frame.events:
        if event.kind == 5 and player == 1: check(room.game.answer(event, true), "approve object writer")
    if player == 1 and not started and frame.seats == PLAYERS and not frame.round and room.game.start():
        started = check(spawn_objects(), "spawn, dispatch or bind prediction")
    if not frame.round: return
    if not prepared:
        prepare()
        return
    if player == 1:
        if area == 1 and flag("switch_area") and check(configure_areas(2), "switch visibility"): area = 2
        if area == 2 and flag("final_area") and check(configure_areas(1, 2), "restore visibility"): area = 0
    elif not switched and flag("switch_area") and check_visibility(2):
        switched = true
        emit("switched")
    var queues := room.game.transport_stats()
    queue_peak = maxi(queue_peak, queues.provider_peak_queued_bytes + queues.core_peak_queued_bytes)
    queue_age = maxi(queue_age, queues.oldest_queued_us)
    if measure:
        write_queue_peak = maxi(write_queue_peak, queues.queued_bytes)
        write_queue_age = maxi(write_queue_age, queues.oldest_queued_us)
        skipped_snapshots = int(queues.get("snapshot_backpressure_skips", 0)) - skipped_before
    elif not start_us:
        skipped_before = int(queues.get("snapshot_backpressure_skips", 0))
    if write_end_us and flag("final_area"):
        if Time.get_ticks_usec() - write_end_us > 12000000:
            check(false, "terminal values or visibility timeout")
        elif check_visibility(0): verify_terminal()

func _physics_process(_dt: float) -> void:
    if not prepared or failed or done or write_end_us or not flag("go"): return
    var now := Time.get_ticks_usec()
    if not start_us:
        start_us = now
        emit("run_started")
    if step_us: gaps.append(now - step_us)
    step_us = now
    steps += 1
    if now - start_us < (wave + 1) * 100000: return
    wave += 1
    var wall := roundi(Time.get_unix_time_from_system() * 1000.0)
    for i in owned: actors[i].write(wave, wall)
    if wave == WAVES / 2: emit("halfway")
    if not portable and player == 2:
        if not check(room.game.predict(prediction_sync.object_id, PackedByteArray([1])), "advance prediction"): return
        prediction_inputs += 1
    write_us += Time.get_ticks_usec() - now
    if wave == WAVES:
        write_end_us = Time.get_ticks_usec()
        emit("written", owned.size() * wave)

func percentile(values: PackedInt32Array) -> float:
    if values.is_empty(): return 0.0
    values.sort()
    return values[ceili(values.size() * 0.95) - 1]

func verify_terminal() -> void:
    if not portable:
        var state := room.game.read_fields(prediction_sync.object_id, false)
        if state.is_empty() or state[0].decode_s64(0) != WAVES: return
    var ages := PackedInt32Array()
    for actor in actors:
        if not check(actor.errors == 0, "field version replayed or invalid"): return
        if not actor.terminal(): return
        ages.append_array(actor.ages)
    done = true
    emit("final", {"checked": actors.size() * 8, "player": player, "writes": owned.size() * wave * 8,
        "step_hz": steps * 1000000.0 / (write_end_us - start_us), "step_gap_p95_ms": percentile(gaps) / 1000.0,
        "age_p95_ms": percentile(ages), "applied_samples": ages.size(), "queue_peak_bytes": queue_peak,
        "sampled_queue_age_us": queue_age,
        "write_queue_peak_bytes": write_queue_peak, "write_queue_age_us": write_queue_age,
        "snapshot_backpressure_skips": skipped_snapshots,
        "advance_calls": advance_calls, "advance_ms": advance_us / 1000.0, "write_ms": write_us / 1000.0,
        "prediction_replays": prediction_steps - prediction_inputs if player == 2 else 0,
        "prediction_total": 0 if portable else WAVES})
