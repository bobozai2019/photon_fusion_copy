
# Appended only to the temporary benchmark scene.
var profile_start := 0
var profile_ticks := 0
var profile_reported := false
var audit_stage := 0
var audit_start := 0
var audit_actor
var browser_bounds := PackedFloat64Array()

func browser_clock_ms() -> float:
    return -1.0

func profile_tick() -> void:
    if not profile_start:
        if command() != "go": return
        profile_start = Time.get_ticks_usec()
        browser_bounds.append(browser_clock_ms())
        ProfileMeter.active = true
    if not profile_reported:
        var elapsed := Time.get_ticks_usec() - profile_start
        if elapsed >= 6000000:
            ProfileMeter.active = false
            profile_reported = true
            browser_bounds.append(browser_clock_ms())
            emit_result("meter", {"elapsed_us":elapsed,"ticks":profile_ticks,"replica_calls":ProfileMeter.calls,
                "replica_us":ProfileMeter.replica_us,"sample_us":ProfileMeter.read_us,"sync_us":ProfileMeter.sync_us,
                "apply_us":ProfileMeter.apply_us,"advance_us":ProfileMeter.advance_us,"browser_bounds":browser_bounds})
        else:
            profile_ticks += 1
    if finished and workload == "idle": check_local_write()

func audit_command(_op: String) -> bool:
    return false

func check_local_write() -> void:
    if audit_stage == 0:
        if not audit_command("audit_arm"): return
        for actor in slots:
            if actor.slot == 0: audit_actor = actor
        if not audit_actor: return
        if not writer:
            Actor.observing = false
            audit_actor.field_00 = 0
            emit_result("local_assignment", audit_actor.field_00)
        audit_start = Time.get_ticks_msec()
        audit_stage = 1
    if not writer and audit_stage == 1 and Time.get_ticks_msec() - audit_start >= 750:
        emit_result("quiet_value", audit_actor.field_00)
        audit_stage = 2
    elif writer and audit_stage == 1 and audit_command("audit_sibling"):
        emit_result("owner_before", audit_actor.field_00)
        audit_actor.field_01 = Actor.encode(2, 1)
        audit_stage = 2
    elif not writer and audit_stage == 2 and audit_actor.field_01 == Actor.encode(2, 1):
        emit_result("sibling_arrived", [audit_actor.field_00, audit_actor.field_01])
        audit_stage = 3
    elif writer and audit_stage == 2 and audit_command("audit_target"):
        audit_actor.field_00 = Actor.encode(1, 2)
        audit_stage = 3
    elif not writer and audit_stage == 3 and audit_actor.field_00 == Actor.encode(1, 2):
        emit_result("target_arrived", audit_actor.field_00)
        audit_stage = 4
