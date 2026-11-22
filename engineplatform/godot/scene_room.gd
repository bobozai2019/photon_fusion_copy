class_name NetcodeRoom
extends RefCounted

const Replica = preload("scene_replicator.gd")
const Fields = preload("scene_fields.gd")
const Values = preload("scene_values.gd")
const MODEL_TRANSACTION := 0
const MODEL_PLAYER_ATTACHED := 2
const TARGET_ALL := 0
const TARGET_MASTER := -1
const TARGET_OWNER := -2

class RpcInfo extends RefCounted:
    var sender: int
    var target: int
    var target_node: Node

    func _init(origin:int,route:int,node:Node) -> void:
        sender=origin
        target=route
        target_node=node

var game := NetcodeGame.new()
var _layouts: Dictionary = {}
var _actors: Dictionary = {}
var _scene_nodes: Dictionary = {}
var _scene_keys: Dictionary = {}
var _methods: Dictionary = {}
var _ids: Dictionary = {}
var _receivers: Dictionary = {}
var _open := false
var _rpc_info: RpcInfo
var _pending:Array=[]

func get_rpc_sender() -> int:
    return _rpc_info.sender if _rpc_info else 0

func get_rpc_info() -> RpcInfo:
    return _rpc_info

static func collect_replicas(node:Node, found:Array) -> void:
    if node is Replica: found.append(node)
    for child in node.get_children(): collect_replicas(child,found)

## Replicas in the scene and optional spawn templates form the pre-round catalog.
func open(root:Node,host:bool,address:String,port:int,capacity:int=4,templates:Array[PackedScene]=[],watch:bool=false) -> String:
    if _open or (root==null and templates.is_empty()): return "A new room and a scene root or spawn templates are required"
    var replicas:Array=[]
    if root: collect_replicas(root,replicas)
    for replica:Replica in replicas:
        var actor := replica.replication_root()
        if not actor or (actor != root and not root.is_ancestor_of(actor)):
            return _abort("Sync root_path must point inside the room scene: " + str(root.get_path_to(replica)), [])
        var path:=root.get_path_to(actor)
        var key:=String(path).sha256_text().substr(0,15).hex_to_int()+1
        if _scene_nodes.has(key): return _abort("Multiple scene replicators for "+String(path),[])
        _scene_nodes[key]=weakref(replica)
        _scene_keys[replica.get_instance_id()]=key
    var temporary:Array[Node]=[]
    for scene in templates:
        if not scene: return _abort("Missing spawn template",temporary)
        var prototype:=scene.instantiate()
        temporary.append(prototype)
        collect_replicas(prototype,replicas)
    for replica:Replica in replicas:
        var layout:=replica.definition()
        if layout.is_empty(): return _abort("Invalid replicated properties on "+replica.type_key(),temporary)
        if _actors.has(layout.key) and _actors[layout.key]!=layout:
            return _abort("Different layouts share key "+layout.key,temporary)
        if _layouts.has(layout.id) and not _actors.has(layout.key):
            return _abort("Generated schema ID collision for "+layout.key,temporary)
        _actors[layout.key]=layout
        _layouts[layout.id]=layout.text
        var error:=_declare(replica.replication_root(),layout.id)
        if not error.is_empty(): return _abort(error,temporary)
    for weak:WeakRef in _receivers.values():
        var node:Node=weak.get_ref()
        if node:
            var error:=_declare(node,0)
            if not error.is_empty(): return _abort(error,temporary)
    for node in temporary: node.free()
    if _layouts.is_empty(): return _abort("No replicated objects or annotated broadcast methods",[])
    var text:=""
    for layout:String in _layouts.values(): text+=layout
    var error:=game.open(text,host,address,port,capacity,watch)
    if not error.is_empty(): return _abort(error,[])
    for layout:Dictionary in _actors.values():
        if not layout.writable.is_empty() and not game.model(layout.id,layout.writable,MODEL_PLAYER_ATTACHED if layout.player_attached else MODEL_TRANSACTION,0):
            return _abort("Cannot register writable fields for "+layout.key,[])
    for m:Dictionary in _methods.values():
        if not game.bind_array_method(m.up,m.up_id,m.args,0,m.reliable,_relay.bind(m)):
            return _abort("Cannot register "+m.name+" uplink",[])
        if not game.bind_array_method(m.down,m.down_id,m.args,1,m.reliable,_receive.bind(m)):
            return _abort("Cannot register "+m.name+" delivery",[])
    _open=true
    return ""

