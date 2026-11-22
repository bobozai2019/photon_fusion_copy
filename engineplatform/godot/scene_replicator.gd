@tool
class_name NetcodeReplicator
extends "res://addons/netcode/replicated_object.gd"

const Values = preload("scene_values.gd")
const Fields = preload("scene_fields.gd")
enum RootMode { NONE, AUTO }
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
var _pending_refs: Dictionary = {}
var _smooth: Dictionary = {}
var _snap := true
var room: WeakRef
var _bound_actor: WeakRef
var _baseline_epoch := -1

func replication_root() -> Node:
    if _bound_actor: return _bound_actor.get_ref()
    if root_path.is_empty() or root_path.is_absolute() or root_path.get_subname_count(): return null
    var actor := get_node_or_null(root_path)
    if actor == self or actor is NetcodeReplicatedObject: return null
    return actor

static func binding_key(session: NetcodeGame) -> StringName:
    return "netcode_binding_" + String.num_uint64(session.get_instance_id(), 16)

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
    return true


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
        var root := replication_root()
        if root == null: return []
        for i in _root_integer_members.size():
            var value: Variant = root.get(_root_integer_members[i])
            if typeof(value) != TYPE_INT: return []
            if _root_integer_values[i] != value:
                _root_integer_values[i] = value
                _root_integer_bytes[i] = Values.encode_integer(value)
        return _root_integer_bytes.duplicate()
    if not _uses_body() and _paths.is_empty(): return [true]
    return super.sample_fields()

func _read_member(target:Node,path:NodePath) -> Variant:
    var value = super._read_member(target,path)
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
    if session==null: return 0
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
    return session.node_reference(value) if is_instance_valid(value) and value is Node else 0

func _before_apply(baseline:bool) -> void:
    if baseline: _baseline_epoch = game.routing_info(object_id, false).epoch
    _snap=baseline or has_authority()
    if _snap: _smooth.clear()

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
    var state:=game.routing_info(object_id,false)
    if state.is_empty() or state.schema!=schema_id:
        if initialized: game.unbind_replica(object_id)
        initialized=false
        _smooth.clear()
        _pending_refs.clear()
        return
    super._physics_process(dt)
    if _smooth.is_empty(): return
    var weight:=1.0-exp(-dt/maxf(smoothing_seconds,0.0001))
    for path:NodePath in _smooth.keys():
        var target:=_target(path)
        if target==null: continue
        var current=super._read_member(target,path)
        var shadow:Dictionary=_smooth[path]
        shadow.age+=dt
        var goal=shadow.value
        var value=goal
        var angle:bool=_interpolation.get(path,0)==2 or path==^".:rotation" and path in _automatic_paths
        match typeof(goal):
            TYPE_FLOAT: value=lerp_angle(current,goal,weight) if angle else lerpf(current,goal,weight)
            TYPE_VECTOR3:
                if angle: value=Vector3(lerp_angle(current.x,goal.x,weight),lerp_angle(current.y,goal.y,weight),lerp_angle(current.z,goal.z,weight))
                else: value=current.lerp(goal,weight)
            TYPE_VECTOR2,TYPE_VECTOR4,TYPE_COLOR: value=current.lerp(goal,weight)
            TYPE_QUATERNION: value=current.slerp(goal,weight)
            _:
                if shadow.age<smoothing_seconds*0.5: continue
        super._write_member(target,path,value)
        if value==goal: _smooth.erase(path)

func unbind() -> void:
    var actor: Node = _bound_actor.get_ref() if _bound_actor else null
    if actor and game: actor.remove_meta(binding_key(game))
    _bound_actor = null
    _baseline_epoch = -1
    _root_integer_members.clear()
    _root_integer_values.clear()
    _root_integer_bytes.clear()
    _smooth.clear()
    _pending_refs.clear()
    super.unbind()
