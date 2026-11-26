@tool
class_name NetcodeLegacyReplicator
extends "res://addons/netcode/replicated_object.gd"

const Values = preload("scene_values.gd")
const Fields = preload("scene_fields.gd")
enum RootMode { NONE, AUTO }
enum SampleKind { RAW, INTEGER, ARRAY, DICTIONARY }
## Relative to this Sync. Select the root of the .tscn to cover all its authored branches.
@export var root_path: NodePath = ^".."
@export var root_mode := RootMode.AUTO
## Set when two different layouts use the same gameplay script.
@export var layout_key := ""
## The creator drives this object and it leaves the world with the player.
@export var player_attached := false
@export var client_writable: Array[NodePath] = []
@export var delegate_root_motion := false
@export var limits: Dictionary[NodePath,int] = {}
@export var smooth_properties: Array[NodePath] = []
@export var angle_properties: Array[NodePath] = []
@export_range(0,1,0.01) var smoothing_seconds := 0.15

var schema_id := 0
var _paths: Array[NodePath] = []
var _records: Dictionary = {}
var _kinds: Dictionary = {}
var _hints: Dictionary = {}
var _caps: Dictionary = {}
var _interpolation: Dictionary = {}
var _automatic_paths: Array[NodePath] = []
var _root_integer_members: Array[StringName] = []
var _root_integer_values := PackedInt64Array()
var _root_integer_bytes: Array = []
var _use_internal_integer_sample := false
var _root_sampler: NetcodeSceneSampler
var _root_apply_members: Array[StringName] = []
var _root_apply_kinds := PackedByteArray()
var _proxy_changes_only := false
var _pending_refs: Dictionary = {}
var _smooth: Dictionary = {}
var _snap := true
var room: WeakRef
var _bound_actor: WeakRef
var _native_actor: WeakRef
var _native_game: WeakRef
var _baseline_epoch := -1

func replication_root() -> Node:
    if _native_actor: return _native_actor.get_ref()
    if _bound_actor: return _bound_actor.get_ref()
    if root_path.is_empty() or root_path.is_absolute() or root_path.get_subname_count(): return null
    var actor := get_node_or_null(root_path)
    if actor == self or actor is NetcodeReplicatedObject: return null
    return actor

func configure_native(actor: Node, sync: NetcodeReplicator) -> void:
    _native_actor = weakref(actor)
    root_mode = sync.root_replication_mode
    delegate_root_motion = true
    var config := sync.replication_config
    if config:
        properties = config.properties
        client_writable = config.properties
        limits.assign(config.limits)
        smooth_properties = config.smooth_properties
        angle_properties = config.angle_properties
    if sync is NetcodeServerReplicator:
        layout_key = "server:" + type_key()

func apply_native_model(fields: Array) -> bool:
    if game == null or fields.is_empty() or _uses_body(): return false
    var generation := _binding_generation
    var sampled := sample_fields()
    if generation != _binding_generation or sampled.size() != fields.size(): return false
    _before_apply(true)
    return _apply_fields(sampled, fields, 0, false)

func set_native_game(session: NetcodeGame) -> void:
    _native_game = weakref(session)

func prime_native_writer() -> bool:
    var baseline := game.sync_replica(object_id, game.read_fields(object_id, false), body_adapter != null)
    if not baseline.get("baseline", false): return false
    _before_apply(true)
    initialized = true
    return true

static func binding_key(session: NetcodeGame) -> StringName:
    return "netcode_binding_" + String.num_uint64(session.get_instance_id(), 16)

func bind_native(session: NetcodeGame, id: int, proxy_changes_only: bool) -> bool:
    _proxy_changes_only = proxy_changes_only
    return bind(session, id)

