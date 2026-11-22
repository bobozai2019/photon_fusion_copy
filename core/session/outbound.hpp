#pragma once

#include "transport/transport.hpp"

#include <deque>
#include <map>
#include <optional>

namespace netcode {

class Outbound final : public Transport {
public:
    explicit Outbound(Transport& link);
    explicit Outbound(std::unique_ptr<Transport> link);
    bool send(Peer peer, Lane lane, const Bytes& payload) override;
    void send_messages(Peer peer, Lane lane, std::vector<Bytes> messages);
    void close(Peer peer) override;
    std::vector<Event> poll(TimePoint now) override;
    Traffic traffic() const override;
    bool reliable_ordered(Lane lane) const override { return link_.reliable_ordered(lane); }
    void flush(TimePoint now);

    // The fixed step can collect multiple submissions before the next flush.
    class Collect {
    public:
        explicit Collect(Outbound& out) : out_(out) { ++out_.collecting_; }
        ~Collect() { --out_.collecting_; }
        Collect(const Collect&) = delete;
        Collect& operator=(const Collect&) = delete;
    private:
        Outbound& out_;
    };

private:
    struct Message {
        Lane lane;
        Bytes payload;
        TimePoint queued_at;
    };
    struct Stream {
        std::deque<Message> queued;
        std::size_t bytes = 0;
        std::optional<TimePoint> close_started;
    };
    bool can_queue(const Stream& stream, Lane lane, std::size_t size) const;
    void queue(Stream& stream, Lane lane, Bytes payload);
    void submit(Peer peer, Stream& stream);
    void discard(Stream& stream);

    std::unique_ptr<Transport> owned_;
    Transport& link_;
    const bool backpressure_;
    unsigned collecting_ = 0;
    TimePoint now_{};
    std::map<Peer, Stream> peers_;
    std::size_t bytes_ = 0;
    std::size_t peak_ = 0;
};

}
