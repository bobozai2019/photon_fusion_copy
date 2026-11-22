extends RefCounted

enum Correction { VELOCITY, FORCE, BLEND }
var mode := Correction.VELOCITY
var max_forecast := 0.25
var stiffness := 50.0
var damping := 10.0
var blend_factor := 0.1
var gravity := Vector3.ZERO
var generation := -1

func sample(body: RigidBody3D, teleport: int, phase: int) -> Array:
    return [body.global_position,body.global_basis.get_rotation_quaternion(),body.linear_velocity,
        body.angular_velocity,body.sleeping,teleport,phase]

func apply(body: RigidBody3D, fields: Array, elapsed: float) -> void:
    var snap := generation != int(fields[5]) or bool(fields[4])
    generation = fields[5]
    var age := 0.0 if snap else clampf(elapsed,0,maxf(0,max_forecast))
    var velocity: Vector3 = fields[2]
    var spin: Vector3 = fields[3]
    var target: Vector3 = fields[0] + velocity*age + gravity*(0.5*age*age)
    velocity += gravity*age
    var rotation: Quaternion = fields[1]
    if spin.length_squared() > 0.000001:
        rotation = Quaternion(spin.normalized(),spin.length()*age) * rotation
    var current := body.global_basis.get_rotation_quaternion()
    var error := (rotation * current.inverse()).normalized()
    if error.w < 0: error = -error
    var angular_error := Vector3.ZERO
    if error.w < 0.999999: angular_error = error.get_axis()*error.get_angle()
    body.sleeping = false
    if snap:
        body.global_transform = Transform3D(Basis(rotation),target)
        body.linear_velocity = velocity
        body.angular_velocity = spin
    elif mode == Correction.BLEND:
        var weight := clampf(blend_factor,0,1)
        body.global_transform = Transform3D(Basis(current.slerp(rotation,weight)),body.global_position.lerp(target,weight))
        body.linear_velocity = velocity
        body.angular_velocity = spin
    elif mode == Correction.FORCE:
        body.apply_central_force(body.mass*(stiffness*(target-body.global_position)+damping*(velocity-body.linear_velocity)))
        var inverse := body.get_inverse_inertia_tensor()
        if absf(inverse.determinant()) > 0.0000001:
            body.apply_torque(inverse.inverse()*(stiffness*angular_error+damping*(spin-body.angular_velocity)))
    else:
        body.linear_velocity = velocity + 6*(target-body.global_position)
        body.angular_velocity = spin + 6*angular_error
    if fields[4]: body.sleeping = true
