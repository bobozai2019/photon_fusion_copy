#include "scene_editor.hpp"
#include "native_nodes.hpp"
#include "scene_binding.hpp"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/shortcut.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/undo_redo.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {
const char* options[] = {"properties", "client_writable", "smooth_properties", "angle_properties", "limits"};

bool is_replica(Node* node)
{
    return Object::cast_to<NetcodeReplicator>(node) ||
           (node && node->has_method("definition") && node->has_method("replication_root") &&
            node->has_method("_auto_paths"));
}

String path_key(const NodePath& path)
{
    const String text = path;
    return text.begins_with(".:") ? text.trim_prefix(".") : text;
}

Array keys(const Variant& options)
{
    return options.get_type() == Variant::DICTIONARY ? Dictionary(options).keys() : Array(options);
}

bool contains(const Variant& options, const NodePath& path)
{
    const auto list = keys(options);
    for (int64_t i = 0; i < list.size(); ++i) if (path_key(list[i]) == path_key(path)) return true;
    return false;
}

Variant without(const Variant& options, const NodePath& path)
{
    const auto values = keys(options);
    if (options.get_type() == Variant::DICTIONARY) {
        auto result = Dictionary(options).duplicate();
        for (int64_t i = 0; i < values.size(); ++i) if (path_key(values[i]) == path_key(path)) result.erase(values[i]);
        return result;
    }
    auto result = values.duplicate();
    for (int64_t i = result.size(); i-- > 0;) if (path_key(result[i]) == path_key(path)) result.remove_at(i);
    return result;
}

Variant copy_option(const Variant& value)
{
    if (value.get_type() == Variant::DICTIONARY) return Dictionary(value).duplicate();
    return Array(value).duplicate();
}

Dictionary choices(Node* node)
{
    Dictionary declarations;
    const auto reflected = node->get_property_list();
    for (int64_t i = 0; i < reflected.size(); ++i) {
        const Dictionary info = reflected[i];
        if (int(info["usage"]) & (PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_SCRIPT_VARIABLE)) declarations[info["name"]] = info;
    }
    Ref<Script> script = node->get_script();
    while (script.is_valid()) {
        const auto members = script->get_script_property_list();
        for (int64_t i = 0; i < members.size(); ++i) {
            const Dictionary info = members[i];
            if ((int(info["usage"]) & PROPERTY_USAGE_SCRIPT_VARIABLE) && !declarations.has(info["name"])) {
                declarations[info["name"]] = info;
            }
        }
        script = script->get_base_script();
    }
    Dictionary result;
    const auto infos = declarations.values();
    for (int64_t i = 0; i < infos.size(); ++i) {
        const Dictionary info = infos[i];
        if (int(info["usage"]) & PROPERTY_USAGE_READ_ONLY) continue;
        const auto kind = NetcodeSceneBinding::field_kind(info);
        if (!kind.is_empty()) {
            result[String(info["name"])] = kind;
            continue;
        }
        if (int(info["type"]) != Variant::OBJECT) continue;
        const StringName class_name = info.get("class_name", StringName());
        if (class_name.is_empty()) continue;
        const auto entries = ProjectSettings::get_singleton()->get_global_class_list();
        for (int64_t j = 0; j < entries.size(); ++j) {
            const Dictionary entry = entries[j];
            if (StringName(entry["class"]) != class_name) continue;
            Ref<Script> record = ResourceLoader::get_singleton()->load(entry["path"], "Script");
            if (record.is_null() || !ClassDB::is_parent_class(record->get_instance_base_type(), "RefCounted")) break;
            while (record.is_valid()) {
                const auto members = record->get_script_property_list();
                for (int64_t k = 0; k < members.size(); ++k) {
                    const Dictionary member = members[k];
                    if (!(int(member["usage"]) & PROPERTY_USAGE_SCRIPT_VARIABLE)) continue;
                    const auto type = int(member["type"]);
                    if (type == Variant::BOOL || type == Variant::INT || type == Variant::FLOAT || type == Variant::VECTOR2 ||
                        type == Variant::VECTOR3 || type == Variant::VECTOR4 || type == Variant::QUATERNION || type == Variant::COLOR) {
                        result[vformat("%s:%s", info["name"], member["name"])] = NetcodeSceneBinding::field_kind(member);
                    }
                }
                record = record->get_base_script();
            }
            break;
        }
    }
    return result;
}

