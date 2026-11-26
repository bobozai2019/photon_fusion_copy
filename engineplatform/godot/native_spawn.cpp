#include "native_session.hpp"

#include <godot_cpp/classes/time.hpp>
#include <algorithm>
#include <utility>

using namespace godot;

namespace {

constexpr uint64_t spawn_timeout = 12000;
constexpr size_t max_staged_spawns = 64;

template<class T>
T* lookup_node(ObjectID id)
{
    return Object::cast_to<T>(ObjectDB::get_instance(id));
}

NetcodeReplicator* find_sync(Node* branch, Node* root)
{
    auto* sync = Object::cast_to<NetcodeReplicator>(branch);
    if (sync && sync->replication_root() == root) return sync;
    for (int i = 0; i < branch->get_child_count(); ++i) {
        if (auto* child = find_sync(branch->get_child(i), root)) return child;
    }
    return nullptr;
}

netcode::ClaimMode core_mode(int declared)
{
    switch (declared) {
    case NetcodeSharedReplicator::PLAYER_ATTACHED: return netcode::ClaimMode::PlayerAttached;
    case NetcodeSharedReplicator::DYNAMIC: return netcode::ClaimMode::Dynamic;
    case NetcodeSharedReplicator::MASTER_CLIENT: return netcode::ClaimMode::MasterClient;
    default: return netcode::ClaimMode::Transaction;
    }
}

} // namespace

bool NetcodeSession::Instance::mounted() const
{
    return mounted(lookup_node<NetcodeReplicator>(sync));
}

bool NetcodeSession::Instance::mounted(const NetcodeReplicator* replica) const
{
    if (!replica || !replica->is_inside_tree() || replica->is_queued_for_deletion()) return false;
    const auto* root = replica->replication_root();
    return root && ObjectID(root->get_instance_id()) == actor &&
           root->is_inside_tree() && !root->is_queued_for_deletion();
}

bool NetcodeSession::Instance::bound_to(const Ref<NetcodeGame>& game, int64_t object) const
{
    const auto* replica = lookup_node<NetcodeReplicator>(sync);
    return replica && replica->game_ == game && replica->object_id_ == object && mounted(replica);
}

void NetcodeSession::register_spawner(NetcodeSpawner* spawner)
{
    ObjectID id(spawner->get_instance_id());
    if (std::find(spawners_.begin(), spawners_.end(), id) == spawners_.end()) spawners_.push_back(id);
}

void NetcodeSession::unregister_spawner(NetcodeSpawner* spawner)
{
    const ObjectID id(spawner->get_instance_id());
    spawners_.erase(std::remove(spawners_.begin(), spawners_.end(), id), spawners_.end());
    std::vector<Instance> retired;
    for (auto it = staged_spawns_.begin(); it != staged_spawns_.end();) {
        if (catalog_[it->instance.template_id - 1].spawner == id) {
            retired.push_back(it->instance);
            it = staged_spawns_.erase(it);
        } else {
            ++it;
        }
    }
    for (size_t i = 0; i < catalog_.size(); ++i) {
        auto& entry = catalog_[i];
        if (entry.spawner != id) continue;
        entry.spawner = ObjectID();
        const auto template_id = i + 1;
        for (auto it = instances_.begin(); it != instances_.end();) {
            if (it->second.template_id != template_id) {
                ++it;
                continue;
            }
            if (host_ && game_.is_valid()) {
                if (game_->is_stepping()) deferred_host_despawns_.insert(it->first);
                else game_->destroy_object(it->first);
            }
            retired.push_back(it->second);
            it = instances_.erase(it);
        }
        for (auto it = retained_.begin(); it != retained_.end();) {
            if (it->second.template_id != template_id) {
                ++it;
                continue;
            }
            retired.push_back(it->second);
            it = retained_.erase(it);
        }
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.template_id != template_id) {
                ++it;
                continue;
            }
            const auto request = it->first;
            if (game_.is_valid()) game_->request_despawn(0, (uint64_t(creation_namespace_) << 32) | request);
            expired_.insert(request);
            retired.push_back(it->second);
            it = pending_.erase(it);
        }
    }
    for (const auto& record : retired) remove_instance(record);
}

