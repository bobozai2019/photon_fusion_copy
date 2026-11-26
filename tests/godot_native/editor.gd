@tool
extends EditorPlugin

func _enter_tree() -> void:
    _test.call_deferred()

func _test() -> void:
    var deadline := Time.get_ticks_msec() + 10000
    while not EditorInterface.get_edited_scene_root() and Time.get_ticks_msec() < deadline:
        await get_tree().process_frame
    var path := "res://orb.tscn"
    EditorInterface.open_scene_from_path(path)
    var replica: NetcodeSharedReplicator
    deadline = Time.get_ticks_msec() + 10000
    while not replica and Time.get_ticks_msec() < deadline:
        var scene := EditorInterface.get_edited_scene_root()
        if scene and scene.scene_file_path == path:
            replica = scene.get_node_or_null("Sync") as NetcodeSharedReplicator
        if not replica:
            await get_tree().process_frame
    if not replica:
        _fail("Open orb.tscn and find Sync")
        return
    EditorInterface.get_selection().add_node(replica)
    EditorInterface.edit_node(replica)
    var panel: Node
    deadline = Time.get_ticks_msec() + 10000
    while not panel and Time.get_ticks_msec() < deadline:
        for control in EditorInterface.get_base_control().find_children("*", "VBoxContainer", true, false):
            if control.is_visible_in_tree() and control is NetcodeFieldPanel and control.replica == replica:
                panel = control
                break
        if not panel:
            await get_tree().process_frame
    if not panel:
        _fail("Native Shared field panel")
        return
    panel.get_child(0).pressed.emit()
    await get_tree().process_frame
    var picker: ConfirmationDialog = panel.get_children().back()
    var choice := -1
    for row in picker.fields.item_count:
        if picker.fields.get_item_metadata(row) == ^":trail":
            choice = row
            break
    if choice == -1:
        _fail("Native trail picker")
        return
    picker.fields.select(choice)
    picker.fields.item_selected.emit(choice)
    picker.confirmed.emit()
    for step in 3: await get_tree().process_frame
    var config := replica.replication_config
    if config.properties != [^":value", ^":trail"]:
        _fail("Picked field stored in shared configuration")
        return
    panel._capacity(4, ^":trail")
    panel._mode(1, ^":trail")
    var manager := EditorInterface.get_editor_undo_redo()
    var history := manager.get_history_undo_redo(manager.get_object_history_id(replica))
    history.undo()
    if not config.smooth_properties.is_empty() or config.properties != [^":value", ^":trail"]:
        _fail("Undo interpolation")
        return
    history.undo()
    if not config.limits.is_empty():
        _fail("Undo capacity")
        return
    history.undo()
    if config.properties != [^":value"]:
        _fail("Undo selection preserves the earlier fields")
        return
    history.redo()
    history.redo()
    history.redo()
    if config.smooth_properties != [^":trail"] or config.limits.get(^":trail", 0) != 4:
        _fail("Redo interpolation and capacity")
        return
    if ResourceSaver.save(config) != OK:
        _fail("Save shared resource")
        return
    var reopened := ResourceLoader.load(config.resource_path, "", ResourceLoader.CACHE_MODE_IGNORE) as NetcodeReplicationConfig
    if reopened.properties != config.properties or reopened.limits != config.limits or reopened.smooth_properties != config.smooth_properties:
        _fail("Reload shared resource after editor change")
        return
    print("NATIVE editor PASS")
    get_tree().quit()

func _fail(reason: String) -> void:
    push_error(reason)
    get_tree().quit(1)
