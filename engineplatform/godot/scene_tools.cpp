#include "scene_tools.hpp"
#include "game.hpp"
#include "scene_binding.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cmath>
#include <set>

using namespace godot;

void NetcodePhysicsIsland3D::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("add", "id", "body"), &NetcodePhysicsIsland3D::add);
    ClassDB::bind_method(D_METHOD("capture", "tick"), &NetcodePhysicsIsland3D::capture);
    ClassDB::bind_method(D_METHOD("restore", "fields"), &NetcodePhysicsIsland3D::restore);
    ClassDB::bind_method(D_METHOD("get_bodies"), &NetcodePhysicsIsland3D::get_bodies);
    ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "bodies", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_bodies");
}

bool NetcodePhysicsIsland3D::add(int64_t id, RigidBody3D* body)
{
    if (id <= 0 || id > UINT32_MAX || !body || bodies_.count(id)) return false;
    const ObjectID key(body->get_instance_id());
    for (const auto& [_, registered] : bodies_) {
        if (registered == key) return false;
    }
    return bodies_.emplace(uint32_t(id), key).second;
}

Dictionary NetcodePhysicsIsland3D::get_bodies() const
{
    Dictionary result;
    for (const auto& [id, body] : bodies_) result[int64_t(id)] = ObjectDB::get_instance(body);
    return result;
}

Array NetcodePhysicsIsland3D::capture(int64_t tick) const
{
    PackedInt32Array ids, flags;
    PackedFloat32Array state;
    for (const auto& [id, body] : bodies_) {
        const auto* node = static_cast<RigidBody3D*>(ObjectDB::get_instance(body));
        if (!node) return {};
        const auto transform = node->get_global_transform();
        const auto q = transform.basis.get_rotation_quaternion();
        const auto p = transform.origin;
        const auto v = node->get_linear_velocity();
        const auto w = node->get_angular_velocity();
        ids.push_back(id);
        flags.push_back(node->is_sleeping());
        for (const auto number : {p.x, p.y, p.z, q.x, q.y, q.z, q.w, v.x, v.y, v.z, w.x, w.y, w.z}) {
            state.push_back(number);
        }
    }
    return Array::make(tick, ids, flags, state);
}

bool NetcodePhysicsIsland3D::restore(const Array& fields) const
{
    if (fields.size() != 4 || fields[0].get_type() != Variant::INT ||
        fields[1].get_type() != Variant::PACKED_INT32_ARRAY || fields[2].get_type() != Variant::PACKED_INT32_ARRAY ||
        fields[3].get_type() != Variant::PACKED_FLOAT32_ARRAY) return false;
    const PackedInt32Array ids = fields[1], flags = fields[2];
    const PackedFloat32Array state = fields[3];
    if (std::size_t(ids.size()) != bodies_.size() || ids.size() != flags.size() || state.size() != 13 * ids.size()) return false;
    std::set<uint32_t> seen;
    for (int64_t i = 0; i < ids.size(); ++i) {
        const uint32_t id = ids[i];
        const auto body = bodies_.find(id);
        const auto offset = 13 * i;
        if (body == bodies_.end() || !ObjectDB::get_instance(body->second) || !seen.insert(id).second ||
            (flags[i] != 0 && flags[i] != 1)) return false;
        for (int j = 0; j < 13; ++j) if (!std::isfinite(state[offset + j])) return false;
        const Quaternion q(state[offset + 3], state[offset + 4], state[offset + 5], state[offset + 6]);
        if (std::abs(q.length_squared() - 1.0) > 0.001) return false;
    }
    for (int64_t i = 0; i < ids.size(); ++i) {
        auto* body = static_cast<RigidBody3D*>(ObjectDB::get_instance(bodies_.at(uint32_t(ids[i]))));
        if (!body) return false;
        const auto* value = state.ptr() + i * 13;
        body->set_global_transform(Transform3D(Basis(Quaternion(value[3], value[4], value[5], value[6])),
                                               Vector3(value[0], value[1], value[2])));
        body->set_linear_velocity(Vector3(value[7], value[8], value[9]));
        body->set_angular_velocity(Vector3(value[10], value[11], value[12]));
        body->set_sleeping(flags[i] != 0);
    }
    return true;
}

void NetcodeLayoutExporter::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("build_text", "root"), &NetcodeLayoutExporter::build_text);
    ClassDB::bind_method(D_METHOD("export_layout"), &NetcodeLayoutExporter::export_layout);
    ClassDB::bind_method(D_METHOD("get_export_action"), &NetcodeLayoutExporter::get_export_action);
    ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "export_action", PROPERTY_HINT_TOOL_BUTTON, "Export replication layout",
                              PROPERTY_USAGE_EDITOR), "", "get_export_action");