func bind(session: NetcodeGame, id: int) -> bool:
    var actor := replication_root()
    if actor == null or session == null: return false
    var key := binding_key(session)
    if actor.has_meta(key):
        var other = actor.get_meta(key).get_ref()
        if other != null and other != self: return false
    if not super.bind(session, id): return false
    _bound_actor = weakref(actor)
    actor.set_meta(key, weakref(self))
    if not _uses_body():
        for path in _paths:
            if _kinds[path] != "integer64" or not _node_paths[path].is_empty() or not _member_names.has(path):
                _root_integer_members.clear()
                break
            _root_integer_members.append(_member_names[path])
    _root_integer_values.resize(_root_integer_members.size())
    _root_integer_bytes.resize(_root_integer_members.size())
    if not _root_integer_members.is_empty(): _root_integer_bytes.fill(Values.encode_integer(0))
    # Keep script subclasses' sample_fields overrides on the virtual path.
    if get_script().get_base_script() == preload("replicated_object.gd"):
        _use_internal_integer_sample = not _root_integer_members.is_empty()
        if (not _use_internal_integer_sample or _proxy_changes_only) and not _uses_body():
            _bind_root_sample()
    return true

func _bind_root_sample() -> void:
    for path in _paths:
        if not _node_paths[path].is_empty() or not _member_names.has(path) or _kinds[path] == "object_ref" or _records.has(path):
            return
    if _paths.is_empty(): return
    var members: Array[StringName] = []
    var kinds := PackedByteArray()
    var caps := PackedInt32Array()
    # Native root fields without interpolation can reuse the sampler's member order on apply.
    var direct_apply := _native_actor != null and _interpolation.is_empty() and _automatic_paths.is_empty()
    for path in _paths:
        var kind := SampleKind.RAW
        match _kinds[path]:
            "integer64": kind = SampleKind.INTEGER
            "array_value": kind = SampleKind.ARRAY
            "dictionary_value": kind = SampleKind.DICTIONARY
        direct_apply = direct_apply and kind <= SampleKind.INTEGER
        members.append(_member_names[path])
        kinds.append(kind)
        caps.append(_caps.get(path, 0))
    var sampler := NetcodeSceneSampler.new()
    if not sampler.configure(members, kinds, caps): return
    _root_sampler = sampler
    if direct_apply:
        _root_apply_members = members
        _root_apply_kinds = kinds


func _record_fields(script:Script) -> Array:
    var fields:Array=[]
    var names:Dictionary={}
    while script:
        for info in script.get_script_property_list():
            if names.has(info.name) or not (int(info.usage) & PROPERTY_USAGE_SCRIPT_VARIABLE): continue
            if info.type in [TYPE_BOOL,TYPE_INT,TYPE_FLOAT,TYPE_VECTOR2,TYPE_VECTOR3,TYPE_VECTOR4,TYPE_QUATERNION,TYPE_COLOR]:
                fields.append({"name":info.name,"type":info.type})
                names[info.name]=true
        script=script.get_base_script()
    return fields

func type_key() -> String:
    if not layout_key.is_empty(): return layout_key
    var actor := replication_root()
    if actor == null: return "<invalid root_path>"
    if player_attached and not actor.scene_file_path.is_empty(): return actor.scene_file_path
    var script: Script = actor.get_script()
    return script.resource_path if script else actor.get_class()

func _uses_body() -> bool:
    return root_mode == RootMode.AUTO and (replication_root() is RigidBody2D or replication_root() is RigidBody3D)

func field_paths() -> Array[NodePath]:
    return _paths

func _auto_paths() -> Array[NodePath]:
    var paths: Array[NodePath] = []
    var root := replication_root()
    if root_mode == RootMode.AUTO and not _uses_body():
        if root is Node2D or root is Node3D:
            paths.append(^".:position")
            paths.append(^".:quaternion" if root is Node3D else ^".:rotation")
        if root is CharacterBody2D or root is CharacterBody3D:
            paths.append(^".:velocity")
    return paths

func _canonical(path:NodePath) -> NodePath:
    if path.is_absolute() or path.get_subname_count()<1: return ^""
    var target:=_target(path)
    var root:=replication_root()
    if target==null: return ^""
    if target!=root and not root.is_ancestor_of(target): return ^""
    return NodePath("%s:%s"%[root.get_path_to(target),path.get_concatenated_subnames()])

func _property_info(path:NodePath) -> Dictionary:
    var owner:Object=_target(path)
    for i in path.get_subname_count()-1:
        var member=owner.get(path.get_subname(i))
        if not member is RefCounted: return {}
        owner=member
    for info in owner.get_property_list():
        if info.name==path.get_subname(path.get_subname_count()-1) and not (int(info.usage) & PROPERTY_USAGE_READ_ONLY):
            return info
    return {}

