extends "res://check.gd"

const KeyOne = preload("res://key_one.tscn")
const KeyTwo = preload("res://key_two.tscn")
const Transaction = preload("res://transaction.tscn")
const Server = preload("res://server.tscn")
const Spatial = preload("res://spatial.tscn")
const SpatialPosition = preload("res://spatial_position.tscn")
const Reference = preload("res://reference.tscn")
const Fragile = preload("res://fragile.tscn")
const SetterReject = preload("res://setter_reject.tscn")
const DetachingProxy = preload("res://detaching_proxy.tscn")
const SignalReject = preload("res://signal_reject.tscn")
const SUSTAINED_INPUTS := 360
const HOST_SIGNED_VALUE: int = -9223372036854775807 - 1
const GUEST_SIGNED_VALUE: int = 9223372036854775807
const RpcReceiver = preload("res://rpc_receiver.gd")

var _pool: Node
var _actors: Dictionary = {}
var _exit_count := 0
var _entered_count := 0
var _requests := 0
var _responses: Array[bool] = []
var _rpc_error := ""
var _inputs := PackedInt32Array()
var _replayed := 0
var _spawn_errors := 0
var _stale_rpc: NetcodeRpcResult
var _receiver: Node
var _other_receiver: Node
var _server_authority_changes := 0
var _reference_first_value := 0
var _getter_detach_reads := -1
var _proxy_detach_writes := -1

func _run() -> void:
    var args := OS.get_cmdline_user_args()
    role = args[0]
    var port := int(args[1])
    var host := role.ends_with("_host")
    var denial := role.begins_with("reject")
    var cancellation := role.begins_with("cancel")
    var simulation := role.begins_with("simulation")
    var lifecycle := role.begins_with("lifecycle")
    var detached := role.begins_with("authority_detach")
    var catalog: Array[PackedScene] = []
    if denial or cancellation: catalog.assign([Orb])
    elif detached: catalog.assign([Transaction])
    elif simulation: catalog.assign([Server, Spatial, SpatialPosition])
    elif lifecycle: catalog.assign([Reference, Other, Fragile, SetterReject, SignalReject, DetachingProxy])
    else: catalog.assign([Transaction, KeyOne, KeyTwo])
    _configure(catalog)
    if role.begins_with("shared"):
        _receiver = RpcReceiver.new()
        root.add_child(_receiver)
        _other_receiver = RpcReceiver.new()
        root.add_child(_other_receiver)
    Netcode.connection_failed.connect(func(reason: String): check(false, reason))
    Netcode.interest_exit.connect(func(_node: Node): _exit_count += 1)
    Netcode.interest_enter.connect(func(_node: Node): _entered_count += 1)
    if host:
        var options := NetcodeRoomOptions.new()
        options.max_players = 2
        options.custom_properties = {"host_step": 0, "guest_step": 0}
        options.allow_guest_spawns = not denial
        if simulation:
            options.retain_spatial_writer_interest = true
        check(Netcode.create_room("127.0.0.1", port, options) == OK, "advanced create_room")
    else:
        check(Netcode.join_room("127.0.0.1", port) == OK, "advanced join_room")
    if not await wait_for(Netcode.is_simulation_ready, "advanced simulation ready"):
        _finish(role)
        return
    if host: _touch(args[2].path_join("host.ready"))
    if denial:
        if not host: await _denial()
    elif cancellation:
        if host: await _cancel_host()
        else: await _cancel_guest()
    elif simulation:
        if host: await _simulation_host()
        else: await _simulation_guest()
    elif lifecycle:
        if host: await _lifecycle_host()
        else: await _lifecycle_guest()
    elif detached:
        if host: await _authority_detach_host()
        else: await _authority_detach_guest()
    elif host:
        await _shared_host()
    else:
        await _shared_guest()
    if host:
        await wait_for(func(): return FileAccess.file_exists(args[2].path_join("leave")), "advanced host leave")
    Netcode.leave_room()
    _finish(role)

func _configure(catalog: Array[PackedScene]) -> void:
    _pool = Node.new()
    root.add_child(_pool)
    primary = NetcodeSpawner.new()
    primary.spawnable_scenes = catalog
    _pool.add_child(primary)
    primary.spawned.connect(func(actor: Node):
        var template := actor.scene_file_path.get_file().get_basename()
        _actors[template] = actor
        if role == "lifecycle_guest" and template == "reference":
            _reference_first_value = actor.value)
    primary.spawn_failed.connect(func(actor: Node, reason: String):
        if role == "reject_guest":
            _spawn_errors += 1
            check("rejected" in reason, "rejection reason")
        elif role == "lifecycle_guest":
            _spawn_errors += 1
            if actor == _actors.get("detaching_proxy"):
                check("exited the tree" in reason and actor.value == HOST_SIGNED_VALUE and
                    actor.trail_writes == _proxy_detach_writes, "proxy setter detaches before applying the next changed field")
        elif role == "authority_detach_host":
            check("exited the tree" in reason, "host retires the actor after sync detached")
            _spawn_errors += 1
        elif role == "lifecycle_host" and _getter_detach_reads >= 0:
            check("exited the tree" in reason and actor.trail_reads == _getter_detach_reads,
                "detaching in a root getter stops sampling later fields")
            _spawn_errors += 1
        else:
            check(false, reason))

