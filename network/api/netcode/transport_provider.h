#ifndef NETCODE_TRANSPORT_PROVIDER_H
#define NETCODE_TRANSPORT_PROVIDER_H

#include <stdint.h>

#if defined(_WIN32)
#define NC_TRANSPORT_CALL __cdecl
#if defined(NC_TRANSPORT_BUILD_DLL)
#define NC_TRANSPORT_EXPORT __declspec(dllexport)
#else
#define NC_TRANSPORT_EXPORT
#endif
#else
#define NC_TRANSPORT_CALL
#if defined(NC_TRANSPORT_BUILD_DLL)
#define NC_TRANSPORT_EXPORT __attribute__((visibility("default")))
#else
#define NC_TRANSPORT_EXPORT
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define NC_TRANSPORT_ABI_V1 1u
#define NC_TRANSPORT_ENTRY "netcode_transport_get_api"
#define NC_TRANSPORT_CONTROL_MAX 65535u
#define NC_TRANSPORT_GAME_MAX 1176u
#define NC_TRANSPORT_ALL_PEERS UINT64_C(0)

typedef struct nc_transport_instance nc_transport_instance;
typedef uint64_t nc_transport_peer;
typedef uint32_t nc_transport_result;

enum {
    NC_TRANSPORT_OK = 0,
    NC_TRANSPORT_WOULD_BLOCK = 1,
    NC_TRANSPORT_PEER_CLOSED = 2,
    NC_TRANSPORT_TOO_LARGE = 3,
    NC_TRANSPORT_INVALID_CONFIG = 4,
    NC_TRANSPORT_UNSUPPORTED_ABI = 5,
    NC_TRANSPORT_IO_ERROR = 6,
    NC_TRANSPORT_INVALID_ARGUMENT = 7,
    NC_TRANSPORT_SHORT_TABLE = 8
};

enum {
    NC_TRANSPORT_LISTEN = 1,
    NC_TRANSPORT_CONNECT = 2
};

enum {
    NC_TRANSPORT_CONTROL = 0,
    NC_TRANSPORT_GAME = 1,
    NC_TRANSPORT_LANE_COUNT = 2
};

enum {
    NC_TRANSPORT_OPEN = 1,
    NC_TRANSPORT_MESSAGE = 2,
    NC_TRANSPORT_CLOSED = 3
};

enum {
    NC_TRANSPORT_CLOSE_NONE = 0,
    NC_TRANSPORT_CLOSE_LOCAL = 1,
    NC_TRANSPORT_CLOSE_REMOTE = 2,
    NC_TRANSPORT_CLOSE_TIMEOUT = 3,
    NC_TRANSPORT_CLOSE_IO_ERROR = 4,
    NC_TRANSPORT_CLOSE_PROTOCOL = 5,
    NC_TRANSPORT_CLOSE_REFUSED = 6
};

enum {
    NC_TRANSPORT_RELIABLE_ORDERED = 1u
};

typedef struct nc_transport_bytes {
    const uint8_t *data;
    uint32_t size;
} nc_transport_bytes;

typedef struct nc_transport_open_args {
    uint32_t role;
    nc_transport_bytes endpoint;
    nc_transport_bytes config;
} nc_transport_open_args;

typedef struct nc_transport_event {
    uint32_t type;
    uint32_t lane;
    nc_transport_peer peer;
    nc_transport_bytes payload;
    uint32_t reason;
} nc_transport_event;

typedef struct nc_transport_lane_caps {
    uint32_t max_payload;
    uint32_t flags;
} nc_transport_lane_caps;

typedef struct nc_transport_caps {
    nc_transport_lane_caps lane[NC_TRANSPORT_LANE_COUNT];
} nc_transport_caps;

typedef struct nc_transport_lane_stats {
    uint64_t sent_bytes;
    uint64_t received_bytes;
    uint64_t queued_bytes;
    uint64_t peak_queued_bytes;
    uint64_t oldest_queued_us;
} nc_transport_lane_stats;

typedef struct nc_transport_stats {
    nc_transport_lane_stats lane[NC_TRANSPORT_LANE_COUNT];
} nc_transport_stats;

/* Functions on one instance are serialized by the host. Callbacks are synchronous
 * within poll and may not reenter the instance. No exceptions cross this boundary.
 * Byte spans with size zero may use NULL. Event data expires on callback return. */
typedef void (NC_TRANSPORT_CALL *nc_transport_receive)(void *context, const nc_transport_event *event);

typedef struct nc_transport_api_v1 {
    uint32_t abi_version;
    uint32_t struct_size;
    nc_transport_bytes provider_id;
    nc_transport_bytes build_id;

    /* Arguments expire on return. On failure *instance is NULL. Success starts
     * the service. Completion or asynchronous failure is delivered via poll. */
    nc_transport_result (NC_TRANSPORT_CALL *open)(const nc_transport_open_args *args,
                                                 nc_transport_instance **instance);
    /* Copies the accepted prefix before returning. Always writes *accepted.
     * OK accepts the entire batch. WOULD_BLOCK permits a partially accepted
     * prefix. Other errors leave the whole batch unaccepted. */
    nc_transport_result (NC_TRANSPORT_CALL *send_batch)(nc_transport_instance *instance,
        nc_transport_peer peer, uint32_t lane, const nc_transport_bytes *messages,
        uint32_t count, uint32_t *accepted);
    /* now_us is a nondecreasing monotonic clock in microseconds. The callback
     * runs at most event_budget times, including zero if the budget is zero. */
    nc_transport_result (NC_TRANSPORT_CALL *poll)(nc_transport_instance *instance,
        uint64_t now_us, uint32_t event_budget, nc_transport_receive receive, void *context);
    /* Stops accepting sends for this peer and reports one CLOSED in a later poll.
     * Queue draining is provider specific. destroy invalidates all peers. */
    nc_transport_result (NC_TRANSPORT_CALL *close_peer)(nc_transport_instance *instance, nc_transport_peer peer);
    void (NC_TRANSPORT_CALL *destroy)(nc_transport_instance *instance);
    nc_transport_result (NC_TRANSPORT_CALL *get_caps)(nc_transport_instance *instance, nc_transport_caps *out);
    /* Bytes count complete business payloads, queue age uses the last poll clock.
     * Peer zero requests instance totals, including bytes on retired peers. */
    nc_transport_result (NC_TRANSPORT_CALL *get_stats)(nc_transport_instance *instance,
                                                      nc_transport_peer peer, nc_transport_stats *out);
} nc_transport_api_v1;

/* Use the platform's default C layout. The caller supplies suitably aligned
 * storage. A v1 provider writes at most out_capacity bytes, size reports the
 * initialized prefix, and both identifiers survive until module unload. */
typedef nc_transport_result (NC_TRANSPORT_CALL *nc_transport_get_api_fn)(uint32_t abi_version,
                                                                        uint32_t out_capacity, void *out);
NC_TRANSPORT_EXPORT nc_transport_result NC_TRANSPORT_CALL
netcode_transport_get_api(uint32_t abi_version, uint32_t out_capacity, void *out);

#ifdef __cplusplus
}
#endif
#endif