func definition() -> Dictionary:
    var root := replication_root()
    if root == null: return {}
    var lines := PackedStringArray()
    var writable := PackedInt32Array()
    var auto := _auto_paths()
    _automatic_paths = auto
    _paths = auto.duplicate()
    _records.clear()
    _kinds.clear()
    _hints.clear()
    _caps.clear()
    _interpolation.clear()
    var delegates:Array[NodePath]=[]
    for path in client_writable: delegates.append(_canonical(path))
    for path in smooth_properties: _interpolation[_canonical(path)]=1
    for path in angle_properties: _interpolation[_canonical(path)]=2
    var sizes:Dictionary={}
    for path in limits: sizes[_canonical(path)]=limits[path]
    var declarations:=PackedStringArray()
    if _uses_body():
        var kinds := ["vec2","float","vec2","float"] if root is RigidBody2D else ["vec3","quat","vec3","vec3"]
        kinds.append_array(["flag","int","int"])
        for kind in kinds:
            var id := lines.size()+1
            lines.append("%s %d . motion_%d" % [kind,id,id])
            if delegate_root_motion: writable.append(id)
    for path in properties:
        var canonical:=_canonical(path)
        if canonical.is_empty() or canonical in _paths: return {}
        _paths.append(canonical)
    for path in _paths:
        var info:=_property_info(path)
        if info.is_empty(): return {}
        var kind:=Fields.kind(info)
        if kind.is_empty(): return {}
        if kind=="array_value":
            var array:Array=super._read_member(_target(path),path)
            if array.get_typed_builtin()==TYPE_OBJECT:
                var script:Script=array.get_typed_script()
                if script==null or script.get_global_name().is_empty() or not ClassDB.is_parent_class(script.get_instance_base_type(),"RefCounted"): return {}
                var fields:=_record_fields(script)
                if fields.is_empty(): return {}
                _records[path]={"script":script,"fields":fields}
                declarations.append(str(fields))
        var id:=lines.size()+1
        if path in delegates or (path in auto and delegate_root_motion): writable.append(id)
        _kinds[path]=kind
        _hints[path]=String(info.get("hint_string",""))
        var wire:=Fields.wire(kind)
        var line:="%s %d . f%d"%[wire,id,id]
        if Fields.ELEMENT.has(wire):
            var default_size:=128 if wire=="bytes" else (64 if wire=="string" else 8)
            var cap:=8 if kind=="integer64" else int(sizes.get(path,default_size))
            if cap<1 or cap>65535: return {}
            _caps[path]=cap
            line+=" %d"%cap
        if sizes.has(path) and (not _caps.has(path) or kind=="integer64"): return {}
        lines.append(line)
        declarations.append("%s/%s/%s"%[path,kind,info.get("hint_string","")])
    for options in [delegates,_interpolation.keys(),sizes.keys()]:
        for path in options:
            if path not in _paths: return {}
    # An RPC-only object still needs an identity in Core.
    if lines.is_empty(): lines.append("flag 1 . present")
    var body := "\n".join(lines)+"\n"
    schema_id=Fields.wire_id("object:"+type_key()+"\n"+body+str(declarations)+str(writable))
    return {"id":schema_id,"key":type_key(),"text":"schema %d 1 Actor\n"%schema_id+body,"writable":writable,"player_attached":player_attached}

func has_authority() -> bool:
    if game==null: return false
    var info:=game.routing_info(object_id,false)
    return not info.is_empty() and info.schema==schema_id and info.player==info.writer

## True after this writer has applied the incoming baseline for its current grant.
func is_write_ready() -> bool:
    if game == null or not initialized: return false
    var state := game.routing_info(object_id, false)
    return not state.is_empty() and state.player == state.writer and state.epoch == _baseline_epoch

func sample_fields() -> Array:
    if not _root_integer_members.is_empty():
        var fields := _sample_root_integers()
        for i in fields.size():
            fields[i] = fields[i].duplicate()
        return fields
    if not _uses_body() and _paths.is_empty(): return [true]
    return super.sample_fields()

