extends Node

var messages: Array = []
var arguments: Array = []
var nested_senders: Array[int] = []
var outer_senders: Array[int] = []

func _ready() -> void:
    Netcode.register_broadcast_receiver(self)

func _exit_tree() -> void:
    Netcode.unregister_broadcast_receiver(self)

@rpc("any_peer", "call_local", "reliable")
func room_message(message: String) -> void:
    var info := Netcode.get_rpc_info()
    var sender := Netcode.get_rpc_sender()
    messages.append([message, sender if sender == info.sender else -1, info.target, info.target_node])
    if message == "from guest" and Netcode.is_master_client():
        outer_senders.append(sender)
        Netcode.rpc_to_player(Netcode.get_local_player_id(), room_nested)
        outer_senders.append(Netcode.get_rpc_sender())

@rpc("any_peer", "call_local", "reliable")
func room_nested() -> void:
    nested_senders.append(Netcode.get_rpc_sender())

@rpc("any_peer", "call_local", "reliable")
func room_values(values: Array) -> void:
    arguments.append(values.duplicate(true))
    values[0].clear()
