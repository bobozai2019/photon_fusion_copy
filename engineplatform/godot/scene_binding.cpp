#include "scene_binding.hpp"
#include "native_nodes.hpp"
#include "scene_value.hpp"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/rigid_body2d.hpp>
#include <godot_cpp/classes/rigid_body3d.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstddef>

using namespace godot;

namespace {
struct RootPropertyValues {
    RootPropertyValues() : scratch(storage, sizeof(storage)), fields(&scratch) {}

    // Schemas hold at most 64 fields. Each call owns its values across script hooks.
    alignas(Variant) std::byte storage[64 * sizeof(Variant)];
    std::pmr::monotonic_buffer_resource scratch;
    std::pmr::vector<Variant> fields;
};

StringName root_key(const NetcodeGame& game)
{
    return String("netcode_binding_") + String::num_uint64(game.get_instance_id(), 16);
}

StringName replica_key(int64_t id)
{
    return String("netcode_replica_") + String::num_int64(id);
}

Object* binding_owner(Object* owner, const StringName& key)
{
    if (!owner->has_meta(key)) return nullptr;
    Object* reference = owner->get_meta(key);
    return reference ? static_cast<Object*>(reference->call("get_ref")) : nullptr;
}

Array record_fields(Ref<Script> script)
{
    Array result;
    Dictionary names;
    while (script.is_valid()) {
        const auto properties = script->get_script_property_list();
        for (int64_t i = 0; i < properties.size(); ++i) {
            const Dictionary info = properties[i];
            const Variant name = info["name"];
            if (names.has(name) || !(int(info["usage"]) & PROPERTY_USAGE_SCRIPT_VARIABLE)) continue;
            switch (int(info["type"])) {
            case Variant::BOOL: case Variant::INT: case Variant::FLOAT: case Variant::VECTOR2:
            case Variant::VECTOR3: case Variant::VECTOR4: case Variant::QUATERNION: case Variant::COLOR: {
                Dictionary field;
                field["name"] = name;
                field["type"] = info["type"];
                result.push_back(field);
                names[name] = true;
                break;
            }
            default:
                break;
            }
        }
        script = script->get_base_script();
    }
    return result;
}

Variant member_value(Object* target, const StringName& member, const NodePath& indexed)
{
    return indexed.is_empty() ? target->get(member) : target->get_indexed(indexed);
}

void set_member(Object* target, const StringName& member, const NodePath& indexed, const Variant& value)
{
    if (indexed.is_empty()) target->set(member, value);
    else target->set_indexed(indexed, value);
}
}

NetcodeSceneBinding::NetcodeSceneBinding(Node* root, NetcodeReplicator* sync)
    : root_id_(root->get_instance_id()), sync_id_(sync->get_instance_id())
{
    if (sync->get_root_replication_mode() != NetcodeReplicator::AUTO) return;
    if (Object::cast_to<RigidBody2D>(root)) body2d_.instantiate();
    else if (Object::cast_to<RigidBody3D>(root)) body3d_.instantiate();
}

Node* NetcodeSceneBinding::root() const
{
    return static_cast<Node*>(ObjectDB::get_instance(root_id_));
}

Node* NetcodeSceneBinding::target(const Field& field) const
{
    auto* node = root();
    return node && !field.target.is_empty() ? node->get_node_or_null(field.target) : node;
}

Dictionary NetcodeSceneBinding::property_info(Node* node, const NodePath& path)
{
    if (!node || !path.get_subname_count()) return {};
    Object* owner = node;
    Variant child;
    for (int64_t i = 0; i + 1 < path.get_subname_count(); ++i) {
        child = owner->get(path.get_subname(i));
        owner = Object::cast_to<RefCounted>(static_cast<Object*>(child));
        if (!owner) return {};
    }
    const auto member = path.get_subname(path.get_subname_count() - 1);
    const auto properties = owner->get_property_list();
    for (int64_t i = 0; i < properties.size(); ++i) {
        const Dictionary info = properties[i];
        if (StringName(info["name"]) == member && !(int(info["usage"]) & PROPERTY_USAGE_READ_ONLY)) return info;
    }
    return {};
}