func _abort(reason:String,temporary:Array) -> String:
    for node in temporary: node.free()
    close()
    return reason

func close() -> void:
    for key:StringName in game.get_meta_list():
        if String(key).begins_with("netcode_replica_"):
            var replica = game.get_meta(key).get_ref()
            if replica: replica.unbind()
    game.close()
    _open=false
    _methods.clear()
    _layouts.clear()
    _actors.clear()
    _scene_nodes.clear()
    _scene_keys.clear()
    _ids.clear()
    _receivers.clear()
    _pending.clear()

func advance() -> Dictionary:
    var frame:=game.advance()
    if frame.is_empty() or not frame.round:
        _pending.clear()
        return frame
    for key:int in _scene_nodes:
        var replica:Replica=_scene_nodes[key].get_ref()
        if replica==null: continue
        if replica.game==game and replica.reference(): continue
        var id:=game.find_object(0,key)
        if id and (replica.game!=game or replica.object_id!=id): bind_replica(replica,id)
        elif not id and replica.game==game and game.routing_info(replica.object_id,false).is_empty(): replica.unbind()
    var waiting:=_pending
    _pending=[]
    for item in waiting:
        var id:int=item.values[0]
        if id and _node(id)==null and item.m.reliable and Time.get_ticks_msec()<item.until:
            if _open and not game.routing_info(id,false).is_empty(): _pending.append(item)
            continue
        _deliver(item.peer,item.values,item.m)
    return frame

func bind_replica(replica:Replica,id:int) -> bool:
    if not _open or replica==null or id<=0 or (replica.game!=null and replica.game!=game): return false
    var layout:=replica.definition()
    if layout.is_empty() or _actors.get(layout.key,{})!=layout: return false
    if not _declare(replica.replication_root(),layout.id,true).is_empty(): return false
    var state:=game.routing_info(id,false)
    if not state.is_empty() and state.schema!=layout.id: return false
    if replica.game==game and replica.object_id==id: return true
    if not replica.bind(game,id): return false
    replica.room=weakref(self)
    return true

func spawn(replica:Replica) -> int:
    if not _open or replica==null or (replica.game!=null and replica.game!=game): return 0
    if replica.game==game and replica.reference(): return 0
    var layout:=replica.definition()
    if layout.is_empty() or _actors.get(layout.key,{})!=layout: return 0
    replica.room=weakref(self)
    var id:=game.spawn(layout.id,replica.sample_fields(),0,_scene_keys.get(replica.get_instance_id(),0),0)
    if id and not bind_replica(replica,id):
        game.destroy_object(id)
        return 0
    return id

func _replica(node:Object) -> Variant:
    if not node is Node: return null
    var key := Replica.binding_key(game)
    return node.get_meta(key).get_ref() if node.has_meta(key) else null

func node_reference(node:Node) -> int:
    var replica = _replica(node)
    return replica.reference() if replica else 0

func _node(id:int) -> Node:
    var key:="netcode_replica_%d"%id
    if not game.has_meta(key): return null
    var replica:Node=game.get_meta(key).get_ref()
    return replica.replication_root() if replica!=null else null

func register_broadcast_receiver(node:Node) -> bool:
    if node==null or node.get_script()==null: return false
    if _open and not _declare(node,0,true).is_empty(): return false
    _receivers[node.get_instance_id()]=weakref(node)
    return true

func unregister_broadcast_receiver(node:Node) -> void:
    if node: _receivers.erase(node.get_instance_id())

func _declare(node:Node,subject:int,check_only:=false) -> String:
    var script:Script=node.get_script()
    var seen:Dictionary={}
    while script:
        var functions:Dictionary={}
        for info in script.get_script_method_list(): functions[info.name]=info
        var raw=script.get_rpc_config()
        var config:Dictionary=raw if raw is Dictionary else {}
        for method:StringName in config:
            if seen.has(method): continue
            seen[method]=true
            if not functions.has(method): return "Missing RPC signature for "+String(method)
            var error:=_method(subject,method,functions[method],config[method],check_only)
            if not error.is_empty(): return error
        script=script.get_base_script()
    return ""