func _lifecycle_host() -> void:
    check(primary.spawn(null, func(_sync, actor): actor.set_meta("reject_mount", true)) == null, "ready cancellation rolls back")
    check(primary.spawn(null, func(_sync, actor): actor.set_meta("cancel_in_ready", primary)) == null, "despawn in ready stops spawn")
    var target := primary.spawn(Other, _set_value.bind(2))
    var actor := primary.spawn(Reference, func(_sync, node):
        node.value = 1
        node.target = target
        node.levels.assign([4, 7])
        node.ledger["value"] = 73)
    var fragile := primary.spawn(Fragile, _set_value.bind(3))
    if actor == null or target == null or fragile == null:
        check(false, "ready actor, reference target and fragile observer mounted")
        return
    actor.value = 73
    actor.levels[1] = 9
    var setter_reject := primary.spawn(SetterReject, _set_value.bind(74))
    check(setter_reject != null, "observer setter fixture")
    var proxy := primary.spawn(DetachingProxy, _set_value.bind(75))
    check(proxy != null, "change-only observer fixture")
    check(primary.spawn(SignalReject, _set_value.bind(75)) != null, "observer signal fixture")
    if not await _wait_step("guest_step", 1): return
    check(actor.value == 73 and actor.target == target, "author retains immediate writes")
    if setter_reject == null or proxy == null: return
    proxy.value = HOST_SIGNED_VALUE
    proxy.trail = PackedFloat32Array([7.0])
    _getter_detach_reads = setter_reject.trail_reads
    setter_reject.detach_on_value_read = true
    if not await wait_for(func(): return _spawn_errors == 1, "host retires actor detached by root getter"): return
    await _wait_step("guest_step", 2)

func _lifecycle_guest() -> void:
    var all_mounted := func(): return _actors.has_all(["reference", "other", "detaching_proxy"]) and _spawn_errors == 2
    if not await wait_for(all_mounted,
        "mounted replicas and isolated ready and setter failures"): return
    check(_reference_first_value == 73, "first host baseline includes the assignment after spawn returned")
    if not await wait_for(func(): return _actors.reference.value == 73 and _actors.reference.target == _actors.other,
        "first host write and reference resolve on guest"): return
    check(_actors.reference.levels == [4, 9] and _actors.reference.ledger == {"value": 73},
        "typed collections include the immediate post-spawn mutation")
    await process_frame
    check(_pool.get_child_count() == 4 and not _actors.has("fragile") and not _actors.has("signal_reject"),
        "failed mounts left no ghosts or later spawned signals")
    var proxy: Node = _actors.detaching_proxy
    check(proxy.get_node("Sync").is_proxy_changes_only_enabled() and proxy.value == 75, "change-only fixture baseline")
    _proxy_detach_writes = proxy.trail_writes
    proxy.detach_on_value_write = true
    _step("guest_step", 1)
    if not await wait_for(func(): return _spawn_errors == 3, "detached proxy retired once"): return
    _step("guest_step", 2)
    await _wait_step("guest_step", 2)

func _denial() -> void:
    var actor := primary.spawn(null, _set_value.bind(731))
    check(is_instance_valid(actor), "provisional guest instance")
    var reference: WeakRef = weakref(actor)
    await wait_for(func(): return _spawn_errors == 1 and reference.get_ref() == null, "rejected instance freed")
    check(not is_instance_valid(_actors.get("orb")), "guest rejection leaves an empty world")

func _cancel_host() -> void:
    if not await _wait_step("guest_step", 1): return
    check(_pool.get_child_count() == 1, "cancelled request did not leave an authoritative object")
    _step("host_step", 1)
    if not await wait_for(func(): return Netcode.get_room().get_player_count() == 1, "pending creator left"): return
    await create_timer(0.25).timeout
    check(_pool.get_child_count() == 1, "late confirmation did not leave a ghost")

func _cancel_guest() -> void:
    var actor := primary.spawn(null, _set_value.bind(731))
    if not is_instance_valid(actor):
        check(false, "cancel provisional instance")
        return
    var first: WeakRef = weakref(actor)
    await process_frame
    await process_frame
    check(primary.despawn(actor), "cancel before confirmation")
    if not await wait_for(func(): return first.get_ref() == null, "cancel frees the local actor"): return
    _step("guest_step", 1)
    if not await _wait_step("host_step", 1): return
    var departing := primary.spawn(null, _set_value.bind(732))
    if not is_instance_valid(departing):
        check(false, "pending actor returned on departure")
        return
    var second: WeakRef = weakref(departing)
    await process_frame
    await process_frame
    Netcode.leave_room()
    await wait_for(func(): return second.get_ref() == null, "leave frees the pending local actor")