bool NetcodeSession::build_catalog(String& layout, uint64_t& fingerprint)
{
    catalog_.clear();
    writable_.clear();
    prediction_.clear();
    std::map<int, String> schemas;
    String manifest;
    for (auto id : spawners_) {
        auto* spawner = lookup_node<NetcodeSpawner>(id);
        if (!spawner || spawner->is_queued_for_deletion()) continue;
        const auto scenes = spawner->get_spawnable_scenes();
        if (!scenes.is_empty() && !spawner->get_node_or_null(spawner->get_spawn_path())) return false;
        for (int64_t i = 0; i < scenes.size(); ++i) {
            Ref<PackedScene> scene = scenes[i];
            auto* actor = scene.is_valid() ? scene->instantiate() : nullptr;
            if (!actor) return false;
            auto* sync = find_sync(actor, actor);
            const auto description = sync ? sync->prepare_adapter() : Dictionary();
            auto* shared = Object::cast_to<NetcodeSharedReplicator>(sync);
            auto* server = Object::cast_to<NetcodeServerReplicator>(sync);
            const auto mode = shared ? core_mode(shared->get_owner_mode()) : netcode::ClaimMode::MasterClient;
            const auto keys = sync ? sync->get_user_keys() : PackedInt64Array();
            const auto methods = sync ? sync->get_rpc_methods() : PackedStringArray();
            const bool destroy_on_exit = sync && sync->get_interest_destroy_on_exit();
            const bool spatial = sync && sync->get_spatial_interest();
            const int input_version = server ? server->get_input_model_version() : 0;
            const auto neutral = server ? server->get_neutral_input() : PackedByteArray();
            const auto spatial_position = sync ? sync->get_spatial_position_property() : StringName();
            const bool prediction_supported = !server || (!server->adapter_uses_body() &&
                input_version >= 1 && input_version <= UINT16_MAX && !neutral.is_empty() && neutral.size() <= 96);
            const ObjectID actor_id(actor->get_instance_id());
            const bool spatial_supported = !spatial || (keys.is_empty() && sync->spatial_cell());
            actor = lookup_node<Node>(actor_id);
            if (!actor) return false;
            memdelete(actor);
            if (description.is_empty() || keys.size() > 64 || !spatial_supported || !prediction_supported) return false;
            const int schema = description["id"];
            const String text = description["text"];
            PackedInt32Array fields = description["writable"];
            if (fields.is_empty()) fields.push_back(1);
            auto [schema_entry, inserted] = schemas.emplace(schema, text);
            if (inserted) writable_.emplace(schema, fields);
            else if (schema_entry->second != text || writable_.at(schema) != fields) return false;
            if (input_version) {
                auto [model, added] = prediction_.emplace(schema, std::make_pair(input_version, neutral));
                if (!added && model->second != std::make_pair(input_version, neutral)) return false;
            }
            catalog_.push_back({ id, scene, schema, mode, keys, spatial });
            manifest += vformat("%d|%s|%d|%d|%s|%s|%s|%s|%d|%s\n", i, scene->get_path(), schema, int(mode),
                                keys, destroy_on_exit, spatial, methods, input_version, neutral);
            if (spatial && !spatial_position.is_empty())
                manifest += vformat("spatial_position=%s\n", spatial_position);
        }
    }
    for (const auto& [_, text] : schemas) layout += text;
    const auto hash = (layout + manifest).sha256_buffer();
    fingerprint = 0;
    for (int i = 0; i < 8; ++i) fingerprint = (fingerprint << 8) | hash[i];
    return true;
}

bool NetcodeSession::register_catalog(const Ref<NetcodeGame>& game)
{
    for (const auto& [schema, mask] : writable_) {
        // This registers the disposal callback used by PlayerAttached instances.
        // Each spawn supplies the explicit policy from its own template.
        if (!game->model(schema, mask, int(netcode::ClaimMode::PlayerAttached), 0)) return false;
    }
    for (const auto& [schema, model] : prediction_) {
        if (!game->register_native_prediction(schema, model.first, model.second,
                [this](int64_t object, const Array& fields, const PackedByteArray& payload, double delta, int64_t tick,
                       netcode::SimulationStep stage) -> Array {
                    auto found = instances_.find(object);
                    if (found != instances_.end()) {
                        if (auto* sync = lookup_node<NetcodeServerReplicator>(found->second.sync))
                            return sync->simulate(fields, payload, delta, tick, stage);
                    }
                    return fields;
                })) return false;
    }
    for (size_t i = 0; i < catalog_.size(); ++i) {
        const auto& entry = catalog_[i];
        if (!game->register_spawn_template(uint32_t(i + 1), entry.schema, entry.mode, entry.user_keys, entry.spatial)) return false;
    }
    return true;
}

