#pragma once

#include "state/replication.hpp"

#include <functional>
#include <set>
#include <unordered_map>

namespace netcode {

// Values are in the order of the model's registered field IDs. Recovery is
// opaque model state required by the next writer, e.g. an integrator's velocity.
struct AuthoritySample {
    Tick sequence = 0;
    Tick source_step = 0;
    Values values;
    Bytes recovery;
};

struct ObjectGrant {
    ObjectId object = 0;
    SchemaId schema = 0;
    PlayerId writer = host_player;
    std::uint32_t epoch = 0;
    Tick baseline_revision = 0;
    Tick committed_at = 0;
    Tick confirmed = 0;
    AuthoritySample sample;
    Tick offered_sequence = 0, offered_at = 0;
};

enum class ClaimMode { Transaction, Dynamic, PlayerAttached, MasterClient, GameGlobal };
inline bool master_only(ClaimMode m) { return m==ClaimMode::MasterClient || m==ClaimMode::GameGlobal; }
enum class ClaimStatus : std::uint8_t { Granted, Denied, Busy, Stale, Unavailable, Expired, Cooldown };
struct Claim {
    ObjectId object;
    PlayerId requester;
    Tick epoch, sequence;
};

struct ClaimPolicy {
    ClaimMode mode = ClaimMode::Transaction;
    Tick cooldown = 0;
    std::function<Bytes(ObjectId)> recovery;
    std::function<void(ObjectId, Tick)> destroy;
};

class ObjectAuthority {
public:
    using Lookup = std::function<const Object*(ObjectId)>;
    using Commit = std::function<bool(ObjectId, const Values&, Tick)>;
    using Validate = std::function<bool(const Object&, const AuthoritySample&, const AuthoritySample&, Tick)>;
    using Changed = std::function<void(const ObjectGrant&)>;
    using Known = std::function<bool(Peer, ObjectId)>;

    using InstanceMode = std::function<std::optional<ClaimMode>(ObjectId)>;
    ObjectAuthority(Transport& link, const Schemas& schemas, Lookup lookup, Commit commit, Changed changed = {},
                    Known known = {}, InstanceMode instance_mode = {});
    bool register_model(SchemaId schema, std::uint16_t model_version, const std::vector<FieldId>& fields,
                        std::size_t max_recovery = 0, Validate validate = {});
    void reset(RoomId room, RoundId round, PlayerId local, Peer coordinator = 0, PlayerId master = host_player);
    void attach(Peer peer, PlayerId player, bool writable = true);
    void detach(Peer peer, Tick now);
    void forget(ObjectId object);
    void unobserve(ObjectId object);
    bool supports(PlayerId player, SchemaId schema) const;
    bool supports_claims(Peer peer) const { return claim_peers_.count(peer) != 0; }

    // Only the coordinator assigns writers. The outgoing remote writer's last
    // accepted recovery is retained automatically on revoke or direct transfer.
    bool assign(ObjectId object, PlayerId writer, Tick now, const Bytes& recovery = {}, Tick source_step = 0);
    const ObjectGrant* grant(ObjectId object) const;
    PlayerId default_writer() const { return master_; }
    PlayerId writer(ObjectId object) const;
    bool submit(ObjectId object, std::uint32_t epoch, Tick source_step, Values values, Bytes recovery = {});
    bool overlay(Object& view) const;
    Object presentation(const Object& canonical);
    bool receive(const Event& event, Tick now);
    // Pass the snapshot sender so a published grant can fence its receiver's baseline.
    void flush(Tick now = 0, AuthorityState* replicas = nullptr, Tick retry_ticks = 0);
    bool negotiate_word_state(AuthorityState& state, Peer peer, GameMessage type, Reader& payload);
    void probe_word_state(Tick send_tick);
    bool allow_requests(SchemaId schema, ClaimPolicy policy);
    std::optional<ClaimMode> mode(SchemaId schema) const {
        auto it=policies_.find(schema);return it==policies_.end()?std::nullopt:std::optional{it->second.mode};
    }
    std::optional<ClaimMode> mode(ObjectId id, SchemaId schema) const {
        const auto override_mode = instance_mode_ ? instance_mode_(id) : std::nullopt;
        return override_mode ? override_mode : mode(schema);
    }
    bool supports_mode(SchemaId schema, ClaimMode value) const {
        auto it = policies_.find(schema);
        return value >= ClaimMode::Transaction && value <= ClaimMode::GameGlobal && it != policies_.end()
            && (value != ClaimMode::PlayerAttached || it->second.destroy);
    }
    void on_request(std::function<void(const Claim&)> callback) { prompt_ = std::move(callback); }
    void on_result(std::function<void(const Claim&, ClaimStatus)> callback) { result_ = std::move(callback); }
    bool request(ObjectId object);
    bool request_coordinator(ObjectId object, Tick now);
    bool release(ObjectId object);
    bool has_predicted_claim(ObjectId id) const { return predicted_.count(id) != 0; }
    PlayerId presentation_writer(ObjectId id) const { return has_predicted_claim(id) ? local_ : writer(id); }
    bool submit_predicted(ObjectId id, Tick step, Values values, Bytes recovery = {});
    void on_prediction_failed(std::function<void(ObjectId)> callback) { prediction_failed_ = std::move(callback); }
    bool answer(const Claim& claim, bool accept, Tick now);
    // Lifetime totals survive round resets for disconnect and rejoin telemetry.
    std::uint64_t submissions() const { return submissions_; }
    std::uint64_t acceptances() const { return acceptances_; }
    std::uint64_t receipt_coalesces() const { return receipt_coalesces_; }

private:
    friend class ScriptGame;
    bool submit_scene_sample(ObjectId id, std::uint32_t epoch, const Values& values);
    template<class Sample>
    bool submit_values(ObjectId id, std::uint32_t epoch, Tick step, Sample&& values, Bytes recovery);