func _set_value(_sync: NetcodeReplicator, actor: Node, value: int) -> void:
    actor.value = value

func _authority_detach_host() -> void:
    var actor := primary.spawn(Transaction)
    if actor == null:
        check(false, "authority detach fixture spawned")
        return
    var sync: NetcodeSharedReplicator = actor.get_node("Sync")
    sync.authority_response.connect(func(approved: bool): _responses.append(approved))
    sync.authority_requested.connect(func(requester: int) -> bool:
        _requests += 1
        if _requests == 1: return true
        actor.remove_child(sync)
        check(not sync.answer_authority(requester, true), "detached sync cannot submit an old answer")
        sync.queue_free()
        _step("host_step", 2)
        return true)
    if not await _wait_step("guest_step", 1): return
    check(not sync.has_authority() and sync.want_authority(true), "host requests the guest writer's approval")
    if not await wait_for(func(): return _responses.size() == 1 and sync.is_write_ready(), "host ordinary request approved"): return
    check(_responses[0], "host receives the positive response")
    _step("host_step", 1)
    if await _wait_step("guest_step", 2):
        check(_spawn_errors == 1, "detached actor retired once")

func _authority_detach_guest() -> void:
    if not await wait_for(func(): return _actors.has("transaction"), "replica for detach fixture"): return
    var actor: Node = _actors.transaction
    var sync: NetcodeSharedReplicator = actor.get_node("Sync")
    var actor_ref: WeakRef = weakref(actor)
    sync.authority_requested.connect(func(requester: int) -> bool: return requester == 1)
    check(sync.want_authority(true), "guest prompts host writer")
    if not await wait_for(sync.is_write_ready, "guest gets the first write grant"): return
    _step("guest_step", 1)
    if not await _wait_step("host_step", 1): return
    if not await wait_for(func(): return not sync.has_authority(), "guest observes the approved transfer"): return
    check(sync.want_authority(true), "guest prompts host again before detach")
    if not await _wait_step("host_step", 2): return
    if not await wait_for(func(): return actor_ref.get_ref() == null, "detached authority actor despawned"): return
    _step("guest_step", 2)
    await _wait_step("guest_step", 2)