std::deque<NetcodeSession::StagedSpawn>::iterator NetcodeSession::find_staged_spawn(ObjectID actor)
{
    return std::find_if(staged_spawns_.begin(), staged_spawns_.end(),
                        [actor](const StagedSpawn& spawn) { return spawn.instance.actor == actor; });
}

bool NetcodeSession::submit_guest_spawn(const Instance& record, const netcode::Values& fields)
{
    const auto identity = game_->spawn_template(record.template_id, fields, false);
    if (!identity) return false;
    creation_namespace_ = uint32_t(uint64_t(identity) >> 32);
    auto waiting = record;
    waiting.deadline = Time::get_singleton()->get_ticks_msec() + spawn_timeout;
    pending_.emplace(uint32_t(identity), waiting);
    return true;
}

void NetcodeSession::flush_staged_spawns()
{
    const auto game = game_;
    const auto now = Time::get_singleton()->get_ticks_msec();
    const auto count = staged_spawns_.size();
    for (size_t i = 0; i < count && game_ == game && !staged_spawns_.empty(); ++i) {
        const auto record = staged_spawns_.front().instance;
        if (!record.mounted() || now >= record.deadline) {
            staged_spawns_.pop_front();
            remove_instance(record, now >= record.deadline ? "Spawn readiness timed out" :
                                                             "The staged network instance exited the tree");
            continue;
        }
        if (!game->can_request_native_spawn()) return;

        // Keep the record searchable while calling gameplay getters.
        const auto schema = catalog_[record.template_id - 1].schema;
        auto* sync = lookup_node<NetcodeReplicator>(record.sync);
        const Array sample = sync->sample_adapter();
        if (game_ != game) return;
        const auto waiting = find_staged_spawn(record.actor);
        if (waiting == staged_spawns_.end()) continue;
        auto fields = std::move(waiting->fields);
        staged_spawns_.erase(waiting);
        if (!record.mounted()) {
            remove_instance(record, "The staged network instance exited the tree");
            continue;
        }
        netcode::Values latest;
        if (game->prepare_spawn(schema, sample, latest)) fields = std::move(latest);
        if (!submit_guest_spawn(record, fields)) remove_instance(record, "Unable to submit the staged spawn");
    }
}

void NetcodeSession::capture_host_spawn_baselines()
{
    const auto game = game_;
    while (!initial_host_spawns_.empty()) {
        const auto next = initial_host_spawns_.begin();
        const auto id = *next;
        initial_host_spawns_.erase(next);
        const auto found = instances_.find(id);
        if (found == instances_.end()) continue;
        const auto record = found->second;
        if (record.bound_to(game, id) && game->native_authority(id)) {
            auto* sync = lookup_node<NetcodeReplicator>(record.sync);
            const Array sample = sync->sample_adapter();
            if (game_ != game) return;
            if (record.bound_to(game, id)) {
                game->submit_fields(id, sample);
                lookup_node<NetcodeReplicator>(record.sync)->update_spatial_distribution();
                if (game_ != game) return;
            }
        }
        if (!record.bound_to(game, id) && instances_.erase(id)) {
            game->destroy_object(id);
            remove_instance(record, "The network instance exited before publishing");
            if (game_ != game) return;
        }
    }
}

