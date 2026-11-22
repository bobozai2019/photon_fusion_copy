@tool
extends EditorPlugin

const Inspector = preload("inspector.gd")
const Replica = preload("../scene_replicator.gd")
const FieldPanel = preload("field_panel.gd")
const FIELD_PANEL_SCENE := preload("field_panel.tscn")
var inspector: EditorInspectorPlugin
var panel: FieldPanel

func _enter_tree() -> void:
    inspector = Inspector.new()
    add_inspector_plugin(inspector)
    EditorInterface.get_selection().selection_changed.connect(_selection_changed)
    _selection_changed.call_deferred()

func _selection_changed() -> void:
    if not is_inside_tree(): return
    var selected := EditorInterface.get_selection().get_selected_nodes()
    var replica: Replica = selected[0] as Replica if selected.size() == 1 else null
    if replica and panel and panel.replica == replica: return
    _remove_panel()
    if replica == null: return
    panel = FIELD_PANEL_SCENE.instantiate() as FieldPanel
    panel.replica = replica
    add_control_to_bottom_panel(panel, "Netcode 同步属性")
    make_bottom_panel_item_visible(panel)

func _remove_panel() -> void:
    if panel:
        panel.replica = null
        remove_control_from_bottom_panel(panel)
        panel.queue_free()
        panel = null

func _exit_tree() -> void:
    var selection := EditorInterface.get_selection()
    if selection.selection_changed.is_connected(_selection_changed):
        selection.selection_changed.disconnect(_selection_changed)
    _remove_panel()
    remove_inspector_plugin(inspector)
    inspector = null
