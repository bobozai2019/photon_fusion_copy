#include "session/script_game.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace netcode {
ScriptGame::ScriptGame(std::unique_ptr<Transport> link, SessionOptions options,
                       const ReplicationLayout &layout, MigratingSession::Open open)
    : discipline_(options.clock_discipline), term_(options.term), schemas_(layout.schemas()) {
    runtime_ = std::make_unique<MigratingSession>(std::move(link), options, schemas_, MigrationOptions{}, std::move(open),
        [this](GameSession& game) { configure(game); });
}
GameSession& ScriptGame::session() {
    auto* live = active();
    if (!live) throw std::logic_error("Session authority is suspended during migration");
    return *live;
}
void ScriptGame::pump(TimePoint now) {
    runtime_->pump(now);
    auto* game = active();
    if (!game) {
        events_.clear();
        calls_.clear();
        typed_calls_.clear();
        replicas_.clear();
        return;
    }
    if (game->term() == term_) return;
    term_ = game->term();
    events_.clear();
    calls_.clear();
    typed_calls_.clear();
    replicas_.clear();
    events_.push_back({ScriptEventKind::SessionReplaced,0,game->coordinator(),term_});
    for (const auto& [id,map] : game->world().maps())
        events_.push_back({ScriptEventKind::MapAdded,id,0,map.generation});
    for (auto id : game->objects()) events_.push_back({ScriptEventKind::Ready,id});
}
bool ScriptGame::remember(std::function<bool(GameSession&)> install) {
    if (!active() || !install(session())) return false;
    registrations_.push_back(std::move(install));
    return true;
}
bool ScriptGame::threshold_clock(bool enabled) {
    auto next = enabled ? ClockDiscipline::Threshold : ClockDiscipline::Monotonic;
    if (!active() || !session().set_clock_discipline(next)) return false;
    discipline_ = next;
    return true;
}
bool ScriptGame::prediction(SchemaId id, SimulationModel model) {
    return remember([id,model=std::move(model)](GameSession& game){return game.prediction().register_model(id,model);});
}
bool ScriptGame::spawn_factory(SchemaId id, Values values, bool creator) {
    if (!active() || !schemas_.valid(id,values)) return false;
    factories_[id] = {values,creator};
    session().world().allow_spawn(id,[values=std::move(values)](PlayerId,SceneId){return values;},creator);
    return true;
}
void ScriptGame::configure(GameSession& game) {
    game.set_clock_discipline(discipline_);
    for (auto& install : registrations_)
        if (!install(game)) throw std::logic_error("Could not restore a registered script model");
    for (const auto& [id,factory] : factories_)
        game.world().allow_spawn(id,[values=factory.first](PlayerId,SceneId){return values;},factory.second);
    game.prediction().on_reconciled = [this,&game](ObjectId id, Tick tick, Tick through) {
        events_.push_back({ScriptEventKind::Reconciled, id, game.local(), tick, through});
    };
    game.on_rpc = [this](const RpcCall &call) { receive_call(call); };
    game.on_snapshot = [this,&game](const AppliedSnapshot &s) {
        for (auto id : s.ready)
            events_.push_back({ScriptEventKind::Ready, id});
        for (auto id : s.hidden)
            events_.push_back({ScriptEventKind::Hidden, id});
        for (auto id : s.destroyed)
            events_.push_back({ScriptEventKind::Destroyed, id});
        for (auto id : s.changed)
            if (game.object(id))
                events_.push_back({ScriptEventKind::Changed, id});
    };
    game.on_grant = [this](const ObjectGrant &g) {
        events_.push_back({ScriptEventKind::Grant, g.object, g.writer, g.epoch});
    };
    game.writers().on_request([this](const Claim &c) {
        events_.push_back({ScriptEventKind::Request, c.object, c.requester, c.epoch, c.sequence});
    });
    game.writers().on_result([this](const Claim &c, ClaimStatus s) {
        events_.push_back(
            {ScriptEventKind::Result, c.object, c.requester, c.epoch, c.sequence, static_cast<unsigned>(s)});
    });
    game.writers().on_prediction_failed(
        [this](ObjectId id) { events_.push_back({ScriptEventKind::PredictionFailed, id}); });
    game.world().on_map = [this](const MapInstance &m, bool added) {
        events_.push_back(
            {added ? ScriptEventKind::MapAdded : ScriptEventKind::MapRemoved, m.id, 0, m.generation});
    };
    game.world().on_spawn_result = [this](std::uint32_t seq, ObjectId id) {
        events_.push_back({ScriptEventKind::SpawnResult, id, 0, 0, seq});
    };
}
bool ScriptGame::distribute(ObjectId object, InterestKind kind, const std::vector<uint64_t> &keys,
                            int divisor) {
    bool keyed = kind == InterestKind::Area || kind == InterestKind::User;
    if (!session().host() || int(kind) < 0 || kind > InterestKind::Inherit || keyed != !keys.empty() ||
        keys.size() > 64 || divisor < 1 || divisor > UINT16_MAX)
        return false;
    Distribution d{kind, keyed ? keys.front() : 0, uint16_t(divisor)};
    if (keyed)
        d.additional_keys.insert(keys.begin() + 1, keys.end());
    return session().authority().set_distribution(object, std::move(d));
}
bool ScriptGame::subscribe(PlayerId player, InterestKind kind, const std::vector<uint64_t> &keys,
                           const std::vector<int> &rates) {
    auto *host = session().host();
    if (!host || keys.size() != rates.size() || keys.size() > 64 ||
        (kind != InterestKind::Area && kind != InterestKind::User))
        return false;
    auto member = host->members().find(player);
    if (member == host->members().end() || !member->second.peer)
        return false;
    std::map<uint64_t, uint16_t> out;
    for (size_t i = 0; i < keys.size(); ++i)
        if (rates[i] < 1 || rates[i] > UINT16_MAX || !out.emplace(keys[i], uint16_t(rates[i])).second)
            return false;
    return session().authority().subscribe_rates(member->second.peer, kind, std::move(out));
}
bool ScriptGame::set_player_ttl(int64_t ms) {
    return session().host() && ms >= -1 && ms <= UINT32_MAX &&
           session().host()->set_player_ttl(ms < 0 ? std::nullopt : std::optional{uint32_t(ms)});
}
bool ScriptGame::register_rpc(int method, int direction, bool reliable, int max_bytes, bool writer,
                              bool room) {
    if (session().round() || method < 1 || method > UINT16_MAX || direction < 0 ||
        direction > int(Direction::Remote) || max_bytes < 0)
        return false;
    Method description{MethodId(method), room, Direction(direction),
                                        reliable ? Reliability::Reliable : Reliability::BestEffort,
                                        size_t(max_bytes),
                                        writer ? RpcSubject::StateWriter : RpcSubject::BusinessOwner, true};
    return remember([description](GameSession& g){ return g.rpc().register_method(description); });
}

