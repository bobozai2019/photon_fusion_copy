#pragma once

#include "2d/CCComponent.h"
#include "session/script_game.hpp"
#include "host/game.hpp"

namespace netcode::cocos {

// Scene owns the component. Network and scene notifications run on the engine thread.
class GameComponent final : public cocos2d::Component {
  public:
    static GameComponent* create(std::unique_ptr<ScriptGame> game);
    static GameComponent* create_with_provider(std::string_view endpoint, SessionOptions options,
                                               const ReplicationLayout& layout, const ProviderConfig& provider = {});
    static const char* platform_build_id() noexcept;
    const ProviderIdentity& transport_identity() const { return provider_; }
    bool add_provider_standby(PlayerId player, std::string endpoint) {
        return provider_.abi && game_->migration().add_standby(player, {{}, 0, std::move(endpoint)});
    }
    std::weak_ptr<ScriptGame> session_handle() const { return game_; }
    using ReplicaNodes = std::map<ObjectId,cocos2d::Node*>;
    std::shared_ptr<ReplicaNodes> replica_nodes() const { return nodes_; }
    cocos2d::Node* resolve_node(ObjectReference reference) const {
        if (!game_->active()) return nullptr;
        auto it=nodes_->find(game_->session().world().resolve_field(reference));
        return it==nodes_->end() ? nullptr : it->second;
    }
    using MethodHandler = std::function<void(PlayerId,ObjectId,const Values&)>;
    bool bind_method(const std::string& name,MethodId id,SchemaId args,Direction dir,bool reliable,
                     MethodHandler handler,bool writer=false,bool room=false) {
        if (!game_->bind_method(name,id,args,int(dir),reliable,writer,room)) return false;
        methods_[id]=std::move(handler); return true;
    }
    template<class... Args,class Fn>
    bool bind_callable(const std::string& name,MethodId id,SchemaId schema,Direction dir,bool reliable,
                       Fn handler,bool writer=false,bool room=false) {
        const auto args=game_->schemas().get(schema);
        std::array<size_t,sizeof...(Args)> types{Value(Args{}).index()...};
        if ((args ? args->fields.size() : 0)!=types.size()) return false;
        for (size_t i=0;i<types.size();++i) if (size_t(args->fields[i].type)!=types[i]) return false;
        return bind_method(name,id,schema,dir,reliable,[handler](PlayerId from,ObjectId object,const Values& fields) {
            dispatch<Args...>(handler,from,object,fields,std::index_sequence_for<Args...>{});
        },writer,room);
    }
    ScriptGame& game() { return *game_; }
    std::function<void(const ScriptEvent&)> on_event;
    std::function<void()> on_frame;
    void update(float) override {
        game_->pump(Clock::now());
        if (!game_->active()) return;
        for (const auto& event : game_->take_events())
            if (on_event)
                on_event(event);
        while (auto call=game_->next_typed()) {
            auto event=*call; game_->pop_typed();
            auto handler=methods_.find(event.method);
            if (handler!=methods_.end() && handler->second) handler->second(event.sender,event.object,event.values);
        }
        if (on_frame)
            on_frame();
    }

  private:
    template<class... Args,class Fn,size_t... I>
    static void dispatch(const Fn& fn,PlayerId sender,ObjectId object,const Values& fields,std::index_sequence<I...>) {
        fn(sender,object,std::get<Args>(fields[I])...);
    }
    std::shared_ptr<ScriptGame> game_;
    ProviderIdentity provider_{"direct"};
    std::shared_ptr<ReplicaNodes> nodes_=std::make_shared<ReplicaNodes>();
    std::map<MethodId,MethodHandler> methods_;
};

}
