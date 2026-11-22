@tool
extends ConfirmationDialog

signal chosen(paths: Array[NodePath])
const Fields = preload("../scene_fields.gd")
const Replica = preload("../scene_replicator.gd")
var actor: Node
var sync: Node
@onready var nodes: Tree = %Tree
@onready var fields: ItemList = %Fields
@onready var filter: LineEdit = %Filter
var available: Dictionary = {}

# Non-tool scripts may expose only exported fields on their editor placeholder.
# Read their declared variables from the Script as well, without running gameplay code.
static func _members(node: Node) -> Dictionary:
    var result := {}
    for info in node.get_property_list():
        if int(info.usage) & (PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_SCRIPT_VARIABLE):
            result[info.name] = info
    var script: Script = node.get_script()
    while script:
        for info in script.get_script_property_list():
            if int(info.usage) & PROPERTY_USAGE_SCRIPT_VARIABLE and not result.has(info.name):
                result[info.name] = info
        script = script.get_base_script()
    return result

static func _choices(node: Node) -> Dictionary:
    var result := {}
    for info: Dictionary in _members(node).values():
        if int(info.usage) & PROPERTY_USAGE_READ_ONLY: continue
        var kind := Fields.kind(info)
        if not kind.is_empty():
            result[String(info.name)] = kind
        elif info.type == TYPE_OBJECT and not String(info.class_name).is_empty():
            for entry in ProjectSettings.get_global_class_list():
                if entry.class != info.class_name: continue
                var record: Script = load(entry.path)
                if not ClassDB.is_parent_class(record.get_instance_base_type(), "RefCounted"): break
                while record:
                    for member in record.get_script_property_list():
                        if int(member.usage) & PROPERTY_USAGE_SCRIPT_VARIABLE and member.type in [
                            TYPE_BOOL, TYPE_INT, TYPE_FLOAT, TYPE_VECTOR2, TYPE_VECTOR3, TYPE_VECTOR4, TYPE_QUATERNION, TYPE_COLOR]:
                            result["%s:%s" % [info.name, member.name]] = Fields.kind(member)
                    record = record.get_base_script()
                break
    return result

static func kind_for(replica: Node, path: NodePath) -> String:
    if path.get_subname_count() == 0 or path.is_absolute(): return ""
    var target: Node = replica._target(path)
    return _choices(target).get(str(path.get_concatenated_subnames()), "") if target else ""

func open_for(replica: Node) -> void:
    sync = replica
    actor = replica.replication_root()
    _branch(actor, null).select(0)
    _select_node()
    popup_centered(Vector2i(660, 460) * EditorInterface.get_editor_scale())

func _on_field_selected(_index: int) -> void:
    get_ok_button().disabled = false

func _branch(node: Node, parent: TreeItem) -> TreeItem:
    var item := nodes.create_item(parent)
    item.set_text(0, str(node.name))
    item.set_metadata(0, node)
    for child in node.get_children():
        if child is Replica or child.owner == null: continue
        _branch(child, item)
    return item

func _select_node() -> void:
    var item := nodes.get_selected()
    if not item: return
    available = _choices(item.get_metadata(0))
    filter.clear()
    _show_fields()

func _show_fields() -> void:
    fields.clear()
    get_ok_button().disabled = true
    var keys := available.keys()
    keys.sort()
    var target: Node = nodes.get_selected().get_metadata(0)
    var prefix := "" if target == actor else str(actor.get_path_to(target))
    var groups := {}
    for member: String in keys:
        if ":" in member:
            var group := member.get_slice(":", 0)
            if not groups.has(group): groups[group] = []
            groups[group].append(NodePath("%s:%s" % [prefix, member]))
    for group: String in groups:
        if filter.text.is_empty() or group.containsn(filter.text):
            var index := fields.add_item(group + "　（添加全部子属性）")
            fields.set_item_metadata(index, groups[group])
    for member: String in keys:
        if not filter.text.is_empty() and not member.containsn(filter.text): continue
        var path := NodePath("%s:%s" % [prefix, member])
        var index := fields.add_item(member)
        fields.set_item_metadata(index, path)
        fields.set_item_tooltip(index, str(path))
        if sync._canonical(path) in sync._auto_paths(): fields.set_item_disabled(index, true)

func _accept() -> void:
    var selected := fields.get_selected_items()
    if selected.is_empty(): return
    var selected_value = fields.get_item_metadata(selected[0])
    var paths: Array[NodePath] = []
    if selected_value is Array: paths.assign(selected_value)
    else: paths.append(selected_value)
    chosen.emit(paths)
    queue_free()