func _shared_host() -> void:
    check(not Netcode.add_user_key(1) and not Netcode.remove_user_key(1), "coordinator has no local user filter")
    var actor := primary.spawn(Transaction, _set_value.bind(431))
    var one := primary.spawn(KeyOne, _set_value.bind(901))
    var two := primary.spawn(KeyTwo, _set_value.bind(902))
    if not actor or not one or not two:
        check(false, "host spawned global and two keyed templates")
        return
    var keyed_sync := one.get_node("Sync") as NetcodeSharedReplicator
    keyed_sync.authority_requested.connect(func(requester: int) -> bool: return requester == 2)
    var reclaim_frame := [-1]
    var authority_frame := [-1]
    one.value_written = func(next: int):
        if next == GUEST_SIGNED_VALUE and reclaim_frame[0] < 0:
            reclaim_frame[0] = Engine.get_physics_frames()
            check(keyed_sync.reclaim_authority(), "reclaim change-only object from its proxy setter")
    keyed_sync.authority_changed.connect(func(owns: bool):
        if owns and reclaim_frame[0] >= 0 and authority_frame[0] < 0:
            authority_frame[0] = Engine.get_physics_frames())
    if not await _wait_step("proxy_guest", 1): return
    one.value = HOST_SIGNED_VALUE
    if not await _wait_step("proxy_guest", 2): return
    one.trail = PackedFloat32Array([7.0])
    if not await _wait_step("proxy_guest", 3): return
    if not await wait_for(func(): return one.value == GUEST_SIGNED_VALUE, "former proxy publishes the maximum signed integer"): return
    if not await wait_for(keyed_sync.is_write_ready, "reclaimed change-only object is ready"): return
    check(reclaim_frame[0] >= 0 and authority_frame[0] == reclaim_frame[0],
        "proxy setter's writer change is reported in the same physics step")
    one.value_written = Callable()
    var sync := actor.get_node("Sync") as NetcodeSharedReplicator
    sync.authority_requested.connect(func(requester: int, source: Object) -> bool:
        _requests += 1
        check(requester == 2 and source == sync, "requester and source object passed to synchronous voter")
        if _requests == 2:
            _stale_rpc = sync.rpc_to(Netcode.TARGET_OWNER, actor.note_value, 1000)
        return _requests == 2, CONNECT_APPEND_SOURCE_OBJECT)
    var source_only := func(source: Object) -> bool:
        check(source == sync, "source object stays in the argument list after unbind")
        return false
    sync.authority_requested.connect(source_only.unbind(1), CONNECT_APPEND_SOURCE_OBJECT)
    sync.authority_response.connect(func(approved: bool): _responses.append(approved))
    if not await wait_for(func(): return actor.value == 500, "guest first authorized write"): return
    check(_requests == 2 and not sync.has_authority(), "denial then transfer")
    check(_stale_rpc.is_finished() and not _stale_rpc.get_error().is_empty(), "old owner epoch rejected")
    var result := sync.rpc_to(Netcode.TARGET_OWNER, actor.note_value, 49) as NetcodeRpcResult
    if not await wait_for(result.is_finished, "owner RPC receipt"): return
    check(result.get_error().is_empty(), "owner RPC delivered")
    if not await wait_for(func(): return actor.value == 549, "owner RPC changed guest state"): return
    check(sync.want_authority(true), "host requests authority from the current guest writer")
    if not await wait_for(func(): return _responses.size() == 1, "host receives a response"): return
    check(not _responses[0] and not sync.has_authority(), "owner can refuse a normal host request")
    check(sync.reclaim_authority(), "host can explicitly reclaim writer as coordinator")
    check(not sync.is_write_ready(), "new owner baseline still pending")
    var early := sync.rpc_to(Netcode.TARGET_OWNER, actor.note_value, 3) as NetcodeRpcResult
    var remote := sync.rpc_to(Netcode.TARGET_ALL, actor.note_remote) as NetcodeRpcResult
    if not await wait_for(early.is_finished, "early owner RPC receipt"): return
    check(early.get_error().is_empty() and actor.value == 552, "queued until baseline then invoked once")
    if not await wait_for(remote.is_finished, "annotated broadcast receipt"): return
    check(remote.get_error().is_empty() and actor.remote_calls == 0, "call_remote skips sender")
    if not await wait_for(sync.is_write_ready, "host writer baseline"): return
    var reference := Netcode.rpc_to_player(2, actor.note_reference, one) as NetcodeRpcResult
    if not await wait_for(reference.is_finished, "singleton player RPC result"): return
    check(reference.get_error().is_empty() and actor.reference_calls.is_empty(), "annotated player RPC uses object context")
    var broadcast := Netcode.rpc(_receiver.room_message, "hello") as NetcodeRpcResult
    check(_received_broadcast(), "host call_local runs before rpc returns")
    if not await wait_for(broadcast.is_finished, "singleton broadcast result"): return
    check(broadcast.get_error().is_empty() and _received_broadcast(),
        "broadcast includes sender once and has room context: %s %s" % [broadcast.get_error(), _receiver.messages])
    var collections := Netcode.rpc(_receiver.room_values.unbind(1), [[7]], "discarded") as NetcodeRpcResult
    if not await wait_for(collections.is_finished, "broadcast with unbound tail argument"): return
    check(collections.get_error().is_empty() and _received_collections(), "broadcast copies arguments per local handler")
    check(Netcode.get_rpc_info() == null and Netcode.get_rpc_sender() == 0, "RPC context is scoped to the handler")
    var values := [[8]]
    var local_owner := Netcode.rpc_to(Netcode.TARGET_OWNER, actor.note_local, values) as NetcodeRpcResult
    check(actor.local_calls == 1 and values == [[8]], "ready coordinator owner runs synchronously with independent arguments")
    if not await wait_for(local_owner.is_finished, "local-owner receipt"): return
    check(local_owner.get_error().is_empty() and actor.local_calls == 1, "local owner executes only once")
    actor.value = 937
    if not await _wait_step("guest_step", 1): return
    check(_receiver.messages.size() == 2 and _other_receiver.messages.size() == 2 and
        _receiver.messages[1].slice(0, 3) == ["from guest", 2, Netcode.TARGET_ALL],
        "host receives the guest call once per receiver")
    check(_receiver.outer_senders == [2, 2] and _receiver.nested_senders == [1, 1] and
        Netcode.get_rpc_sender() == 0, "nested local RPC restores the remote sender then clears the context")
    var hidden := Netcode.rpc_to_player(2, actor.note_reference, two) as NetcodeRpcResult
    if not await wait_for(hidden.is_finished, "hidden reference result"): return
    check(hidden.get_error().is_empty(), "reference out of scope arrives as null")
    one.value = 911
    two.value = 912
    _step("host_step", 1)
    if not await _wait_step("guest_step", 2): return
    check(primary.despawn(one) and primary.despawn(two), "destroy both hidden objects")
    _step("host_step", 2)
    await _wait_step("guest_step", 3)

