#include "state/layout.hpp"

#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace netcode {
namespace {

constexpr const char* axes_2d[] = {"x", "y", "angle", "sx", "sy"};
constexpr const char* axes_3d[] = {"x", "y", "z", "rx", "ry", "rz", "sx", "sy", "sz"};

bool valid_id(unsigned value)
{
    return value > 0 && value <= std::numeric_limits<std::uint16_t>::max();
}

}

ReplicationLayout::ReplicationLayout(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot open replication config: " + path.string());
    parse(stream);
}

ReplicationLayout::ReplicationLayout(std::istream& stream)
{
    parse(stream);
}

void ReplicationLayout::parse(std::istream& stream)
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
        const auto add = [&](unsigned field, FieldType type, const std::string& name, bool transform, unsigned limit = 0) {
            if (!valid_id(field)) throw invalid();
            if (limit > UINT16_MAX) throw invalid();
            const Field definition{static_cast<FieldId>(field), type, static_cast<std::uint16_t>(limit)};
            current.fields.push_back(definition);
            bindings_[current.id].push_back({definition, target, name, transform});
        };
        if (kind == "transform2") {
            for (const auto* axis : axes_2d) add(id++, FieldType::Float, axis, true);
        } else if (kind == "transform3") {
            for (const auto* axis : axes_3d) add(id++, FieldType::Float, axis, true);
        } else {
            static const std::map<std::string, FieldType> types{
                {"float", FieldType::Float}, {"flag", FieldType::Flag}, {"int", FieldType::Int},
                {"vec2", FieldType::Vec2}, {"vec3", FieldType::Vec3}, {"quat", FieldType::Quat},
                {"string", FieldType::String}, {"int_array", FieldType::IntList}, {"float_array", FieldType::FloatList},
                {"vec4",FieldType::Vec4},{"color",FieldType::Color4},{"object_ref",FieldType::Reference},
                {"bytes",FieldType::ByteArray},{"vec2_array",FieldType::Vec2List},{"int_map",FieldType::IntDictionary},
                {"int_list",FieldType::LinkedInts}};
            auto type = types.find(kind);
            if (type == types.end() || !(entry >> property)) throw invalid();
            unsigned limit = 0;
            if (!field_capacity({0,type->second,0}) && (!(entry >> limit) || !limit)) throw invalid();
            add(id, type->second, property, false, limit);
        }
        if (entry >> extra) throw invalid();
    }
    commit();
}

}