String NetcodeSceneBinding::field_kind(const Dictionary& info)
{
    switch (int(info.get("type", Variant::NIL))) {
    case Variant::FLOAT: return "float";
    case Variant::BOOL: return "flag";
    case Variant::INT: return "integer64";
    case Variant::VECTOR2: return "vec2";
    case Variant::VECTOR3: return "vec3";
    case Variant::QUATERNION: return "quat";
    case Variant::STRING: return "string";
    case Variant::VECTOR4: return "vec4";
    case Variant::COLOR: return "color";
    case Variant::PACKED_BYTE_ARRAY: return "bytes";
    case Variant::PACKED_VECTOR2_ARRAY: return "vec2_array";
    case Variant::DICTIONARY: return "dictionary_value";
    case Variant::ARRAY: return "array_value";
    case Variant::PACKED_INT32_ARRAY: return "int_array";
    case Variant::PACKED_FLOAT32_ARRAY: return "float_array";
    case Variant::OBJECT: {
        StringName name = info.get("class_name", StringName());
        if (name.is_empty()) name = info.get("hint_string", String());
        if (name == StringName("Node") || name == StringName("Object") || ClassDB::is_parent_class(name, "Node")) {
            return "object_ref";
        }
        const auto classes = ProjectSettings::get_singleton()->get_global_class_list();
        for (int64_t i = 0; i < classes.size(); ++i) {
            const Dictionary entry = classes[i];
            if (StringName(entry["class"]) != name) continue;
            const Ref<Script> script = ResourceLoader::get_singleton()->load(entry["path"], "Script");
            if (script.is_valid() && ClassDB::is_parent_class(script->get_instance_base_type(), "Node")) return "object_ref";
        }
        return {};
    }
    default: return {};
    }
}

String NetcodeSceneBinding::wire_kind(const String& kind)
{
    if (kind == "array_value" || kind == "dictionary_value" || kind == "integer64") return "bytes";
    return kind;
}