func _shared_guest() -> void:
    check(primary.spawn(KeyOne) == null, "unsubscribed guest cannot create a keyed template")
    check(Netcode.add_user_key(1) and Netcode.add_user_key(2), "subscribe both user keys")
    if not await wait_for(func(): return _actors.size() == 3, "all template groups arrive"): return
    var actor: Node = _actors.transaction
    var one: Node = _actors.key_one
    var two: Node = _actors.key_two
    var sync: NetcodeSharedReplicator = actor.get_node("Sync")
    sync.authority_requested.connect(func(requester: int, source: Object):
        check(requester == 1 and source == sync and sync.answer_authority(requester, false),
            "deferred explicit answer receives source object"),
        CONNECT_DEFERRED | CONNECT_ONE_SHOT | CONNECT_APPEND_SOURCE_OBJECT)
    check(one.value == 901 and two.value == 902, "keyed baselines")
    check(one.get_node("Sync").is_proxy_changes_only_enabled(),
        "binding uses the change-only setting configured in ready")
    check(not two.get_node("Sync").is_proxy_changes_only_enabled(), "default binding keeps local correction")
    var baseline_reads: int = one.trail_reads
    var baseline_writes: int = one.trail_writes
    check(baseline_writes == 1, "change-only proxy applies a full initial baseline")
    one.trail = PackedFloat32Array([99.0])
    _step("proxy_guest", 1)
    if not await wait_for(func(): return one.value == HOST_SIGNED_VALUE, "change-only proxy receives the minimum signed integer"): return
    check(one.trail_reads == baseline_reads and one.trail_writes == baseline_writes + 1,
        "stable proxy field is neither sampled nor reapplied")
    check(one.trail == PackedFloat32Array([99.0]), "local proxy edit remains until that field changes")
    _step("proxy_guest", 2)
    if not await wait_for(func(): return one.trail_writes > baseline_writes + 1, "changed proxy field applied"): return
    check(one.trail == PackedFloat32Array([7.0]), "changed proxy field overwrites the local edit")
    one.trail = PackedFloat32Array([99.0])
    _step("proxy_guest", 3)
    var keyed_sync := one.get_node("Sync") as NetcodeSharedReplicator
    check(keyed_sync.want_authority(true), "change-only observer requests write authority")
    if not await wait_for(keyed_sync.is_write_ready, "change-only observer applies writer baseline"): return
    check(one.trail == PackedFloat32Array([7.0]), "writer baseline replaces the former proxy's local edit")
    one.value = GUEST_SIGNED_VALUE
    if not await wait_for(func(): return not keyed_sync.has_authority(), "former writer returns to proxy mode"): return
    var collision := Netcode.rpc_to(Netcode.TARGET_MASTER, sync.set_user_keys, PackedInt64Array([99])) as NetcodeRpcResult
    check(collision.is_finished() and not collision.get_error().is_empty(), "root's RPC list cannot expose a same-name native setter")
    var unauthorized := sync.rpc_to(Netcode.TARGET_MASTER, actor.note_remote) as NetcodeRpcResult
    check(unauthorized.is_finished() and "authority" in unauthorized.get_error(), "annotated RPC rejects non-writer")
    sync.authority_response.connect(func(approved: bool): _responses.append(approved))
    check(sync.want_authority(true), "request remote write access")
    if not await wait_for(func(): return _responses.size() == 1, "observed rejection"): return
    check(not _responses[0] and not sync.has_authority(), "rejected guest remains read only")
    check(sync.want_authority(true), "request again after rejection")
    if not await wait_for(func(): return _responses.size() == 2 and sync.is_write_ready(), "accepted and writable"): return
    check(_responses[1], "approval observable")
    actor.value = 500
    var owner_rpc := Netcode.rpc_to(Netcode.TARGET_OWNER, actor.note_local, [[8]]) as NetcodeRpcResult
    check(actor.local_calls == 0, "guest owner waits for the coordinator to validate its epoch")
    if not await wait_for(owner_rpc.is_finished, "guest owner receives validated RPC"): return
    check(owner_rpc.get_error().is_empty() and actor.local_calls == 1, "guest owner executes once after validation")
    var missing := sync.rpc_to(999, actor.note_value, 1) as NetcodeRpcResult
    missing.on_fail(func(reason: String): _rpc_error = reason)
    if not await wait_for(func(): return not _rpc_error.is_empty(), "unavailable RPC invokes failure callback"): return
    check("unavailable" in _rpc_error, "explicit unavailable result")
    var missing_signal := [false]
    var missing_no_args := sync.rpc_to(999, actor.note_value, 1) as NetcodeRpcResult
    check(not missing_no_args.is_finished(), "missing player is an asynchronous failure")
    missing_no_args.on_fail(func(): missing_signal[0] = true, 2.0)
    if not await wait_for(func(): return missing_signal[0], "asynchronous failure invokes a zero-argument callback"): return
    var plugin := sync.rpc_to(Netcode.TARGET_PLUGIN, actor.note_value, 1) as NetcodeRpcResult
    check(plugin.is_finished() and "unsupported" in plugin.get_error(), "unsupported plugin RPC")
    missing_signal[0] = false
    plugin.on_fail(func(): missing_signal[0] = true, 2.0)
    check(missing_signal[0], "immediate failure invokes a zero-argument callback synchronously")
    var failure_args: Array = []
    plugin.on_fail(func(...args): failure_args.assign(args), 2.0)
    check(failure_args == [plugin.get_error()], "variadic failure callback still receives the reason")
    if not await wait_for(func(): return actor.value == 937 and not sync.has_authority(), "host reclaimed writer"): return
    if not await wait_for(func(): return not actor.trail_after_setter.is_empty(), "observer ran the value setter"): return
    check(actor.trail_after_setter == PackedFloat32Array([0.0]), "later field restored in the same apply pass")
    check(sync.authority_requested.get_connections().is_empty(), "deferred one-shot voter was disconnected")
    check(actor.remote_calls == 1, "annotated remote callable ran once")
    check(actor.reference_calls == [[one, 1, Netcode.get_local_player_id(), true]], "target node and argument resolve locally")
    check(_received_broadcast(),
        "registered broadcast arrived once: %s" % [_receiver.messages])
    check(_received_collections(), "two remote handlers see independent arguments")
    check(actor.local_calls == 1, "coordinator's owner-only RPC did not run on the guest")
    var local_guest := Netcode.rpc(_receiver.room_message, "from guest") as NetcodeRpcResult
    check(_receiver.messages.size() == 2 and _other_receiver.messages.size() == 2,
        "guest call_local runs before rpc returns")
    if not await wait_for(local_guest.is_finished, "guest broadcast result"): return
    check(local_guest.get_error().is_empty() and _receiver.messages.size() == 2 and _other_receiver.messages.size() == 2,
        "no echo of the guest call_local RPC")
    # Remove keys independently so each exit can be attributed to its own subscription.
    check(Netcode.remove_user_key(1), "unsubscribe key one")
    var first_instance: WeakRef = weakref(one)
    if not await wait_for(func(): return first_instance.get_ref() == null, "key one actor freed"): return
    check(two.get_node("Sync").get_object_id() > 0 and is_instance_valid(actor), "key two and global retained")
    check(Netcode.remove_user_key(2), "unsubscribe key two")
    if not await wait_for(func(): return two.get_node("Sync").get_object_id() == 0, "key two replica retained but unbound"): return
    _step("guest_step", 1)
    if not await _wait_step("host_step", 1): return
    check(actor.reference_calls.size() == 2 and actor.reference_calls[1] == [null, 1, 2, true],
        "hidden reference decoded to null")
    check(Netcode.add_user_key(1) and Netcode.add_user_key(2), "resubscribe both keys")
    if not await wait_for(func(): return is_instance_valid(_actors.key_one) and _actors.key_one.value == 911 and two.value == 912,
        "reentry installs newest baselines"): return
    check(_actors.key_one.trail == PackedFloat32Array([7.0]) and _actors.key_one.trail_writes == 1,
        "change-only proxy restores an unchanged field on reentry")
    check(_actors.key_two == two and _entered_count >= 1, "retained actor identity restored")
    check(Netcode.remove_user_key(1) and Netcode.remove_user_key(2), "hide both before world destruction")
    if not await wait_for(func(): return _exit_count >= 4, "both hidden again"): return
    var retained: WeakRef = weakref(two)
    _step("guest_step", 2)
    if not await _wait_step("host_step", 2): return
    if not await wait_for(func(): return retained.get_ref() == null, "hidden retained actor released by world destruction"): return
    check(Netcode.add_user_key(1) and Netcode.add_user_key(2), "subscribe after destruction")
    await create_timer(0.45).timeout
    check(not is_instance_valid(_actors.key_one) and not is_instance_valid(_actors.key_two), "destroyed objects do not reappear")
    check(Netcode.remove_user_key(1) and Netcode.add_user_key(1), "restore creation key in the same frame")
    var created := primary.spawn(KeyOne, _set_value.bind(913))
    if created == null:
        check(false, "subscribed guest may create a keyed instance")
        return
    if not await wait_for(created.get_node("Sync").is_write_ready, "keyed guest creation confirms"): return
    _step("guest_step", 3)
    await _wait_step("guest_step", 3)

