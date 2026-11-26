extends Node

const Actor = preload("res://property_actor.gd")
const Schema = preload("res://actor.gd")
const Scene = preload("res://property_object.tscn")
var objects := Node3D.new()
var spawner
var online := false
var writer := false
var count := 0
var workload := ""
var spawned := 0
var prepared := false
var running := false
var finished := false
var slots: Array = []
const OMIT_WAVE := 0
var wave := 0
var start_us := 0
var window_us := 0
var stop_notice_us := 0
var written := 0
var baseline_correct := 0
var ticks := PackedInt64Array()
var times := PackedInt64Array()

func emit_result(kind: String, value: Variant) -> void:
    print("CLIENT_EVENT ", JSON.stringify({"event": kind, "value": value}))

func configure() -> void:
    Actor.configure(not writer, workload == "sparse", count, Schema.FIELD_COUNT)
    objects.name = "Objects"
    add_child(objects)
    times.resize(Actor.WAVES + 1)

func command() -> String:
    return ""

func bound(_actor: Node) -> bool:
    return true

func stop_notified() -> bool:
    return false

func prepare() -> void:
    var expected := count / Schema.FIELD_COUNT
    if writer and spawned < expected and command() == "spawn":
        for i in mini(4, expected-spawned):
            spawner.spawn(Scene)
            spawned += 1
    if objects.get_child_count() != expected: return
    var actors := objects.get_children()
    for actor in actors:
        if not bound(actor): return
    if writer:
        for i in actors.size():
            actors[i].slot = i
            actors[i].write_wave(0)
    else:
        var distinct := {}
        for actor in actors:
            if actor.baseline_fields != Schema.FIELD_COUNT: return
            distinct[actor.slot] = true
        if distinct.size() != expected:
            emit_result("failed", "duplicate baseline object identity")
            finished = true
            return
    slots = actors
    for actor in slots: baseline_correct += actor.correct_final(true)
    if baseline_correct != count:
        emit_result("failed", "baseline readback mismatch")
        finished = true
        return
    prepared = true
    emit_result("prepared", count)

func arm() -> bool:
    if not running:
        if command() != "go": return false
        running = true
        start_us = Time.get_ticks_usec()
        window_us = Actor.wall_us()
        Actor.reads = 0
        Actor.sets = 0
        if not writer: emit_result("armed", window_us)
    ticks.append(Actor.wall_us())
    return true

func drive() -> void:
    if not arm(): return
    if workload == "idle":
        if Time.get_ticks_usec() - start_us >= 6000000: report_writer()
        return
    # Keep offered load comparable even when one backend's fixed-step rate falls.
    if Time.get_ticks_usec() - start_us < (wave + 1) * 100000: return
    wave += 1
    if wave != OMIT_WAVE:
        times[wave] = Actor.wall_us()
        for actor in slots: written += actor.write_wave(wave)
    if wave == Actor.WAVES: report_writer()

func report_writer() -> void:
    var stopped := Actor.wall_us()
    var reads := Actor.reads
    var sets := Actor.sets
    var correct := 0
    for actor in slots: correct += actor.correct_final(workload == "idle")
    emit_result("writer", {"wave_us":times, "start_us":window_us, "stopped_us":stopped, "ticks_us":ticks,
        "reads":reads, "sets":sets, "written":written, "correct":correct, "baseline":baseline_correct})
    finished = true

func observe() -> void:
    if not arm(): return
    if not stop_notice_us:
        if not stop_notified(): return
        stop_notice_us = Time.get_ticks_usec()
    if workload != "idle" and not Actor.settled_us:
        if Time.get_ticks_usec() - stop_notice_us < 12000000: return
    var reads := Actor.reads
    var sets := Actor.sets
    if Actor.updates: Actor.history.append([Actor.clock_ms, Actor.updates])
    var correct := 0
    for actor in slots: correct += actor.correct_final(workload == "idle")
    emit_result("observer", {"received":Actor.received, "first_us":Actor.first_us, "settled_us":Actor.settled_us,
        "reads":reads, "sets":sets, "correct":correct, "checked":slots.size()*Schema.FIELD_COUNT,
        "start_us":window_us, "ticks_us":ticks, "history":Actor.history, "errors":Actor.errors,
        "baseline":baseline_correct})
    finished = true

func _physics_process(_dt: float) -> void:
    if not online or finished: return
    if not prepared: prepare()
    elif writer: drive()
    else: observe()
