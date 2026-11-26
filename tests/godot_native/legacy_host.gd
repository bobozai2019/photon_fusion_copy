extends SceneTree

var game := NetcodeGame.new()
var rejected := ""

func _initialize() -> void:
    _run.call_deferred()

func _run() -> void:
    var args := OS.get_cmdline_user_args()
    if args.size() == 3:
        _native_peer(args[0].to_int(), args[1], args[2])
        return
    var endpoint := "127.0.0.1:" + args[0]
    var error := game.open_with_provider("", true, endpoint, 2, false, "tcp", "", PackedByteArray(), PackedByteArray())
    if not error.is_empty() or not game.start():
        printerr(error)
        quit(1)
        return
    var ready := FileAccess.open(args[1], FileAccess.WRITE)
    ready.store_string("ready")
    ready.close()
    var done := args[1] + ".done"
    process_frame.connect(func():
        if FileAccess.file_exists(done):
            game.close()
            quit()
        else:
            game.advance())
    create_timer(16.0).timeout.connect(func():
        game.close()
        quit())

func _native_peer(port: int, marker: String, role: String) -> void:
    if not Engine.has_singleton("Netcode"):
        quit(77)
        return
    var netcode := Engine.get_singleton("Netcode")
    var deadline := Time.get_ticks_msec() + 15000
    if role == "host":
        if netcode.create_room("127.0.0.1", port) != OK:
            quit(1)
            return
        while netcode.get_room() == null and Time.get_ticks_msec() < deadline:
            await process_frame
        if netcode.get_room() == null:
            quit(1)
            return
        var ready := FileAccess.open(marker, FileAccess.WRITE)
        ready.store_string("ready")
        ready.close()
        while not FileAccess.file_exists(marker + ".done") and Time.get_ticks_msec() < deadline:
            await process_frame
        netcode.leave_room()
        quit(int(not FileAccess.file_exists(marker + ".done")))
    else:
        netcode.connection_failed.connect(func(reason: String): rejected = reason, CONNECT_ONE_SHOT)
        if netcode.join_room("127.0.0.1", port) != OK:
            quit(1)
            return
        while rejected.is_empty() and netcode.get_room() == null and Time.get_ticks_msec() < deadline:
            await process_frame
        if rejected.is_empty():
            printerr("Mixed native room did not reject the connection")
            quit(1)
            return
        print("NATIVE MIXED REJECTED ", rejected)
        quit()
