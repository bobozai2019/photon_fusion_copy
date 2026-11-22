#include "session/discovery.hpp"
#include "transport/steam_p2p.hpp"
#include <steam/steam_api.h>
#include <stdexcept>

namespace netcode {
namespace {
using namespace std::chrono_literals;
constexpr const char* game_key="netcode_list_protocol", *data_key="netcode_room";
std::string hex(const Bytes& data) {
    static constexpr char digits[]="0123456789abcdef";
    std::string out; out.reserve(data.size()*2);
    for(auto n:data){out.push_back(digits[n>>4]);out.push_back(digits[n&15]);}return out;
}
Bytes unhex(const std::string& text) {
    if(text.size()>2048 || text.size()%2)return {};
    auto digit=[](char c)->int{return c>='0' && c<='9'? c-'0' : c>='a'&&c<='f'?c-'a'+10:-1;};
    Bytes out;
    for(size_t i=0;i<text.size();i+=2){int a=digit(text[i]),b=digit(text[i+1]); if(a<0||b<0)return {};out.push_back(uint8_t(a*16+b));}
    return out;
}
class SteamDirectory final : public RoomDirectory {
public:
    explicit SteamDirectory(std::shared_ptr<SteamRuntime> runtime) : runtime_(std::move(runtime)) {
        if(!runtime_ || !SteamMatchmaking() || !SteamUser())throw std::runtime_error("Steam matchmaking unavailable");
    }
    ~SteamDirectory() override {
        withdraw();
        if(joined_.IsValid()) SteamMatchmaking()->LeaveLobby(joined_);
    }
    bool join(RoomId id,Joined done) override {
        auto it=platform_ids_.find(id);
        if(!done || it==platform_ids_.end() || entering_.IsActive() || lobby_.IsValid() ||
           identity()!=IdentityStatus::PlatformAuthenticated) return false;
        if(joined_.IsValid()){SteamMatchmaking()->LeaveLobby(joined_);joined_.Clear();}
        auto op=SteamMatchmaking()->JoinLobby(it->second);
        if(op==k_uAPICallInvalid)return false;
        wanted_=id; completion_=std::move(done);
        entering_.Set(op,this,&SteamDirectory::entered);
        return true;
    }
    IdentityStatus identity() const override {
        return SteamUser()->BLoggedOn() && SteamUser()->GetSteamID().IsValid() ? IdentityStatus::PlatformAuthenticated : IdentityStatus::Unavailable;
    }
    bool publish(const RoomListing& r) override {
        auto encoded=encode_listing(r);
        if(encoded.empty() || identity()!=IdentityStatus::PlatformAuthenticated)return false;
        auto value=hex(encoded);
        if(value!=desired_) { desired_=std::move(value); info_=r; dirty_=true; }
        return true;
    }
    void withdraw() override {
        desired_.clear(); dirty_=false;
        if(lobby_.IsValid()){SteamMatchmaking()->LeaveLobby(lobby_);lobby_.Clear();}
    }
    void poll(TimePoint now) override {
        SteamAPI_RunCallbacks();
        if(identity()!=IdentityStatus::PlatformAuthenticated) { rooms_.clear(); return; }
        if(!desired_.empty() && !lobby_.IsValid() && !creation_.IsActive() && now>=next_create_) {
            next_create_=now+5s;
            auto op=SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic,info_.capacity);
            if(op!=k_uAPICallInvalid)creation_.Set(op,this,&SteamDirectory::created);
        }
        if(dirty_ && lobby_.IsValid()) {
            auto* api=SteamMatchmaking();
            dirty_=!(api->SetLobbyData(lobby_,game_key,"1") && api->SetLobbyData(lobby_,data_key,desired_.c_str()) &&
                      api->SetLobbyMemberLimit(lobby_,info_.capacity) && api->SetLobbyJoinable(lobby_,info_.joinable && info_.occupied<info_.capacity));
        }
        if(now>=next_search_ && !list_.IsActive()) {
            next_search_=now+3s;
            auto* api=SteamMatchmaking();
            api->AddRequestLobbyListStringFilter(game_key,"1",k_ELobbyComparisonEqual);
            api->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
            api->AddRequestLobbyListResultCountFilter(100);
            auto op=api->RequestLobbyList();
            if(op!=k_uAPICallInvalid)list_.Set(op,this,&SteamDirectory::listed);
        }
    }
    std::vector<RoomListing> search(const RoomFilter& f) const override {
        std::vector<RoomListing> result;
        for(const auto& r:rooms_)if(f.matches(r))result.push_back(r);
        return result;
    }
private:
    void created(LobbyCreated_t* result,bool failed) {
        if(failed || result->m_eResult!=k_EResultOK)return;
        lobby_=CSteamID(result->m_ulSteamIDLobby);
        if(desired_.empty())withdraw();else dirty_=true;
    }
    void listed(LobbyMatchList_t* matches,bool failed) {
        rooms_.clear(); platform_ids_.clear(); if(failed)return;
        auto* api=SteamMatchmaking();
        for(uint32 i=0;i<matches->m_nLobbiesMatching;++i) {
            auto lobby=api->GetLobbyByIndex(i); RoomListing r;
            if(!decode_listing(unhex(api->GetLobbyData(lobby,data_key)),r))continue;
            platform_ids_[r.room]=lobby; rooms_.push_back(std::move(r));
        }
    }
    void entered(LobbyEnter_t* event,bool failed) {
        std::optional<RoomListing> result;
        if(!failed && event->m_EChatRoomEnterResponse==k_EChatRoomEnterResponseSuccess) {
            joined_=CSteamID(event->m_ulSteamIDLobby);
            auto api=SteamMatchmaking(); auto owner=api->GetLobbyOwner(joined_); RoomListing listing;
            if(owner.IsValid() && decode_listing(unhex(api->GetLobbyData(joined_,data_key)),listing) &&
               listing.room==wanted_ && RoomFilter{}.matches(listing)) {
                listing.endpoint=std::to_string(owner.ConvertToUint64()); result=std::move(listing);
            } else { api->LeaveLobby(joined_); joined_.Clear(); }
        }
        auto done=std::move(completion_);
        if(done) done(std::move(result));
    }
    std::map<RoomId,CSteamID> platform_ids_;
    CSteamID joined_; RoomId wanted_=0; Joined completion_;
    CCallResult<SteamDirectory,LobbyEnter_t> entering_;
    std::shared_ptr<SteamRuntime> runtime_;
    CSteamID lobby_; RoomListing info_; std::string desired_;
    bool dirty_=false; TimePoint next_create_{},next_search_{};
    CCallResult<SteamDirectory,LobbyCreated_t> creation_;
    CCallResult<SteamDirectory,LobbyMatchList_t> list_;
    std::vector<RoomListing> rooms_;
};
}
std::unique_ptr<RoomDirectory> steam_directory(std::shared_ptr<SteamRuntime> runtime) {
    return std::make_unique<SteamDirectory>(std::move(runtime));
}
}
