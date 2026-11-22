extends RefCounted

# An image is one replicated object. The caller recreates collision geometry and joints
# before restoring all participating bodies at a physics-frame boundary.
var bodies: Dictionary[int, RigidBody3D] = {}

func add(id: int, body: RigidBody3D) -> bool:
    if id <= 0 or id > 0xffffffff or not is_instance_valid(body) or bodies.has(id) or body in bodies.values():
        return false
    bodies[id] = body
    return true

func capture(tick: int) -> Array:
    var ids := PackedInt32Array()
    var flags := PackedInt32Array()
    var poses := PackedFloat32Array()
    var keys := bodies.keys()
    keys.sort()
    for id in keys:
        var b := bodies[id]
        var transform := b.global_transform
        var q := transform.basis.get_rotation_quaternion()
        var p := transform.origin
        var v := b.linear_velocity
        var w := b.angular_velocity
        ids.append(id)
        flags.append(int(b.sleeping))
        poses.append_array(PackedFloat32Array([p.x,p.y,p.z,q.x,q.y,q.z,q.w,v.x,v.y,v.z,w.x,w.y,w.z]))
    return [tick,ids,flags,poses]

func restore(fields: Array) -> bool:
    if fields.size() != 4 or not fields[0] is int or not fields[1] is PackedInt32Array or not fields[2] is PackedInt32Array or not fields[3] is PackedFloat32Array:
        return false
    var ids: PackedInt32Array = fields[1]
    var flags: PackedInt32Array = fields[2]
    var p: PackedFloat32Array = fields[3]
    if ids.size() != bodies.size() or ids.size() != flags.size() or p.size() != ids.size()*13: return false
    var seen := {}
    for i in ids.size():
        var id := ids[i] & 0xffffffff
        var j := i*13
        if not is_instance_valid(bodies.get(id)) or seen.has(id) or flags[i] not in [0,1]: return false
        seen[id]=true
        for k in 13:
            if not is_finite(p[j+k]): return false
        if absf(Quaternion(p[j+3],p[j+4],p[j+5],p[j+6]).length_squared()-1.0) > 0.001: return false
    for i in ids.size():
        var j := i*13
        var b := bodies[ids[i] & 0xffffffff]
        b.global_transform = Transform3D(Basis(Quaternion(p[j+3],p[j+4],p[j+5],p[j+6])),Vector3(p[j],p[j+1],p[j+2]))
        b.linear_velocity=Vector3(p[j+7],p[j+8],p[j+9])
        b.angular_velocity=Vector3(p[j+10],p[j+11],p[j+12])
        b.sleeping=flags[i]!=0
    return true
