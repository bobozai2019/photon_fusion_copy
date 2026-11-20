#pragma once

#include "state/replication.hpp"

#include <filesystem>
#include <iosfwd>
#include <string>

namespace yard {

enum Template : netcode::SchemaId { Car = 1, Gate, Beacon, Barrier, Load };

struct FieldBinding {
    netcode::Field field;
    std::string path;
    std::string property;
    bool transform;
};

class Layout {
public:
    explicit Layout(const std::filesystem::path& path);
    explicit Layout(std::istream& stream);
    const netcode::Schemas& schemas() const { return schemas_; }
    const std::vector<FieldBinding>& bindings(netcode::SchemaId id) const { return bindings_.at(id); }

private:
    void parse(std::istream& stream);
    netcode::Schemas schemas_;
    std::map<netcode::SchemaId, std::vector<FieldBinding>> bindings_;
};

}
