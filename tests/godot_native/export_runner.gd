extends Node

func _ready() -> void:
    var loop := get_tree()
    loop.set_script(preload("res://advanced.gd"))
    loop.call_deferred("_run")