    struct Model {
        std::vector<std::size_t> slots;
        std::vector<Field> fields;
        std::size_t max_recovery;
        std::uint32_t signature;
        Validate validate;
        WordLayout layout{};
    };
    struct PendingReceipt {
        std::uint32_t epoch;
        std::uint32_t sequence;
    };
    using Receipts = std::map<ObjectId, PendingReceipt>;
    struct Participant {
        PlayerId player;
        bool offered = false;
        std::map<SchemaId, std::uint32_t> models;
        std::unordered_map<ObjectId, std::uint32_t> delivered;
        Tick last_request = 0;
        bool writable = true;
        bool state_batches = false;
        Receipts receipts{};
        ObjectId receipt_cursor = 0;
        bool receipt_first = true;
    };
    struct ReceiveBuffers {
        AuthoritySample sample;
        Words words;
        Values merged;
    };
    static void encode_sample(Writer& out, const AuthoritySample& sample);
    static bool decode_sample(Reader& in, const Model& model, AuthoritySample& sample);
    static void merge(const Model& model, const Values& fields, Values& target);
    void notify(const ObjectGrant& grant);
    void receive_grant(Reader& in);
    void receive_state(Peer peer, PlayerId player, Reader& in, Tick now, bool delta, Receipts* receipts = nullptr);
    void receive_state_batch(Peer peer, Participant& participant, Reader in, Tick now);
    static constexpr std::size_t max_receipt_packets = 8;
    std::size_t initial_receipt_budget(Peer peer, std::size_t available);
    std::size_t flush_receipts(Peer peer, std::size_t budget);
    void receive_ack(Reader& in);
    GameMessage encode_state(Writer& out, Writer& delta, const ObjectGrant& grant);
    bool send_request(ObjectId object, bool release);
    void receive_claim(Peer peer, GameMessage type, Reader& in, Tick now);
    void maintain_claims(Tick now);
    void resolve_prediction(ObjectId id, bool granted);
    void finish_claim(const Claim& claim, ClaimStatus status);
    Peer peer_for(PlayerId player) const;

    friend class CheckpointCodec;
    friend class GameSession;
    PlayerId master_ = host_player;
    Transport& link_;
    const Schemas& schemas_;
    Lookup lookup_;
    Commit commit_;
    std::function<bool(ObjectId, const Values&, Tick, const Words&)> commit_words_;
    std::function<void(ObjectId, PlayerId)> spatial_writer_changed_;
    Changed changed_;
    Known known_;
    InstanceMode instance_mode_;
    RoomId room_ = 0;
    RoundId round_ = 0;
    PlayerId local_ = 0;
    Peer coordinator_ = 0;
    bool reply_pending_ = false;
    bool word_deltas_ = false;
    std::optional<Tick> word_state_probe_;
    bool state_batches_ = false;
    std::optional<Tick> state_batch_probe_;
    std::map<ObjectId, WordState> writable_words_;
    std::map<ObjectId, Words> accepted_words_;
    Words sampled_words_;
    ReceiveBuffers receive_buffers_;
    std::uint64_t submissions_ = 0, acceptances_ = 0, receipt_coalesces_ = 0;
    std::map<SchemaId, Model> models_;
    std::map<ObjectId, ObjectGrant> grants_;
    std::set<ObjectId> awaiting_object_, restoring_writer_;
    std::set<ObjectId> removed_;
    struct Display { Values values; Tick revision = 0; std::uint32_t epoch = 0; };
    std::map<ObjectId, Display> displays_;
    std::map<Peer, Participant> peers_;
    std::map<SchemaId, ClaimPolicy> policies_;
    std::function<void(const Claim&)> prompt_;
    std::function<void(const Claim&, ClaimStatus)> result_;
    struct PendingClaim { Claim claim; Tick since; };
    std::map<ObjectId, PendingClaim> pending_;
    std::map<ObjectId, Claim> outgoing_;
    struct PredictedClaim { Tick epoch; std::optional<AuthoritySample> pending; };
    std::map<ObjectId, PredictedClaim> predicted_;
    std::function<void(ObjectId)> prediction_failed_;
    Tick local_step_ = 0;
    std::set<Peer> claim_peers_;
    std::map<ObjectId, Tick> last_transfer_;
    Tick request_sequence_ = 0;
    std::optional<Tick> last_claim_probe_;
    bool claim_supported_ = false;
};

}