Dictionary NetcodeSceneBinding::definition()
{
    if (object_) return {};
    auto* actor = root();
    auto* sync = static_cast<NetcodeReplicator*>(ObjectDB::get_instance(sync_id_));
    if (!actor || !sync) return {};
    const auto config = sync->get_replication_config();
    const auto automatic = sync->auto_paths();
    auto paths = automatic.duplicate();
    Dictionary interpolation;
    Dictionary capacities;
    if (config.is_valid()) {
        const auto properties = config->get_properties();
        const auto smooth = config->get_smooth_properties();
        const auto angles = config->get_angle_properties();
        const auto limits = config->get_limits();
        for (int64_t i = 0; i < properties.size(); ++i) {
            const auto path = sync->canonical(properties[i]);
            if (path.is_empty() || paths.has(path)) return {};
            paths.push_back(path);
        }
        for (int64_t i = 0; i < smooth.size(); ++i) interpolation[sync->canonical(smooth[i])] = 1;
        for (int64_t i = 0; i < angles.size(); ++i) interpolation[sync->canonical(angles[i])] = 2;
        const auto keys = limits.keys();
        for (int64_t i = 0; i < keys.size(); ++i) capacities[sync->canonical(keys[i])] = limits[keys[i]];
        for (const auto& options : {interpolation.keys(), capacities.keys()}) {
            for (int64_t i = 0; i < options.size(); ++i) if (!paths.has(options[i])) return {};
        }
    }

    PackedStringArray lines;
    PackedStringArray declarations;
    PackedInt32Array writable;
    std::vector<Field> fields;
    uint64_t integer_fields = 0;
    bool direct = !uses_body() && !paths.is_empty() && interpolation.is_empty() && automatic.is_empty();
    if (uses_body()) {
        const bool two_d = body2d_.is_valid();
        const char* motion[] = {two_d ? "vec2" : "vec3", two_d ? "float" : "quat", two_d ? "vec2" : "vec3",
                                two_d ? "float" : "vec3", "flag", "int", "int"};
        for (const auto* type : motion) {
            const auto id = lines.size() + 1;
            lines.push_back(vformat("%s %d . motion_%d", String(type), id, id));
            writable.push_back(id);
        }
    }
    for (int64_t i = 0; i < paths.size(); ++i) {
        Field field;
        field.path = paths[i];
        const auto node_path = field.path.get_concatenated_names();
        if (node_path != StringName(".")) field.target = NodePath(node_path);
        field.member = field.path.get_subname(0);
        if (field.path.get_subname_count() > 1) {
            field.indexed = NodePath(String(":") + field.path.get_concatenated_subnames());
        }
        auto* node = target(field);
        const auto info = property_info(node, field.path);
        const auto name = field_kind(info);
        if (name.is_empty()) return {};
        field.hint = info.get("hint_string", String());
        field.interpolation = interpolation.get(field.path, automatic.has(field.path) ? 1 : 0);
        if (automatic.has(field.path) && field.path == NodePath(".:rotation")) field.interpolation = 2;
        if (name == "integer64") {
            field.kind = Kind::Integer;
            if (i < 64) integer_fields |= uint64_t{1} << i;
        }
        else if (name == "object_ref") field.kind = Kind::Reference;
        else if (name == "array_value") field.kind = Kind::Array;
        else if (name == "dictionary_value") field.kind = Kind::Dictionary;

        if (field.kind == Kind::Array) {
            const Array array = member_value(node, field.member, field.indexed);
            if (array.get_typed_builtin() == Variant::OBJECT) {
                field.record = array.get_typed_script();
                if (field.record.is_null() || field.record->get_global_name().is_empty() ||
                    !ClassDB::is_parent_class(field.record->get_instance_base_type(), "RefCounted")) return {};
                field.record_fields = record_fields(field.record);
                if (field.record_fields.is_empty()) return {};
                declarations.push_back(UtilityFunctions::str(field.record_fields));
            }
        }

        const auto wire = wire_kind(name);
        const auto id = lines.size() + 1;
        auto line = vformat("%s %d . f%d", wire, id, id);
        if (wire == "bytes" || wire == "string" || wire == "int_array" || wire == "float_array" || wire == "vec2_array") {
            const int default_size = wire == "bytes" ? 128 : (wire == "string" ? 64 : 8);
            field.capacity = field.kind == Kind::Integer ? 8 : int(capacities.get(field.path, default_size));
            if (field.capacity < 1 || field.capacity > 65535) return {};
            line += vformat(" %d", field.capacity);
        }
        if (capacities.has(field.path) && (!field.capacity || field.kind == Kind::Integer)) return {};
        writable.push_back(id);
        lines.push_back(line);
        declarations.push_back(vformat("%s/%s/%s", field.path, name, field.hint));
        direct = direct && field.target.is_empty() && field.indexed.is_empty() &&
                 (field.kind == Kind::Raw || field.kind == Kind::Integer);
        fields.push_back(std::move(field));
    }

    if (lines.is_empty()) lines.push_back("flag 1 . present");
    const Ref<Script> script = actor->get_script();
    String key = script.is_valid() ? script->get_path() : String(actor->get_class());
    if (Object::cast_to<NetcodeServerReplicator>(sync)) key = String("server:") + key;
    const auto body = String("\n").join(lines) + "\n";
    const auto source = String("object:") + key + "\n" + body + UtilityFunctions::str(declarations) +
                        UtilityFunctions::str(writable);
    schema_ = 1 + source.sha256_text().substr(0, 8).hex_to_int() % 65535;
    fields_ = std::move(fields);
    direct_ = direct;
    integer_fields_ = integer_fields;
    set_spatial_position_property(sync->get_spatial_position_property());
    Dictionary result;
    result["id"] = schema_;
    result["key"] = key;
    result["text"] = vformat("schema %d 1 Actor\n", schema_) + body;
    result["writable"] = writable;
    result["player_attached"] = false;
    return result;
}

void NetcodeSceneBinding::set_game(const Ref<NetcodeGame>& game)
{
    reference_game_id_ = game.is_valid() ? ObjectID(game->get_instance_id()) : ObjectID();
}

bool NetcodeSceneBinding::bind(const Ref<NetcodeGame>& game, int64_t object)
{
    unbind();
    auto* actor = root();
    auto* sync = ObjectDB::get_instance(sync_id_);
    if (!actor || !sync || game.is_null() || object <= 0 || !schema_) return false;
    const auto actor_key = root_key(*game.ptr());
    if (binding_owner(actor, actor_key) || binding_owner(game.ptr(), replica_key(object))) return false;
    for (auto& field : fields_) {
        if (!target(field)) return false;
    }
    const auto reference = UtilityFunctions::weakref(sync);
    game->set_meta(replica_key(object), reference);
    actor->set_meta(actor_key, reference);
    game_ = game;
    set_game(game);
    object_ = object;
    return true;
}