func _method(subject:int,method:StringName,info:Dictionary,config:Dictionary,check_only:bool) -> String:
    var name:=String(method)
    var key:="%d/%s"%[subject,name]
    var transfer:int=config.get("transfer_mode",MultiplayerPeer.TRANSFER_MODE_UNRELIABLE)
    if transfer==MultiplayerPeer.TRANSFER_MODE_UNRELIABLE_ORDERED or int(config.get("channel",0))!=0:
        return "Unsupported RPC transfer mode/channel on "+name
    var auth:bool=config.get("rpc_mode",MultiplayerAPI.RPC_MODE_AUTHORITY)==MultiplayerAPI.RPC_MODE_AUTHORITY
    var local:bool=config.get("call_local",false)
    var kinds:=PackedStringArray()
    var classes:=PackedStringArray()
    var declarations:=PackedStringArray()
    var capacities:=PackedInt32Array()
    # Object ID, grant epoch, target and original sender share the typed argument budget.
    var available:=252-32
    var variable:=0
    for arg:Dictionary in info.args:
        var kind:=Fields.kind(arg)
        if kind=="": return "Unsupported or untyped RPC argument in "+name+": "+String(arg.name)
        kinds.append(kind)
        classes.append(String(arg.class_name) if kind=="object_ref" else "")
        declarations.append(str(arg.get("hint_string","")))
        if kind=="integer64": available-=10
        elif Fields.WIDTH.has(kind): available-=int(Fields.WIDTH[kind])
        else:
            variable+=1
            available-=2
    if available<0: return "RPC too large: "+name
    var lines:="object_ref 1 . object_id\nobject_ref 2 . epoch\nobject_ref 3 . route\nobject_ref 4 . origin\n"
    for i in kinds.size():
        var kind:=kinds[i]
        var wire:=Fields.wire(kind)
        var line:="%s %d . a%d"%[wire,i+5,i]
        var cap:=0
        if Fields.ELEMENT.has(wire):
            cap=8 if kind=="integer64" else floori(float(available)/variable/int(Fields.ELEMENT[wire]))
            if cap<1: return "RPC too large: "+name
            line+=" %d"%cap
        capacities.append(cap)
        lines+=line+"\n"
    var signature:=(key+lines+str(kinds)+str(declarations)+str(classes)+str(auth)+str(local)+str(transfer)).sha256_text()
    if _methods.has(key):
        return "" if _methods[key].signature==signature else "Conflicting RPC declarations for "+key
    if check_only: return "Unregistered RPC "+key
    var schema:=Fields.wire_id("args:"+signature)
    var up:=Fields.wire_id("up:"+signature)
    var down:=Fields.wire_id("down:"+signature)
    if _layouts.has(schema) or _ids.has(up) or _ids.has(down) or up==down:
        return "Generated ID collision for "+key
    _layouts[schema]="schema %d 1 RpcArgs\n"%schema+lines
    _ids[up]=key
    _ids[down]=key
    _methods[key]={"signature":signature,"name":method,"subject":subject,"authority":auth,"local":local,
        "reliable":transfer==MultiplayerPeer.TRANSFER_MODE_RELIABLE,"kinds":kinds,"classes":classes,
        "hints":declarations,"capacities":capacities,"defaults":info.get("default_args",[]),
        "args":schema,"up":"u"+signature,"down":"d"+signature,"up_id":up,"down_id":down}
    return ""

func rpc(method:Callable,...args:Array) -> bool:
    return _send(TARGET_ALL,method,args)

func rpc_to(target:int,method:Callable,...args:Array) -> bool:
    return target in [TARGET_ALL,TARGET_MASTER,TARGET_OWNER] and _send(target,method,args)

func rpc_to_player(player:int,method:Callable,...args:Array) -> bool:
    return player>0 and _send(player,method,args)

