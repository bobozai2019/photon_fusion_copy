#include <netcode/transport_provider.h>

#include <stdlib.h>
#include <string.h>

typedef struct frame {
    uint8_t data[NC_TRANSPORT_CONTROL_MAX];
    uint32_t size;
    uint32_t lane;
    uint64_t queued_at;
} frame;

struct nc_transport_instance {
    int opened;
    int closing;
    int closed;
    int pending;
    uint64_t now;
    frame item;
    nc_transport_stats stats;
};

static nc_transport_result NC_TRANSPORT_CALL open_fixture(const nc_transport_open_args *args,
                                                          nc_transport_instance **out) {
    if (!out) return NC_TRANSPORT_INVALID_ARGUMENT;
    *out = NULL;
    if (!args || args->role != NC_TRANSPORT_CONNECT || args->endpoint.size != 4 ||
        !args->endpoint.data || memcmp(args->endpoint.data, "echo", 4) || args->config.size)
        return NC_TRANSPORT_INVALID_CONFIG;
    *out = calloc(1, sizeof **out);
    return *out ? NC_TRANSPORT_OK : NC_TRANSPORT_IO_ERROR;
}

static nc_transport_result NC_TRANSPORT_CALL send_fixture(nc_transport_instance *instance,
        nc_transport_peer peer, uint32_t lane, const nc_transport_bytes *messages,
        uint32_t count, uint32_t *accepted) {
    uint32_t i;
    uint32_t max_payload;
    nc_transport_lane_stats *stats;
    if (!accepted) return NC_TRANSPORT_INVALID_ARGUMENT;
    *accepted = 0;
    if (!instance || lane >= NC_TRANSPORT_LANE_COUNT || (count && !messages))
        return NC_TRANSPORT_INVALID_ARGUMENT;
    if (!instance->opened || instance->closing || peer != 1) return NC_TRANSPORT_PEER_CLOSED;
    max_payload = lane == NC_TRANSPORT_CONTROL ? NC_TRANSPORT_CONTROL_MAX : NC_TRANSPORT_GAME_MAX;
    for (i = 0; i < count; ++i) {
        if (messages[i].size > max_payload) return NC_TRANSPORT_TOO_LARGE;
        if (messages[i].size && !messages[i].data) return NC_TRANSPORT_INVALID_ARGUMENT;
    }
    if (!count) return NC_TRANSPORT_OK;
    if (instance->pending) return NC_TRANSPORT_WOULD_BLOCK;
    instance->item.size = messages[0].size;
    instance->item.lane = lane;
    instance->item.queued_at = instance->now;
    if (messages[0].size) memcpy(instance->item.data, messages[0].data, messages[0].size);
    instance->pending = 1;
    stats = &instance->stats.lane[lane];
    stats->queued_bytes = instance->item.size;
    if (stats->peak_queued_bytes < stats->queued_bytes) stats->peak_queued_bytes = stats->queued_bytes;
    *accepted = 1;
    return count == 1 ? NC_TRANSPORT_OK : NC_TRANSPORT_WOULD_BLOCK;
}

static nc_transport_result NC_TRANSPORT_CALL poll_fixture(nc_transport_instance *instance, uint64_t now,
        uint32_t budget, nc_transport_receive receive, void *context) {
    nc_transport_event event = {0};
    if (!instance || !receive || now < instance->now) return NC_TRANSPORT_INVALID_ARGUMENT;
    instance->now = now;
    event.peer = 1;
    if (budget && !instance->opened) {
        instance->opened = 1;
        event.type = NC_TRANSPORT_OPEN;
        receive(context, &event);
        --budget;
    }
    if (budget && instance->pending) {
        nc_transport_lane_stats *stats = &instance->stats.lane[instance->item.lane];
        event.type = NC_TRANSPORT_MESSAGE;
        event.lane = instance->item.lane;
        event.payload.data = instance->item.data;
        event.payload.size = instance->item.size;
        receive(context, &event);
        memset(instance->item.data, 0, instance->item.size);
        stats->sent_bytes += event.payload.size;
        stats->received_bytes += event.payload.size;
        stats->queued_bytes = 0;
        instance->pending = 0;
        --budget;
    }
    if (budget && instance->closing && !instance->closed) {
        instance->closed = 1;
        memset(&event, 0, sizeof event);
        event.peer = 1;
        event.type = NC_TRANSPORT_CLOSED;
        event.reason = NC_TRANSPORT_CLOSE_LOCAL;
        receive(context, &event);
    }
    return NC_TRANSPORT_OK;
}

static nc_transport_result NC_TRANSPORT_CALL close_fixture(nc_transport_instance *instance,
                                                          nc_transport_peer peer) {
    if (!instance) return NC_TRANSPORT_INVALID_ARGUMENT;
    if (!instance->opened || instance->closing || peer != 1) return NC_TRANSPORT_PEER_CLOSED;
    instance->closing = 1;
    return NC_TRANSPORT_OK;
}

static void NC_TRANSPORT_CALL destroy_fixture(nc_transport_instance *instance) { free(instance); }

static nc_transport_result NC_TRANSPORT_CALL caps_fixture(nc_transport_instance *instance,
                                                         nc_transport_caps *out) {
    if (!instance || !out) return NC_TRANSPORT_INVALID_ARGUMENT;
    out->lane[NC_TRANSPORT_CONTROL] = (nc_transport_lane_caps){NC_TRANSPORT_CONTROL_MAX, NC_TRANSPORT_RELIABLE_ORDERED};
    out->lane[NC_TRANSPORT_GAME] = (nc_transport_lane_caps){NC_TRANSPORT_GAME_MAX, 0};
    return NC_TRANSPORT_OK;
}

static nc_transport_result NC_TRANSPORT_CALL stats_fixture(nc_transport_instance *instance,
        nc_transport_peer peer, nc_transport_stats *out) {
    if (!instance || !out) return NC_TRANSPORT_INVALID_ARGUMENT;
    if (peer && (peer != 1 || !instance->opened || instance->closed)) return NC_TRANSPORT_PEER_CLOSED;
    *out = instance->stats;
    if (instance->pending) out->lane[instance->item.lane].oldest_queued_us = instance->now - instance->item.queued_at;
    return NC_TRANSPORT_OK;
}

nc_transport_result NC_TRANSPORT_CALL netcode_transport_get_api(uint32_t abi, uint32_t capacity, void *out) {
    static const uint8_t provider[] = "fixture";
    static const uint8_t build[] = "c-echo-v1";
    static const nc_transport_api_v1 api = {
        NC_TRANSPORT_ABI_V1, sizeof api, {provider, sizeof provider - 1}, {build, sizeof build - 1},
        open_fixture, send_fixture, poll_fixture, close_fixture, destroy_fixture, caps_fixture, stats_fixture
    };
    if (abi != NC_TRANSPORT_ABI_V1) return NC_TRANSPORT_UNSUPPORTED_ABI;
    if (!out) return NC_TRANSPORT_INVALID_ARGUMENT;
    if (capacity < sizeof api) return NC_TRANSPORT_SHORT_TABLE;
    memcpy(out, &api, sizeof api);
    return NC_TRANSPORT_OK;
}