void NetcodeSceneBinding::unbind()
{
    ++binding_;
    if (game_.is_valid()) {
        if (auto* actor = root()) actor->remove_meta(root_key(*game_.ptr()));
        game_->remove_meta(replica_key(object_));
        game_->unbind_replica(object_);
    }
    game_.unref();
    object_ = 0;
    baseline_epoch_ = -1;
    initialized_ = false;
    body_fields_.clear();
    if (body2d_.is_valid()) body2d_->generation = -1;
    if (body3d_.is_valid()) body3d_->generation = -1;
    for (auto& field : fields_) {
        field.reference = 0;
        field.reference_node = ObjectID();
    }
    clear_goals();
}

bool NetcodeSceneBinding::is_write_ready() const
{
    return initialized_ && baseline_epoch_ >= 0 && game_->replica_epoch(object_, true) == baseline_epoch_;
}

bool NetcodeSceneBinding::prime_writer()
{
    const auto baseline = game_->sync_scene_replica(object_, game_->read_fields(object_, false), uses_body());
    if (!baseline || !baseline->baseline) return false;
    before_apply(true);
    initialized_ = true;
    return true;
}

Variant NetcodeSceneBinding::read(Field& field, Node* node)
{
    const auto generation = binding_;
    const ObjectID target_id(node->get_instance_id());
    auto value = member_value(node, field.member, field.indexed);
    if (generation != binding_ || !ObjectDB::get_instance(target_id)) return {};
    switch (field.kind) {
    case Kind::Raw:
        return value;
    case Kind::Integer:
        if (value.get_type() != Variant::INT) return {};
        if (field.previous_integer.get_type() != Variant::INT || field.previous_integer != value) {
            field.previous_integer = value;
            field.encoded_integer = netcode_godot::encode_integer(value);
        }
        return field.encoded_integer;
    case Kind::Reference: {
        auto* game = static_cast<NetcodeGame*>(ObjectDB::get_instance(reference_game_id_));
        if (!game) return int64_t(0);
        auto* source = Object::cast_to<Node>(static_cast<Object*>(value));
        if (field.reference && source == ObjectDB::get_instance(field.reference_node)) {
            auto* sync = Object::cast_to<NetcodeReplicator>(binding_owner(game, replica_key(game->resolve_field(field.reference))));
            auto* resolved = sync ? sync->replication_root() : nullptr;
            if (source != resolved) {
                field.reference_node = resolved ? ObjectID(resolved->get_instance_id()) : ObjectID();
                set_member(node, field.member, field.indexed, resolved);
            }
            return field.reference;
        }
        field.reference = 0;
        if (!source) return int64_t(0);
        auto* other = Object::cast_to<NetcodeReplicator>(binding_owner(source, root_key(*game)));
        return other ? game->field_reference(other->get_object_id()) : int64_t(0);
    }
    case Kind::Array:
        if (value.get_type() != Variant::ARRAY) return {};
        if (field.record.is_valid()) {
            const Array records = value;
            Array rows;
            for (int64_t i = 0; i < records.size(); ++i) {
                Object* record = records[i];
                if (!record) return {};
                Array row;
                for (int64_t j = 0; j < field.record_fields.size(); ++j) {
                    const Dictionary info = field.record_fields[j];
                    row.push_back(record->get(info["name"]));
                    if (generation != binding_) return {};
                }
                rows.push_back(row);
            }
            value = rows;
        }
        return netcode_godot::encode_collection(value, field.capacity);
    case Kind::Dictionary:
        return value.get_type() == Variant::DICTIONARY ? netcode_godot::encode_collection(value, field.capacity) : Variant();
    }
    return {};
}

Array NetcodeSceneBinding::sample()
{
    auto* actor = root();
    if (!actor) return {};
    if (fields_.empty() && !uses_body()) return Array::make(true);
    const auto generation = binding_;
    Array sampled;
    if (body2d_.is_valid()) sampled = body2d_->sample(static_cast<RigidBody2D*>(actor), teleport_, phase_);
    if (body3d_.is_valid()) sampled = body3d_->sample(static_cast<RigidBody3D*>(actor), teleport_, phase_);
    const auto offset = sampled.size();
    sampled.resize(offset + fields_.size());
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        auto* node = field.target.is_empty() ? actor : target(field);
        if (!node) return {};
        const auto value = read(field, node);
        if (generation != binding_ || value.get_type() == Variant::NIL) return {};
        sampled[offset + i] = value;
    }
    return sampled;
}

