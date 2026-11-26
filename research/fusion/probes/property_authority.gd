extends Node

const Room = preload("res://addons/netcode/scene_room.gd")
const Replica = preload("res://addons/netcode/scene_replicator.gd")
const Values = preload("res://addons/netcode/scene_values.gd")
const DYNAMIC := 1
const WRITE_REQUEST := 5
const WRITE_RESULT := 6
const PREDICTION_FAILED := 7

class Actor extends Node3D:
    var value := 0

var room := Room.new()
var leased := Actor.new()
var racing := Actor.new()
var lease: Replica
var race: Replica
var folder := ""
var role := ""
var player := 0
var started := false
var baseline_ready := false
var frozen := false
var failed := false
var rejected := false
var got_result := false
var stale_seen := false

func emit(event: String, value: Variant = true) -> void:
    print("CLIENT_EVENT ", JSON.stringify({"event": event, "value": value}))

func check(condition: bool, message: String) -> bool:
    if not condition and not failed:
        failed = true
        emit("failed", message)
    return condition

func flag(name_: String) -> bool:
    return FileAccess.file_exists(folder.path_join(name_))

func until(predicate: Callable, message: String) -> bool:
    var deadline := Time.get_ticks_msec() + 12000
    while not failed and Time.get_ticks_msec() < deadline:
        if predicate.call(): return true
        await get_tree().process_frame
    return check(false, message)

func attach(actor: Actor, name_: String) -> Replica:
    actor.name = name_
    actor.value = 10
    add_child(actor)
    var sync := Replica.new()
    sync.root_mode = Replica.RootMode.NONE
    sync.layout_key = "authority-test"
    sync.properties = [^":value"]
    sync.client_writable = [^":value"]
    actor.add_child(sync)
    return sync

func _ready() -> void:
    var args := OS.get_cmdline_user_args()
    role = args[0]
    folder = args[3]
    lease = attach(leased, "Leased")
    race = attach(racing, "Race")
    var port := int(args[2]) if role == "A" else int(FileAccess.get_file_as_string(folder.path_join("relay_port")))
    var error := room.open_with_provider(self, role == "A", "127.0.0.1:%d" % port, 3)
    if check(error.is_empty(), error): emit("opened")

func _process(_dt: float) -> void:
    if failed: return
    if role == "A" and flag("freeze") and not flag("unfreeze"):
        if not frozen:
            frozen = true
            emit("frozen")
        return
    var frame := room.advance()
    if frame.is_empty(): return
    player = frame.player
    for event in frame.events:
        if event.kind == WRITE_REQUEST and role == "A": check(room.game.answer(event, true), "claim response")
        if event.kind == WRITE_RESULT and event.object == race.object_id: got_result = true
        if event.kind == PREDICTION_FAILED and event.object == race.object_id: rejected = true
    if role == "A" and not started and frame.seats == 3 and not frame.round and room.game.start():
        started = check(room.spawn(lease) > 0 and room.spawn(race, DYNAMIC) > 0, "spawn")
    if role != "B" and leased.value == 777: stale_seen = true
    if not baseline_ready and lease.initialized and race.initialized:
        baseline_ready = check(leased.value == 10 and racing.value == 10, "initial values")
        if baseline_ready:
            emit("baseline")
            scenario()

func epoch() -> int:
    return room.game.routing_info(lease.object_id, false).epoch

func scenario() -> void:
    if role == "A":
        if not await until(func(): return leased.value == 42, "first guest commit"): return
        emit("committed", leased.value)
        if not await until(func(): return flag("reclaim_when_held") and flag("state_held"), "hold old write"): return
        if not check(room.game.reclaim(lease.object_id), "reclaim"): return
        if not await until(lease.is_write_ready, "reclaim baseline"): return
        var revision := room.game.revision(lease.object_id)
        emit("reclaimed", {"epoch": epoch(), "value": leased.value})
        if not await until(func(): return flag("check_stale") and flag("state_forwarded"), "forward old write"): return
        await get_tree().create_timer(0.15).timeout
        if not check(leased.value == 42 and not stale_seen and room.game.revision(lease.object_id) == revision,
                "old epoch changed host state"): return
        leased.value = 43
        emit("stale_rejected", leased.value)
    elif role == "B":
        if not await until(func(): return flag("claim") and room.game.claim(lease.object_id), "claim"): return
        if not await until(lease.is_write_ready, "guest baseline"): return
        var old := epoch()
        leased.value = 42
        emit("writer_ready", {"epoch": old})
        if not await until(func(): return flag("hold_state"), "second write command"): return
        leased.value = 777
        emit("second_write")
        if not await until(func(): return epoch() > old and not lease.has_authority(), "guest sees reclaim"): return
        emit("reclaim_seen", epoch())
    else:
        if not await until(func(): return leased.value == 43, "observer sees new writer"): return
        if not check(not stale_seen, "observer saw old epoch value"): return
        emit("stale_rejected", leased.value)
    if not await until(func(): return flag("race"), "race command"): return
    if role != "A":
        var predicted := 71 if role == "B" else 72
        if not check(room.game.claim(race.object_id) and room.game.writer(race.object_id) == player,
                "optimistic writer"): return
        if not check(room.game.submit(race.object_id, [Values.encode_integer(predicted)])
                and room.game.read_fields(race.object_id, true)[0].decode_s64(0) == predicted,
                "optimistic presentation"): return
        emit("predicted", {"value": predicted})
        if not await until(func(): return got_result, "race claim result"): return
    if not await until(func():
            var writer: int = room.game.routing_info(race.object_id, false).writer
            var public_value: int = room.game.read_fields(race.object_id, false)[0].decode_s64(0)
            return writer != 1 and public_value in [71, 72] and racing.value == public_value,
            "prediction rolled back to the winning commit"): return
    var winner: int = room.game.routing_info(race.object_id, false).writer
    if not check(role == "A" or rejected == (winner != player), "prediction failure event"): return
    emit("race_done", {"value": racing.value, "rejected": rejected, "writer": winner})