#define NETCODE_LAYOUT_PROPERTY(type, name, ...) \
    ClassDB::bind_method(D_METHOD("get_" #name), &NetcodeLayoutExporter::get_##name); \
    ClassDB::bind_method(D_METHOD("set_" #name, "value"), &NetcodeLayoutExporter::set_##name); \
    ADD_PROPERTY(PropertyInfo(Variant::type, #name, __VA_ARGS__), "set_" #name, "get_" #name)
    NETCODE_LAYOUT_PROPERTY(INT, schema_id, PROPERTY_HINT_RANGE, "1,65535");
    NETCODE_LAYOUT_PROPERTY(INT, version, PROPERTY_HINT_RANGE, "1,65535");
    NETCODE_LAYOUT_PROPERTY(STRING, schema_name, PROPERTY_HINT_NONE, "");
    NETCODE_LAYOUT_PROPERTY(DICTIONARY, fields, PROPERTY_HINT_DICTIONARY_TYPE, "int;NodePath");
    NETCODE_LAYOUT_PROPERTY(PACKED_INT32_ARRAY, reference_fields, PROPERTY_HINT_NONE, "");
    NETCODE_LAYOUT_PROPERTY(DICTIONARY, limits, PROPERTY_HINT_DICTIONARY_TYPE, "int;int");
    NETCODE_LAYOUT_PROPERTY(STRING, output, PROPERTY_HINT_FILE, "*.cfg");
#undef NETCODE_LAYOUT_PROPERTY
}

String NetcodeLayoutExporter::build_text(Node* root) const
{
    if (!root || fields.is_empty() || fields.size() > 64 || !schema_name.is_valid_identifier()) return {};
    auto text = vformat("schema %d %d %s\n", schema_id, version, schema_name);
    auto ids = fields.keys();
    ids.sort();
    for (int64_t i = 0; i < ids.size(); ++i) {
        const int64_t id = ids[i];
        const NodePath path = fields[id];
        if (id < 1 || id > 65535 || path.is_absolute() || path.get_subname_count() != 1) return {};
        auto* target = path.get_name_count() ? root->get_node_or_null(NodePath(path.get_concatenated_names())) : root;
        if (!target || (target != root && !root->is_ancestor_of(target))) return {};
        const String target_path = root->get_path_to(target);
        const auto member = path.get_subname(0);
        if (!String(member).is_valid_identifier()) return {};
        for (int64_t j = 0; j < target_path.length(); ++j) if (target_path[j] <= 32 || target_path[j] == '#') return {};
        const auto value = target->get(member);
        Dictionary info;
        info["type"] = value.get_type();
        auto kind = NetcodeSceneBinding::field_kind(info);
        if (kind == "integer64") kind = "int";
        if (kind == "dictionary_value") {
            const Dictionary map = value;
            if ((map.is_typed_key() && map.get_typed_key_builtin() != Variant::INT) ||
                (map.is_typed_value() && map.get_typed_value_builtin() != Variant::INT)) return {};
            const auto keys = map.keys();
            for (int64_t j = 0; j < keys.size(); ++j) {
                if (keys[j].get_type() != Variant::INT || map[keys[j]].get_type() != Variant::INT) return {};
            }
            kind = "int_map";
        } else if (kind == "array_value") {
            const Array array = value;
            if (array.is_typed() && array.get_typed_builtin() != Variant::INT) return {};
            for (int64_t j = 0; j < array.size(); ++j) if (array[j].get_type() != Variant::INT) return {};
            kind = "int_list";
        }
        if (reference_fields.has(id)) {
            if (kind != "int") return {};
            kind = "object_ref";
        }
        if (kind.is_empty()) return {};
        text += vformat("%s %d %s %s", kind, id, target_path, member);
        if (kind == "string" || kind == "int_array" || kind == "float_array" || kind == "bytes" ||
            kind == "vec2_array" || kind == "int_map" || kind == "int_list") {
            const int capacity = limits.get(id, 0);
            if (capacity < 1 || capacity > 65535) return {};
            text += vformat(" %d", capacity);
        }
        text += "\n";
    }
    return NetcodeGame::validate_layout(text) ? text : String();
}

void NetcodeLayoutExporter::export_layout() const
{
    const auto text = build_text(get_parent());
    if (text.is_empty()) {
        UtilityFunctions::push_error("Check the schema, field IDs, paths, property types and collection limits.");
        return;
    }
    const auto file = FileAccess::open(output, FileAccess::WRITE);
    if (file.is_valid()) file->store_string(text);
    else UtilityFunctions::push_error(String("Could not write ") + output);
}
