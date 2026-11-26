#include "native_session.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <algorithm>
#include <cmath>

using namespace godot;

namespace {
constexpr double cell_size = 16;
constexpr int cell_extent = 1 << 19;
struct Cell { int x, y, z; bool spatial_3d; };

bool cell_indices(const Vector3& location, bool spatial_3d, Cell& result)
{
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(double(location[i])) || location[i] < -cell_extent * cell_size ||
            location[i] >= cell_extent * cell_size) return false;
    }
    result = {int(std::floor(location.x / cell_size)), int(std::floor(location.y / cell_size)),
              int(std::floor(location.z / cell_size)), spatial_3d};
    return true;
}

bool cell_indices(const Variant& position, Cell& result)
{
    if (position.get_type() == Variant::VECTOR3) return cell_indices(Vector3(position), true, result);
    if (position.get_type() == Variant::VECTOR2) {
        const Vector2 xz = position;
        return cell_indices(Vector3(xz.x, 0, xz.y), true, result);
    }
    return false;
}

bool cell_indices(Node* target, Cell& result,
                  const StringName& position_property = {})
{
    if (!target) return false;
    if (!position_property.is_empty()) return cell_indices(target->get(position_property), result);
    if (auto* three = Object::cast_to<Node3D>(target)) {
        const auto point = three->is_inside_tree() ? three->get_global_position() : three->get_position();
        return cell_indices(point, true, result);
    }
    if (auto* two = Object::cast_to<Node2D>(target)) {
        const auto point = two->is_inside_tree() ? two->get_global_position() : two->get_position();
        return cell_indices(Vector3(point.x, point.y, 0), false, result);
    }
    return false;
}

uint64_t key(const Cell& cell)
{
    if (cell.x < -cell_extent || cell.x >= cell_extent || cell.y < -cell_extent || cell.y >= cell_extent ||
        cell.z < -cell_extent || cell.z >= cell_extent) return 0;
    return (uint64_t(cell.spatial_3d ? 2 : 1) << 62) | (uint64_t(cell.x + cell_extent) << 40) |
           (uint64_t(cell.y + cell_extent) << 20) | uint64_t(cell.z + cell_extent);
}
}

void NetcodeInterestArea::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("get_target_path"), &NetcodeInterestArea::get_target_path);
    ClassDB::bind_method(D_METHOD("set_target_path", "path"), &NetcodeInterestArea::set_target_path);
    ClassDB::bind_method(D_METHOD("get_radius"), &NetcodeInterestArea::get_radius);
    ClassDB::bind_method(D_METHOD("set_radius", "radius"), &NetcodeInterestArea::set_radius);
    ClassDB::bind_method(D_METHOD("get_send_divisor"), &NetcodeInterestArea::get_send_divisor);
    ClassDB::bind_method(D_METHOD("set_send_divisor", "divisor"), &NetcodeInterestArea::set_send_divisor);
    ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "target_path"), "set_target_path", "get_target_path");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "radius", PROPERTY_HINT_RANGE, "0,1,1"), "set_radius", "get_radius");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "send_divisor", PROPERTY_HINT_RANGE, "1,65535,1"),
                 "set_send_divisor", "get_send_divisor");
}

void NetcodeInterestArea::set_radius(int radius) { radius_ = std::clamp(radius, 0, 1); }
void NetcodeInterestArea::set_send_divisor(int divisor) { send_divisor_ = std::clamp(divisor, 1, int(UINT16_MAX)); }

uint64_t NetcodeInterestArea::cell(Node* target, const StringName& position_property)
{
    Cell result{};
    return cell_indices(target, result, position_property) ? key(result) : 0;
}

uint64_t NetcodeInterestArea::cell(const Variant& position)
{
    Cell result{};
    return cell_indices(position, result) ? key(result) : 0;
}

void NetcodeInterestArea::_notification(int what)
{
    if (what == NOTIFICATION_ENTER_TREE) {
        set_physics_process_internal(!Engine::get_singleton()->is_editor_hint());
    } else if (what == NOTIFICATION_EXIT_TREE || what == NOTIFICATION_INTERNAL_PHYSICS_PROCESS) {
        auto* session = NetcodeSession::instance();
        if (!session || !session->has_open_session() || session->is_host()) return;
        NetcodeSession::AreaCells keys{};
        std::size_t count = 0;
        Cell center{};
        if (what == NOTIFICATION_INTERNAL_PHYSICS_PROCESS && cell_indices(get_node_or_null(target_path_), center)) {
            const int height = center.spatial_3d ? radius_ : 0;
            for (int x = -radius_; x <= radius_; ++x)
                for (int y = -radius_; y <= radius_; ++y)
                    for (int z = -height; z <= height; ++z) {
                        const auto encoded = key({center.x + x, center.y + y, center.z + z, center.spatial_3d});
                        if (encoded) keys[count++] = encoded;
                    }
        }
        session->area_interest(ObjectID(get_instance_id()), keys, send_divisor_);
    }
}