template <class T>
T* load_view(const String& path)
{
    const Ref<PackedScene> scene = ResourceLoader::get_singleton()->load(path, "PackedScene");
    if (scene.is_null()) return nullptr;
    auto* node = scene->instantiate();
    auto* view = Object::cast_to<T>(node);
    if (!view && node) memdelete(node);
    return view;
}
}

void NetcodeFieldPicker::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("_select_node"), &NetcodeFieldPicker::select_node);
    ClassDB::bind_method(D_METHOD("_show_fields"), &NetcodeFieldPicker::show_fields);
    ClassDB::bind_method(D_METHOD("_on_field_selected", "index"), &NetcodeFieldPicker::on_field_selected);
    ClassDB::bind_method(D_METHOD("_accept"), &NetcodeFieldPicker::accept);
    ClassDB::bind_method(D_METHOD("get_fields"), &NetcodeFieldPicker::get_fields);
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "fields", PROPERTY_HINT_NODE_TYPE, "ItemList", PROPERTY_USAGE_NONE), "", "get_fields");
    ADD_SIGNAL(MethodInfo("chosen", PropertyInfo(Variant::ARRAY, "paths")));
}

String NetcodeFieldPicker::kind_for(Node* replica, const NodePath& path)
{
    if (path.is_absolute() || !path.get_subname_count()) return {};
    auto* node = Object::cast_to<Node>(static_cast<Object*>(replica->call("_target", path)));
    return node ? String(choices(node).get(String(path.get_concatenated_subnames()), String())) : String();
}

void NetcodeFieldPicker::open(Node* replica)
{
    auto* actor = Object::cast_to<Node>(static_cast<Object*>(replica->call("replication_root")));
    if (!actor) return;
    sync_id_ = replica->get_instance_id();
    actor_id_ = actor->get_instance_id();
    nodes_ = get_node<Tree>(NodePath("%Tree"));
    fields_ = get_node<ItemList>(NodePath("%Fields"));
    filter_ = get_node<LineEdit>(NodePath("%Filter"));
    add_branch(actor, nullptr)->select(0);
    select_node();
    popup_centered(Vector2i(660, 460) * EditorInterface::get_singleton()->get_editor_scale());
}

TreeItem* NetcodeFieldPicker::add_branch(Node* node, TreeItem* parent)
{
    auto* item = nodes_->create_item(parent);
    item->set_text(0, node->get_name());
    item->set_metadata(0, int64_t(node->get_instance_id()));
    const auto children = node->get_children();
    for (int64_t i = 0; i < children.size(); ++i) {
        auto* child = Object::cast_to<Node>(static_cast<Object*>(children[i]));
        if (child && child->get_owner() && !is_replica(child)) add_branch(child, item);
    }
    return item;
}

void NetcodeFieldPicker::select_node()
{
    auto* item = nodes_->get_selected();
    if (!item) return;
    auto* node = static_cast<Node*>(ObjectDB::get_instance(ObjectID(int64_t(item->get_metadata(0)))));
    if (!node) return;
    available_ = choices(node);
    filter_->clear();
    show_fields();
}