bool NetcodeSceneBinding::write(Field& field, Node* node, const Variant& encoded)
{
    const auto generation = binding_;
    auto value = encoded;
    if (field.kind == Kind::Integer) {
        const PackedByteArray bytes = encoded;
        if (bytes.size() != 8) return false;
        value = bytes.decode_s64(0);
    } else if (field.kind == Kind::Reference) {
        field.reference = encoded;
        auto* sync = Object::cast_to<NetcodeReplicator>(binding_owner(game_.ptr(), replica_key(game_->resolve_field(field.reference))));
        auto* resolved = sync ? sync->replication_root() : nullptr;
        field.reference_node = resolved ? ObjectID(resolved->get_instance_id()) : ObjectID();
        value = resolved;
    } else if (field.kind == Kind::Array || field.kind == Kind::Dictionary) {
        value = netcode_godot::decode_collection(encoded);
        if (value.get_type() != (field.kind == Kind::Array ? Variant::ARRAY : Variant::DICTIONARY)) return false;
        if (field.record.is_valid()) {
            const Array rows = value;
            for (int64_t i = 0; i < rows.size(); ++i) {
                if (rows[i].get_type() != Variant::ARRAY) return false;
                const Array row = rows[i];
                if (row.size() != field.record_fields.size()) return false;
                for (int64_t j = 0; j < row.size(); ++j) {
                    const Dictionary info = field.record_fields[j];
                    if (row[j].get_type() != int(info["type"])) return false;
                }
            }
            Array array = member_value(node, field.member, field.indexed);
            if (binding_ != generation) return false;
            array.resize(rows.size());
            for (int64_t i = 0; i < rows.size(); ++i) {
                if (array[i].get_type() == Variant::NIL || !static_cast<Object*>(array[i])) array[i] = field.record->call("new");
                Object* record = array[i];
                if (!record || binding_ != generation) return false;
                const Array row = rows[i];
                for (int64_t j = 0; j < row.size(); ++j) {
                    const Dictionary info = field.record_fields[j];
                    record->set(info["name"], row[j]);
                    if (binding_ != generation) return false;
                }
            }
            return true;
        }
        value = netcode_godot::type_collection(value, field.hint);
        if (value.get_type() == Variant::NIL) return false;
    }
    if (!snap_ && field.interpolation && value.get_type() != Variant::ARRAY) {
        if (field.goal != value) {
            field.goal = value;
            field.goal_age = 0.0;
        }
        return true;
    }
    set_member(node, field.member, field.indexed, value);
    return binding_ == generation;
}

bool NetcodeSceneBinding::apply(const Array& sampled, const Array& fields, bool baseline, int offset, bool invalidated)
{
    const auto minimum = offset + int64_t(fields_.size());
    if (fields.size() < minimum || sampled.size() < minimum) return false;
    const auto generation = binding_;
    bool complete = true;
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        const auto& value = fields[offset + i];
        const bool matched = !baseline && sampled[offset + i] == value;
        if (matched && !invalidated) {
            field.goal = {};
            continue;
        }
        auto* node = target(field);
        if (!node) {
            complete = false;
            continue;
        }
        const ObjectID node_id(node->get_instance_id());
        if (matched) {
            const auto current = read(field, node);
            if (binding_ != generation || !ObjectDB::get_instance(node_id)) return false;
            if (current == value) {
                field.goal = {};
                continue;
            }
        }
        if (!write(field, node, value) || !ObjectDB::get_instance(node_id)) return false;
        invalidated = true;
    }
    return complete;
}