func _simulation_host() -> void:
    if not await wait_for(func(): return Netcode.get_room().get_player_count() == 2, "prediction client room seat"): return
    var server := primary.spawn(Server, func(sync: NetcodeServerReplicator, actor: Node):
        actor.value = 0
        check(sync.set_input_authority(2) and sync.get_input_authority() == 2, "stage input authority before spawn"))
    var near := primary.spawn(Spatial, _place.bind(801, 2.0))
    var far := primary.spawn(Spatial, _place.bind(802, 26.0))
    var plain := primary.spawn(SpatialPosition, func(_sync: NetcodeReplicator, actor: Node):
        actor.set("position_xz", Vector2(2, 2))
        actor.set("value", 804))
    if not server or not near or not far or not plain:
        check(false, "spawn predicted actor and spatial pair")
        return
    var sync: NetcodeServerReplicator = server.get_node("Sync")
    sync.on_process_input.connect(_simulate.bind(server, true))
    sync.authority_changed.connect(func(granted: bool): _server_authority_changes += int(granted))
    check(sync.get_input_authority() == 2, "staged input authority is active after spawn")
    if not await _wait_step("guest_step", 1): return
    near.value = 803
    await physics_frame
    var reads: int = plain.position_reads
    for i in 4: await physics_frame
    check(plain.position_reads - reads == 4, "AOI and authority sampling share one coordinate read per physics step")
    check(plain.get_node("Sync").want_authority(true, 4), "configure a throttled spatial writer")
    plain.set("position_xz", Vector2(2, 26))
    plain.set("value", 805)
    _step("host_step", 1)
    if not await wait_for(func(): return sync.get_confirmed_input() == 12, "host consumes all commands"): return
    var expected := PackedInt32Array()
    for i in 12: expected.append(i + 1)
    check(_inputs == expected and server.value == 24 and sync.get_input_queue_count() == 0, "host inputs unique, ordered and drained")
    check(_server_authority_changes == 1, "host model invokes its state authority signal once")
    if not await _wait_step("guest_step", 2): return
    if not await wait_for(func(): return _actors.spatial.value == 812, "host applies an out-of-area writer's value"): return
    check(_actors.spatial.get_node("Sync").reclaim_authority(), "host reclaims the out-of-area writer")
    check(sync.get_confirmed_input() == 12 + SUSTAINED_INPUTS and server.value == 24 + SUSTAINED_INPUTS * 2,
        "host consumed sustained inputs")
    print("NATIVE prediction host confirmed=", sync.get_confirmed_input(), " value=", server.value)
    check(sync.set_input_authority(Netcode.get_local_player_id()), "host takes input authority")
    if not await _wait_step("guest_step", 3): return
    check(sync.queue_input(PackedByteArray([255])), "host submits observer interpolation stimulus")
    check(server.smoothed == 100.0 and sync.get_confirmed_input() == 1 and sync.get_input_queue_count() == 0,
        "coordinator commits its input before queue_input returns")
    if not await _wait_step("guest_step", 4): return
    check(not sync.queue_input(PackedByteArray([252])), "unbound actor's in-progress input cannot commit")
    await _wait_step("guest_step", 5)