void NetcodeFieldPicker::show_fields()
{
    auto* sync = static_cast<Node*>(ObjectDB::get_instance(sync_id_));
    auto* actor = static_cast<Node*>(ObjectDB::get_instance(actor_id_));
    auto* selected = nodes_->get_selected();
    if (!sync || !actor || !selected) return;
    auto* node = static_cast<Node*>(ObjectDB::get_instance(ObjectID(int64_t(selected->get_metadata(0)))));
    if (!node) return;
    fields_->clear();
    get_ok_button()->set_disabled(true);
    auto keys = available_.keys();
    keys.sort();
    const String prefix = node == actor ? String() : String(actor->get_path_to(node));
    const auto filter = filter_->get_text();
    Dictionary groups;
    for (int64_t i = 0; i < keys.size(); ++i) {
        const String member = keys[i];
        if (!member.contains(":")) continue;
        const auto group = member.get_slice(":", 0);
        if (!groups.has(group)) groups[group] = Array();
        Array(groups[group]).push_back(NodePath(prefix + String(":") + member));
    }
    const auto names = groups.keys();
    for (int64_t i = 0; i < names.size(); ++i) {
        const String group = names[i];
        if (filter.is_empty() || group.containsn(filter)) {
            const auto index = fields_->add_item(group + String::utf8("　（添加全部子属性）"));
            fields_->set_item_metadata(index, groups[group]);
        }
    }
    const Array automatic = sync->call("_auto_paths");
    for (int64_t i = 0; i < keys.size(); ++i) {
        const String member = keys[i];
        if (!filter.is_empty() && !member.containsn(filter)) continue;
        const NodePath path(prefix + String(":") + member);
        const auto index = fields_->add_item(member);
        fields_->set_item_metadata(index, path);
        fields_->set_item_tooltip(index, String(path));
        if (automatic.has(sync->call("_canonical", path))) fields_->set_item_disabled(index, true);
    }
}

void NetcodeFieldPicker::on_field_selected(int64_t)
{
    get_ok_button()->set_disabled(false);
}

void NetcodeFieldPicker::accept()
{
    const auto selected = fields_->get_selected_items();
    if (selected.is_empty() || fields_->is_item_disabled(selected[0])) return;
    const auto value = fields_->get_item_metadata(selected[0]);
    TypedArray<NodePath> paths;
    if (value.get_type() == Variant::ARRAY) paths.assign(value);
    else paths.push_back(value);
    emit_signal("chosen", paths);
    queue_free();
}

void NetcodeFieldPanel::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("get_replica"), &NetcodeFieldPanel::get_replica);
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "replica", PROPERTY_HINT_NODE_TYPE, "Node", PROPERTY_USAGE_NONE), "", "get_replica");
    ClassDB::bind_method(D_METHOD("_open_picker"), &NetcodeFieldPanel::open_picker);
    ClassDB::bind_method(D_METHOD("_mode", "mode", "path"), &NetcodeFieldPanel::mode);
    ClassDB::bind_method(D_METHOD("_capacity", "value", "path"), &NetcodeFieldPanel::capacity);
}

void NetcodeFieldPanel::_notification(int what)
{
    if (what == NOTIFICATION_EXIT_TREE) {
        rows_ = nullptr;
        return;
    }
    if (what != NOTIFICATION_READY) return;
    auto* editor = EditorInterface::get_singleton();
    const auto scale = editor->get_editor_scale();
    get_node<ScrollContainer>(NodePath("Scroll"))->set_custom_minimum_size(Vector2(0, 120 * scale));
    rows_ = get_node<GridContainer>(NodePath("Scroll/Rows"));
    rows_->add_theme_constant_override("h_separation", std::lround(12 * scale));
    editor->get_editor_undo_redo()->connect("version_changed", callable_mp(this, &NetcodeFieldPanel::refresh), CONNECT_DEFERRED);
    refresh();
}

void NetcodeFieldPanel::set_replica(Node* replica)
{
    replica_id_ = replica ? ObjectID(replica->get_instance_id()) : ObjectID();
    if (replica && rows_) refresh();
}

Node* NetcodeFieldPanel::get_replica() const
{
    return static_cast<Node*>(ObjectDB::get_instance(replica_id_));
}

void NetcodeFieldPanel::open_picker()
{
    auto* replica = get_replica();
    if (!replica) return;
    if (!static_cast<Object*>(replica->call("replication_root"))) {
        UtilityFunctions::push_warning(String::utf8("请先在 Root Path 选择有效的复制根节点。"));
        return;
    }
    auto* dialog = load_view<NetcodeFieldPicker>("res://addons/netcode/editor/field_picker.tscn");
    if (!dialog) return;
    add_child(dialog);
    dialog->connect("chosen", callable_mp(this, &NetcodeFieldPanel::add_fields));
    dialog->open(replica);
}

Variant NetcodeFieldPanel::option(const StringName& key) const
{
    auto* replica = get_replica();
    auto* sync = Object::cast_to<NetcodeReplicator>(replica);
    if (!sync) return replica->get(key);
    const auto config = sync->get_replication_config();
    if (config.is_valid() && key != StringName("client_writable")) return config->get(key);
    return key == StringName("limits") ? Variant(Dictionary()) : Variant(TypedArray<NodePath>());
}

