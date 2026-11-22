#include "transport/steam_p2p.hpp"

#include <steam/steam_api.h>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace netcode {

class SteamRuntime {
public:
    SteamRuntime()
    {
        SteamErrMsg detail{};
        if (SteamAPI_InitEx(&detail) != k_ESteamAPIInitResult_OK) {
            throw std::runtime_error(std::string("Steamworks initialization failed: ") + detail);
        }
        sockets = SteamNetworkingSockets();
        auto* utils = SteamNetworkingUtils();
        if (!sockets || !utils) {
            SteamAPI_Shutdown();
            throw std::runtime_error("Steamworks networking interfaces unavailable");
        }
        utils->InitRelayNetworkAccess();
    }

    ~SteamRuntime() { SteamAPI_Shutdown(); }

    ISteamNetworkingSockets* sockets = nullptr;
};

namespace {

constexpr int virtual_port = 0;
constexpr std::uint8_t control_lane = 1, game_lane = 2;
constexpr int receive_budget = 128;

struct Connection {
    Peer peer;
    bool opened = false, closing = false;
    Disconnect ended = Disconnect::None;
};

Disconnect disconnect_reason(const SteamNetConnectionInfo_t& info)
{
    if (info.m_eEndReason == k_ESteamNetConnectionEnd_Remote_Timeout ||
        info.m_eEndReason == k_ESteamNetConnectionEnd_Misc_Timeout) return Disconnect::Timeout;
    return info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ?
        Disconnect::Remote : Disconnect::IoError;
}

class SteamP2P final : public Transport {
public:
    SteamP2P(std::shared_ptr<SteamRuntime> runtime, bool host, std::uint64_t host_id)
        : runtime_(std::move(runtime)), host_(host), callback_(this, &SteamP2P::on_status)
    {
        if (host_) {
            listener_ = sockets().CreateListenSocketP2P(virtual_port, 0, nullptr);
            if (listener_ == k_HSteamListenSocket_Invalid) {
                throw std::runtime_error("Could not open Steam P2P listener");
            }
            return;
        }
        const CSteamID steam_id{host_id};
        if (!steam_id.IsValid() || steam_id.GetEAccountType() != k_EAccountTypeIndividual) {
            throw std::invalid_argument("Expected an individual SteamID64 for the host");
        }
        SteamNetworkingIdentity identity{};
        identity.SetSteamID(steam_id);
        const auto handle = sockets().ConnectP2P(identity, virtual_port, 0, nullptr);
        if (handle == k_HSteamNetConnection_Invalid) {
            throw std::runtime_error("Could not start Steam P2P connection");
        }
        try {
            connections_.emplace(handle, Connection{next_peer_++});
        } catch (...) {
            sockets().CloseConnection(handle, 0, nullptr, false);
            throw;
        }
    }

    ~SteamP2P() override
    {
        callback_.Unregister();
        for (const auto& [handle, connection] : connections_) {
            sockets().CloseConnection(handle, 0, nullptr, connection.closing);
        }
        if (listener_ != k_HSteamListenSocket_Invalid) sockets().CloseListenSocket(listener_);
    }

    std::vector<Event> poll(TimePoint) override
    {
        SteamAPI_RunCallbacks();
        std::vector<Event> events;
        for (const auto& status : statuses_) apply_status(status, events);
        statuses_.clear();
        traffic_.queued_bytes = 0;
        for (auto it = connections_.begin(); it != connections_.end();) {
            auto& [handle, connection] = *it;
            if (connection.opened && !connection.closing) receive(handle, connection, events);
            if (connection.ended == Disconnect::None && !connection.closing) {
                SteamNetConnectionRealTimeStatus_t status{};
                if (connection.opened &&
                    sockets().GetConnectionRealTimeStatus(handle, &status, 0, nullptr) == k_EResultOK) {
                    traffic_.queued_bytes += status.m_cbPendingReliable + status.m_cbPendingUnreliable;
                }
                ++it;
                continue;
            }
            const bool linger = connection.closing && connection.ended == Disconnect::None;
            if (linger) sockets().FlushMessagesOnConnection(handle);
            sockets().CloseConnection(handle, 0, nullptr, linger);
            if (connection.opened || !host_) {
                events.push_back({EventType::Closed, connection.peer, Lane::Control, {},
                                  linger ? Disconnect::Local : connection.ended});
            }
            it = connections_.erase(it);
        }
        traffic_.peak_queued_bytes = std::max(traffic_.peak_queued_bytes, traffic_.queued_bytes);
        return events;
    }

