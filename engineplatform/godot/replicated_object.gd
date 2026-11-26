@tool
class_name NetcodeReplicatedObject
extends Node

# Attach beneath the replicated root. Configure properties in schema order.
# With an empty property list, a native 2D/3D rigid body uses the body.cfg/body3d.cfg contract.
@export var properties: Array[NodePath] = []
@export var teleport_distance := 100.0
@export var teleport_angle_degrees := 15.0
var teleport_generation := 0
var phase := 0
var game: NetcodeGame
var object_id := 0
var body_adapter
var initialized := false
var _binding_generation := 0
var _body_custom:Array=[]
var _node_paths: Dictionary[NodePath, NodePath] = {}
var _member_names: Dictionary[NodePath, StringName] = {}
var _indexed_members: Dictionary[NodePath, NodePath] = {}

func replication_root() -> Node:
    return get_parent()

func bind(session: NetcodeGame, id: int) -> bool:
    unbind()
    var root := replication_root()
    if session == null or id <= 0 or root == null: return false
    if _uses_body():
        body_adapter = _make_body(root)
        if body_adapter == null: return false
    for path in field_paths():
        if path.is_absolute() or path.get_subname_count() < 1: return false
        var target := _target(path)
        if target == null or (target != root and not root.is_ancestor_of(target)): return false
        if String(path.get_subname(0)) not in target: return false
    var key := "netcode_replica_%d" % id
    if session.has_meta(key) and session.get_meta(key).get_ref() != null: return false
    for path in field_paths():
        var names := path.get_concatenated_names()
        _node_paths[path] = NodePath(names) if names != &"" and names != &"." else ^""
        if path.get_subname_count() == 1:
            _member_names[path] = path.get_subname(0)
        else:
            _indexed_members[path] = NodePath(":" + String(path.get_concatenated_subnames()))
    session.set_meta(key,weakref(self))
    game = session
    object_id = id
    return true

func unbind() -> void:
    _binding_generation += 1
    if game != null:
        game.remove_meta("netcode_replica_%d" % object_id)
        game.unbind_replica(object_id)
    game = null
    body_adapter = null
    object_id = 0
    initialized = false
    _body_custom.clear()
    _node_paths.clear()
    _member_names.clear()
    _indexed_members.clear()

func reference() -> int:
    return game.field_reference(object_id) if game != null else 0

func resolve_node(handle: int) -> Node:
    if game == null: return null
    var id := game.resolve_field(handle)
    var key := "netcode_replica_%d" % id
    if id == 0 or not game.has_meta(key): return null
    var replica: Node = game.get_meta(key).get_ref()
    return replica.replication_root() if replica != null else null

func _exit_tree() -> void:
    unbind()

func _target(path: NodePath) -> Node:
    var root := replication_root()
    if root == null: return null
    if _node_paths.has(path):
        var node_path := _node_paths[path]
        return root if node_path.is_empty() else root.get_node_or_null(node_path)
    var names := path.get_concatenated_names()
    return root if names == &"" or names == &"." else root.get_node_or_null(NodePath(names))

func _uses_body() -> bool:
    return properties.is_empty()

func _make_body(root: Node):
    if root is RigidBody3D: return preload("forecast_body_3d.gd").new()
    if root is RigidBody2D: return preload("forecast_body.gd").new()
    return null

func field_paths() -> Array[NodePath]:
    return properties

func _read_member(target: Node, path: NodePath) -> Variant:
    if _member_names.has(path): return target.get(_member_names[path])
    if _indexed_members.has(path): return target.get_indexed(_indexed_members[path])
    return target.get_indexed(NodePath(":"+String(path.get_concatenated_subnames())))

func _write_member(target: Node, path: NodePath, value: Variant) -> void:
    if _member_names.has(path): target.set(_member_names[path], value)
    elif _indexed_members.has(path): target.set_indexed(_indexed_members[path], value)
    else: target.set_indexed(NodePath(":"+String(path.get_concatenated_subnames())),value)

func sample_fields() -> Array:
    var fields: Array = []
    var generation := _binding_generation
    var root := replication_root()
    if root == null: return []
    if _uses_body():
        var adapter = body_adapter if body_adapter != null else _make_body(root)
        if adapter == null: return []
        fields = adapter.sample(root,teleport_generation,phase)
    for path in field_paths():
        if not is_instance_valid(root): return []
        var target := root
        if _node_paths.has(path):
            var node_path := _node_paths[path]
            if not node_path.is_empty(): target = root.get_node_or_null(node_path)
        else: target = _target(path)
        if target == null: return []
        fields.append(_read_member(target,path))
        if generation != _binding_generation: return []
    return fields

func _before_apply(_baseline: bool) -> void:
    pass

func _equal_member(_path: NodePath) -> void:
    pass

func _sample_for_sync() -> Array:
    return sample_fields()

func _apply_fields(sampled: Array, fields: Array, offset: int, baseline: bool, sample_invalidated := false) -> bool:
    var paths := field_paths()
    var generation := _binding_generation
    var complete := true
    for i in paths.size():
        var path := paths[i]
        var index := offset + i
        var matched: bool = not baseline and sampled[index] == fields[index]
        if matched and not sample_invalidated:
            _equal_member(path)
            continue
        var target := _target(path)
        if target == null:
            complete = false
            continue
        # An earlier setter can change a later field after the batch was sampled.
        if matched:
            var current: Variant = _read_member(target, path)
            if generation != _binding_generation or not is_instance_valid(target): return false
            if current == fields[index]:
                _equal_member(path)
                continue
        _write_member(target, path, fields[index])
        if generation != _binding_generation: return false
        sample_invalidated = true
    return complete

func _physics_process(_dt: float) -> void:
    if game == null or Engine.is_editor_hint(): return
    var generation := _binding_generation
    var body := replication_root() if body_adapter != null else null
    var sampled := _sample_for_sync()
    if sampled.is_empty() or generation != _binding_generation: return
    var change := game.sync_replica(object_id,sampled,body_adapter != null)
    if change.is_empty():
        if body_adapter!=null and not _body_custom.is_empty() and sampled.slice(7)!=_body_custom:
            var state:=game.routing_info(object_id,false)
            if not state.is_empty() and state.player!=state.writer:
                _before_apply(false)
                _apply_fields(sampled.slice(7), _body_custom, 0, false)
        return
    _before_apply(change.baseline)
    var fields: Array = change.fields
    if body_adapter != null:
        _body_custom=fields.slice(7)
        var snap: bool = change.baseline
        if teleport_distance > 0 and sampled[0].distance_to(fields[0]) > teleport_distance: snap = true
        var angle: float
        if body is RigidBody3D: angle = sampled[1].angle_to(fields[1])
        else: angle = absf(angle_difference(sampled[1],fields[1]))
        if teleport_angle_degrees > 0 and rad_to_deg(angle) > teleport_angle_degrees: snap = true
        if snap: body_adapter.generation = ~int(fields[5])
        teleport_generation = fields[5]
        phase = fields[6]
        body_adapter.apply(body,fields,change.age)
    var offset := 7 if body_adapter != null else 0
    var complete := _apply_fields(sampled, fields, offset, change.baseline, body_adapter != null)
    if generation != _binding_generation: return
    initialized = complete
    if not initialized: game.unbind_replica(object_id)
