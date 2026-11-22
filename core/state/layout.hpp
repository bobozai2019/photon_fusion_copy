#pragma once

#include "state/replication.hpp"

#include <filesystem>
#include <iosfwd>
#include <string>

namespace netcode {

struct FieldBinding {
    Field field;
    std::string path;
    std::string property;
    bool transform;
};

class ReplicationLayout {
public:
    explicit ReplicationLayout(const std::filesystem::path& path);
    explicit ReplicationLayout(std::istream& stream);
    const Schemas& schemas() const { return schemas_; }
    const std::vector<FieldBinding>& bindings(SchemaId id) const { return bindings_.at(id); }

private:
    void parse(std::istream& stream);
    Schemas schemas_;
    std::map<SchemaId, std::vector<FieldBinding>> bindings_;
};

}