func _sample_for_sync() -> Array:
    if _native_actor and not initialized and not _uses_body():
        return game.read_fields(object_id, false)
    if _use_internal_integer_sample: return _sample_root_integers()
    var sampler := _root_sampler
    if sampler: return sampler.sample(replication_root(), _encode_root_collection)
    return sample_fields()

static func _encode_root_collection(value: Variant, kind: String, capacity: int) -> Variant:
    return Values.new().encode(value, kind, capacity)

func _sample_root_integers() -> Array:
    var root := replication_root()
    if root == null: return []
    var generation := _binding_generation
    for i in _root_integer_members.size():
        var value: Variant = root.get(_root_integer_members[i])
        if generation != _binding_generation or typeof(value) != TYPE_INT: return []
        if _root_integer_values[i] != value:
            _root_integer_values[i] = value
            _root_integer_bytes[i] = Values.encode_integer(value)
    return _root_integer_bytes.duplicate()

func _read_member(target:Node,path:NodePath) -> Variant:
    var generation := _binding_generation
    var value = super._read_member(target,path)
    if generation != _binding_generation or not is_instance_valid(target): return null
    var kind:String=_kinds[path]
    if kind == "integer64":
        if typeof(value) != TYPE_INT: return null
        return Values.encode_integer(value)
    if _records.has(path):
        var rows:Array=[]
        for element in value:
            if element==null: return null
            var row:Array=[]
            for field in _records[path].fields: row.append(element.get(field.name))
            rows.append(row)
        value=rows
    if kind!=Fields.wire(kind): return Values.new().encode(value,kind,_caps[path])
    if kind!="object_ref": return value
    var session = room.get_ref() if room else null
    var native: NetcodeGame = _native_game.get_ref() if _native_game else null
    if session == null and native == null: return 0
    if _pending_refs.has(path):
        var saved:Dictionary=_pending_refs[path]
        var previous:Node=saved.node.get_ref() if saved.node else null
        if value==previous:
            var resolved:=resolve_node(saved.handle)
            if value!=resolved:
                super._write_member(target,path,resolved)
                saved.node=weakref(resolved) if resolved else null
            return saved.handle
        _pending_refs.erase(path)
    if not is_instance_valid(value) or not value is Node: return 0
    if native:
        var key := binding_key(native)
        var replica: Node = value.get_meta(key).get_ref() if value.has_meta(key) else null
        return replica.reference() if replica else 0
    return session.node_reference(value)

func _before_apply(baseline:bool) -> void:
    if baseline: _baseline_epoch = game.routing_info(object_id, false).epoch
    if not _root_apply_members.is_empty(): return
    _snap=baseline or has_authority()
    if _snap: _smooth.clear()

func can_apply_received_fields() -> bool:
    return not _root_apply_members.is_empty()

func apply_received_fields(mask: int, fields: Array, baseline: bool) -> void:
    var root := replication_root()
    if root == null: return
    var generation := _binding_generation
    if baseline: _before_apply(true)
    var next := 0
    for i in _root_apply_members.size():
        if not (mask & (1 << i)): continue
        var value: Variant = fields[next]
        next += 1
        if _root_apply_kinds[i] == SampleKind.INTEGER:
            if value.size() != 8: continue
            value = value.decode_s64(0)
        root.set(_root_apply_members[i], value)
        if generation != _binding_generation or not is_instance_valid(root): return
    initialized = true

func _apply_fields(sampled: Array, fields: Array, offset: int, baseline: bool, sample_invalidated := false) -> bool:
    if _root_apply_members.is_empty() or offset != 0 or sample_invalidated:
        return super._apply_fields(sampled, fields, offset, baseline, sample_invalidated)
    var root := replication_root()
    if root == null: return false
    var generation := _binding_generation
    var wrote_field := false
    for i in _root_apply_members.size():
        var value: Variant = fields[i]
        var matched: bool = not baseline and sampled[i] == value
        if matched and not wrote_field: continue
        var member := _root_apply_members[i]
        var integer := _root_apply_kinds[i] == SampleKind.INTEGER
        if integer:
            if value.size() != 8: continue
            value = value.decode_s64(0)
        # A preceding setter can invalidate even a matching sample.
        if matched:
            var current: Variant = root.get(member)
            if generation != _binding_generation or not is_instance_valid(root): return false
            if (not integer or current is int) and current == value: continue
        root.set(member, value)
        if generation != _binding_generation or not is_instance_valid(root): return false
        wrote_field = true
    return true