bool ScriptGame::bind_method(const std::string& name,MethodId method,SchemaId arguments,int direction,
                             bool reliable,bool writer,bool room) {
    if (!active() || name.empty() || name.size()>96 || method_names_.count(name)) return false;
    const auto* schema=schemas_.get(arguments);
    if (arguments && !schema) return false;
    size_t size=4;
    uint32_t hash=2166136261u;
    const auto mix=[&](uint32_t n){hash=(hash^n)*16777619u;};
    for (unsigned char c:name) mix(c);
    mix(method); mix(direction); mix(reliable); mix(writer); mix(room); mix(arguments);
    if (schema) {
        mix(schema->version);
        mix(schema_fingerprint(*schema));
        for (auto f:schema->fields) size+=field_capacity(f);
    }
    if (size>256 || !register_rpc(method,direction,reliable,int(size),writer,room)) return false;
    method_names_.emplace(name,method);
    signatures_.emplace(method,Signature{arguments,hash});
    return true;
}
SchemaId ScriptGame::argument_schema(const std::string& name) const {
    auto it=method_names_.find(name);
    return it==method_names_.end() ? 0 : signatures_.at(it->second).schema;
}
bool ScriptGame::call_method(const std::string& name,ObjectId object,const Values& values,PlayerId target,uint64_t token) {
    auto it=method_names_.find(name);
    if (!active() || it==method_names_.end()) return false;
    const auto sig=signatures_.at(it->second);
    if (sig.schema ? !schemas_.valid(sig.schema,values) : !values.empty()) return false;
    Writer out; out.u32(sig.hash);
    for (const auto& v:values) write_value(out,v);
    return session().rpc().invoke(object,it->second,out.bytes(),target,token);
}
void ScriptGame::receive_call(const RpcCall& call) {
    auto it=signatures_.find(call.method);
    if (it==signatures_.end()) { calls_.push_back(call); return; }
    Reader in(call.arguments);
    uint32_t hash;
    if (!in.u32(hash) || hash!=it->second.hash) return;
    TypedDelivery result{call.method,it->second.schema,call.object,call.sender,{}};
    if (auto schema=schemas_.get(result.arguments)) {
        for (auto f:schema->fields) {
            Value value;
            if (!read_value(in,f,value)) return;
            result.values.push_back(std::move(value));
        }
    }
    if (in.done()) typed_calls_.push_back(std::move(result));
}

