extends Node2D

enum Form { FIELD, CAR, LAMP, GATE, BARRIER, LOAD }

var form: Form
var tint: Color
var net_input_x := 0.0:
    set(value):
        if net_input_x != value:
            net_input_x = value
            queue_redraw()
var glow := 0.0:
    set(value):
        if glow != value:
            glow = value
            queue_redraw()
var is_open := false:
    set(value):
        if is_open != value:
            is_open = value
            queue_redraw()
var marker := 0.0:
    set(value):
        if marker != value:
            marker = value
            queue_redraw()

func _init(shape: Form, color := Color("#77a9d3")) -> void:
    form = shape
    tint = color

func _draw() -> void:
    match form:
        Form.FIELD:
            draw_field()
        Form.CAR:
            draw_rect(Rect2(-17, -13, 8, 26), Color("#111925"))
            draw_rect(Rect2(10, -13, 8, 26), Color("#111925"))
            draw_colored_polygon(PackedVector2Array([
                Vector2(-22, -9), Vector2(11, -9), Vector2(23, -6),
                Vector2(23, 6), Vector2(11, 9), Vector2(-22, 9)
            ]), tint.lightened(absf(net_input_x) * 0.18))
            draw_rect(Rect2(-8, -6, 13, 12), Color("#172a40"))
        Form.LAMP:
            var light := Color("#fde6a0") * (0.45 + 0.55 * glow)
            light.a = 1.0
            draw_circle(Vector2(20, 4), 2.5, light)
            draw_circle(Vector2(20, -4), 2.5, light)
        Form.GATE:
            var color := Color("#78e1bd") if is_open else Color("#edb66d")
            draw_rect(Rect2(-5, -65, 10, 130), color)
            for y in range(-59, 60, 18):
                draw_rect(Rect2(-5, y, 10, 6), Color("#3c4a5b"))
        Form.BARRIER:
            draw_rect(Rect2(-18, -13, 36, 26), Color("#f2dfbb"))
            draw_line(Vector2(-16, -10), Vector2(16, 10), Color("#af6448"), 2.0)
        Form.LOAD:
            draw_rect(Rect2(-5, -5, 10, 10), tint.lightened(fmod(marker, 40.0) / 80.0))

func draw_field() -> void:
    draw_rect(Rect2(25, 75, 910, 530), Color("#152438"))
    draw_rect(Rect2(25, 75, 910, 530), Color("#4b6682"), false)
    for x in range(42, 935, 42):
        draw_line(Vector2(x, 76), Vector2(x, 604), Color("#243950"))
    for y in range(100, 600, 42):
        draw_line(Vector2(26, y), Vector2(934, y), Color("#243950"))
    draw_rect(Rect2(50, 284, 855, 138), Color("#304258"))
    for x in range(68, 887, 45):
        draw_rect(Rect2(x, 350, 22, 3), Color("#8192a0"))
    draw_rect(Rect2(60, 111, 205, 117), Color("#203d51"))
    for x in range(60, 266, 41):
        draw_line(Vector2(x, 111), Vector2(x, 228), Color("#5d8795"))
    draw_rect(Rect2(590, 125, 226, 143), Color("#4b6682"), false)
    draw_circle(Vector2(690, 220), 65, Color("#24364b"))
    draw_arc(Vector2(690, 220), 65, 0.0, TAU, 48, Color("#5d8795"))