void NetcodeServerReplicator::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("get_neutral_input"), &NetcodeServerReplicator::get_neutral_input);
    ClassDB::bind_method(D_METHOD("set_neutral_input", "payload"), &NetcodeServerReplicator::set_neutral_input);
    ClassDB::bind_method(D_METHOD("get_input_model_version"), &NetcodeServerReplicator::get_input_model_version);
    ClassDB::bind_method(D_METHOD("set_input_model_version", "version"), &NetcodeServerReplicator::set_input_model_version);
    ClassDB::bind_method(D_METHOD("get_input_authority"), &NetcodeServerReplicator::get_input_authority);
    ClassDB::bind_method(D_METHOD("set_input_authority", "player"), &NetcodeServerReplicator::set_input_authority);
    ClassDB::bind_method(D_METHOD("has_input_authority"), &NetcodeServerReplicator::has_input_authority);
    ClassDB::bind_method(D_METHOD("queue_input", "delta_or_payload", "payload"), &NetcodeServerReplicator::queue_input, DEFVAL(Variant()));
    ClassDB::bind_method(D_METHOD("get_input_queue_count"), &NetcodeServerReplicator::get_input_queue_count);
    ClassDB::bind_method(D_METHOD("get_confirmed_input"), &NetcodeServerReplicator::get_confirmed_input);
    ClassDB::bind_method(D_METHOD("get_replayed_input_count"), &NetcodeServerReplicator::get_replayed_input_count);
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "neutral_input"), "set_neutral_input", "get_neutral_input");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "input_model_version", PROPERTY_HINT_RANGE, "1,65535,1"),
                 "set_input_model_version", "get_input_model_version");
    ADD_SIGNAL(MethodInfo("input_authority_changed", PropertyInfo(Variant::INT, "player")));
    ADD_SIGNAL(MethodInfo("on_process_input", PropertyInfo(Variant::INT, "tick"), PropertyInfo(Variant::FLOAT, "delta"),
        PropertyInfo(Variant::PACKED_BYTE_ARRAY, "payload"), PropertyInfo(Variant::BOOL, "is_new")));
}

int64_t NetcodeServerReplicator::get_input_authority() const
{
    return game_.is_valid() ? game_->input_player(object_id_) : staged_input_player_;
}

bool NetcodeServerReplicator::set_input_authority(int64_t player)
{
    if (game_.is_valid()) return game_->bind_prediction(object_id_, player);
    const auto* session = NetcodeSession::instance();
    if (!provisional_ || !session || !session->is_host() || player <= 0 || player > UINT32_MAX) return false;
    staged_input_player_ = player;
    return true;
}

bool NetcodeServerReplicator::has_input_authority() const
{
    auto* session = NetcodeSession::instance();
    const auto player = get_input_authority();
    return session && player && player == session->get_local_player_id();
}

bool NetcodeServerReplicator::queue_input(const Variant& delta_or_payload, const Variant& payload)
{
    if (game_.is_null()) return false;
    if (delta_or_payload.get_type() == Variant::PACKED_BYTE_ARRAY && payload.get_type() == Variant::NIL)
        return game_->predict_native(object_id_, game_->simulation_delta(), delta_or_payload);
    if ((delta_or_payload.get_type() != Variant::FLOAT && delta_or_payload.get_type() != Variant::INT) ||
        payload.get_type() != Variant::PACKED_BYTE_ARRAY) return false;
    return game_->predict_native(object_id_, delta_or_payload, payload);
}

int64_t NetcodeServerReplicator::get_input_queue_count() const
{
    return game_.is_valid() ? game_->input_queue(object_id_) : 0;
}

int64_t NetcodeServerReplicator::get_confirmed_input() const
{
    return game_.is_valid() ? game_->input_confirmed(object_id_) : 0;
}

int64_t NetcodeServerReplicator::get_replayed_input_count() const
{
    return game_.is_valid() ? game_->input_replayed(object_id_) : 0;
}

Array NetcodeServerReplicator::simulate(const Array& state, const PackedByteArray& payload, double delta, int64_t tick,
                                       netcode::SimulationStep stage)
{
    const ObjectID id(get_instance_id());
    const auto game = game_;
    const auto object = object_id_;
    const auto bound = [&] {
        return ObjectDB::get_instance(id) && game_.is_valid() && game_ == game &&
               object_id_ == object && !is_queued_for_deletion();
    };
    if (!apply_adapter_model(state)) return {};
    if (!bound()) return {};
    emit_signal("on_process_input", tick, delta, payload, stage != netcode::SimulationStep::Replay);
    return bound() ? sample_adapter() : Array();
}

bool NetcodeServerReplicator::apply_model_frame()
{
    auto player = get_input_authority();
    if (player != reported_input_player_) {
        reported_input_player_ = player;
        const ObjectID self(get_instance_id());
        const auto game = game_;
        const auto object = object_id_;
        emit_signal("input_authority_changed", player);
        if (!ObjectDB::get_instance(self) || game_ != game || object_id_ != object || is_queued_for_deletion()) return true;
        player = get_input_authority();
    }
    const auto* session = NetcodeSession::instance();
    if (!player || !session || (!session->is_host() && player != session->get_local_player_id())) return false;
    auto fields = game_->predicted_fields(object_id_);
    if (fields.is_empty()) fields = game_->read_fields(object_id_, false);
    apply_adapter_model(fields);
    return true;
}