bool ScriptGame::model(SchemaId schema, std::vector<FieldId> ids, ClaimMode mode, Tick cooldown) {
    if (!remember([schema,ids,mode,cooldown](GameSession& game){
        if (!game.writers().register_model(schema,1,ids)) return false;
        ClaimPolicy policy{mode,cooldown};
        policy.destroy = [&game](ObjectId id,Tick){game.world().destroy(id);};
        return game.writers().allow_requests(schema,std::move(policy));
    })) return false;
    const auto &all = schemas_.get(schema)->fields;
    for (auto id : ids)
        writable_[schema].push_back(
            *std::find_if(all.begin(), all.end(), [id](Field f) { return f.id == id; }));
    return true;
}
int ScriptGame::sync_replica(ObjectId id,const Values& sample,Values& applied,bool forecast) {
    applied.clear();
    auto* g=active();
    auto* obj=g ? g->object(id) : nullptr;
    if (!obj || !g->round()) { replicas_.erase(id); return 0; }
    if (!schemas_.valid(obj->schema,sample)) return 0;
    auto writer=g->writers().writer(id);
    auto* grant=g->writers().grant(id);
    auto epoch=grant ? grant->epoch : 0;
    auto it=replicas_.find(id);
    if (it==replicas_.end() || it->second.round!=g->round() || it->second.writer!=writer || it->second.epoch!=epoch) {
        auto baseline=g->writers().presentation(*obj).values;
        replicas_[id]={g->round(),writer,epoch,baseline};
        applied=std::move(baseline);
        return 2;
    }
    it->second.received_revision.reset();
    auto& previous=it->second.previous;
    if (g->local()!=writer) {
        auto next=g->writers().presentation(*obj).values;
        if (next==previous && (forecast || next==sample)) return 0;
        applied=next;
        previous=std::move(next);
        return 1;
    }
    Values next=sample;
    if (!g->host()) {
        const auto mask=writable_.find(obj->schema);
        if (mask==writable_.end()) return 0;
        const auto& fields=schemas_.get(obj->schema)->fields;
        for (size_t i=0;i<fields.size();++i) {
            if (std::none_of(mask->second.begin(),mask->second.end(),[&](Field f){return f.id==fields[i].id;}))
                next[i]=obj->values[i];
        }
    }
    if (next!=previous && !submit_fields(id,next)) return 0;
    if (next!=sample) applied=next;
    previous=std::move(next);
    return applied.empty() ? 0 : 1;
}
int ScriptGame::receive_replica(ObjectId id, Values& applied, std::uint64_t& mask) {
    applied.clear();
    mask = 0;
    auto* game = active();
    const auto* object = game ? game->object(id) : nullptr;
    if (!object || !game->round()) {
        replicas_.erase(id);
        return 0;
    }
    auto& authority = game->writers();
    const auto* grant = authority.grant(id);
    const auto writer = grant ? grant->writer : authority.writer(id);
    if (writer == game->local()) return 0;
    const auto epoch = grant ? grant->epoch : 0;
    auto it = replicas_.find(id);
    const bool baseline = it == replicas_.end() || it->second.round != game->round() ||
                          it->second.writer != writer || it->second.epoch != epoch;
    // A pending grant can change the presented value without a new snapshot revision.
    const bool canonical = game->host() || (authority.presentation_writer(id) != game->local() &&
                          (!grant || object->revision >= grant->baseline_revision));
    if (!baseline && canonical && it->second.received_revision == object->revision) return 0;

    std::optional<Object> presented;
    if (!canonical) presented = authority.presentation(*object);
    const auto& fields = presented ? presented->values : object->values;
    const auto revision = canonical ? std::optional<Tick>(object->revision) : std::nullopt;
    if (baseline) {
        for (std::size_t i = 0; i < fields.size(); ++i) mask |= std::uint64_t{1} << i;
        replicas_[id] = {game->round(), writer, epoch, fields, revision};
        applied = fields;
        return 2;
    }
    auto& cursor = it->second;
    cursor.received_revision = revision;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i] == cursor.previous[i]) continue;
        mask |= std::uint64_t{1} << i;
        applied.push_back(fields[i]);
        cursor.previous[i] = fields[i];
    }
    return mask ? 1 : 0;
}