func _simulation_guest() -> void:
    var observer := Node2D.new()
    var area := NetcodeInterestArea.new()
    area.radius = 0
    root.add_child(observer)
    observer.add_child(area)
    var observer_3d := Node3D.new()
    var area_3d := NetcodeInterestArea.new()
    area_3d.radius = 0
    root.add_child(observer_3d)
    observer_3d.add_child(area_3d)
    if not await wait_for(func(): return _actors.has("server") and _actors.has("spatial"), "one spatial cell and server visible"): return
    if not await wait_for(func(): return _actors.has("spatial_position"), "plain Node's position property selects its XZ cell"): return
    var near: Node = _actors.spatial
    var first_instance: WeakRef = weakref(near)
    var plain_instance: WeakRef = weakref(_actors.spatial_position)
    check(near.value == 801, "only near cell initially visible")
    check(_actors.spatial_position.value == 804, "3D property spawn baseline")
    observer.position.x = 26.0
    if not await wait_for(func(): return first_instance.get_ref() == null and is_instance_valid(_actors.spatial) and _actors.spatial.value == 802,
        "cross into far cell and free the near actor"): return
    _step("guest_step", 1)
    if not await _wait_step("host_step", 1): return
    if not await wait_for(func(): return plain_instance.get_ref() == null, "plain Node moved out of the subscribed XZ cell"): return
    observer_3d.position.z = 26.0
    if not await wait_for(func(): return is_instance_valid(_actors.spatial_position) and _actors.spatial_position.value == 805,
        "plain Node re-enters with the latest state"): return
    observer.position.x = 2.0
    if not await wait_for(func(): return is_instance_valid(_actors.spatial) and _actors.spatial.value == 803,
        "spatial reentry reapplies latest state"): return
    var server: Node = _actors.server
    var sync: NetcodeServerReplicator = server.get_node("Sync")
    sync.on_process_input.connect(_simulate.bind(server, false))
    if not await wait_for(sync.has_input_authority, "guest has model input authority"): return
    check(not sync.queue_input(0.0, PackedByteArray([1])) and not sync.queue_input(INF, PackedByteArray([1])),
        "zero and non-finite input durations are rejected")
    check(not sync.queue_input(1.0e39, PackedByteArray([1])) and not sync.queue_input(1.0e-50, PackedByteArray([1])),
        "input duration must fit positive float32")
    check(not sync.queue_input(2.0, PackedByteArray([1])), "an oversized delta cannot stall the authoritative queue")
    for i in 12:
        check(sync.queue_input(0.006 + i * 0.001, PackedByteArray([i + 1])), "queue timed input " + str(i + 1))
    check(server.value == 12 and sync.get_input_queue_count() == 12, "guest immediately predicts inputs")
    if not await wait_for(func(): return sync.get_input_queue_count() == 0 and server.value == 24, "guest reconciled and emptied queue"): return
    check(sync.get_confirmed_input() == 12 and _replayed > 0 and sync.get_replayed_input_count() > 0, "guest replayed unconfirmed inputs")
    var peak := 0
    for i in SUSTAINED_INPUTS:
        await physics_frame
        check(sync.queue_input(PackedByteArray([254])), "accept sustained 60 Hz input " + str(i))
        peak = maxi(peak, sync.get_input_queue_count())
    if not await wait_for(func(): return (sync.get_confirmed_input() == 12 + SUSTAINED_INPUTS and
        sync.get_input_queue_count() == 0 and server.value == 24 + SUSTAINED_INPUTS * 2), "sustained input drains"): return
    check(peak < 32, "sustained input peak remains bounded")
    print("NATIVE prediction guest confirmed=", sync.get_confirmed_input(), " queue_peak=", peak, " value=", server.value)
    var created := primary.spawn(Spatial, _place.bind(811, 2.0))
    if created == null:
        check(false, "guest may create within its subscribed cell")
        return
    if not await wait_for(created.get_node("Sync").is_write_ready, "spatial guest creation confirms"): return
    var created_ref: WeakRef = weakref(created)
    observer.position.x = 26.0
    if not await wait_for(func(): return is_instance_valid(_actors.spatial) and _actors.spatial.value == 802,
        "observer subscribed to another cell"): return
    check(created.get_node("Sync").is_write_ready(), "leaving the interest cell retains the writer")
    created.value = 812
    _step("guest_step", 2)
    if not await wait_for(func(): return created_ref.get_ref() == null, "reclaim drops an out-of-area replica"): return
    await _wait_step("guest_step", 2)
    if not await wait_for(func(): return sync.get_input_authority() == 1, "guest sees reassignment to host"): return
    check(not sync.queue_input(PackedByteArray([1])), "observer cannot submit input after reassignment")
    _step("guest_step", 3)
    if not await wait_for(func(): return server.smoothed > 0.0, "observer receives authoritative value"): return
    check(server.smoothed < 100.0 and not sync.has_input_authority(), "observer interpolates without predicting")
    if not await wait_for(func(): return server.smoothed == 100.0, "observer interpolation reaches its exact target"): return
    var writes: int = server.smoothed_writes
    for i in 6: await physics_frame
    check(server.smoothed_writes == writes, "converged interpolation stops calling the property setter")
    _step("guest_step", 4)
    await _wait_step("guest_step", 4)
    var server_ref: WeakRef = weakref(server)
    if not await wait_for(func(): return server_ref.get_ref() == null, "remote server actor despawns after spawner removal"): return
    _step("guest_step", 5)
    await _wait_step("guest_step", 5)

