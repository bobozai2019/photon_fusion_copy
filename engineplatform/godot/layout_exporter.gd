@tool
class_name NetcodeLayoutExporter
extends Node

# Add as a child of the replicated root. Each value names a property relative to that root.
@export_range(1, 65535) var schema_id := 1
@export_range(1, 65535) var version := 1
@export var schema_name := "Actor"
@export var fields: Dictionary[int, NodePath] = {}
@export var reference_fields: PackedInt32Array = []
@export var limits: Dictionary[int, int] = {}
@export_file("*.cfg") var output := "res://replicators.cfg"
@export_tool_button("Export replication layout") var export_action := export_layout

func build_text(root: Node) -> String:
    if not root or fields.is_empty() or fields.size() > 64 or not schema_name.is_valid_identifier():
        return ""
    var types := preload("scene_fields.gd").KINDS
    var text := "schema %d %d %s\n" % [schema_id, version, schema_name]
    var ids := fields.keys()
    ids.sort()
    for id: int in ids:
        var path: NodePath = fields[id]
        if id < 1 or id > 65535 or path.is_absolute() or path.get_subname_count() != 1: return ""
        var target_path := String(path.get_concatenated_names())
        var target := root if target_path in [".", ""] else root.get_node_or_null(NodePath(target_path))
        if not target or (target != root and not root.is_ancestor_of(target)): return ""
        target_path = str(root.get_path_to(target))
        var property := String(path.get_subname(0))
        if not property.is_valid_identifier() or property not in target: return ""
        for character in target_path:
            if character.unicode_at(0) <= 32 or character == "#": return ""
        var kind: String = types.get(typeof(target.get(property)), "")
        var value: Variant = target.get(property)
        if kind == "int_map":
            if value.is_typed_key() and value.get_typed_key_builtin() != TYPE_INT: return ""
            if value.is_typed_value() and value.get_typed_value_builtin() != TYPE_INT: return ""
            for key in value:
                if typeof(key) != TYPE_INT or typeof(value[key]) != TYPE_INT: return ""
        elif kind == "int_list":
            if value.is_typed() and value.get_typed_builtin() != TYPE_INT: return ""
            for element in value:
                if typeof(element) != TYPE_INT: return ""
        if id in reference_fields:
            if kind != "int": return ""
            kind = "object_ref"
        if kind == "": return ""
        text += "%s %d %s %s" % [kind, id, target_path, property]
        if kind in ["string", "int_array", "float_array", "bytes", "vec2_array", "int_map", "int_list"]:
            if limits.get(id,0) < 1 or limits[id] > 65535: return ""
            text += " %d" % limits[id]
        text += "\n"
    return text if NetcodeGame.validate_layout(text) else ""

func export_layout() -> void:
    var text := build_text(get_parent())
    if text.is_empty():
        push_error("Check the schema, field IDs, paths, property types and collection limits.")
        return
    var file := FileAccess.open(output,FileAccess.WRITE)
    if not file: push_error("Could not write " + output); return
    file.store_string(text)
