extends Node2D

@export var value := -1:
    set(next):
        value = next
        Engine.get_main_loop().record("setter", value, $Sync.has_authority())

func _enter_tree() -> void:
    var sync := $Sync
    sync.spawned.connect(func(): Engine.get_main_loop().record("replicator_spawned", value, sync.has_authority()))
    sync.authority_changed.connect(func(authority: bool): Engine.get_main_loop().record("authority_changed", value, authority))
    Engine.get_main_loop().record("enter_tree", value, sync.has_authority())

func _ready() -> void:
    Engine.get_main_loop().record("ready", value, $Sync.has_authority())
