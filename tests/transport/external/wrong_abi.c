#include <netcode/transport_provider.h>

nc_transport_result NC_TRANSPORT_CALL netcode_transport_get_api(uint32_t version, uint32_t capacity, void *out) {
    (void)version;
    (void)capacity;
    (void)out;
    return NC_TRANSPORT_UNSUPPORTED_ABI;
}