Node* NetcodeSession::spawn(NetcodeSpawner* spawner, const Ref<PackedScene>& scene, const Callable& pre_spawn)
{
    if (!joined_ || game_->is_stepping() || !spawner->is_inside_tree()) return nullptr;
    if (!host_ && staged_spawns_.size() + pending_.size() >= max_staged_spawns) return nullptr;
    if (!pre_spawn.is_null() && !pre_spawn.is_valid()) return nullptr;
    const ObjectID spawner_id(spawner->get_instance_id());
    const auto found = std::find_if(catalog_.begin(), catalog_.end(), [&](const Template& entry) {
        return entry.spawner == spawner_id && (scene.is_null() || entry.scene == scene);
    });
    if (found == catalog_.end() || (!host_ && netcode::master_only(found->mode))) return nullptr;
    const auto entry = *found;
    if (!host_ && !entry.user_keys.is_empty() &&
        std::none_of(entry.user_keys.begin(), entry.user_keys.end(), [&](int64_t key) { return user_keys_.count(key); }))
        return nullptr;
    const auto template_id = uint32_t(found - catalog_.begin() + 1);
    auto* parent = spawner->get_node_or_null(spawner->get_spawn_path());
    if (!parent || parent->is_queued_for_deletion()) return nullptr;
    const ObjectID parent_id(parent->get_instance_id());
    const auto game = game_;
    auto* actor = entry.scene->instantiate();
    if (!actor) return nullptr;
    auto* sync = find_sync(actor, actor);
    if (!sync) {
        memdelete(actor);
        return nullptr;
    }
    const Instance record{template_id, ObjectID(actor->get_instance_id()), ObjectID(sync->get_instance_id())};
    sync->provisional_ = true;
    GDExtensionCallError callback_error{};
    if (pre_spawn.is_valid()) {
        Variant callback(pre_spawn), replicator(sync), root(actor), result;
        const Variant* args[]{&replicator, &root};
        callback.callp("call", args, 2, result, callback_error);
    }
    const auto staged = [&] {
        actor = lookup_node<Node>(record.actor);
        sync = lookup_node<NetcodeReplicator>(record.sync);
        parent = lookup_node<Node>(parent_id);
        const auto* current = lookup_node<NetcodeSpawner>(spawner_id);
        return game_ == game && current && current->is_inside_tree() && !current->is_queued_for_deletion() &&
               actor && !actor->get_parent() && !actor->is_queued_for_deletion() && sync &&
               !sync->is_queued_for_deletion() && sync->replication_root() == actor && actor->is_ancestor_of(sync) &&
               parent && parent->is_inside_tree() && !parent->is_queued_for_deletion();
    };
    const auto discard = [&] {
        actor = lookup_node<Node>(record.actor);
        if (actor && !actor->get_parent() && !actor->is_queued_for_deletion()) memdelete(actor);
    };
    if (callback_error.error != GDEXTENSION_CALL_OK || !staged()) {
        discard();
        return nullptr;
    }
    const auto definition = sync->prepare_adapter();
    if (!staged() || int(definition.get("id", 0)) != entry.schema) {
        discard();
        return nullptr;
    }
    const Array sample = sync->sample_adapter(game);
    netcode::Values fields;
    if (!staged() || !game->prepare_spawn(entry.schema, sample, fields) ||
        (!host_ && staged_spawns_.size() + pending_.size() >= max_staged_spawns)) {
        discard();
        return nullptr;
    }
    const auto identity = host_ ? game->spawn_template(template_id, fields, true) : 0;
    if (host_ && !identity) {
        memdelete(actor);
        return nullptr;
    }
    if (host_) {
        auto* server = Object::cast_to<NetcodeServerReplicator>(sync);
        const auto input_player = server ? std::exchange(server->staged_input_player_, 0) : 0;
        if (!sync->bind_adapter(game, identity) || !sync->prime_adapter() ||
            (input_player && !game->bind_prediction(identity, input_player))) {
            game->destroy_object(identity);
            sync->unbind();
            memdelete(actor);
            return nullptr;
        }
        instances_.emplace(identity, record);
    } else {
        auto waiting = record;
        waiting.deadline = Time::get_singleton()->get_ticks_msec() + spawn_timeout;
        staged_spawns_.push_back({waiting, std::move(fields)});
    }
    parent->add_child(actor);
    const auto rollback = [&] {
        if (game_ != game) return;
        if (host_) {
            game->destroy_object(identity);
            instances_.erase(identity);
        } else {
            const auto waiting = find_staged_spawn(record.actor);
            if (waiting != staged_spawns_.end()) staged_spawns_.erase(waiting);
        }
        remove_instance(record);
    };
    const auto still_spawned = [&] {
        if (game_ != game) return false;
        if (host_) return instances_.count(identity) && record.bound_to(game, identity);
        return record.mounted() && find_staged_spawn(record.actor) != staged_spawns_.end();
    };
    if (!still_spawned()) {
        rollback();
        return nullptr;
    }
    sync = lookup_node<NetcodeReplicator>(record.sync);
    if (host_) {
        sync->provisional_ = false;
        const bool spatial_property = sync->get_spatial_interest() && !sync->get_spatial_position_property().is_empty();
        sync->update_spatial_distribution();
        if (spatial_property && !still_spawned()) {
            rollback();
            return nullptr;
        }
        sync->set_physics_process_internal(true);
    }
    sync->emit_signal("spawned");
    if (!still_spawned()) {
        rollback();
        return nullptr;
    }
    actor = lookup_node<Node>(record.actor);
    if (auto* current = lookup_node<NetcodeSpawner>(spawner_id); actor && current && game_ == game) {
        current->emit_signal("spawned", actor);
    }
    if (!still_spawned()) {
        rollback();
        return nullptr;
    }
    if (host_ && lookup_node<NetcodeSharedReplicator>(record.sync))
        initial_host_spawns_.insert(identity);
    return lookup_node<Node>(record.actor);
}