    bool send(Peer peer, Lane lane, const Bytes& payload) override
    {
        const auto it = find(peer);
        if (it == connections_.end() || !it->second.opened || it->second.closing ||
            it->second.ended != Disconnect::None || payload.empty()) return false;
        const bool control = lane == Lane::Control;
        if (payload.size() > (control ? control_payload_limit : game_payload_limit)) return false;
        Bytes message;
        message.reserve(payload.size() + 1);
        message.push_back(control ? control_lane : game_lane);
        message.insert(message.end(), payload.begin(), payload.end());
        const auto flags = control ? k_nSteamNetworkingSend_ReliableNoNagle :
                                     k_nSteamNetworkingSend_UnreliableNoNagle;
        if (sockets().SendMessageToConnection(it->first, message.data(),
                static_cast<std::uint32_t>(message.size()), flags, nullptr) != k_EResultOK) {
            if (!control) ++traffic_.udp_dropped;
            return false;
        }
        (control ? traffic_.tcp_sent : traffic_.udp_sent) += payload.size();
        return true;
    }

    void close(Peer peer) override
    {
        if (const auto it = find(peer); it != connections_.end()) it->second.closing = true;
    }

    Traffic traffic() const override { return traffic_; }

private:
    ISteamNetworkingSockets& sockets() const { return *runtime_->sockets; }

    std::map<HSteamNetConnection, Connection>::iterator find(Peer peer)
    {
        return std::find_if(connections_.begin(), connections_.end(), [peer](const auto& entry) {
            return entry.second.peer == peer;
        });
    }

    void on_status(SteamNetConnectionStatusChangedCallback_t* status)
    {
        if (connections_.count(status->m_hConn) ||
            (host_ && status->m_info.m_hListenSocket == listener_ &&
             status->m_eOldState == k_ESteamNetworkingConnectionState_None &&
             status->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting)) {
            statuses_.push_back(*status);
        }
    }

    void apply_status(const SteamNetConnectionStatusChangedCallback_t& status,
                      std::vector<Event>& events)
    {
        const auto handle = status.m_hConn;
        const auto state = status.m_info.m_eState;
        const auto it = connections_.find(handle);
        if (it == connections_.end()) {
            if (state != k_ESteamNetworkingConnectionState_Connecting) return;
            if (!status.m_info.m_identityRemote.GetSteamID64() ||
                sockets().AcceptConnection(handle) != k_EResultOK) {
                sockets().CloseConnection(handle, 0, nullptr, false);
                return;
            }
            connections_.emplace(handle, Connection{next_peer_++});
            return;
        }
        auto& connection = it->second;
        if (state == k_ESteamNetworkingConnectionState_Connected && !connection.opened) {
            connection.opened = true;
            events.push_back({EventType::Open, connection.peer});
        } else if (state == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                   state == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
            connection.ended = disconnect_reason(status.m_info);
        }
    }

    void receive(HSteamNetConnection handle, Connection& connection, std::vector<Event>& events)
    {
        for (int count = 0; count < receive_budget || connection.ended != Disconnect::None; ++count) {
            SteamNetworkingMessage_t* raw = nullptr;
            const int received = sockets().ReceiveMessagesOnConnection(handle, &raw, 1);
            if (received <= 0) {
                if (received < 0 && connection.ended == Disconnect::None) connection.ended = Disconnect::IoError;
                return;
            }
            const auto release = [](SteamNetworkingMessage_t* message) { message->Release(); };
            const std::unique_ptr<SteamNetworkingMessage_t, decltype(release)> message(raw, release);
            const auto* start = static_cast<const std::uint8_t*>(message->m_pData);
            if (message->m_cbSize < 2 || (start[0] != control_lane && start[0] != game_lane)) {
                connection.ended = Disconnect::Protocol;
                return;
            }
            const bool control = start[0] == control_lane;
            const auto size = static_cast<std::size_t>(message->m_cbSize - 1);
            if (size > (control ? control_payload_limit : game_payload_limit) ||
                (control && !(message->m_nFlags & k_nSteamNetworkingSend_Reliable))) {
                connection.ended = Disconnect::Protocol;
                return;
            }
            (control ? traffic_.tcp_received : traffic_.udp_received) += size;
            events.push_back({EventType::Message, connection.peer,
                              control ? Lane::Control : Lane::Game, Bytes(start + 1, start + 1 + size)});
        }
    }

    std::shared_ptr<SteamRuntime> runtime_;
    const bool host_;
    HSteamListenSocket listener_ = k_HSteamListenSocket_Invalid;
    Peer next_peer_ = 1;
    std::map<HSteamNetConnection, Connection> connections_;
    std::vector<SteamNetConnectionStatusChangedCallback_t> statuses_;
    Traffic traffic_;
    CCallback<SteamP2P, SteamNetConnectionStatusChangedCallback_t> callback_;
};

}

std::shared_ptr<SteamRuntime> start_steam()
{
    static std::weak_ptr<SteamRuntime> current;
    if (auto runtime = current.lock()) return runtime;
    auto runtime = std::make_shared<SteamRuntime>();
    current = runtime;
    return runtime;
}

std::unique_ptr<Transport> listen_steam(const std::shared_ptr<SteamRuntime>& runtime)
{
    return std::make_unique<SteamP2P>(runtime, true, 0);
}

std::unique_ptr<Transport> connect_steam(const std::shared_ptr<SteamRuntime>& runtime,
                                       std::uint64_t host_steam_id)
{
    return std::make_unique<SteamP2P>(runtime, false, host_steam_id);
}

}
