extends Node

var reads := 0
var last_value := 1.0
@export var value := 1.0:
    get:
        reads += 1
        return value
    set(v):
        value = v
        last_value = v
