#include "native_session.hpp"

#include <godot_cpp/classes/multiplayer_api.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cmath>
#include <utility>

using namespace godot;

namespace {
constexpr int max_arguments = 32;
constexpr uint64_t rpc_timeout = 12000;
constexpr uint64_t baseline_timeout = 3000;
constexpr size_t max_deferred_rpcs = 512;

uint64_t now_ms() { return Time::get_singleton()->get_ticks_msec(); }

Dictionary script_rpc_config(Object* node, const StringName& method)
{
    for (Ref<Script> script = node->get_script(); script.is_valid(); script = script->get_base_script()) {
        const Variant raw = script->get_rpc_config();
        if (raw.get_type() != Variant::DICTIONARY) continue;
        const Dictionary config = raw;
        if (config.has(method)) return config[method];
    }
    return {};
}

bool authority_only(const Dictionary& config)
{
    return int(config.get("rpc_mode", MultiplayerAPI::RPC_MODE_ANY_PEER)) == MultiplayerAPI::RPC_MODE_AUTHORITY;
}

bool is_annotated(const Dictionary& config)
{
    return int(config.get("rpc_mode", MultiplayerAPI::RPC_MODE_DISABLED)) != MultiplayerAPI::RPC_MODE_DISABLED;
}

bool has_script_method(Node* node, const StringName& method)
{
    for (Ref<Script> script = node->get_script(); script.is_valid(); script = script->get_base_script()) {
        const auto methods = script->get_script_method_list();
        for (int64_t i = 0; i < methods.size(); ++i) {
            const Dictionary info = methods[i];
            if (StringName(info.get("name", StringName())) == method) return true;
        }
    }
    return false;
}

String error_text(netcode::NativeRpc::Status status)
{
    switch (status) {
    case netcode::NativeRpc::Status::Delivered: return {};
    case netcode::NativeRpc::Status::Unavailable: return "RPC target is unavailable";
    case netcode::NativeRpc::Status::Rejected: return "RPC target rejected delivery";
    case netcode::NativeRpc::Status::Expired: return "RPC delivery timed out";
    }
    return "RPC did not complete";
}

Ref<NetcodeRpcResult> rejected_rpc(const String& reason)
{
    Ref<NetcodeRpcResult> result;
    result.instantiate();
    result->complete(reason);
    return result;
}

void notify_rpc_failure(const Callable& callback, const String& reason)
{
    Variant callable(callback), argument(reason), result;
    const Variant* args[]{&argument};
    GDExtensionCallError error{};
    callable.callp("call", args, 1, result, error);
    if (error.error == GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS) callback.call();
}
}

void NetcodeRpcInfo::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("get_sender"), &NetcodeRpcInfo::get_sender);
    ClassDB::bind_method(D_METHOD("get_target"), &NetcodeRpcInfo::get_target);
    ClassDB::bind_method(D_METHOD("get_target_node"), &NetcodeRpcInfo::get_target_node);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "sender"), "", "get_sender");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "target"), "", "get_target");
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "target_node", PROPERTY_HINT_NODE_TYPE, "Node"), "", "get_target_node");
}

Node* NetcodeRpcInfo::get_target_node() const
{
    return Object::cast_to<Node>(ObjectDB::get_instance(target_node_));
}

void NetcodeRpcResult::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("is_finished"), &NetcodeRpcResult::is_finished);
    ClassDB::bind_method(D_METHOD("get_error"), &NetcodeRpcResult::get_error);
    ClassDB::bind_method(D_METHOD("on_fail", "callback", "ttl"), &NetcodeRpcResult::on_fail, DEFVAL(0.0));
}

Ref<NetcodeRpcResult> NetcodeRpcResult::on_fail(const Callable& callback, double ttl)
{
    Ref<NetcodeRpcResult> self(this);
    if (!callback.is_valid()) return self;
    if (finished_) {
        if (!error_.is_empty()) notify_rpc_failure(callback, error_);
    } else {
        failure_ = callback;
        deadline_ = std::isfinite(ttl) && ttl > 0 ? now_ms() + uint64_t(std::min(ttl, 3600.0) * 1000) : 0;
    }
    return self;
}

void NetcodeRpcResult::complete(const String& error)
{
    if (finished_) return;
    finished_ = true;
    error_ = error;
    auto callback = std::exchange(failure_, Callable());
    if (!error_.is_empty() && callback.is_valid() && (!deadline_ || now_ms() <= deadline_))
        notify_rpc_failure(callback, error_);
}

