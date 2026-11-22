@tool
extends EditorInspectorPlugin

const Replica = preload("../scene_replicator.gd")
const FieldPanel = preload("field_panel.gd")

func _can_handle(object: Object) -> bool:
    return object is Replica

func _parse_property(_object: Object, _type: Variant.Type, name: String, _hint: PropertyHint,
        _hint_text: String, _usage: int, _wide: bool) -> bool:
    return name in FieldPanel.OPTIONS