void NetcodeSceneBinding::apply_body(const Array& sampled, const Array& fields, bool baseline, double age)
{
    if (sampled.size() < 7 || fields.size() < 7) return;
    body_fields_ = fields.slice(7);
    teleport_ = fields[5];
    phase_ = fields[6];
    constexpr double teleport_distance = 100.0;
    constexpr double teleport_angle_degrees = 15.0;
    if (body2d_.is_valid()) {
        const auto distance = Vector2(sampled[0]).distance_to(fields[0]);
        const auto angle = std::abs(UtilityFunctions::angle_difference(sampled[1], fields[1]));
        if (baseline || distance > teleport_distance || Math::rad_to_deg(angle) > teleport_angle_degrees) {
            body2d_->generation = ~teleport_;
        }
        body2d_->apply(static_cast<RigidBody2D*>(root()), fields, age);
    } else {
        const auto distance = Vector3(sampled[0]).distance_to(fields[0]);
        const auto angle = Quaternion(sampled[1]).angle_to(fields[1]);
        if (baseline || distance > teleport_distance || Math::rad_to_deg(angle) > teleport_angle_degrees) {
            body3d_->generation = ~teleport_;
        }
        body3d_->apply(static_cast<RigidBody3D*>(root()), fields, age);
    }
}

void NetcodeSceneBinding::before_apply(bool baseline)
{
    if (baseline) baseline_epoch_ = game_->replica_epoch(object_);
    if (direct_) return;
    snap_ = baseline || game_->native_authority(object_);
    if (snap_) clear_goals();
}

void NetcodeSceneBinding::clear_goals()
{
    for (auto& field : fields_) field.goal = {};
}

static Variant interpolate(const Variant& current, const Variant& goal, bool angle, double weight)
{
    if (current.get_type() != goal.get_type()) return goal;
    const auto lerp_angle = [weight](double from, double to) { return Math::lerp_angle(from, to, weight); };
    switch (goal.get_type()) {
    case Variant::FLOAT: {
        const auto value = angle ? lerp_angle(current, goal) : Math::lerp(double(current), double(goal), weight);
        const bool done = angle ? Math::is_zero_approx(UtilityFunctions::angle_difference(value, double(goal))) :
                                 Math::is_equal_approx(value, double(goal));
        return done ? goal : Variant(value);
    }
    case Variant::VECTOR3: {
        const Vector3 from = current, to = goal;
        if (angle) return Vector3(lerp_angle(from.x, to.x), lerp_angle(from.y, to.y), lerp_angle(from.z, to.z));
        return from.lerp(to, weight);
    }
    case Variant::VECTOR2: return Vector2(current).lerp(goal, weight);
    case Variant::VECTOR4: return Vector4(current).lerp(goal, weight);
    case Variant::COLOR: return Color(current).lerp(goal, weight);
    case Variant::QUATERNION: return Quaternion(current).slerp(goal, weight);
    default: return goal;
    }
}

void NetcodeSceneBinding::advance_interpolation(double delta)
{
    constexpr double smoothing_seconds = 0.15;
    const auto generation = binding_;
    const auto weight = 1.0 - std::exp(-delta / smoothing_seconds);
    for (auto& field : fields_) {
        if (field.goal.get_type() == Variant::NIL) continue;
        auto* node = target(field);
        if (!node) continue;
        const ObjectID id(node->get_instance_id());
        const auto current = member_value(node, field.member, field.indexed);
        if (generation != binding_ || !ObjectDB::get_instance(id)) return;
        field.goal_age += delta;
        auto value = interpolate(current, field.goal, field.interpolation == 2, weight);
        if (value == field.goal && field.goal_age < smoothing_seconds * 0.5 &&
            field.goal.get_type() != Variant::FLOAT && field.goal.get_type() != Variant::VECTOR2 &&
            field.goal.get_type() != Variant::VECTOR3 && field.goal.get_type() != Variant::VECTOR4 &&
            field.goal.get_type() != Variant::COLOR && field.goal.get_type() != Variant::QUATERNION) continue;
        if (value == current) value = field.goal;
        if (value != current) set_member(node, field.member, field.indexed, value);
        if (generation != binding_) return;
        if (value == field.goal) field.goal = {};
    }
}

bool NetcodeSceneBinding::apply_model(const Array& fields)
{
    if (game_.is_null() || fields.is_empty() || uses_body()) return false;
    const auto generation = binding_;
    const auto sampled = sample();
    if (generation != binding_ || sampled.size() != fields.size()) return false;
    before_apply(true);
    return apply(sampled, fields, false);
}