Variant NetcodeReplicator::rpc_to(const Variant** args, GDExtensionInt count, GDExtensionCallError& error)
{
    error.error = GDEXTENSION_CALL_OK;
    auto* session = NetcodeSession::instance();
    if (!session || !object_id_ || count < 2 || count > max_arguments + 2 ||
        args[0]->get_type() != Variant::INT || args[1]->get_type() != Variant::CALLABLE) {
        return rejected_rpc("RPC requires a bound object, a target, and a callable");
    }
    return session->dispatch_rpc(int64_t(*args[0]), *args[1], args + 2, count - 2, this);
}

Variant NetcodeSession::rpc(const Variant** args, GDExtensionInt count, GDExtensionCallError& error)
{
    error.error = GDEXTENSION_CALL_OK;
    if (count < 1 || count > max_arguments + 1 || args[0]->get_type() != Variant::CALLABLE)
        return rejected_rpc("RPC requires a callable");
    return dispatch_rpc(TARGET_ALL, *args[0], args + 1, count - 1);
}

Variant NetcodeSession::rpc_to(const Variant** args, GDExtensionInt count, GDExtensionCallError& error)
{
    error.error = GDEXTENSION_CALL_OK;
    if (count < 2 || count > max_arguments + 2 || args[0]->get_type() != Variant::INT ||
        args[1]->get_type() != Variant::CALLABLE) return rejected_rpc("RPC requires a target and a callable");
    return dispatch_rpc(int64_t(*args[0]), *args[1], args + 2, count - 2);
}

Variant NetcodeSession::rpc_to_player(const Variant** args, GDExtensionInt count, GDExtensionCallError& error)
{
    error.error = GDEXTENSION_CALL_OK;
    if (count < 2 || args[0]->get_type() != Variant::INT || int64_t(*args[0]) <= 0)
        return rejected_rpc("RPC requires a positive player ID");
    return rpc_to(args, count, error);
}

void NetcodeSession::register_broadcast_receiver(Node* node)
{
    if (node && node->is_inside_tree()) broadcast_receivers_.insert(ObjectID(node->get_instance_id()));
}

void NetcodeSession::unregister_broadcast_receiver(Node* node)
{
    if (node) broadcast_receivers_.erase(ObjectID(node->get_instance_id()));
}

NetcodeReplicator* NetcodeSession::find_rpc_replica(const Object* node, bool allow_sync) const
{
    const ObjectID id(node->get_instance_id());
    for (const auto& [object, instance] : instances_) {
        if ((id == instance.actor || (allow_sync && id == instance.sync)) && instance.bound_to(game_, object))
            return Object::cast_to<NetcodeReplicator>(ObjectDB::get_instance(instance.sync));
    }
    return nullptr;
}

Ref<NetcodeRpcResult> NetcodeSession::dispatch_rpc(int64_t target, const Callable& method, const Variant** args,
                                                   GDExtensionInt count, NetcodeReplicator* replica)
{
    if (target == TARGET_PLUGIN) return rejected_rpc("TARGET_PLUGIN is unsupported");
    auto* node = Object::cast_to<Node>(method.get_object());
    if (!joined_ || !node || !method.is_valid() || !node->is_inside_tree() || node->is_queued_for_deletion())
        return rejected_rpc("RPC requires a live node in a room");
    if (!replica) replica = find_rpc_replica(node, true);
    const bool sync = node == replica;
    const auto name = method.get_method();
    if (name == StringName("free")) return rejected_rpc("free is reserved by the engine");
    const auto config = script_rpc_config(node, name);
    const bool annotated = is_annotated(config);
    const auto object = replica ? replica->object_id_ : 0;
    if (replica) {
        if (replica->game_ != game_ || (!sync && node != replica->replication_root()) ||
            (!annotated && (!replica->rpc_methods_.has(String(name)) || !has_script_method(node, name))))
            return rejected_rpc("Object RPC method must be annotated or declared by the target script and listed in rpc_methods");
    } else if (!annotated || !broadcast_receivers_.count(ObjectID(node->get_instance_id())) || target == TARGET_OWNER) {
        return rejected_rpc("Broadcast RPC requires a registered node, an annotated method, and a room target");
    }
    if (authority_only(config) && !(object ? game_->native_authority(object) : is_master_client()))
        return rejected_rpc("RPC method requires state authority");
    const auto unbound = method.get_unbound_arguments_count();
    if (unbound > count) return rejected_rpc("RPC callable unbinds more arguments than were supplied");
    Array values;
    for (int i = 0; i < count - unbound; ++i) values.push_back(*args[i]);
    values.append_array(method.get_bound_arguments());
    if (values.size() > max_arguments) return rejected_rpc("Too many RPC arguments");
    Dictionary references;
    for (int i = 0; i < values.size(); ++i) {
        const Variant value = values[i];
        if (value.get_type() != Variant::OBJECT) continue;
        Object* arg = value;
        const auto* bound = arg ? find_rpc_replica(arg, false) : nullptr;
        if (arg && !bound) return rejected_rpc("RPC node arguments require a bound network root");
        if (bound) references[i] = game_->field_reference(bound->object_id_);
        values[i] = Variant();
    }
    if (!NetcodeGame::is_portable_value(values)) return rejected_rpc("Unsupported RPC arguments");
    return send_object_rpc(target, Array::make(sync, String(name), values, references), replica,
                           annotated, bool(config.get("call_local", false)));
}

