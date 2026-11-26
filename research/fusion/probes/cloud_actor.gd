extends Node3D

@export var kind := ""
var reads := 0
var sets := 0
var watch_score := false
var observed_scores: Array[float] = []
var last_score := 0.0
@export var score := 0.0:
    get:
        reads += 1
        return score
    set(value):
        score = value
        last_score = value
        sets += 1
        if watch_score:
            observed_scores.append(value)
        if is_inside_tree() and get_parent().get_parent().config.mode == "spawn":
            get_parent().get_parent().record("spawn_setter", value)
@export var label := ""
@export var marks: Array[float] = []
var allow_transfer := false
var input_calls := 0
var replay_calls := 0
var resets := 0
var seen_inputs := {}
var highest_payload := 0
var detached: Node

func _exit_tree() -> void:
    if is_instance_valid(detached):
        detached.free()

func _ready() -> void:
    var sync = get_node("Sync")
    var room := get_parent().get_parent()
    if room.config.mode == "spawn":
        room.record("spawn_ready", score)
        sync.spawned.connect(func(): room.record("replicator_spawned", score))
    if sync.has_signal("authority_requested"):
        sync.authority_requested.connect(authority_request)
        sync.authority_response.connect(func(ok: bool): get_parent().get_parent().record("response", [kind, ok]))
    if sync.has_signal("on_process_input"):
        sync.on_process_input.connect(process_input)
        sync.state_reset.connect(func(info):
            if info.get_reason() == 1:
                resets += 1)

func authority_request(player: int) -> bool:
    get_parent().get_parent().record("request", [kind, player, allow_transfer])
    return allow_transfer

func process_input(_tick: int, _delta: float, payload: PackedByteArray, _is_new: bool) -> void:
    if payload.size() != 4:
        return
    var serial := payload.decode_u32(0)
    input_calls += 1
    if seen_inputs.has(serial):
        replay_calls += 1
    seen_inputs[serial] = true
    highest_payload = maxi(highest_payload, serial)
    score += 1.0

@rpc("any_peer", "call_remote", "reliable")
func reliable_mark(sequence: int) -> void:
    get_parent().get_parent().record("rpc", [kind, sequence, Fusion.get_rpc_sender()])

@rpc("any_peer", "call_remote", "unreliable")
func unreliable_mark(sequence: int) -> void:
    get_parent().get_parent().record("rpc_u", [kind, sequence, Fusion.get_rpc_sender()])

