extends Node

var position_reads := 0
var position_xz := Vector2.ZERO:
    get:
        position_reads += 1
        return position_xz
var value := 0
