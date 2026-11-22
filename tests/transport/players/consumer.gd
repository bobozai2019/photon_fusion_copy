extends Node

const FIELDS := [42, 11, 3, 5, 0, 7, -4, 16777219]
var game := NetcodeGame.new()
var host := false
var resuming := false
var ticket_path := ""
var sent := false
var reported := 0
var quit_after := 0

func argument(args: PackedStringArray, name: String, fallback: String = "") -> String:
    var at := args.find(name)
    return args[at + 1] if at >= 0 and at + 1 < args.size() else fallback

func _ready() -> void:
    var args := OS.get_cmdline_user_args()
    host = "--host" in args
    resuming = "--resume" in args
    ticket_path = argument(args, "--ticket")
    var layout := FileAccess.get_file_as_string("res://consumer.cfg")
    var provider := argument(args, "--provider", "tcp")
    var path := argument(args, "--module")
    var ticket := FileAccess.get_file_as_bytes(ticket_path) if resuming else PackedByteArray()
    var error := game.open_with_provider(layout, host, argument(args, "--endpoint"), 2, false, provider, path,
        PackedByteArray(), ticket)
    if "--expect-error" in args:
        if not error.contains(argument(args, "--expect-error")):
            fail("Unexpected open result: " + error)
        else:
            print("OPEN_ERROR ", error)
            get_tree().quit()
        set_process(false)
        return
    if not error.is_empty():
        fail(error)
        return
    if not game.bind_method("Proof", 71, 2, 0, true, false, true, proof) or \
        not game.bind_method("Reply", 72, 2, 1, true, false, true, reply):
        fail("RPC registration")
        return
    print("OPEN sdk=", NetcodeGame.sdk_build_id(), " provider=", game.transport_build_id())

func fail(reason: String) -> void:
    printerr("FAIL ", reason)
    set_process(false)
    get_tree().quit(1)

func proof(sender: int, object: int, phase: int) -> void:
    if not host or sender != 2 or object != 0 or phase != reported + 1:
        fail("Unexpected proof")
        return
    reported += 1
    if not game.call_method("Reply", 0, [42], 2):
        fail("Reply rejected")
        return
    print("HOST_RPC phase=", phase, " player=", sender)
    if phase == 2:
        quit_after = Time.get_ticks_msec() + 500

func reply(sender: int, object: int, value: int) -> void:
    if host or not sent or sender != 1 or object != 0 or value != 42:
        fail("Unexpected reply")
        return
    if not resuming:
        var ticket := game.resume_ticket()
        if ticket.is_empty():
            fail("Empty ticket")
            return
        var file := FileAccess.open(ticket_path, FileAccess.WRITE)
        if file == null:
            fail("Cannot save ticket")
            return
        file.store_buffer(ticket)
        file.close()
    print("GUEST_PASS fields=8 player=2 resumed=", 1 if resuming else 0)
    set_process(false)
    get_tree().quit()

func _process(_delta: float) -> void:
    var frame := game.advance()
    if frame.is_empty(): return
    if host:
        if not frame.round and frame.seats == 2:
            if not game.start() or game.spawn(1, FIELDS, 0, 0, 0) != 1:
                fail("Cannot start round")
        if quit_after and Time.get_ticks_msec() >= quit_after:
            print("HOST_PASS")
            get_tree().quit()
    elif not sent and frame.round and frame.player == 2 and game.read_fields(1, false) == FIELDS:
        sent = game.call_method("Proof", 0, [2 if resuming else 1])