func _equal_member(path: NodePath) -> void:
    _smooth.erase(path)

func _write_member(target:Node,path:NodePath,value:Variant) -> void:
    var kind:String=_kinds[path]
    if kind=="object_ref":
        var node:=resolve_node(value)
        _pending_refs[path]={"handle":value,"node":weakref(node) if node else null}
        super._write_member(target,path,node)
        return
    if kind == "integer64":
        if value.size() != 8: return
        value = value.decode_s64(0)
    elif kind!=Fields.wire(kind):
        value=Values.new().decode(value,kind)
        if value==null: return
    if _records.has(path):
        var record:Dictionary=_records[path]
        for row in value:
            if not row is Array or row.size()!=record.fields.size(): return
            for i in row.size():
                if typeof(row[i])!=record.fields[i].type: return
        var array:Array=super._read_member(target,path)
        array.resize(value.size())
        for i in value.size():
            if array[i]==null: array[i]=record.script.new()
            for j in record.fields.size(): array[i].set(record.fields[j].name,value[i][j])
        return
    if kind in ["array_value","dictionary_value"]:
        value=Fields.collection(value,_hints[path])
        if value==null: return
    if not _snap and (_interpolation.has(path) or path in _automatic_paths) and smoothing_seconds>0 and not value is Array:
        if not _smooth.has(path) or _smooth[path].value!=value:
            _smooth[path]={"value":value,"age":0.0}
    else: super._write_member(target,path,value)

func _physics_process(dt:float) -> void:
    if game==null or Engine.is_editor_hint(): return
    if replication_root() == null:
        unbind()
        return
    if not game._has_replica_layout(object_id, schema_id):
        if initialized: game.unbind_replica(object_id)
        initialized=false
        _smooth.clear()
        _pending_refs.clear()
        return
    var generation := _binding_generation
    super._physics_process(dt)
    if generation != _binding_generation or _smooth.is_empty(): return
    var weight:=1.0-exp(-dt/maxf(smoothing_seconds,0.0001))
    for path:NodePath in _smooth.keys():
        var target:=_target(path)
        if target==null: continue
        var current=super._read_member(target,path)
        if generation != _binding_generation or not is_instance_valid(target): return
        var shadow:Dictionary=_smooth[path]
        shadow.age+=dt
        var goal=shadow.value
        var value=goal
        var angle:bool=_interpolation.get(path,0)==2 or path==^".:rotation" and path in _automatic_paths
        match typeof(goal):
            TYPE_FLOAT:
                value=lerp_angle(current,goal,weight) if angle else lerpf(current,goal,weight)
                if (is_zero_approx(angle_difference(value, goal)) if angle else is_equal_approx(value, goal)): value=goal
            TYPE_VECTOR3:
                if angle: value=Vector3(lerp_angle(current.x,goal.x,weight),lerp_angle(current.y,goal.y,weight),lerp_angle(current.z,goal.z,weight))
                else: value=current.lerp(goal,weight)
            TYPE_VECTOR2,TYPE_VECTOR4,TYPE_COLOR: value=current.lerp(goal,weight)
            TYPE_QUATERNION: value=current.slerp(goal,weight)
            _:
                if shadow.age<smoothing_seconds*0.5: continue
        if value==current: value=goal
        if value!=current:
            super._write_member(target,path,value)
            if generation != _binding_generation: return
        if value==goal: _smooth.erase(path)

func unbind() -> void:
    var actor: Node = _bound_actor.get_ref() if _bound_actor else null
    if actor and game: actor.remove_meta(binding_key(game))
    _bound_actor = null
    _baseline_epoch = -1
    _root_integer_members.clear()
    _root_integer_values.clear()
    _root_integer_bytes.clear()
    _use_internal_integer_sample = false
    _root_sampler = null
    _root_apply_members.clear()
    _root_apply_kinds.clear()
    _smooth.clear()
    _pending_refs.clear()
    super.unbind()