void NetcodeSceneBinding::apply_change(const Array& sampled, const NetcodeGame::ReplicaChange& change,
                                      bool body, unsigned generation)
{
    before_apply(change.baseline);
    if (body) apply_body(sampled, change.fields, change.baseline, change.age);
    if (generation != binding_) return;
    const bool applied = apply(sampled, change.fields, change.baseline, body ? 7 : 0, body);
    if (generation != binding_) return;
    initialized_ = applied;
    if (!initialized_) game_->unbind_replica(object_);
}

void NetcodeSceneBinding::set_spatial_position_property(const StringName& property)
{
    spatial_position_slot_ = -1;
    if (!direct_ || property.is_empty()) return;
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i].member == property && fields_[i].kind == Kind::Raw) {
            spatial_position_slot_ = static_cast<int>(i);
            return;
        }
    }
}

void NetcodeSceneBinding::step_root(Node* actor, Variant* sampled_position)
{
    const auto generation = binding_;
    const auto position_slot = spatial_position_slot_;
    RootPropertyValues values;
    auto& sampled = values.fields;
    sampled.reserve(fields_.size());
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto value = actor->get(fields_[i].member);
        if (generation != binding_ || !UtilityFunctions::is_instance_id_valid(root_id_)) return;
        const auto type = value.get_type();
        if (type == Variant::NIL || ((integer_fields_ & (uint64_t{1} << i)) && type != Variant::INT)) return;
        sampled.push_back(std::move(value));
    }
    if (const auto change = game_->sync_root_replica(object_, sampled, integer_fields_)) {
        Array encoded;
        encoded.resize(sampled.size());
        for (std::size_t i = 0; i < sampled.size(); ++i) {
            if (integer_fields_ & (uint64_t{1} << i)) encoded[i] = netcode_godot::encode_integer(sampled[i]);
            else encoded[i] = sampled[i];
        }
        apply_change(encoded, *change, false, generation);
    }
    if (sampled_position && generation == binding_ && position_slot >= 0 && position_slot == spatial_position_slot_)
        *sampled_position = sampled[position_slot];
}

void NetcodeSceneBinding::step(double delta, Variant* sampled_position)
{
    if (game_.is_null()) return;
    auto* actor = root();
    if (!actor) {
        unbind();
        return;
    }
    if (!game_->has_replica_layout(object_, schema_)) {
        if (initialized_) game_->unbind_replica(object_);
        initialized_ = false;
        clear_goals();
        for (auto& field : fields_) field.reference = 0;
        return;
    }
    if (direct_ && initialized_) {
        step_root(actor, sampled_position);
        return;
    }
    const auto generation = binding_;
    const bool body = uses_body();
    const auto sampled = initialized_ || body ? sample() : game_->read_fields(object_, false);
    if (generation != binding_ || sampled.is_empty()) return;
    if (const auto change = game_->sync_scene_replica(object_, sampled, body)) {
        apply_change(sampled, *change, body, generation);
        if (generation != binding_) return;
    } else if (body && !body_fields_.is_empty() && !game_->native_authority(object_)) {
        const auto custom = sampled.slice(7);
        if (custom != body_fields_) {
            before_apply(false);
            apply(custom, body_fields_, false);
        }
    }
    if (!direct_) advance_interpolation(delta);
}

NetcodeSceneBinding::ReceiveResult NetcodeSceneBinding::receive(bool check_pending_creator)
{
    if (game_.is_null()) return ReceiveResult::Unavailable;
    auto* actor = root();
    if (!actor) return ReceiveResult::Unavailable;
    RootPropertyValues values;
    const auto update = game_->take_received_replica(object_, integer_fields_, values.fields, check_pending_creator);
    if (update.role == netcode::ScriptGame::ReplicaRole::Writer) return ReceiveResult::Writer;
    if (update.role == netcode::ScriptGame::ReplicaRole::PendingCreator) return ReceiveResult::PendingCreator;
    if (!update.mask) return ReceiveResult::Unchanged;
    if (update.baseline) before_apply(true);
    const auto generation = binding_;
    for (std::size_t i = 0, next = 0; i < fields_.size(); ++i) {
        const auto bit = uint64_t{1} << i;
        if (!(update.mask & bit)) continue;
        if (next >= values.fields.size()) return ReceiveResult::Applied;
        actor->set(fields_[i].member, values.fields[next++]);
        if (generation != binding_ || !UtilityFunctions::is_instance_id_valid(root_id_)) return ReceiveResult::Applied;
    }
    initialized_ = true;
    return ReceiveResult::Applied;
}
