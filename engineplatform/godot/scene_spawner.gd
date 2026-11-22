class_name NetcodeSpawner
extends Node

## Drives one local room and its dynamic scene instances.
signal room_joined(player_id: int)
signal room_closed(reason: String)
signal _advanced

const Room = preload("scene_room.gd")
const Replica = preload("scene_replicator.gd")
const SPAWN_RESULT := 10 # ScriptEventKind::SpawnResult

@export var spawn_path: NodePath = ^"."
@export var spawnable_scenes: Array[PackedScene] = []
var room := Room.new()
var _schemas := {}
var _scenes := {}
var _instances := {}
var _requests := {}
var _parent: Node
var _session := 0
var _connected := false
var _host := false
var _round := 0
var _connect_deadline := 0

func add_spawnable_scene(scene: PackedScene) -> bool:
    if _connected or not scene: return false
    if scene not in spawnable_scenes: spawnable_scenes.append(scene)
    return true

func open(host: bool, address: String, port: int, capacity := 4) -> String:
    if _connected: return "房间已打开"
    _parent = get_node_or_null(spawn_path)
    if not is_inside_tree() or not _parent or spawnable_scenes.is_empty(): return "请设置生成位置和场景模板"
    # Only the catalog belongs to this room. Other spawners can share the scene tree.
    var error := room.open(null, host, address, port, capacity, spawnable_scenes)
    if not error.is_empty(): return error
    # Joining through this component creates a fresh player. Free the seat on disconnect.
    if host and not room.game.set_player_ttl(0):
        room.close()
        return "无法配置离房席位回收"
    for scene in spawnable_scenes:
        var actor := scene.instantiate()
        var sync := _sync(actor)
        if sync:
            var layout := sync.definition()
            if layout.is_empty() or _scenes.has(layout.id):
                error = "模板布局重复或无效：" + scene.resource_path
            elif sync.player_attached and layout.writable.is_empty():
                error = "玩家持有的模板需要至少一个可写属性：" + scene.resource_path
            elif host and not room.game.allow_spawn(layout.id, sync.sample_fields(), not layout.writable.is_empty()):
                error = "无法注册生成模板：" + scene.resource_path
            else:
                _scenes[layout.id] = scene
                _schemas[scene] = layout
        else:
            error = "模板需要一个指向场景根的 Sync：" + scene.resource_path
        actor.free()
        if not error.is_empty():
            close()
            return error
    _host = host
    _connected = true
    _connect_deadline = Time.get_ticks_msec() + 12000
    return ""

func _sync(actor: Node) -> Replica:
    var found: Array = []
    Room.collect_replicas(actor, found)
    if found.size() != 1: return null
    var sync: Replica = found[0]
    return sync if sync.replication_root() == actor else null

## Returns the local instance after its baseline (and any creator write grant) is ready.
func spawn(scene: PackedScene) -> Node:
    if not _round or not _schemas.has(scene): return null
    var generation := _session
    var layout: Dictionary = _schemas[scene]
    var id := 0
    var request := 0
    var next_attempt := 0
    var deadline := Time.get_ticks_msec() + 12000
    if _host:
        var actor := scene.instantiate()
        _parent.add_child(actor)
        var sync := _sync(actor)
        id = room.spawn(sync)
        if not id:
            actor.queue_free()
            return null
        _instances[id] = sync
    while generation == _session and _round and Time.get_ticks_msec() < deadline:
        if not _host and not id:
            if not request and Time.get_ticks_msec() >= next_attempt:
                # The namespace arrives after the guest enters the running round.
                var handle := room.game.request_reference(layout.id, 0)
                if handle:
                    request = handle & 0xffffffff
                    _requests[request] = -1
            elif request and _requests[request] >= 0:
                id = _requests[request]
                _requests.erase(request)
                # The host can explicitly refuse a request while writer models are
                # still being negotiated. Only resubmit a confirmed empty result.
                if not id:
                    request = 0
                    next_attempt = Time.get_ticks_msec() + 300
        if id and _instances.has(id):
            var sync: Replica = _instances[id]
            if sync.initialized and (layout.writable.is_empty() or sync.is_write_ready()):
                return sync.replication_root()
        await _advanced
    if generation == _session: _requests.erase(request)
    return null

func _process(_delta: float) -> void:
    if not _connected: return
    var frame := room.advance()
    if not frame.get("round", 0):
        if _round or Time.get_ticks_msec() >= _connect_deadline:
            close("房间已结束或连接超时")
        elif _host:
            room.game.start()
        return
    for event in frame.events:
        if event.kind == SPAWN_RESULT and _requests.has(event.sequence):
            _requests[event.sequence] = event.object
    var visible := {}
    for id in frame.objects:
        visible[id] = true
        if _instances.has(id): continue
        var schema: int = room.game.routing_info(id, false).get("schema", 0)
        if not _scenes.has(schema): continue
        var actor: Node = _scenes[schema].instantiate()
        _parent.add_child(actor)
        var sync := _sync(actor)
        if room.bind_replica(sync, id): _instances[id] = sync
        else: actor.queue_free()
    for id in _instances.keys():
        if not visible.has(id): _remove(id)
    if not _round:
        _round = frame.round
        room_joined.emit(frame.player)
    _advanced.emit()

func _remove(id: int) -> void:
    var entry = _instances[id]
    _instances.erase(id)
    if not is_instance_valid(entry): return
    var sync: Replica = entry
    var actor := sync.replication_root()
    sync.unbind()
    if is_instance_valid(actor): actor.queue_free()

func close(reason := "") -> void:
    var active := _connected
    _connected = false
    _round = 0
    _session += 1
    for id in _instances.keys(): _remove(id)
    _requests.clear()
    _scenes.clear()
    _schemas.clear()
    room.close()
    _advanced.emit()
    if active: room_closed.emit(reason)

func _exit_tree() -> void:
    close()
