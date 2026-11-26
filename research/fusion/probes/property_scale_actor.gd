extends Node3D

static var width := 20
const WAVES := 60
const MASK := (1 << 30) - 1
const STRIDE := 8192
static var observing := false
static var sparse := false
static var reads := 0
static var sets := 0
static var updates := 0
static var terminal_fields := 0
static var errors := 0
static var received := PackedInt32Array()
static var first_us := PackedInt64Array()
static var clock_ms := 0
static var settled_us := 0
static var expected_count := 0
static var history: Array = []
var slot := -1
var baseline_fields := 0
var revisions := PackedInt32Array()

func _init() -> void:
    revisions.resize(width)
    revisions.fill(-1)

static func configure(as_observer: bool, rotating: bool, field_count: int, fields_per_object: int) -> void:
    observing = as_observer
    sparse = rotating
    expected_count = field_count
    width = fields_per_object
    received.resize(WAVES + 1)
    first_us.resize(WAVES + 1)

static func wall_us() -> int:
    return roundi(Time.get_unix_time_from_system() * 1000000.0)

static func encode(field_id: int, wave: int) -> int:
    return (((wave * STRIDE + field_id) * 15485863 + 32452843) & MASK) + 1

func final_wave(index: int) -> int:
    return WAVES - 9 + ((slot * width + index) % 10) if sparse else WAVES

func applied(index: int, value: int) -> void:
    if not observing: return
    if value < 1 or value > MASK + 1:
        errors += 1
        return
    var decoded := (((value - 1 - 32452843) & MASK) * 44542999) & MASK
    var field_id := decoded % STRIDE - 1
    var wave := decoded / STRIDE
    if slot == -1 and field_id >= 0: slot = field_id / width
    if field_id != slot * width + index or field_id >= expected_count or wave > WAVES or wave <= revisions[index]:
        errors += 1
        return
    revisions[index] = wave
    if wave == 0:
        baseline_fields += 1
        return
    if sparse and (wave - 1) % 10 != field_id % 10:
        errors += 1
        return
    var now := wall_us()
    var bucket := now / 1000
    if clock_ms != bucket:
        if clock_ms: history.append([clock_ms, updates])
        clock_ms = bucket
    updates += 1
    received[wave] += 1
    if not first_us[wave]: first_us[wave] = now
    if wave == final_wave(index):
        terminal_fields += 1
        if terminal_fields == expected_count: settled_us = now