bool NetcodeSession::deliver_rpc(const netcode::NativeRpc::Delivery& delivery, NetcodeReplicator* replica)
{
    PackedByteArray data;
    data.resize(delivery.data.size());
    std::copy(delivery.data.begin(), delivery.data.end(), data.ptrw());
    const Variant decoded = UtilityFunctions::bytes_to_var(data);
    if (decoded.get_type() != Variant::ARRAY) return false;
    const Array invocation = decoded;
    if (invocation.size() != 4 || invocation[0].get_type() != Variant::BOOL ||
        invocation[1].get_type() != Variant::STRING || invocation[2].get_type() != Variant::ARRAY ||
        invocation[3].get_type() != Variant::DICTIONARY) return false;
    const String name = invocation[1];
    const Array args = invocation[2];
    const Dictionary references = invocation[3];
    if (args.size() > max_arguments || references.size() > args.size() || !NetcodeGame::is_portable_value(args)) return false;
    const auto indices = references.keys();
    for (int i = 0; i < indices.size(); ++i) {
        const Variant key = indices[i];
        if (key.get_type() != Variant::INT) return false;
        const auto index = int64_t(key);
        const auto handle = references[key];
        if (index < 0 || index >= args.size() || args[index].get_type() != Variant::NIL ||
            handle.get_type() != Variant::INT) return false;
    }
    const auto arguments = [&] {
        Array values = replica ? args : args.duplicate(true);
        for (int i = 0; i < indices.size(); ++i) {
            const Variant key = indices[i];
            const auto object = game_->resolve_field(references[key]);
            const auto instance = instances_.find(object);
            if (instance != instances_.end() && instance->second.bound_to(game_, object))
                values[int64_t(key)] = ObjectDB::get_instance(instance->second.actor);
        }
        return values;
    };
    if (replica) {
        auto* node = bool(invocation[0]) ? replica : replica->replication_root();
        return node && invoke_rpc(node, name, arguments(), delivery);
    }
    if (bool(invocation[0])) return false;
    const auto game = game_;
    const auto receivers = broadcast_receivers_;
    bool delivered = false;
    for (auto id : receivers) {
        auto* node = Object::cast_to<Node>(ObjectDB::get_instance(id));
        if (!node) broadcast_receivers_.erase(id);
        else if (broadcast_receivers_.count(id) && node->is_inside_tree() && !node->is_queued_for_deletion() &&
                 node->has_method(name))
            delivered = invoke_rpc(node, name, arguments(), delivery) || delivered;
        if (game_ != game) break;
    }
    return delivered;
}

bool NetcodeSession::invoke_rpc(Node* destination, const StringName& name, const Array& args,
                                const netcode::NativeRpc::Delivery& delivery)
{
    if (name == StringName("free") || !destination->has_method(name)) return false;
    const auto config = script_rpc_config(destination, name);
    if (!is_annotated(config)) {
        const auto found = instances_.find(delivery.object);
        if (found == instances_.end()) return false;
        const auto* replica = Object::cast_to<NetcodeReplicator>(ObjectDB::get_instance(found->second.sync));
        if (!replica || !replica->rpc_methods_.has(String(name)) || !has_script_method(destination, name)) return false;
    }
    if (authority_only(config) && delivery.sender != delivery.writer) return false;
    if (delivery.sender == player_ && !bool(config.get("call_local", true))) return true;
    Variant target(destination), result;
    const Variant* pointers[max_arguments];
    Variant arguments[max_arguments];
    for (int64_t i = 0; i < args.size(); ++i) {
        arguments[i] = args[i];
        pointers[i] = &arguments[i];
    }
    GDExtensionCallError error{};
    const auto game = game_;
    Ref<NetcodeRpcInfo> info;
    info.instantiate();
    info->sender_ = delivery.sender;
    info->target_ = delivery.target;
    if (delivery.object) info->target_node_ = ObjectID(destination->get_instance_id());
    auto previous = std::exchange(rpc_info_, info);
    target.callp(name, pointers, int(args.size()), result, error);
    if (game_ == game) rpc_info_ = std::move(previous);
    return error.error == GDEXTENSION_CALL_OK;
}