void NetcodeFieldPanel::apply(const String& label, const Dictionary& changes)
{
    auto* replica = get_replica();
    if (!replica) return;
    auto* history = EditorInterface::get_singleton()->get_editor_undo_redo();
    history->create_action(label, UndoRedo::MERGE_DISABLE, replica);
    Object* target = replica;
    Ref<NetcodeReplicationConfig> config;
    if (auto* sync = Object::cast_to<NetcodeReplicator>(replica)) {
        config = sync->get_replication_config();
        if (config.is_null()) {
            config.instantiate();
            history->add_do_property(replica, "replication_config", config);
            history->add_undo_property(replica, "replication_config", Variant());
        }
        target = config.ptr();
    }
    const auto keys = changes.keys();
    for (int64_t i = 0; i < keys.size(); ++i) {
        const StringName key = keys[i];
        history->add_do_property(target, key, changes[key]);
        history->add_undo_property(target, key, copy_option(option(key)));
    }
    history->commit_action();
    callable_mp(this, &NetcodeFieldPanel::refresh).call_deferred();
}

void NetcodeFieldPanel::add_fields(const TypedArray<NodePath>& paths)
{
    auto* replica = get_replica();
    if (!replica) return;
    const Array current = option("properties");
    const Array automatic = replica->call("_auto_paths");
    auto next = current.duplicate();
    for (int64_t i = 0; i < paths.size(); ++i) {
        if (!contains(next, paths[i]) && !contains(automatic, paths[i])) next.push_back(paths[i]);
    }
    if (next.size() == current.size()) return;
    Dictionary changes;
    changes["properties"] = next;
    apply(String::utf8("添加同步属性"), changes);
}

void NetcodeFieldPanel::toggle(bool enabled, const NodePath& path)
{
    auto next = Array(without(option("client_writable"), path));
    if (enabled) next.push_back(path);
    Dictionary changes;
    changes["client_writable"] = next;
    apply(String::utf8("修改客户端写权"), changes);
}

void NetcodeFieldPanel::mode(int interpolation, const NodePath& path)
{
    auto smooth = Array(without(option("smooth_properties"), path));
    auto angle = Array(without(option("angle_properties"), path));
    if (interpolation == 1) smooth.push_back(path);
    if (interpolation == 2) angle.push_back(path);
    Dictionary changes;
    changes["smooth_properties"] = smooth;
    changes["angle_properties"] = angle;
    apply(String::utf8("修改属性插值"), changes);
}

void NetcodeFieldPanel::capacity(double value, const NodePath& path)
{
    auto sizes = Dictionary(without(option("limits"), path));
    if (value > 0) sizes[path] = int(value);
    Dictionary changes;
    changes["limits"] = sizes;
    apply(String::utf8("修改属性容量"), changes);
}

void NetcodeFieldPanel::remove(const NodePath& path)
{
    Dictionary changes;
    const bool native = Object::cast_to<NetcodeReplicator>(get_replica());
    for (const auto* key : options) {
        if (!native || StringName(key) != StringName("client_writable")) changes[key] = without(option(key), path);
    }
    apply(String::utf8("移除同步属性"), changes);
}

