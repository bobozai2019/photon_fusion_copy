extends Node2D

var trail_reads := 0
var trail_writes := 0
@export var trail := PackedFloat32Array([0.0]):
    get:
        trail_reads += 1
        return trail
    set(next):
        trail = next
        trail_writes += 1
var trail_after_setter := PackedFloat32Array()
@export var target: Node
@export var levels: Array[int] = []
@export var ledger: Dictionary[String, int] = {}
var smoothed_writes := 0
@export var smoothed := 0.0:
    set(next):
        smoothed = next
        smoothed_writes += 1
var detach_on_value_read := false
var detach_on_value_write := false
var value_written: Callable
@export var value := -1:
    get:
        if detach_on_value_read:
            detach_on_value_read = false
            var sync := get_node("Sync")
            remove_child(sync)
            sync.queue_free()
        return value
    set(next):
        value = next
        if detach_on_value_write or (next == 74 and is_inside_tree() and scene_file_path.ends_with("/setter_reject.tscn") and Netcode.get_local_player_id() != 1):
            detach_on_value_write = false
            var sync := get_node("Sync")
            remove_child(sync)
            sync.queue_free()
            return
        if next == 937 and scene_file_path.ends_with("/transaction.tscn") and not $Sync.has_authority():
            trail[0] = 99.0
            (func(): trail_after_setter = trail.duplicate()).call_deferred()
        _record("setter")
        if value_written.is_valid():
            value_written.call(next)
var remote_calls := 0
var local_calls := 0
var reference_calls: Array = []

func _enter_tree() -> void:
    $Sync.spawned.connect(func(): _record("replicator_spawned"))
    $Sync.authority_changed.connect(func(_value: bool): _record("authority_changed"))
    _record("enter_tree")

func _ready() -> void:
    if scene_file_path.ends_with("/key_one.tscn"):
        var config := $Sync.replication_config.duplicate() as NetcodeReplicationConfig
        config.apply_proxy_changes_only = true
        $Sync.replication_config = config
    if get_meta("reject_mount", false):
        queue_free()
        return
    if has_meta("cancel_in_ready"):
        get_meta("cancel_in_ready").despawn(self)
        return
    if scene_file_path.ends_with("/fragile.tscn") and Netcode.get_local_player_id() != 1:
        $Sync.free()
        return
    if scene_file_path.ends_with("/signal_reject.tscn") and Netcode.get_local_player_id() != 1:
        $Sync.spawned.connect(queue_free)
    _record("ready")

func _record(event: String) -> void:
    var loop := Engine.get_main_loop()
    if loop.has_method("record_order"):
        loop.call("record_order", self, event, value, $Sync.has_authority())

func note_value(delta: int) -> void:
    value += delta

func set_user_keys(_keys: PackedInt64Array) -> void:
    pass

@rpc("authority", "call_remote", "reliable")
func note_remote() -> void:
    remote_calls += 1

@rpc("any_peer", "call_local", "reliable")
func note_local(values: Array) -> void:
    local_calls += 1
    values[0].clear()

@rpc("any_peer", "call_remote", "reliable")
func note_reference(node: Node) -> void:
    var info := Netcode.get_rpc_info()
    reference_calls.append([node, info.sender, info.target, info.target_node == self])