bool ScriptGame::submit_fields(ObjectId id,const Values& full) {
    if (!active()) return false;
    const auto* obj=session().object(id);
    if (!obj || !schemas_.valid(obj->schema,full)) return false;
    if (session().host()) return submit(id,full);
    auto it=writable_.find(obj->schema);
    if (it==writable_.end()) return false;
    Values subset;
    const auto& fields=schemas_.get(obj->schema)->fields;
    for (const auto& f:it->second) {
        auto pos=std::lower_bound(fields.begin(),fields.end(),f.id,[](Field a,FieldId b){return a.id<b;});
        subset.push_back(full[pos-fields.begin()]);
    }
    return submit(id,subset);
}
bool ScriptGame::submit(ObjectId id, const Values &values) {
    auto *o = session().object(id);
    if (!o)
        return false;
    auto &writers = session().writers();
    if (session().host()) {
        if (writers.writer(id) != session().local())
            return false;
        return session().authority().capture(id, values, session().tick());
    }
    if (writers.presentation_writer(id) != session().local())
        return false;
    if (writers.writer(id) != session().local())
        return writers.submit_predicted(id, 0, values);
    auto *grant = writers.grant(id);
    return grant && writers.submit(id, grant->epoch, 0, values);
}
bool ScriptGame::read(ObjectId id, bool presentation, Values &out) {
    auto *obj = session().object(id);
    if (!obj)
        return false;
    out = presentation ? session().writers().presentation(*obj).values : obj->values;
    return true;
}
bool ScriptGame::decode(SchemaId schema, bool model_only, const Bytes &encoded, Values &values) const {
    const auto *def = schemas_.get(schema);
    if (!def || (model_only && !writable_.count(schema)))
        return false;
    const auto &fields = model_only ? writable_.at(schema) : def->fields;
    Reader reader(encoded);
    Values next;
    for (const auto field : fields) {
        Value v;
        if (!read_value(reader, field, v))
            return false;
        next.push_back(std::move(v));
    }
    if (!reader.done())
        return false;
    values = std::move(next);
    return true;
}
std::deque<ScriptEvent> ScriptGame::take_events() { return std::exchange(events_, {}); }
} // namespace netcode
