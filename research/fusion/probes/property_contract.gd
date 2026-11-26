extends Node

const Room = preload("res://addons/netcode/scene_room.gd")
const Replica = preload("res://addons/netcode/scene_replicator.gd")
const LOW := -9223372036854775807 - 1
const HIGH := 9223372036854775807

class Integers extends Node3D:
    var calls: Array[String] = []
    var interfere_on_correction := false
    var corrections_with_side_effect := 0
    var a: int = 0:
        set(value):
            calls.append("a")
            a = value
    var b: int = 0:
        set(value):
            calls.append("b")
            if interfere_on_correction and b == 99 and value == -1:
                corrections_with_side_effect += 1
                a = 123
            b = value

class Stats extends Resource:
    var points: int = 0

class Mixed extends Node3D:
    var stats := Stats.new()
    var table: Dictionary = {}
    var enabled := false
    var meter := 0.0
    var focus: Node

class Part extends Node:
    var value: int = 0

class Flat extends Node:
    var pulse := 0
    var list: Array[int] = []
    var table: Dictionary[String, int] = {}
    var loose: Dictionary = {}

var room := Room.new()
var host := false
var folder := ""
var integers := Integers.new()
var mixed := Mixed.new()
var fast: Replica
var generic: Replica
var flat := Flat.new()
var direct: Replica
var part := Part.new()
var detached: Part
var spawned := false
var baseline_ready := false
var failed := false

func report(event: String, value: Variant = true) -> void:
    print("CLIENT_EVENT ", JSON.stringify({"event": event, "value": value}))

func require(ok: bool, message: String) -> bool:
    if not ok and not failed:
        failed = true
        report("failed", message)
    return ok

func attach(actor: Node, layout: String, properties: Array[NodePath]) -> Replica:
    var sync := Replica.new()
    sync.root_mode = Replica.RootMode.NONE
    sync.layout_key = layout
    sync.properties = properties
    actor.add_child(sync)
    return sync

func _ready() -> void:
    var args := OS.get_cmdline_user_args()
    host = args[0] == "A"
    folder = args[3]
    integers.name = "Integers"
    mixed.name = "Mixed"
    flat.name = "Flat"
    part.name = "Part"
    add_child(integers)
    add_child(mixed)
    add_child(flat)
    mixed.add_child(part)
    fast = attach(integers, "integers", [^":b", ^":a"])
    fast.client_writable = [^":a"]
    generic = attach(mixed, "mixed", [^":stats:points", ^":table", ^":enabled", ^":meter", ^":focus", ^"Part:value"])
    generic.smooth_properties = [^":meter"]
    generic.smoothing_seconds = 0.05
    direct = attach(flat, "flat", [^":pulse", ^":list", ^":table", ^":loose"])
    var error := room.open(self, host, "127.0.0.1", int(args[2]), 2)
    if require(error.is_empty(), error): report("opened")

func _process(_dt: float) -> void:
    if failed: return
    var frame := room.advance()
    if frame.is_empty(): return
    for event in frame.events:
        if event.kind == 5 and host: require(room.game.answer(event, true), "approve claim")
    if host and not spawned and frame.seats == 2 and not frame.round and room.game.start():
        spawned = require(room.spawn(fast) != 0 and room.spawn(generic) != 0 and room.spawn(direct) != 0, "spawn scene objects")
    if not baseline_ready and fast.initialized and generic.initialized and direct.initialized:
        baseline_ready = require(integers.calls == ["b", "a"], "full baseline in schema order")
        if baseline_ready:
            report("baseline")
            if host: writer_sequence()
            else: observer_sequence()

func until(test: Callable, message: String) -> bool:
    var deadline := Time.get_ticks_msec() + 8000
    while not failed and Time.get_ticks_msec() < deadline:
        if test.call(): return true
        await get_tree().process_frame
    return require(false, message)

func command(op: String) -> bool:
    return await until(func(): return FileAccess.file_exists(folder.path_join(op)), op)