bool NetcodeSession::despawn(NetcodeSpawner* spawner, Node* actor)
{
    if (!joined_ || !actor) return false;
    const ObjectID id(actor->get_instance_id());
    const ObjectID spawner_id(spawner->get_instance_id());
    if (const auto waiting = find_staged_spawn(id); waiting != staged_spawns_.end() &&
        catalog_[waiting->instance.template_id - 1].spawner == spawner_id) {
        const auto record = waiting->instance;
        staged_spawns_.erase(waiting);
        remove_instance(record);
        return true;
    }
    for (const auto& [object, mounted] : instances_) {
        if (mounted.actor == id && catalog_[mounted.template_id - 1].spawner == spawner_id) {
            if (!host_) return game_->native_authority(object) && game_->request_despawn(object);
            if (!game_->destroy_object(object)) return false;
            auto record = instances_.extract(object);
            remove_instance(record.mapped());
            return true;
        }
    }
    for (const auto& [request, waiting] : pending_) {
        if (waiting.actor != id || catalog_[waiting.template_id - 1].spawner != spawner_id) continue;
        if (!game_->request_despawn(0, (uint64_t(creation_namespace_) << 32) | request)) return false;
        expired_.insert(request);
        auto record = pending_.extract(request);
        remove_instance(record.mapped());
        return true;
    }
    return false;
}

void NetcodeSession::remove_instance(const Instance& record, const String& reason)
{
    if (auto* sync = lookup_node<NetcodeReplicator>(record.sync)) sync->unbind();
    auto* actor = lookup_node<Node>(record.actor);
    if (!actor || actor->is_queued_for_deletion()) return;
    actor->queue_free();
    if (!joined_ || record.template_id > catalog_.size()) return;
    if (auto* spawner = lookup_node<NetcodeSpawner>(catalog_[record.template_id - 1].spawner)) {
        if (reason.is_empty()) spawner->emit_signal("despawned", actor);
        else spawner->emit_signal("spawn_failed", actor, reason);
    }
}

void NetcodeSession::abandon_spawns()
{
    initial_host_spawns_.clear();
    deferred_host_despawns_.clear();
    for (const auto& [_, record] : std::exchange(instances_, {})) remove_instance(record);
    for (const auto& [_, record] : std::exchange(retained_, {})) remove_instance(record);
    for (const auto& [_, record] : std::exchange(pending_, {})) remove_instance(record);
    for (const auto& spawn : std::exchange(staged_spawns_, {})) remove_instance(spawn.instance);
    expired_.clear();
    failed_mounts_.clear();
    creation_namespace_ = 0;
    catalog_.clear();
    writable_.clear();
    prediction_.clear();
    area_interests_.clear();
    sent_area_interests_.clear();
    area_interest_dirty_ = area_interest_overflow_ = area_rates_supported_ = false;
}