func _simulate(_tick: int, dt: float, payload: PackedByteArray, fresh: bool, actor: Node, host: bool) -> void:
    if payload[0] == 0: return
    var expected_delta := 0.016 if payload[0] >= 252 else 0.005 + payload[0] * 0.001
    check(is_equal_approx(dt, expected_delta), "input callback preserves each input's delta")
    if host and payload[0] == 255:
        actor.smoothed = 100.0
        return
    if host and payload[0] == 252:
        _pool.remove_child(primary)
        primary.queue_free()
        return
    actor.value += 2 if host else 1
    if host:
        _inputs.append(payload[0])
    elif not fresh:
        _replayed += 1

func _place(_sync: NetcodeReplicator, actor: Node2D, value: int, x: float) -> void:
    actor.position.x = x
    actor.set("value", value)

func _step(key: String, value: int) -> void:
    check(Netcode.get_room().set_property(key, value), "publish " + key)

func _received_broadcast() -> bool:
    if _receiver.messages.size() != 1: return false
    var message: Array = _receiver.messages[0]
    return message.slice(0, 3) == ["hello", 1, Netcode.TARGET_ALL] and message[3] == null

func _received_collections() -> bool:
    return _receiver.arguments == [[[7]]] and _other_receiver.arguments == [[[7]]]

func _wait_step(key: String, value: int) -> bool:
    return await wait_for(func(): return (Netcode.get_room() != null and
        Netcode.get_room().get_custom_properties().get(key, 0) >= value), "wait for " + key + " " + str(value))