func writer_sequence() -> void:
    if not await command("seed"): return
    integers.a = LOW
    integers.b = HIGH
    var bytes := fast.sample_fields()
    var wire := [bytes[1].hex_encode(), bytes[0].hex_encode()]
    if not require(wire == ["0000000000000080", "ffffffffffffff7f"],
            "signed 64-bit wire bytes"): return
    var edited: PackedByteArray = bytes[1]
    edited.encode_s64(0, 321)
    if not require(fast.sample_fields()[1].decode_s64(0) == LOW and integers.a == LOW, "sampling buffer aliases the SDK cache"): return
    mixed.stats.points = -4294967296
    mixed.table = {"items": [-2, 0, 4]}
    mixed.enabled = true
    mixed.meter = 2.5
    mixed.focus = integers
    part.value = 17
    flat.pulse = 1
    flat.list.assign([LOW, HIGH])
    flat.table.assign({"键": 1, "b": 2})
    flat.loose["phase"] = 1
    if not require(direct._sample_for_sync() == direct.sample_fields(), "root collection wire encoding"): return
    report("seed", wire)
    if not await command("collection_edit"): return
    flat.list[0] = 9
    flat.table.erase("键")
    flat.table["键"] = HIGH
    flat.loose["phase"] = 1.0
    flat.loose["nested"] = [true, -7]
    var snapshot := direct.sample_fields()
    if not require(direct._sample_for_sync() == snapshot, "collection wire encoding after in-place edits"): return
    snapshot[1].fill(0)
    snapshot[2].fill(0)
    report("collection_edit")
    if not await command("sibling"): return
    if not require(integers.a == LOW, "proxy edit reached writer"): return
    integers.b = -1
    report("sibling")
    if not await command("target"): return
    integers.a = LOW + (1 << 32)
    report("target")
    if not await command("equal_value"): return
    integers.a = LOW + (1 << 32)
    report("equal_value")
    if not await command("update_detached"): return
    part.value = 19
    await get_tree().create_timer(0.15).timeout
    part.value = 18
    report("update_detached")
    if not await until(func(): return integers.a == 42, "guest write"): return
    if not require(integers.b == -1, "guest changed masked field"): return
    report("partial_write")
    if not await command("reclaim"): return
    var epoch: int = room.game.routing_info(fast.object_id, false).epoch
    if not require(room.game.reclaim(fast.object_id), "host reclaim"): return
    if not await until(fast.is_write_ready, "host reclaim baseline"): return
    if not require(room.game.routing_info(fast.object_id, false).epoch == epoch + 1 and integers.a == 42,
            "host reclaim did not preserve the last committed value"): return
    report("reclaim")
    if not await command("reclaimed"): return
    if not await until(fast.is_write_ready, "host handoff baseline"): return
    integers.a = 43
    report("reclaimed")

func observer_sequence() -> void:
    if not await until(func():
        return integers.a == LOW and integers.b == HIGH and mixed.stats.points == -4294967296 \
            and mixed.table == {"items": [-2, 0, 4]} and mixed.enabled and absf(mixed.meter - 2.5) < 0.001 \
            and mixed.focus == integers and part.value == 17 and flat.pulse == 1 and flat.list == [LOW, HIGH] \
            and flat.table.keys() == ["键", "b"] and typeof(flat.loose.get("phase")) == TYPE_INT,
            "mixed fields and integer extremes"): return
    report("seed")
    if not await until(func():
        return flat.list == [9, HIGH] and flat.table.keys() == ["b", "键"] and flat.table["键"] == HIGH \
            and typeof(flat.loose.get("phase")) == TYPE_FLOAT and flat.loose.get("nested") == [true, -7],
        "collection edits, order and Variant type"): return
    flat.list[1] = -1
    flat.table["b"] = 20
    if not await until(func(): return flat.list == [9, HIGH] and flat.table.b == 2, "collection proxy correction"): return
    report("collection_edit")
    if not await command("edit_proxy"): return
    mixed.stats = Stats.new()
    mixed.table.items.append(55)
    integers.a = 5
    await get_tree().create_timer(0.75).timeout
    if not require(integers.a == LOW and mixed.stats.points == -4294967296 and mixed.table.items == [-2, 0, 4],
            "quiet proxy correction"): return
    report("edit_proxy")
    integers.calls.clear()
    if not await until(func(): return integers.b == -1, "sibling change"): return
    if not require(integers.a == LOW and integers.calls == ["b"], "sibling setter count"): return
    report("sibling", integers.calls.duplicate())
    integers.calls.clear()
    if not await until(func(): return integers.a == LOW + (1 << 32), "upper 32-bit change"): return
    if not require(integers.calls == ["a"], "target setter count"): return
    report("target", integers.calls.duplicate())
    integers.calls.clear()
    if not await command("equal_value"): return
    await get_tree().create_timer(0.15).timeout
    if not require(integers.calls.is_empty(), "equal source write caused a remote setter"): return
    report("equal_value")
    if not await command("detach"): return
    detached = part
    mixed.remove_child(detached)
    report("detach")
    if not await command("replace"): return
    if not await replace_part(): return
    if not require(detached.value == 17, "detached node received an update"): return
    detached.free()
    report("replace")
    if not await command("replace_freed"): return
    part.free()
    if not await replace_part(): return
    report("replace_freed")
    if not await command("claim"): return
    var id := fast.object_id
    if not await until(func(): return room.game.claim(id), "claim ready"): return
    if not await until(fast.is_write_ready, "guest handoff baseline"): return
    if not require(integers.a == LOW + (1 << 32) and integers.b == -1, "before first guest submission"): return
    integers.interfere_on_correction = true
    integers.a = 42
    integers.b = 99
    if not await until(func(): return integers.b == -1, "guest mask correction"): return
    if not require(integers.corrections_with_side_effect == 1 and integers.a == 42,
            "correcting B clobbered the authorized A value"): return
    report("claim")
    var epoch: int = room.game.routing_info(id, false).epoch
    if not require(not room.game.reclaim(id), "guest attempted to force a reclaim"): return
    if not await until(func(): return room.game.routing_info(id, false).epoch > epoch and not fast.has_authority(),
            "new host epoch"): return
    if not require(not room.game.submit_fields(id, fast.sample_fields()), "former writer submission accepted"): return
    report("host_reclaimed")
    if await until(func(): return integers.a == 43, "new host write"): report("reclaimed")

func replace_part() -> bool:
    part = Part.new()
    part.name = "Part"
    mixed.add_child(part)
    return await until(func(): return part.value == 18, "replacement at the configured path")
