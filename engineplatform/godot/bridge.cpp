#include "bridge.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>

#include <sstream>

using namespace godot;

void NetcodeYard::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("configure", "arguments", "config"), &NetcodeYard::configure);
    ClassDB::bind_method(D_METHOD("settings"), &NetcodeYard::settings);
    ClassDB::bind_method(D_METHOD("bindings"), &NetcodeYard::bindings);
    ClassDB::bind_method(D_METHOD("advance"), &NetcodeYard::advance);
    ClassDB::bind_method(D_METHOD("drive", "throttle", "steer"), &NetcodeYard::drive);
    ClassDB::bind_method(D_METHOD("act", "action"), &NetcodeYard::act);
}

String NetcodeYard::configure(const PackedStringArray& arguments, const String& config)
{
    if (!settings_.is_empty()) return "NetcodeYard has already been configured";
    try {
        std::vector<std::string> args{"netcode-godot"};
        for (int64_t i = 0; i < arguments.size(); ++i) {
            if (arguments[i] == "--assets") return "--assets is set by the Godot project";
            args.emplace_back(arguments[i].utf8().get_data());
        }
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        const auto options = yard::parse_options(static_cast<int>(argv.size()), argv.data());
        std::istringstream input(config.utf8().get_data());
        yard::Layout layout(input);
        Dictionary descriptions;
        for (const auto schema : {yard::Car, yard::Gate, yard::Beacon, yard::Barrier, yard::Load}) {
            Array fields;
            for (const auto& binding : layout.bindings(schema)) {
                Dictionary field;
                field["id"] = binding.field.id;
                field["path"] = String::utf8(binding.path.c_str());
                field["property"] = String::utf8(binding.property.c_str());
                field["transform"] = binding.transform;
                fields.push_back(field);
            }
            descriptions[static_cast<int>(schema)] = fields;
        }
        if (!options.smoke && !options.binding_smoke) {
            runner_ = std::make_unique<yard::Runner>(options, std::move(layout));
        }
        bindings_ = descriptions;
        settings_["host"] = options.host;
        settings_["smoke"] = options.smoke;
        settings_["binding_smoke"] = options.binding_smoke;
        settings_["trace"] = options.trace;
        settings_["scripted"] = options.scenario != yard::Scenario::None;
        settings_["capture"] = String(options.capture.c_str());
        return {};
    } catch (const std::exception& error) {
        return String::utf8(error.what());
    }
}

Dictionary NetcodeYard::advance()
{
    Dictionary frame;
    if (!runner_) {
        frame["error"] = "NetcodeYard has no active session";
        return frame;
    }
    try {
        runner_->update(netcode::Clock::now());
        const auto& match = runner_->match();
        frame["status"] = runner_->status();
        frame["phase"] = static_cast<int>(match.phase());
        frame["round"] = static_cast<int64_t>(match.round());
        frame["player"] = match.player();
        frame["car"] = match.car();
        frame["tick"] = match.tick();
        frame["seats"] = static_cast<int>(match.seats());
        frame["ready"] = match.ready();
        frame["reject"] = static_cast<int>(match.rejected());
        frame["loss"] = static_cast<int>(match.loss());
        frame["notice"] = String::utf8(match.notice().c_str());
        Array objects;
        for (const auto id : match.objects()) {
            const auto& object = *match.object(id);
            Dictionary snapshot;
            snapshot["id"] = object.id;
            snapshot["schema"] = object.schema;
            snapshot["owner"] = object.owner;
            snapshot["revision"] = object.revision;
            Array values;
            for (const auto& value : object.values) {
                if (const auto* f = std::get_if<float>(&value)) values.push_back(*f);
                else values.push_back(std::get<bool>(value));
            }
            snapshot["values"] = values;
            objects.push_back(snapshot);
        }
        frame["objects"] = objects;
    } catch (const std::exception& error) {
        frame["error"] = String::utf8(error.what());
    }
    return frame;
}

void NetcodeYard::drive(double throttle, double steer)
{
    if (runner_) runner_->match().set_input({static_cast<float>(throttle), static_cast<float>(steer)});
}

bool NetcodeYard::act(const String& action)
{
    if (!runner_) return false;
    auto& match = runner_->match();
    if (action == "start") return match.start();
    if (action == "gate") return match.open_gate();
    if (action == "place") return match.barrier(true);
    if (action == "remove") return match.barrier(false);
    if (action == "horn") return match.horn();
    if (action == "end_round") match.end_round();
    else if (action == "end_room") match.end_room();
    else if (action == "disconnect") match.disconnect();
    else if (action == "rejoin") runner_->rejoin();
    else return false;
    return true;
}
