@tool
extends VBoxContainer

const Picker = preload("field_picker.gd")
const PICKER_SCENE := preload("field_picker.tscn")
const Fields = preload("../scene_fields.gd")
const OPTIONS := ["properties", "client_writable", "smooth_properties", "angle_properties", "limits"]
var replica: Node
@onready var rows: GridContainer = $Scroll/Rows

func _ready() -> void:
    var scale := EditorInterface.get_editor_scale()
    $Scroll.custom_minimum_size.y = 120 * scale
    rows.add_theme_constant_override("h_separation", roundi(12 * scale))
    EditorInterface.get_editor_undo_redo().version_changed.connect(_refresh, CONNECT_DEFERRED)
    _refresh()

func _open_picker() -> void:
    if not is_instance_valid(replica): return
    if replica.replication_root() == null:
        EditorInterface.get_editor_toaster().push_toast("请先在 Root Path 选择有效的复制根节点。")
        return
    var dialog := PICKER_SCENE.instantiate() as Picker
    add_child(dialog)
    dialog.chosen.connect(_add_fields)
    dialog.open_for(replica)

# Root properties may be serialized as either ":score" or ".:score".
func _key(path: NodePath) -> String:
    return str(path).trim_prefix(".") if str(path).begins_with(".:") else str(path)

func _contains(values: Variant, path: NodePath) -> bool:
    for existing in values:
        if _key(existing) == _key(path): return true
    return false

func _without(values: Variant, path: NodePath) -> Variant:
    var result = values.duplicate()
    for existing in values:
        if _key(existing) == _key(path): result.erase(existing)
    return result

func _option(key: String) -> Variant:
    if replica is not NetcodeReplicator:
        return replica.get(key)
    var config: NetcodeReplicationConfig = replica.replication_config
    if config and key != "client_writable":
        return config.get(key)
    if key == "limits":
        return {}
    var empty: Array[NodePath] = []
    return empty

func _apply(label: String, changes: Dictionary) -> void:
    if not is_instance_valid(replica): return
    var undo := EditorInterface.get_editor_undo_redo()
    undo.create_action(label, UndoRedo.MERGE_DISABLE, replica)
    var target: Object = replica
    if replica is NetcodeReplicator:
        target = replica.replication_config
        if target == null:
            target = NetcodeReplicationConfig.new()
            undo.add_do_property(replica, "replication_config", target)
            undo.add_undo_property(replica, "replication_config", null)
    for key in changes:
        undo.add_do_property(target, key, changes[key])
        undo.add_undo_property(target, key, _option(key).duplicate())
    undo.commit_action()
    _refresh.call_deferred()

func _add_fields(paths: Array[NodePath]) -> void:
    if not is_instance_valid(replica): return
    var current: Array[NodePath] = _option("properties")
    var next: Array[NodePath] = current.duplicate()
    for path in paths:
        if not _contains(next, path) and not _contains(replica._auto_paths(), path): next.append(path)
    if next.size() != current.size():
        _apply("添加同步属性", {"properties": next})

func _toggle(enabled: bool, path: NodePath) -> void:
    var next: Array[NodePath] = _without(_option("client_writable"), path)
    if enabled: next.append(path)
    _apply("修改客户端写权", {"client_writable": next})

func _mode(mode: int, path: NodePath) -> void:
    var smooth: Array[NodePath] = _without(_option("smooth_properties"), path)
    var angle: Array[NodePath] = _without(_option("angle_properties"), path)
    if mode == 1: smooth.append(path)
    if mode == 2: angle.append(path)
    _apply("修改属性插值", {"smooth_properties": smooth, "angle_properties": angle})

func _capacity(value: float, path: NodePath) -> void:
    var sizes: Dictionary = _without(_option("limits"), path)
    if value > 0: sizes[path] = int(value)
    _apply("修改属性容量", {"limits": sizes})

func _remove(path: NodePath) -> void:
    var changes := {}
    for key in OPTIONS:
        if key != "client_writable" or replica is not NetcodeReplicator:
            changes[key] = _without(_option(key), path)
    _apply("移除同步属性", changes)

func _refresh() -> void:
    if not is_inside_tree() or not is_instance_valid(replica): return
    for child in rows.get_children():
        if child.owner == self: continue
        rows.remove_child(child)
        child.queue_free()
    var seen := {}
    var automatic: Array = replica._auto_paths()
    var paths: Array = automatic.duplicate()
    for option in OPTIONS:
        var values = _option(option)
        paths.append_array(values.keys() if values is Dictionary else values)
    for path: NodePath in paths:
        if seen.has(_key(path)): continue
        seen[_key(path)] = true
        var valid: bool = _contains(_option("properties"), path) or _contains(automatic, path)
        var title := Label.new()
        title.text = _key(path) + ("" if valid else "　（未绑定属性）")
        title.tooltip_text = str(path)
        title.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
        title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
        rows.add_child(title)
        var kind := Picker.kind_for(replica, path)
        if kind != "integer64" and Fields.ELEMENT.has(Fields.wire(kind)):
            var size := SpinBox.new()
            size.max_value = 65535
            size.tooltip_text = "0 使用框架默认容量"
            var limits: Dictionary = _option("limits")
            for key in limits:
                if _key(key) == _key(path): size.value = limits[key]
            size.value_changed.connect(_capacity.bind(path))
            rows.add_child(size)
        else:
            var placeholder := Label.new()
            placeholder.text = "—"
            rows.add_child(placeholder)
        var modes := OptionButton.new()
        for name in ["直接同步", "平滑", "角度插值"]: modes.add_item(name)
        modes.select(2 if _contains(_option("angle_properties"), path) else int(_contains(_option("smooth_properties"), path)))
        modes.disabled = not valid
        modes.item_selected.connect(_mode.bind(path))
        rows.add_child(modes)
        var write := CheckBox.new()
        write.tooltip_text = "允许客户端写入 " + _key(path)
        write.button_pressed = _contains(_option("client_writable"), path)
        write.disabled = not valid or replica is NetcodeReplicator
        write.toggled.connect(_toggle.bind(path))
        rows.add_child(write)
        if not _contains(automatic, path):
            var remove := Button.new()
            remove.text = "移除"
            remove.pressed.connect(_remove.bind(path))
            rows.add_child(remove)
        else:
            rows.add_child(Control.new())
