#include "layout.hpp"

#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace yard {
namespace {

using namespace netcode;
constexpr const char* axes_2d[] = {"x", "y", "angle", "sx", "sy"};
constexpr const char* axes_3d[] = {"x", "y", "z", "rx", "ry", "rz", "sx", "sy", "sz"};

bool valid_id(unsigned value)
{
    return value > 0 && value <= std::numeric_limits<std::uint16_t>::max();
}

}

Layout::Layout(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot open replication config: " + path.string());
    parse(stream);
}

Layout::Layout(std::istream& stream)
{
    parse(stream);
}

void Layout::parse(std::istream& stream)
{
    Schema current{};
    const auto commit = [&] {
        if (current.id && !schemas_.add(current)) {
            throw std::runtime_error("Invalid or duplicate schema " + std::to_string(current.id));
        }
    };
    unsigned line_number = 0;
    for (std::string line; std::getline(stream, line);) {
        ++line_number;
        line = line.substr(0, line.find('#'));
        std::istringstream entry(line);
        std::string kind, target, property, extra;
        unsigned id, version;
        const auto invalid = [&] {
            return std::runtime_error("Invalid replication config line " + std::to_string(line_number));
        };
        if (!(entry >> kind)) continue;
        if (kind == "schema") {
            commit();
            if (!(entry >> id >> version >> property) || (entry >> extra) ||
                !valid_id(id) || !valid_id(version)) throw invalid();
            current = {static_cast<SchemaId>(id), static_cast<std::uint16_t>(version), {}};
            continue;
        }
        if (!current.id || !(entry >> id >> target) || !valid_id(id)) throw invalid();
        const auto add = [&](unsigned field, FieldType type, const std::string& name, bool transform) {
            if (!valid_id(field)) throw invalid();
            const Field definition{static_cast<FieldId>(field), type};
            current.fields.push_back(definition);
            bindings_[current.id].push_back({definition, target, name, transform});
        };
        if (kind == "transform2") {
            for (const auto* axis : axes_2d) add(id++, FieldType::Float, axis, true);
        } else if (kind == "transform3") {
            for (const auto* axis : axes_3d) add(id++, FieldType::Float, axis, true);
        } else if ((kind == "float" || kind == "flag") && (entry >> property)) {
            add(id, kind == "float" ? FieldType::Float : FieldType::Flag, property, false);
        } else {
            throw invalid();
        }
        if (entry >> extra) throw invalid();
    }
    commit();
    const std::map<SchemaId, Values> expected{
        {Car, {0.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f}},
        {Gate, {0.f, 0.f, 0.f, 1.f, 1.f, false}},
        {Beacon, {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 0.f}},
        {Barrier, {0.f, 0.f, 0.f, 1.f, 1.f, 0.f}},
        {Load, {0.f, 0.f, 0.f, 1.f, 1.f, 0.f}},
    };
    for (const auto& [id, defaults] : expected) {
        if (!schemas_.valid(id, defaults)) throw std::runtime_error("Incompatible yard schema " + std::to_string(id));
    }
}

}
