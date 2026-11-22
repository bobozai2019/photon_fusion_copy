extends RefCounted

enum Correction { VELOCITY, FORCE, BLEND }
var mode := Correction.VELOCITY
var max_forecast := 0.1
var stiffness := 50.0
var damping := 10.0
var blend_factor := 0.1
# Supply the effective acceleration in engine coordinates, including local Area2D overrides.
var gravity := Vector2.ZERO
var generation := -1
var units_per_metre := 50.0

func sample(body: RigidBody2D, teleport: int, phase: int) -> Array:
    var p := body.global_position / units_per_metre
    var v := body.linear_velocity / units_per_metre
    return [Vector2(p.x,-p.y), -body.global_rotation, Vector2(v.x,-v.y), -body.angular_velocity, body.sleeping, teleport, phase]

func apply(body: RigidBody2D, fields: Array, elapsed: float) -> void:
    var sleep_now: bool = fields[4]
    var snap := generation != int(fields[5]) or sleep_now
    var age := 0.0 if snap else clampf(elapsed,0,maxf(0,max_forecast))
    var p: Vector2 = fields[0]
    var v: Vector2 = fields[2]
    p = Vector2(p.x,-p.y) * units_per_metre
    v = Vector2(v.x,-v.y) * units_per_metre
    var target := p + v * age + gravity * (0.5 * age * age)
    v += gravity * age
    var angle := -float(fields[1]) - age * float(fields[3])
    generation = fields[5]
    body.sleeping = false
    if snap:
        body.global_position = target
        body.global_rotation = angle
        body.linear_velocity = v
    elif mode == Correction.BLEND:
        body.global_position = body.global_position.lerp(target,clampf(blend_factor,0,1))
        body.global_rotation = lerp_angle(body.global_rotation,angle,clampf(blend_factor,0,1))
        body.linear_velocity = v
    elif mode == Correction.FORCE:
        body.apply_central_force(body.mass*(stiffness*(target-body.global_position)+damping*(v-body.linear_velocity)))
    else:
        body.linear_velocity = v + (target - body.global_position) * 6
    var spin := -float(fields[3])
    var error := angle_difference(body.global_rotation,angle)
    if not snap and mode == Correction.FORCE:
        var state := PhysicsServer2D.body_get_direct_state(body.get_rid())
        if state and state.inverse_inertia > 0:
            body.apply_torque((stiffness*error + damping*(spin-body.angular_velocity))/state.inverse_inertia)
    else: body.angular_velocity = spin + (error*6 if not snap and mode == Correction.VELOCITY else 0.0)
    if sleep_now: body.sleeping = true