void NetcodeSession::update_spawns(const Dictionary& frame)
{
    const auto game = game_;
    const auto remove_pending = [&](uint32_t request, const String& reason) {
        auto pending = pending_.extract(request);
        if (!pending.empty()) remove_instance(pending.mapped(), reason);
    };
    const Array events = frame.get("events", Array());
    std::set<int64_t> interest_hidden;
    for (int64_t i = 0; i < events.size(); ++i) {
        const Dictionary event = events[i];
        const auto kind = netcode::ScriptEventKind(int(event["kind"]));
        const int64_t id = event["object"];
        if (kind == netcode::ScriptEventKind::SpawnResult && !id) {
            remove_pending(uint32_t(int64_t(event["sequence"])), "The host rejected the spawn");
        } else if (kind == netcode::ScriptEventKind::Hidden) {
            interest_hidden.insert(id);
        } else if (kind == netcode::ScriptEventKind::Destroyed) {
            interest_hidden.erase(id);
            failed_mounts_.erase(id);
            auto live = instances_.extract(id);
            if (!live.empty()) remove_instance(live.mapped());
            if (game_ != game) return;
            auto gone = retained_.extract(id);
            if (!gone.empty()) remove_instance(gone.mapped());
        } else if (kind == netcode::ScriptEventKind::Request || kind == netcode::ScriptEventKind::Result) {
            if (kind == netcode::ScriptEventKind::Request) {
                const auto state = game->routing_info(id, false);
                if (!game->native_authority(id) || state.get("epoch", -1) != event["epoch"]) continue;
            }
            if (auto it = instances_.find(id); it != instances_.end()) {
                if (auto* sync = lookup_node<NetcodeSharedReplicator>(it->second.sync)) sync->on_authority_event(event);
            }
        }
        if (game_ != game) return;
    }
    const auto now = Time::get_singleton()->get_ticks_msec();
    std::vector<uint32_t> stale;
    for (const auto& [request, record] : pending_) {
        if (now >= record.deadline || !record.mounted()) stale.push_back(request);
    }
    for (auto request : stale) {
        game->request_despawn(0, (uint64_t(creation_namespace_) << 32) | request);
        expired_.insert(request);
        remove_pending(request, "Spawn confirmation timed out");
        if (game_ != game) return;
    }
    const PackedInt32Array objects = frame.get("objects", PackedInt32Array());
    const auto* ids = objects.ptr();
    const auto count = objects.size();
    int64_t i = 0;
    if (instances_.size() == static_cast<size_t>(count) && failed_mounts_.empty()) {
        // Stable frames can validate both ordered collections in one pass.
        // Resume at the first mismatch before invoking any lifecycle callbacks.
        for (auto it = instances_.cbegin(); i < count; ++i, ++it) {
            const int64_t id = uint32_t(ids[i]);
            if (it->first != id || !it->second.bound_to(game, id)) break;
        }
        if (i == count) return;
    }
    for (; i < count; ++i) {
        const int64_t id = uint32_t(ids[i]);
        if (auto it = instances_.find(id); it != instances_.end()) {
            if (it->second.bound_to(game, id)) continue;
            const auto retired = it->second;
            instances_.erase(it);
            if (host_) game->destroy_object(id);
            else if (game->native_authority(id)) game->request_despawn(id);
            failed_mounts_.insert(id);
            remove_instance(retired, "The network instance exited the tree");
            if (game_ != game) return;
        }
        if (failed_mounts_.count(id)) continue;
        if (auto retained = retained_.extract(id); !retained.empty()) {
            const auto record = retained.mapped();
            auto* sync = lookup_node<NetcodeReplicator>(record.sync);
            const bool mounted = retained.mapped().mounted() && sync->bind_adapter(game, id) && sync->apply_baseline();
            if (game_ != game) {
                remove_instance(record);
                return;
            }
            if (mounted && record.bound_to(game, id)) {
                instances_.emplace(id, record);
                sync = lookup_node<NetcodeReplicator>(record.sync);
                sync->set_physics_process_internal(true);
                if (auto* actor = lookup_node<Node>(record.actor)) emit_signal("interest_enter", actor);
                if (game_ != game) return;
                continue;
            }
            remove_instance(record, "Unable to restore the reentered object");
            if (game_ != game) return;
        }
        const auto state = game->routing_info(id, false);
        const auto key = uint64_t(int64_t(state.get("key", 0)));
        const auto template_id = uint32_t(key >> 32);
        if (!template_id || template_id > catalog_.size()) continue;
        const auto entry = catalog_[template_id - 1];
        const auto reference = uint64_t(int64_t(state.get("reference", 0)));
        if (!host_ && reference && uint32_t(reference >> 32) == creation_namespace_) {
            const auto request = uint32_t(reference);
            auto waiting = pending_.find(request);
            if (waiting != pending_.end() && waiting->second.template_id == template_id &&
                game->resolve_reference(int64_t(reference)) == id) {
                const auto record = waiting->second;
                pending_.erase(waiting);
                auto* sync = lookup_node<NetcodeReplicator>(record.sync);
                if (record.mounted() && sync->bind_adapter(game, id)) {
                    sync->set_physics_process_internal(true);
                    instances_.emplace(id, record);
                } else {
                    expired_.insert(request);
                    game->request_despawn(0, reference);
                    remove_instance(record, "Unable to bind the confirmed spawn");
                }
                if (game_ != game) return;
                continue;
            }
            if (expired_.count(request)) {
                game->request_despawn(0, reference);
                continue;
            }
        }
        auto* spawner = lookup_node<NetcodeSpawner>(entry.spawner);
        auto* parent = spawner && spawner->is_inside_tree() ? spawner->get_node_or_null(spawner->get_spawn_path()) : nullptr;
        if (!parent || parent->is_queued_for_deletion()) continue;
        auto* actor = entry.scene->instantiate();
        if (!actor) continue;
        auto* sync = find_sync(actor, actor);
        if (!sync || int(sync->prepare_adapter().get("id", 0)) != entry.schema) {
            memdelete(actor);
            continue;
        }
        const Instance record{template_id, ObjectID(actor->get_instance_id()), ObjectID(sync->get_instance_id())};
        instances_.emplace(id, record);
        parent->add_child(actor);
        sync = lookup_node<NetcodeReplicator>(record.sync);
        if (game_ != game) return;
        const bool mounted = record.mounted() && sync->bind_adapter(game, id) && sync->apply_baseline();
        if (game_ != game) return;
        if (!mounted || !record.bound_to(game, id)) {
            instances_.erase(id);
            failed_mounts_.insert(id);
            remove_instance(record, "Unable to install the remote spawn baseline");
            if (game_ != game) return;
            continue;
        }
        sync = lookup_node<NetcodeReplicator>(record.sync);
        const auto still_mounted = [&] {
            if (game_ != game) return false;
            if (instances_.count(id) && record.bound_to(game, id)) return true;
            if (instances_.erase(id)) {
                failed_mounts_.insert(id);
                if (host_) game->destroy_object(id);
                else if (game->native_authority(id)) game->request_despawn(id);
                remove_instance(record, "The network instance exited during spawn");
            }
            return false;
        };
        const bool spatial_property = sync->get_spatial_interest() && !sync->get_spatial_position_property().is_empty();
        sync->update_spatial_distribution();
        if (spatial_property && !still_mounted()) {
            if (game_ != game) return;
            continue;
        }
        sync->set_physics_process_internal(true);
        sync->emit_signal("spawned");
        if (game_ != game) return;
        if (!still_mounted()) {
            if (game_ != game) return;
            continue;
        }
        actor = lookup_node<Node>(record.actor);
        spawner = lookup_node<NetcodeSpawner>(entry.spawner);
        if (actor && spawner) spawner->emit_signal("spawned", actor);
        if (game_ != game) return;
        if (!still_mounted()) {
            if (game_ != game) return;
            continue;
        }
        actor = lookup_node<Node>(record.actor);
        if (actor && !host_ && (entry.spatial || !entry.user_keys.is_empty())) emit_signal("interest_enter", actor);
        if (game_ != game) return;
    }
    // The frame IDs and the instance maps are in object-ID order.
    const auto visible = [ids, count](int64_t id, int64_t& index) {
        while (index < count && uint32_t(ids[index]) < id) ++index;
        return index < count && uint32_t(ids[index]) == id;
    };
    int64_t index = 0;
    std::vector<int64_t> hidden;
    for (const auto& [id, record] : instances_) {
        if (!visible(id, index) && game->routing_info(id, false).is_empty()) hidden.push_back(id);
    }
    for (auto id : hidden) {
        auto record = instances_.extract(id);
        if (record.empty()) continue;
        const auto& mounted = record.mapped();
        auto* sync = lookup_node<NetcodeReplicator>(mounted.sync);
        auto* actor = lookup_node<Node>(mounted.actor);
        if (interest_hidden.count(id) && sync && actor) {
            const bool destroy = sync->get_interest_destroy_on_exit();
            sync->unbind();
            if (destroy) actor->queue_free();
            else retained_.insert(std::move(record));
            emit_signal("interest_exit", actor);
        } else {
            remove_instance(mounted);
        }
        if (game_ != game) return;
    }
    index = 0;
    for (auto it = failed_mounts_.begin(); it != failed_mounts_.end();) {
        if (!visible(*it, index)) it = failed_mounts_.erase(it);
        else ++it;
    }
}
