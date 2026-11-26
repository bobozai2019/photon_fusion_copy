#pragma once

#include <godot_cpp/classes/confirmation_dialog.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/tree_item.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

class NetcodeFieldPicker : public godot::ConfirmationDialog {
    GDCLASS(NetcodeFieldPicker, godot::ConfirmationDialog)

public:
    void open(godot::Node* replica);
    godot::ItemList* get_fields() const { return fields_; }
    static godot::String kind_for(godot::Node* replica, const godot::NodePath& path);

protected:
    static void _bind_methods();

private:
    godot::TreeItem* add_branch(godot::Node* node, godot::TreeItem* parent);
    void select_node();
    void show_fields();
    void on_field_selected(int64_t index);
    void accept();
    godot::ObjectID sync_id_;
    godot::ObjectID actor_id_;
    godot::Tree* nodes_ = nullptr;
    godot::ItemList* fields_ = nullptr;
    godot::LineEdit* filter_ = nullptr;
    godot::Dictionary available_;
};

class NetcodeFieldPanel : public godot::VBoxContainer {
    GDCLASS(NetcodeFieldPanel, godot::VBoxContainer)

public:
    void set_replica(godot::Node* replica);
    godot::Node* get_replica() const;

protected:
    static void _bind_methods();
    void _notification(int what);

private:
    void open_picker();
    void add_fields(const godot::TypedArray<godot::NodePath>& paths);
    void toggle(bool enabled, const godot::NodePath& path);
    void mode(int interpolation, const godot::NodePath& path);
    void capacity(double value, const godot::NodePath& path);
    void remove(const godot::NodePath& path);
    void refresh();
    godot::Variant option(const godot::StringName& key) const;
    void apply(const godot::String& label, const godot::Dictionary& changes);
    godot::ObjectID replica_id_;
    godot::GridContainer* rows_ = nullptr;
};

class NetcodeEditorPlugin : public godot::EditorPlugin {
    GDCLASS(NetcodeEditorPlugin, godot::EditorPlugin)

protected:
    static void _bind_methods() {}
    void _notification(int what);

private:
    void selection_changed();
    NetcodeFieldPanel* panel_ = nullptr;
    godot::ObjectID panel_tab_;
};
