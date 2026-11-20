#pragma once

#include "layout.hpp"
#include "session/rpc.hpp"

#include <functional>
#include <memory>
#include <set>

namespace yard {

struct Input { float throttle = 0, steer = 0; };

enum Action : netcode::MethodId {
    TryOpen = 1, OpenDenied, PlaceBarrier, RemoveBarrier, RoomNotice, Horn
};

class Match {
public:
    using Log = std::function<void(const std::string&)>;
    Match(const Layout& layout, std::unique_ptr<netcode::Transport> transport, bool host,
          netcode::RoomId room = 0, std::optional<netcode::ResumeTicket> ticket = {}, Log log = {});

    void update(netcode::TimePoint now);
    bool start(std::size_t objects = 0);
    void end_round();
    void end_room();
    void disconnect();
    void set_input(Input input);
    bool open_gate(bool public_object = false);
    bool barrier(bool create);
    bool horn();
    void freeze(bool frozen) { frozen_ = frozen; }

    netcode::RoomPhase phase() const;
    netcode::RoomId room() const;
    netcode::RoundId round() const;
    netcode::PlayerId player() const;
    netcode::ResumeTicket ticket() const;
    netcode::Reject rejected() const;
    netcode::Disconnect loss() const;
    netcode::Tick tick() const { return tick_; }
    netcode::Tick send_tick() const { return send_tick_; }
    std::uint64_t samples() const { return sample_count_; }
    std::uint64_t changes() const { return authority_.changes(); }
    netcode::ObjectId car() const;
    std::size_t seats() const;
    std::size_t ready_guests() const;
    bool ready() const;
    std::vector<netcode::ObjectId> objects() const;
    const netcode::Object* object(netcode::ObjectId id) const;
    std::uint64_t digest() const;
    const std::string& notice() const { return notice_; }
    netcode::Traffic traffic() const { return link_->traffic(); }
    std::size_t rpc_peak() const { return rpc_.peak_queued(); }
    std::size_t rpc_pending() const { return rpc_.queued(); }
    netcode::Transport& transport() { return *link_; }

private:
    struct Driver {
        netcode::ObjectId object;
        float speed = 0;
        Input input;
        std::uint32_t sequence = 0;
        netcode::Tick last_input = 0;
    };

    void receive(const netcode::Event& event);
    void host_message(const netcode::Event& event, netcode::GameMessage type, netcode::Reader& reader);
    void guest_message(const netcode::Event& event, netcode::GameMessage type, netcode::Reader& reader);
    void receive_rpc(const netcode::RpcCall& call);
    void network_step();
    void simulate();
    void begin(std::size_t count);
    void clear_world();
    void log(const std::string& message) const;
    netcode::ObjectId find(netcode::SchemaId schema, netcode::PlayerId owner = 0) const;
    netcode::ObjectId spawn(netcode::SchemaId schema, netcode::PlayerId owner, const netcode::Values& values);

    std::unique_ptr<netcode::Transport> link_;
    std::optional<netcode::RoomHost> host_;
    std::optional<netcode::RoomGuest> guest_;
    netcode::AuthorityState authority_;
    netcode::ReplicaState replica_;
    netcode::Rpc rpc_;
    Log trace_;
    std::map<netcode::PlayerId, Driver> drivers_;
    std::set<netcode::Peer> attached_;
    netcode::ObjectId next_object_ = 1, gate_ = 0, beacon_ = 0;
    netcode::Tick tick_ = 0, send_tick_ = 0;
    std::uint64_t sample_count_ = 0;
    netcode::TimePoint next_simulate_{}, next_network_{};
    std::uint32_t input_sequence_ = 0;
    Input local_;
    std::string notice_;
    bool frozen_ = false;
};

}