func _send(target:int,method:Callable,args:Array) -> bool:
    if not _open or not method.is_valid() or method.get_bound_arguments_count()!=0 or method.get_unbound_arguments_count()!=0: return false
    var node=method.get_object()
    if not node is Node or not _annotated(node,method.get_method()): return false
    var replica=_replica(node)
    var id:int=replica.object_id if replica else 0
    var subject:int=replica.schema_id if replica else 0
    if not replica and not _receivers.has(node.get_instance_id()): return false
    var m:Dictionary=_methods.get("%d/%s"%[subject,method.get_method()],{})
    var state:=game.routing_info(id,false)
    if m.is_empty() or state.is_empty() or state.schema!=subject or state.watch: return false
    if target==TARGET_OWNER and not id: return false
    if m.authority and state.writer!=state.player: return false
    var missing:int=m.kinds.size()-args.size()
    var defaults:Array=m.defaults
    if missing<0 or missing>defaults.size(): return false
    if missing:
        args.append_array(defaults.slice(defaults.size()-missing).duplicate(true))
    var values:Array=[id,state.epoch,target,0]
    for i in args.size():
        if m.kinds[i]=="object_ref":
            var arg=args[i]
            if arg!=null and (not arg is Node or not Fields.matches_node(arg,m.classes[i])): return false
            var handle:=node_reference(arg) if arg!=null else 0
            if arg!=null and not handle: return false
            values.append(handle)
        elif m.kinds[i]!=Fields.wire(m.kinds[i]):
            if m.kinds[i] in ["array_value","dictionary_value"]:
                args[i]=Fields.collection(args[i],m.hints[i])
                if args[i]==null: return false
            var encoded=Values.new().encode(args[i],m.kinds[i],m.capacities[i])
            if encoded==null: return false
            values.append(encoded)
        else: values.append(args[i])
    if not game.call_method(m.up,0,values): return false
    if m.local: _dispatch(id,m,state.player,target,args)
    return true

# Only the coordinator forwards calls. Use the authenticated uplink sender, object schema,
# current grant and confirmed visibility; never use a player identity supplied by the caller.
func _relay(sender:int,_scope:int,values:Array,m:Dictionary) -> void:
    var id:int=values[0]
    var target:int=values[2]
    var state:=game.routing_info(id)
    if state.is_empty() or state.player!=state.master or state.schema!=m.subject: return
    if sender not in state.audience or (m.authority and (state.writer!=sender or state.epoch!=values[1])): return
    if target==TARGET_OWNER:
        if not id or state.epoch!=values[1]: return
        target=state.writer
    elif target==TARGET_MASTER: target=state.master
    elif target<0: return
    var delivery:=values.duplicate()
    delivery[1]=state.epoch
    delivery[3]=sender
    for player:int in state.audience:
        if player!=sender and (target==TARGET_ALL or target==player):
            game.call_method(m.down,0,delivery,player)

func _receive(peer:int,_scope:int,values:Array,m:Dictionary) -> void:
    if _pending.size()<256:
        _pending.append({"peer":peer,"values":values,"m":m,"until":Time.get_ticks_msec()+2000})

func _deliver(peer:int,values:Array,m:Dictionary) -> void:
    var id:int=values[0]
    var state:=game.routing_info(id,false)
    if state.is_empty() or peer!=state.master or state.schema!=m.subject: return
    if values[2]==TARGET_OWNER and (state.epoch!=values[1] or state.player!=state.writer): return
    if m.authority and (state.epoch!=values[1] or state.writer!=values[3]): return
    var args:=values.slice(4)
    for i in args.size():
        if m.kinds[i]=="object_ref":
            var node:=_node(game.resolve_field(args[i]))
            args[i]=node if node!=null and Fields.matches_node(node,m.classes[i]) else null
        elif m.kinds[i]!=Fields.wire(m.kinds[i]):
            args[i]=Values.new().decode(args[i],m.kinds[i])
            if args[i]!=null and m.kinds[i] in ["array_value","dictionary_value"]:
                args[i]=Fields.collection(args[i],m.hints[i])
            if args[i]==null: return
    _dispatch(id,m,values[3],values[2],args)

func _dispatch(id:int,m:Dictionary,sender:int,target:int,args:Array) -> void:
    var previous:=_rpc_info
    var target_node:=_node(id) if id else null
    _rpc_info=RpcInfo.new(sender,target,target_node)
    if id:
        if target_node and _annotated(target_node,m.name): target_node.callv(m.name,args)
    else:
        for instance in _receivers.keys():
            var weak:WeakRef=_receivers.get(instance)
            var node:Node=weak.get_ref() if weak else null
            if node==null: _receivers.erase(instance)
            elif node.has_method(m.name) and _annotated(node,m.name): node.callv(m.name,args)
    _rpc_info=previous

func _annotated(node:Node,method:StringName) -> bool:
    var script:Script=node.get_script()
    while script:
        var config=script.get_rpc_config()
        if config is Dictionary and config.has(method): return true
        script=script.get_base_script()
    return false
