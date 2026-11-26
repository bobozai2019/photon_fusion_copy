extends Node

# One writer and one observer. Both backends use this workload and sampler.
var actors := Node3D.new()
var count := 0
var writer := false
var prepared := false
var running := false
var finished := false
var active_frame := 0
var started_us := 0
var prev := {}
var changes := 0
var ages: Array[int] = []
var age_incomplete := 0
var final_sent := false
var pairs := false
var contacts := {}
var x_ranges := {}
var last_progress := -1
var end_us := 0
var sleep_requested := false

func report(event: String, value: Variant) -> void:
    print("CLIENT_EVENT ", JSON.stringify({"event":event, "value":value}))

func available() -> int:
    return actors.get_child_count()

func ready_check() -> void:
    var n := available()
    if n != last_progress and (n == count or n / 25 != last_progress / 25):
        report("loading", n)
        last_progress = n
    if n == count and count > 0:
        prepared = true
        report("prepared", n)

func start_measurement() -> void:
    running = true
    started_us = Time.get_ticks_usec()

func teleport_body(body: RigidBody3D, destination: Vector3) -> void:
    body.position = destination

func drive() -> void:
    var elapsed := (Time.get_ticks_usec() - started_us) / 1000000.0
    if elapsed >= 6:
        var slot := 0
        for body in actors.get_children():
            var odd := pairs and slot % 2 == 1
            teleport_body(body, Vector3(44 if odd else 42, body.position.y, 0))
            body.linear_velocity = Vector3.ZERO
            body.sleeping = false
            body.stamp = -2 if odd else -1
            slot += 1
        report("writer", {"hz":active_frame / elapsed, "ticks":active_frame, "seconds":elapsed, "contact_bodies":contacts.size()})
        running = false
        final_sent = true
        end_us = Time.get_ticks_usec()
        return
    # The same age tag for every body every 6 steps, with different lanes and speeds.
    var stamp := wall_ms()
    var i := 0
    for body in actors.get_children():
        if active_frame == 0:
            var lane := Vector3(-0.5 if i % 2 == 0 else 0.5, floori(i * 0.5) * 2, 0) if pairs else Vector3(0,i,0)
            teleport_body(body, lane)
            body.sleeping = false
            if pairs: enable_contacts(body)
        if active_frame % 18 == 0:
            var speed := 2.0 + (i % 5) * 0.2
            if pairs and i % 2 == 1: speed *= -1
            body.linear_velocity.x = speed * (1 if active_frame % 36 == 0 else -1)
        if active_frame % 6 == 0: body.stamp = stamp
        if pairs and body.get_contact_count() > 0: contacts[body.get_instance_id()] = true
        i += 1
    active_frame += 1

func enable_contacts(body: RigidBody3D) -> void:
    body.collision_layer = 1
    body.collision_mask = 1
    body.max_contacts_reported = 2
    body.contact_monitor = true

func wall_ms() -> int:
    return int(Time.get_unix_time_from_system() * 1000.0) % 100000000 + 1

func observe() -> void:
    var now := wall_ms()
    var most_stale := 0
    var final_count := 0
    var final_tags := 0
    var final_poses := 0
    var asleep := 0
    var positive := 0
    var ending := false
    for body in actors.get_children():
        var tag: int = body.stamp
        if tag < 0:
            ending = true
            var target := 44.0 if tag == -2 else 42.0
            var close := absf(body.position.x - target) < 0.05
            final_tags += 1
            if close: final_poses += 1
            if body.sleeping: asleep += 1
            if close and body.sleeping: final_count += 1
        if tag <= 0: continue
        positive += 1
        most_stale = maxi(most_stale, posmod(now - tag, 100000000))
        var id: int = body.get_instance_id()
        if pairs:
            if body.collision_layer == 0: enable_contacts(body)
            if body.get_contact_count() > 0: contacts[id] = true
        # Exclude terminal teleports even if their pose arrives before the marker.
        var x: float = body.position.x
        if absf(x) < 5:
            var span: Vector2 = x_ranges.get(id, Vector2(x,x))
            x_ranges[id] = Vector2(minf(x, span.x), maxf(x, span.y))
        if prev.get(id, 0) != tag:
            changes += 1
            prev[id] = tag
    if not started_us and positive: started_us = Time.get_ticks_usec()
    if not started_us: return
    if ending and not final_sent:
        final_sent = true
        end_us = Time.get_ticks_usec()
        var seconds := (Time.get_ticks_usec() - started_us) / 1000000.0
        ages.sort()
        var p95 := ages[floori((ages.size()-1)*0.95)] if not ages.is_empty() else -1
        var moved := 0
        for span: Vector2 in x_ranges.values():
            if span.y - span.x >= 0.2: moved += 1
        report("observer", {"hz":active_frame/seconds, "stamp_hz":float(changes)/count/seconds,
            "worst_age_p95_ms":p95, "frames_with_missing_tags":age_incomplete, "ticks":active_frame,
            "fully_tagged_ticks":ages.size(), "seen_moving_tags":prev.size(), "moved_20cm":moved, "contact_bodies":contacts.size()})
    if not final_sent:
        active_frame += 1
        if positive == count: ages.append(most_stale)
        else: age_incomplete += 1
    if final_count == count or (end_us and Time.get_ticks_usec() - end_us >= 8000000):
        finished = true
        var first: RigidBody3D = actors.get_child(0)
        report("converged", {"tags":final_tags,"poses":final_poses,"asleep":asleep,"all_three":final_count,
            "sample_x":first.position.x,"sample_vx":first.linear_velocity.x,"sample_tag":first.get("stamp")})

func _physics_process(_delta: float) -> void:
    if count == 0 or finished: return
    if not prepared: ready_check()
    if not prepared: return
    if writer and running: drive()
    elif writer and end_us:
        var since_stop := Time.get_ticks_usec() - end_us
        if not sleep_requested and since_stop >= 500000:
            for body in actors.get_children(): body.sleeping = true
            sleep_requested = true
        if since_stop >= 2000000:
            var correct := 0
            for body in actors.get_children():
                var x := 44.0 if body.stamp == -2 else 42.0
                if absf(body.position.x - x) < 0.05 and body.sleeping: correct += 1
            var first: RigidBody3D = actors.get_child(0)
            report("authority_final", {"all_three":correct, "sample_x":first.position.x, "sample_vx":first.linear_velocity.x, "sample_sleeping":first.sleeping})
            finished = true
    elif not writer: observe()