Ref<NetcodeRpcResult> NetcodeSession::send_object_rpc(int64_t target, const Array& invocation,
                                                      NetcodeReplicator* replica, bool annotated, bool call_local)
{
    Ref<NetcodeRpcResult> result;
    result.instantiate();
    if (!joined_ || pending_rpcs_.size() >= 512 || rpc_token_ == UINT32_MAX ||
        target < INT32_MIN || target > INT32_MAX) {
        result->complete("RPC session is unavailable");
        return result;
    }
    const auto game = game_;
    const auto object = replica ? replica->object_id_ : 0;
    const auto state = annotated && object ? game->routing_info(object, false) : Dictionary();
    const int64_t writer = object ? int64_t(state.get("writer", 0)) : room_->get_master_client_id();
    const bool local_target = target == TARGET_ALL || target == player_ ||
        (target == TARGET_MASTER && is_master_client()) ||
        (target == TARGET_OWNER && is_master_client() && writer == player_ && replica && replica->is_write_ready());
    const bool immediate = annotated && call_local && local_target;
    const bool sender_handled = annotated && (!call_local || immediate);
    const auto data = UtilityFunctions::var_to_bytes(invocation);
    const auto token = ++rpc_token_;
    if (game->send_native_rpc(object, int32_t(target), token, data, sender_handled)) {
        pending_rpcs_.emplace(token, PendingRpc{result, now_ms() + rpc_timeout});
        if (immediate) {
            const netcode::NativeRpc::Delivery local{0, netcode::ObjectId(object), netcode::PlayerId(player_),
                netcode::PlayerId(writer), int32_t(target), 0, netcode::Bytes(data.ptr(), data.ptr() + data.size())};
            const bool success = deliver_rpc(local, replica);
            if (!success && game_ == game) {
                if (auto pending = pending_rpcs_.find(token); pending != pending_rpcs_.end()) pending->second.local_failed = true;
            }
        }
    } else {
        result->complete("RPC target or payload is unsupported");
    }
    return result;
}

void NetcodeSession::update_rpc()
{
    const auto game = game_;
    const auto now = now_ms();
    auto batch = std::exchange(deferred_rpcs_, {});
    for (auto& delivery : game->native_rpc_deliveries()) batch.push_back({std::move(delivery), now + baseline_timeout});
    for (auto& item : batch) {
        const auto& delivery = item.delivery;
        const bool owner = delivery.target == TARGET_OWNER;
        const auto state = owner ? game->routing_info(delivery.object, false) : Dictionary();
        const auto epoch = int64_t(state.get("epoch", 0));
        const auto writer = int64_t(state.get("writer", 0));
        auto instance = instances_.find(delivery.object);
        auto* sync = instance == instances_.end() || !instance->second.bound_to(game, delivery.object) ? nullptr :
            Object::cast_to<NetcodeReplicator>(ObjectDB::get_instance(instance->second.sync));
        const bool ready = owner && sync && sync->is_write_ready();
        if (owner && now < item.deadline && deferred_rpcs_.size() < max_deferred_rpcs &&
            (epoch < delivery.epoch || (epoch == delivery.epoch && (state.is_empty() || (writer == player_ && !ready))))) {
            deferred_rpcs_.push_back(std::move(item));
            continue;
        }
        const bool mounted = !delivery.object || sync;
        const bool success = mounted && (!owner || (ready && epoch == delivery.epoch && now < item.deadline)) &&
                             deliver_rpc(delivery, sync);
        if (game_ != game) return;
        game->finish_native_rpc(delivery.receipt, success);
    }
    for (const auto& outcome : game->native_rpc_outcomes()) {
        auto pending = pending_rpcs_.extract(outcome.token);
        if (!pending.empty()) {
            const auto error = error_text(outcome.status);
            pending.mapped().result->complete(error.is_empty() && pending.mapped().local_failed
                ? String("RPC sender rejected its local delivery") : error);
        }
        if (game_ != game) return;
    }
    std::vector<uint32_t> expired;
    for (const auto& [token, pending] : pending_rpcs_) if (now >= pending.deadline) expired.push_back(token);
    for (auto token : expired) {
        auto pending = pending_rpcs_.extract(token);
        if (!pending.empty()) pending.mapped().result->complete("RPC delivery timed out");
        if (game_ != game) return;
    }
}

void NetcodeSession::close_rpc()
{
    rpc_token_ = 0;
    rpc_info_.unref();
    deferred_rpcs_.clear();
    for (const auto& [_, pending] : std::exchange(pending_rpcs_, {})) pending.result->complete("Room closed");
}
