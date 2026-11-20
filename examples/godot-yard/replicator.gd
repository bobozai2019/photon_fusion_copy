extends RefCounted

const BLEND_SECONDS := 0.10
const YARD_HEIGHT := 640.0

class Instance:
    var root: Node
    var schema: int
    var revision: int
    var previous: Array
    var target: Array
    var displayed: Array
    var elapsed := BLEND_SECONDS

    func _init(node: Node, snapshot: Dictionary) -> void:
        root = node
        schema = snapshot.schema
        revision = snapshot.revision
        target = snapshot.values
        previous = target
        displayed = target.duplicate()

var layout: Dictionary
var make_node: Callable
var instances: Dictionary = {}

func _init(fields: Dictionary, factory: Callable) -> void:
    layout = fields
    make_node = factory

func node_for(id: int) -> Node:
    var instance: Instance = instances.get(id)
    if instance != null and is_instance_valid(instance.root):
        return instance.root
    return null

func sync(objects: Array, smooth: bool) -> void:
    var alive := {}
    for snapshot: Dictionary in objects:
        var id: int = snapshot.id
        alive[id] = true
        var instance: Instance = instances.get(id)
        if instance == null or not is_instance_valid(instance.root) or instance.root.is_queued_for_deletion():
            instances[id] = Instance.new(make_node.call(snapshot), snapshot)
        elif snapshot.revision > instance.revision:
            instance.revision = snapshot.revision
            instance.target = snapshot.values
            instance.previous = instance.displayed.duplicate() if smooth else instance.target
            instance.elapsed = 0.0 if smooth else BLEND_SECONDS
    for id: int in instances.keys():
        if alive.has(id):
            continue
        var root := node_for(id)
        if root != null:
            if root.get_parent() != null:
                root.get_parent().remove_child(root)
            root.queue_free()
        instances.erase(id)

func render(delta: float) -> void:
    for instance: Instance in instances.values():
        if not is_instance_valid(instance.root) or instance.root.is_queued_for_deletion():
            continue
        instance.elapsed = minf(BLEND_SECONDS, instance.elapsed + delta)
        var alpha := instance.elapsed / BLEND_SECONDS
        var fields: Array = layout[instance.schema]
        for index in fields.size():
            var field: Dictionary = fields[index]
            var value: Variant = instance.target[index]
            if alpha < 1.0 and field.transform:
                var start := float(instance.previous[index])
                var distance := float(value) - start
                if field.property in ["angle", "rx", "ry", "rz"]:
                    distance = wrapf(distance, -180.0, 180.0)
                value = start + alpha * distance
            instance.displayed[index] = value
            var target := instance.root.get_node_or_null(NodePath(field.path))
            if target == null or target.is_queued_for_deletion():
                continue
            if field.transform:
                write_transform(target, field.property, float(value))
            elif field.property in target:
                target.set(field.property, value)
            else:
                target.set_meta(StringName(field.property), value)

static func write_transform(target: Node, property: String, value: float) -> void:
    if target is Node2D:
        var node := target as Node2D
        match property:
            "x": node.position.x = value
            "y": node.position.y = YARD_HEIGHT - value
            "angle": node.rotation_degrees = value
            "sx": node.scale.x = value
            "sy": node.scale.y = value
    elif target is Node3D:
        var node := target as Node3D
        match property:
            "x": node.position.x = value
            "y": node.position.y = value
            "z": node.position.z = value
            "rx": node.rotation_degrees.x = value
            "ry": node.rotation_degrees.y = value
            "rz": node.rotation_degrees.z = value
            "sx": node.scale.x = value
            "sy": node.scale.y = value
            "sz": node.scale.z = value
