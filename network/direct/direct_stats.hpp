#pragma once

#include <netcode/transport_provider.h>
#include "transport/transport.hpp"

namespace netcode {
bool direct_provider_stats(const Transport& link, Peer peer, nc_transport_stats& out);
}