void NetcodeFieldPanel::refresh()
{
    auto* replica = get_replica();
    if (!is_inside_tree() || !replica || !rows_) return;
    const auto children = rows_->get_children();
    for (int64_t i = 0; i < children.size(); ++i) {
        auto* child = static_cast<Node*>(static_cast<Object*>(children[i]));
        if (child->get_owner() == this) continue;
        memdelete(child);
    }
    const bool native = Object::cast_to<NetcodeReplicator>(replica);
    const Array automatic = replica->call("_auto_paths");
    auto paths = automatic.duplicate();
    for (const auto* key : options) paths.append_array(keys(option(key)));
    Dictionary seen;
    for (int64_t i = 0; i < paths.size(); ++i) {
        const NodePath path = paths[i];
        const auto label = path_key(path);
        if (seen.has(label)) continue;
        seen[label] = true;
        const bool auto_field = contains(automatic, path);
        const bool valid = contains(option("properties"), path) || auto_field;
        auto* title = memnew(Label);
        title->set_text(valid ? label : label + String::utf8("　（未绑定属性）"));
        title->set_tooltip_text(String(path));
        title->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        rows_->add_child(title);

        const auto kind = NetcodeFieldPicker::kind_for(replica, path);
        const auto wire = NetcodeSceneBinding::wire_kind(kind);
        if (kind != "integer64" && (wire == "bytes" || wire == "string" || wire == "int_array" ||
                                    wire == "float_array" || wire == "vec2_array")) {
            auto* size = memnew(SpinBox);
            size->set_max(65535);
            size->set_tooltip_text(String::utf8("0 使用框架默认容量"));
            const Dictionary limits = option("limits");
            const auto keys = limits.keys();
            for (int64_t j = 0; j < keys.size(); ++j) if (path_key(keys[j]) == label) size->set_value(limits[keys[j]]);
            size->connect("value_changed", callable_mp(this, &NetcodeFieldPanel::capacity).bind(path));
            rows_->add_child(size);
        } else {
            auto* placeholder = memnew(Label);
            placeholder->set_text(String::utf8("—"));
            rows_->add_child(placeholder);
        }
        auto* modes = memnew(OptionButton);
        for (const auto* mode : {"直接同步", "平滑", "角度插值"}) modes->add_item(String::utf8(mode));
        modes->select(contains(option("angle_properties"), path) ? 2 : int(contains(option("smooth_properties"), path)));
        modes->set_disabled(!valid);
        modes->connect("item_selected", callable_mp(this, &NetcodeFieldPanel::mode).bind(path));
        rows_->add_child(modes);
        auto* write = memnew(CheckBox);
        write->set_tooltip_text(String::utf8("允许客户端写入 ") + label);
        write->set_pressed(contains(option("client_writable"), path));
        write->set_disabled(!valid || native);
        write->connect("toggled", callable_mp(this, &NetcodeFieldPanel::toggle).bind(path));
        rows_->add_child(write);
        if (auto_field) {
            rows_->add_child(memnew(Control));
        } else {
            auto* remove = memnew(Button);
            remove->set_text(String::utf8("移除"));
            remove->connect("pressed", callable_mp(this, &NetcodeFieldPanel::remove).bind(path));
            rows_->add_child(remove);
        }
    }
}

void NetcodeEditorPlugin::_notification(int what)
{
    if (what == NOTIFICATION_ENTER_TREE) {
        EditorInterface::get_singleton()->get_selection()->connect("selection_changed",
            callable_mp(this, &NetcodeEditorPlugin::selection_changed), CONNECT_DEFERRED);
        callable_mp(this, &NetcodeEditorPlugin::selection_changed).call_deferred();
    } else if (what == NOTIFICATION_EXIT_TREE) {
        auto* editor = EditorInterface::get_singleton();
        if (editor && editor->get_selection()) {
            editor->get_selection()->disconnect("selection_changed", callable_mp(this, &NetcodeEditorPlugin::selection_changed));
        }
        panel_ = nullptr;
        panel_tab_ = ObjectID();
    }
}

void NetcodeEditorPlugin::selection_changed()
{
    if (!is_inside_tree()) return;
    const auto selected = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
    auto* replica = selected.size() == 1 ? static_cast<Node*>(static_cast<Object*>(selected[0])) : nullptr;
    if (!is_replica(replica)) replica = nullptr;
    if (panel_) {
        if (panel_->get_replica() == replica) return;
        if (!replica && panel_->is_visible_in_tree()) hide_bottom_panel();
        panel_->set_replica(replica);
        if (auto* tab = Object::cast_to<Button>(ObjectDB::get_instance(panel_tab_))) tab->set_visible(replica != nullptr);
    } else if (replica) {
        panel_ = load_view<NetcodeFieldPanel>("res://addons/netcode/editor/field_panel.tscn");
        if (!panel_) return;
        panel_->set_replica(replica);
        auto* tab = add_control_to_bottom_panel(panel_, String::utf8("Netcode 同步属性"));
        if (tab) panel_tab_ = tab->get_instance_id();
    }
    if (replica && panel_) make_bottom_panel_item_visible(panel_);
}
