#pragma once

#include "transport/transport.hpp"
#include "transport_provider.h"

#include <filesystem>
#include <string_view>

namespace netcode {

struct ProviderIdentity {
    std::string id;
    std::string build;
    std::uint32_t abi = 0;
    std::string label() const { return abi ? id + "/" + build + "/v" + std::to_string(abi) : id; }
};

class TransportProvider {
public:
    static TransportProvider load(const std::filesystem::path& module, std::string_view expected_id);
    static TransportProvider registered(nc_transport_get_api_fn entry, std::string_view expected_id);
    const ProviderIdentity& identity() const;
    std::unique_ptr<Transport> open(bool listen, std::string_view endpoint, std::string_view config = {}) const;

private:
    struct Module;
    explicit TransportProvider(std::shared_ptr<const Module> module);
    std::shared_ptr<const Module> module_;
};

}
