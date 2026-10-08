#include "prusa_project.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "fatcat/project_settings.h"

namespace fatcat::detail::prusa {
namespace {
using json = nlohmann::json;
constexpr const char *version = "3.0.0-alpha12";
constexpr const char *project_path = "Metadata/PrusaSlicer3_project.json";

[[noreturn]] void invalid(const std::string &message) {
    throw ProjectSettingsError("PrusaSlicer 3: " + message);
}

json read_catalog(const std::filesystem::path &root) {
    std::ifstream file(root / "prusa-3/catalog.json", std::ios::binary);
    if (!file) invalid("bundled native configuration catalog is missing");
    auto value = json::parse(file);
    if (value.at("application_version") != version) invalid("catalog version differs from target");
    return value;
}

const json &source_row(const json &data, const json &request) {
    const auto machine = request.at("machine_uid");
    const auto nozzle = request.at("nozzle_uid");
    for (const auto &source : data.at("sources")) {
        if (source.at("machine_uid") == machine && source.at("nozzle_uid") == nozzle) return source;
    }
    invalid("no native configuration for selected machine/nozzle");
}

json source_config(const json &data, const json &row) {
    const auto &sources = data.at("sources");
    const auto parent = row.value("config_parent", json(nullptr));
    json base = data.at("base");
    if (!parent.is_null()) {
        const auto index = parent.get<std::size_t>();
        const auto current = std::find(sources.begin(), sources.end(), row);
        if (current == sources.end() || index >= static_cast<std::size_t>(std::distance(sources.begin(), current))) invalid("native configuration parent must precede its child");
        base = source_config(data, sources.at(index));
    }
    return base.patch(row.at("config_patch"));
}

json &container(json &project) { return project.at("config_containers").at(0); }
const json &container(const json &project) { return project.at("config_containers").at(0); }

std::string decimal(double value) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << value;
    return text.str();
}

double number(const json &value) {
    if (value.is_object()) return number(value.at("value"));
    if (value.is_array()) {
        if (value.empty()) invalid("empty numeric array");
        return number(value.front());
    }
    if (value.is_number()) {
        const auto numeric = value.get<double>();
        if (std::isfinite(numeric)) return numeric;
        invalid("expected finite numeric value");
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        try {
            const auto parsed = json::parse(text);
            if (parsed.is_number() && std::isfinite(parsed.get<double>())) return parsed.get<double>();
        } catch (const json::exception &) {}
    }
    invalid("expected finite numeric value");
}

json scalar(json value, const json &definition) {
    const auto type = definition.at("type").get<std::string>();
    if (type == "OptInt") {
        if (value.is_null()) return value;
        auto integer = definition;
        integer["type"] = "Int";
        return scalar(std::move(value), integer);
    }
    if (type == "FloatOrPercentage" || type == "Percentage") {
        bool percent = type == "Percentage";
        if (value.is_string()) {
            auto text = value.get<std::string>();
            if (!text.empty() && text.back() == '%') {
                percent = true;
                text.pop_back();
            }
            value = json::parse(text);
        }
        if (!value.is_object()) value = {{"value", number(value)}, {"is_percent", percent}};
        if (value.size() != 2 || !value.contains("is_percent") || !value.at("is_percent").is_boolean()) {
            invalid("percentage values require value and is_percent");
        }
        if (type == "Percentage" && !value.at("is_percent").get<bool>()) invalid("percentage requires is_percent=true");
        value["value"] = number(value.at("value"));
    } else if (type == "Bool") {
        if (value == "1" || value == "true" || value == 1) value = true;
        if (value == "0" || value == "false" || value == 0) value = false;
        if (!value.is_boolean()) invalid("boolean override requires true/false or 1/0");
    } else if (type == "Float" || type == "Int") {
        const auto numeric = number(value);
        if (!std::isfinite(numeric)) invalid("override must be finite");
        if (type == "Int" && std::floor(numeric) != numeric) invalid("integer override is fractional");
        if (type == "Int" && (numeric < static_cast<double>(std::numeric_limits<long long>::min()) ||
            numeric >= static_cast<double>(std::numeric_limits<long long>::max()))) invalid("integer override is too large");
        value = type == "Int" ? json(static_cast<long long>(numeric)) : json(numeric);
    } else if (type == "Enum") {
        const auto &values = definition.at("enum_values");
        if (std::find(values.begin(), values.end(), value) == values.end()) invalid("unknown native enum value: " + value.dump());
    } else if (type == "String") {
        if (!value.is_string()) invalid("string override requires text");
    } else {
        invalid("this override type is not yet supported: " + type);
    }
    if (type == "Int" || type == "Float" || type == "Percentage" || type == "FloatOrPercentage") {
        const auto numeric = number(value);
        for (const auto *bound : {"min", "max"}) {
            if (definition.contains(bound) && !definition.at(bound).is_null()) {
                const auto limit = definition.at(bound).get<double>();
                if ((std::string(bound) == "min" && numeric < limit) ||
                    (std::string(bound) == "max" && numeric > limit)) invalid("override is outside native schema bounds");
            }
        }
    }
    return value;
}

void set_process(json &config, const std::string &key, const json &value, const json &target) {
    const auto &contract = target.at("process_settings_contract");
    const auto native = contract.at("aliases").value(key, key);
    if (!contract.at("fields").contains(native)) invalid("unsupported process override '" + key + "' [sdk_not_supported]");
    const auto &definition = contract.at("fields").at(native);
    const auto parsed = scalar(value, definition);
    config.at("print_settings")[native] = parsed;
    const auto &scopes = definition.at("overrides_in");
    if (std::find(scopes.begin(), scopes.end(), "Tool") != scopes.end()) {
        auto &tools = config.at("toolprint_settings");
        if (!tools.empty()) {
            std::size_t count = tools.begin().value().size();
            tools[native] = json::array();
            for (std::size_t i = 0; i < count; ++i) tools[native].push_back(parsed);
        }
    }
}

void check_identity(const json &request, const json &target) {
    const auto &identity = target.at("target_contract");
    if (request.at("slicer_id") != identity.at("slicer_id") ||
        request.at("application_version") != identity.at("application_version")) invalid("request slicer/version differs from target");
}

void check_presets(const json &box, const json &request) {
    if (request.contains("native_print_profile_name") &&
        request.at("native_print_profile_name") != box.at("preset").at("print").at("name")) invalid("process preset differs from native configuration; select it in PrusaSlicer and export with --save");
    if (!request.contains("native_filament_profile_names")) return;
    const auto &names = request.at("native_filament_profile_names");
    const auto &materials = box.at("preset").at("materials");
    if (!names.is_array() || names.empty() || (names.size() != 1 && names.size() != materials.size())) invalid("native material names must select one preset for all slots or match native capacity");
    for (std::size_t i = 0; i < materials.size(); ++i) {
        if (names.at(names.size() == 1 ? 0 : i) != materials.at(i).at("name")) invalid("material preset differs from native configuration; export the selected native material config");
    }
}

std::string hardware_uid(const json &hardware) {
    std::string uid = "prusa:";
    bool separator = false;
    for (const unsigned char c : hardware.at("model").get<std::string>()) {
        if (std::isalnum(c)) {
            if (separator) uid += '-';
            uid += static_cast<char>(std::tolower(c));
            separator = false;
        } else separator = true;
    }
    const auto tools = hardware.at("tool_count").get<int>();
    if (tools > 1) uid += "-" + std::to_string(tools) + "t";
    const auto &first = hardware.at("tools").at("0");
    if (first.contains("feeder")) {
        auto feeder = first.at("feeder").at("model").get<std::string>();
        std::transform(feeder.begin(), feeder.end(), feeder.begin(), [](unsigned char c) {
            return c == '_' ? '-' : static_cast<char>(std::tolower(c));
        });
        uid += "-" + feeder;
    }
    return uid;
}

std::vector<std::string> material_keys(const json &hardware) {
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < hardware.at("tool_count").get<std::size_t>(); ++i) {
        const auto id = std::to_string(i);
        const auto &tool = hardware.at("tools").at(id);
        const auto count = tool.contains("feeder") ? tool.at("feeder").at("slot_count").get<std::size_t>() : 1;
        for (std::size_t slot = 0; slot < count; ++slot) {
            keys.push_back(tool.contains("feeder") ? id + "." + std::to_string(slot) : id);
        }
    }
    return keys;
}

void apply_materials(json &project, const json &request, const json &target) {
    auto &source = container(project);
    auto &materials = source.at("preset").at("materials");
    auto &filaments = source.at("configuration").at("filament_settings");
    auto &colours = source.at("configuration").at("project_settings")["extruder_colour"];
    if (!colours.is_array() || colours.size() != materials.size()) {
        colours = json::array();
        for (std::size_t i = 0; i < materials.size(); ++i) colours.push_back("#FF8000");
    }
    const auto palette = request.value("source_materials", json::array());
    const auto color_request = request.value("filament_colour", json::array());
    if (!palette.is_array() || !color_request.is_array()) invalid("material palette and colours must be arrays");
    if (!palette.empty() && !color_request.empty() && palette.size() != color_request.size()) invalid("material palette and colour arrays differ in length");
    const auto count = std::max(palette.size(), color_request.size());
    if (count > materials.size()) invalid("selected materials exceed this hardware's native material slots; select an MMU/multi-tool configuration");
    const auto slots = request.value("filament_source_slots", json::array());
    if (!slots.is_array() || (!slots.empty() && slots.size() != count)) invalid("source slot mapping must match requested material count");
    const auto original = filaments;
    const auto original_materials = materials;
    const auto original_colours = colours;
    auto &hardware = source.at("preset").at("hw_config");
    const auto keys = material_keys(hardware);
    if (keys.size() != materials.size()) invalid("native material slots differ from physical tool/feeder capacity");
    const auto original_tools = hardware.at("tools");
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t from = i;
        if (i < slots.size() && !slots.at(i).is_null()) {
            if (!slots.at(i).is_number_integer() || slots.at(i).get<long long>() < 0) invalid("source material slot must be a nonnegative integer");
            from = slots.at(i).get<std::size_t>();
        }
        if (from >= materials.size()) invalid("source material slot is out of range");
        const auto &tools = hardware.at("tools");
        if (hardware.at("tool_count").get<std::size_t>() > 1) {
            if (materials.size() != hardware.at("tool_count").get<std::size_t>()) invalid("combined multi-tool feeder slot remapping is not yet supported");
            if (tools.at(std::to_string(i)).at("features") != tools.at(std::to_string(from)).at("features")) invalid("material remapping between different physical tool features requires a native configuration export");
        }
        materials.at(i) = original_materials.at(from);
        for (const auto *key : {"slicer_material", "material_package_instance"}) {
            const auto &source_tool = original_tools.at(keys.at(from));
            if (source_tool.contains(key)) hardware.at("tools").at(keys.at(i))[key] = source_tool.at(key);
        }
        for (auto &[key, values] : filaments.items()) {
            if (values.is_array() && values.size() == materials.size()) values.at(i) = original.at(key).at(from);
        }
        auto colour = i < color_request.size() ? color_request.at(i) : original_colours.at(from);
        if (i < palette.size()) {
            const auto &material = palette.at(i);
            if (!material.is_object()) invalid("each source material must be an object");
            colour = material.value("colour", colour);
            if (material.contains("material_type") && !material.at("material_type").is_null()) {
                const auto requested = material.at("material_type").get<std::string>();
                const auto type = target.at("material_settings_contract").value("type_aliases", json::object()).value(requested, requested);
                if (type != filaments.at("filament_type").at(i)) invalid("material type differs from native preset; provide a config exported with that material");
            }
            if (material.contains("native_settings")) {
                if (!material.at("native_settings").is_object()) invalid("per-slot native_settings must be an object");
                for (const auto &[key, value] : material.at("native_settings").items()) {
                    if (key == "filament_colour") {
                        if ((material.contains("colour") || i < color_request.size()) && value != colour) invalid("conflicting native filament_colour and palette colour");
                        colour = value;
                    }
                    if (key == "filament_type" && value != filaments.at(key).at(i)) invalid("material type override differs from native preset; use an exported native config");
                    const auto &fields = target.at("material_settings_contract").at("fields");
                    if (!fields.contains(key) || !filaments.contains(key)) invalid("unsupported material override '" + key + "'");
                    filaments.at(key).at(i) = scalar(value, fields.at(key));
                }
            }
        }
        if (!colour.is_string() || colour.get<std::string>().size() != 7 || colour.get<std::string>()[0] != '#') invalid("material colour must be #RRGGBB");
        if (colour.get<std::string>().find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos) invalid("material colour contains non-hex digits");
        colours.at(i) = colour;
        filaments.at("filament_colour").at(i) = colour;
    }
    if (request.contains("source_slot_colours")) {
        for (const auto &[key, colour] : request.at("source_slot_colours").items()) {
            const auto parsed = json::parse(key);
            if (!parsed.is_number_unsigned()) invalid("source colour slot must be a nonnegative integer");
            const auto index = parsed.get<std::size_t>();
            if (index >= materials.size()) invalid("source colour slot is out of range");
            if (index < count) continue; // Explicit output palette is authoritative.
            if (!colour.is_string() || colour.get<std::string>().size() != 7 ||
                colour.get<std::string>()[0] != '#' ||
                colour.get<std::string>().find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos) invalid("source colour must be #RRGGBB");
            colours.at(index) = colour;
            filaments.at("filament_colour").at(index) = colour;
        }
    }
}

json hardware_identity(json hardware) {
    // Configuration UUIDs and material assignments are not physical hardware.
    hardware.erase("config_id");
    for (auto &[key, tool] : hardware.at("tools").items()) {
        tool.erase("slicer_material");
        tool.erase("material_package_instance");
    }
    return hardware;
}

std::size_t slot_index(const json &value) {
    if (!value.is_number_integer() || value.get<long long>() < 0) invalid("slot index must be a nonnegative integer");
    return value.get<std::size_t>();
}

json material_snapshot(const json &box, std::size_t index) {
    const auto keys = material_keys(box.at("preset").at("hw_config"));
    if (index >= keys.size()) invalid("source material index exceeds native hardware capacity");
    json values = json::object();
    for (const auto &[key, rows] : box.at("configuration").at("filament_settings").items()) {
        if (!rows.is_array() || rows.size() != keys.size()) invalid("native filament field has an inconsistent slot count: " + key);
        values[key] = rows.at(index);
    }
    const auto &tool = box.at("preset").at("hw_config").at("tools").at(keys.at(index));
    const auto physical = keys.at(index).substr(0, keys.at(index).find('.'));
    return {{"preset", box.at("preset").at("materials").at(index)}, {"values", values},
        {"tool_features", box.at("preset").at("hw_config").at("tools").at(physical).at("features")},
        {"slicer_material", tool.value("slicer_material", json(nullptr))},
        {"material_package_instance", tool.value("material_package_instance", json(nullptr))}};
}

json object_process_overrides(const json &source, const json &destination, const json &target) {
    json result = json::object();
    const auto &fields = target.at("process_settings_contract").at("fields");
    for (const auto *group : {"print_settings", "toolprint_settings"}) {
        const auto &original = container(source).at("configuration").at(group);
        const auto &output = container(destination).at("configuration").at(group);
        for (const auto &[key, rows] : original.items()) {
            // The final merged bed owns tower width/position, not each object.
            if (key == "wipe_tower_width") continue;
            if (output.contains(key) && output.at(key) == rows) continue;
            if (!fields.contains(key)) invalid("merged process difference cannot be represented per object: " + key);
            const auto &scopes = fields.at(key).at("overrides_in");
            if (std::find(scopes.begin(), scopes.end(), "Object") == scopes.end()) invalid("merged process difference is project-wide: " + key);
            auto value = rows;
            if (std::string(group) == "toolprint_settings") {
                if (!rows.is_array() || rows.empty() || std::any_of(rows.begin(), rows.end(), [&tool_values = rows](const json &row) { return row != tool_values.front(); })) invalid("different per-tool process values cannot become one object override: " + key);
                value = rows.front();
            }
            result[key] = value;
        }
    }
    return result;
}

json merge_materials(json &project, const json &request, const json &target) {
    const auto &inputs = request.at("merge_sources");
    if (!inputs.is_array() || inputs.empty()) invalid("merge_sources must be a nonempty array");
    const auto first = project;
    auto &destination = container(project);
    const auto capacity = destination.at("preset").at("materials").size();
    const auto destination_keys = material_keys(destination.at("preset").at("hw_config"));
    std::map<std::size_t, json> identities, snapshots;
    json mappings = json::array();
    std::set<std::string> source_ids;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto &input = inputs.at(i);
        if ((i == 0) == input.contains("project_settings")) invalid("first merge source uses the base project; later sources require project_settings");
        const auto source = i == 0 ? first : normalized_project(input.at("project_settings"));
        const auto &box = container(source);
        if (hardware_identity(box.at("preset").at("hw_config")) != hardware_identity(container(first).at("preset").at("hw_config")) ||
            box.at("configuration").at("printer_settings") != container(first).at("configuration").at("printer_settings")) invalid("merged sources have different physical hardware or printer settings");
        auto source_settings = box.at("configuration").at("project_settings");
        auto base_settings = container(first).at("configuration").at("project_settings");
        for (const auto *key : {"extruder_colour", "wiping_volumes_matrix"}) {
            source_settings.erase(key);
            base_settings.erase(key);
        }
        if (source_settings != base_settings) invalid("merged sources have different project-wide settings");
        if (box.at("beds").size() != 1 || container(first).at("beds").size() != 1) invalid("merged sources require one bed per configuration");
        if (box.at("beds").front().value("custom_gcode", json(nullptr)) !=
            container(first).at("beds").front().value("custom_gcode", json(nullptr))) invalid("merged sources have different bed custom G-code");
        (void)object_process_overrides(source, first, target);
        const auto id = input.at("source_id").get<std::string>();
        if (id.empty() || !source_ids.insert(id).second) invalid("merge source IDs must be nonempty and unique");
        const auto &slots = input.at("slots");
        if (!slots.is_array() || slots.empty()) invalid("each merge source requires material slots");
        json mapping = {{"source_id", id}, {"slots", json::array()}};
        std::set<std::size_t> seen, source_indices;
        for (std::size_t j = 0; j < slots.size(); ++j) {
            const auto &slot = slots.at(j);
            const auto logical_id = slot_index(slot.at("source_slot_id"));
            const auto from = slot_index(slot.value("source_slot_index", json(j)));
            if (!seen.insert(logical_id).second || !source_indices.insert(from).second) invalid("duplicate source slot mapping");
            json identity = {{"slot_id", logical_id}, {"slot_name", slot.at("slot_name")},
                {"material_id", slot.at("material_id")}, {"preview_color", slot.at("preview_color")}};
            auto snapshot = material_snapshot(box, from);
            snapshot["values"]["filament_colour"] = identity.at("preview_color");
            if (identities.count(logical_id) && (identities.at(logical_id).at("material_id") != identity.at("material_id") ||
                identities.at(logical_id).at("preview_color") != identity.at("preview_color") || snapshots.at(logical_id) != snapshot)) invalid("merged material slot has conflicting identity or native tuning: " + std::to_string(logical_id));
            identities[logical_id] = std::move(identity);
            snapshots[logical_id] = std::move(snapshot);
            mapping["slots"].push_back({{"source_slot_id", logical_id}, {"source_slot_index", from}, {"output_slot_id", logical_id}});
        }
        mappings.push_back(std::move(mapping));
    }
    if (identities.size() > capacity) invalid("merged materials exceed native hardware capacity");
    json logical = json::array();
    std::map<std::size_t, std::size_t> output_indices;
    for (const auto &[id, identity] : identities) {
        const auto index = logical.size();
        output_indices[id] = index;
        logical.push_back(identity);
        const auto &snapshot = snapshots.at(id);
        const auto physical = destination_keys.at(index).substr(0, destination_keys.at(index).find('.'));
        if (snapshot.at("tool_features") != destination.at("preset").at("hw_config").at("tools").at(physical).at("features")) invalid("merged material remapping changes physical nozzle features");
        destination.at("preset").at("materials").at(index) = snapshot.at("preset");
        for (const auto &[key, value] : snapshot.at("values").items()) destination.at("configuration").at("filament_settings").at(key).at(index) = value;
        destination.at("configuration").at("project_settings").at("extruder_colour").at(index) = identity.at("preview_color");
        auto &tool = destination.at("preset").at("hw_config").at("tools").at(destination_keys.at(index));
        for (const auto *key : {"slicer_material", "material_package_instance"}) {
            if (snapshot.at(key).is_null()) tool.erase(key); else tool[key] = snapshot.at(key);
        }
    }
    // Preserve known transitions; a new transition has no native value to infer.
    auto &matrix = destination.at("configuration").at("project_settings").at("wiping_volumes_matrix");
    for (auto &mapping : mappings) {
        for (auto &slot : mapping.at("slots")) slot["output_slot_index"] = output_indices.at(slot.at("output_slot_id").get<std::size_t>());
    }
    const auto matrix_width = static_cast<std::size_t>(std::sqrt(matrix.size()));
    if (matrix.is_array() && matrix_width >= capacity && matrix_width * matrix_width == matrix.size()) {
        std::map<std::pair<std::size_t, std::size_t>, json> transitions;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const auto source = i == 0 ? first : normalized_project(inputs.at(i).at("project_settings"));
            const auto &values = container(source).at("configuration").at("project_settings").at("wiping_volumes_matrix");
            const auto count = static_cast<std::size_t>(std::sqrt(values.size()));
            if (!values.is_array() || count < capacity || values.size() != count * count) invalid("native wiping matrix has an inconsistent shape");
            for (const auto &from : mappings.at(i).at("slots")) for (const auto &to : mappings.at(i).at("slots")) {
                const auto pair = std::pair{slot_index(from.at("output_slot_index")), slot_index(to.at("output_slot_index"))};
                const auto value = values.at(slot_index(from.at("source_slot_index")) * count + slot_index(to.at("source_slot_index")));
                if (transitions.count(pair) && transitions.at(pair) != value) invalid("merged wiping transition has conflicting native values");
                transitions[pair] = value;
            }
        }
        for (std::size_t from = 0; from < logical.size(); ++from) for (std::size_t to = 0; to < logical.size(); ++to) {
            const auto pair = std::pair{from, to};
            if (!transitions.count(pair)) invalid("merged material transition has no source value; provide a source with the complete palette");
            matrix.at(from * matrix_width + to) = transitions.at(pair);
        }
    }
    return {{"merged_slots", logical}, {"source_slot_mappings", mappings}};
}
}  // namespace

bool is_target(const json &target) {
    return target.value("project_format", std::string()) == "prusa3";
}

json normalized_project(json source) {
    if (source.contains("configuration") && source.contains("preset")) {
        source["beds"] = json::array({{{"position_x", 0.0}, {"position_y", 0.0},
                                      {"wipe_tower", nullptr}, {"custom_gcode", nullptr}}});
        const auto project_id = source.at("preset").at("hw_config").at("config_id");
        source = {{"project", {{"version", 1}, {"id", project_id}}}, {"objects", json::array()},
                  {"config_containers", json::array({source})}};
    }
    if (!source.contains("config_containers") || !source.at("config_containers").is_array() ||
        source.at("config_containers").size() != 1) invalid("requires one native FFF config container; multi-configuration projects are not yet supported");
    if (source.at("project").at("version") != 1) invalid("unsupported native project JSON version");
    const auto &box = container(source);
    const auto &preset = box.at("preset");
    if (preset.at("hw_config").at("technology") != "fff") invalid("only FFF hardware is supported");
    if (!preset.at("materials").is_array() || preset.at("materials").empty()) invalid("native material presets are missing");
    const auto &configuration = box.at("configuration");
    for (const auto *group : {"print_settings", "toolprint_settings", "printer_settings", "filament_settings", "project_settings"}) {
        if (!configuration.at(group).is_object()) invalid(std::string("native config group is invalid: ") + group);
    }
    for (const auto *key : {"temperature", "filament_type", "filament_colour"}) {
        const auto &values = configuration.at("filament_settings").at(key);
        if (!values.is_array() || values.size() != preset.at("materials").size()) invalid("material arrays differ from native preset slot count");
    }
    return source;
}

json catalog(const std::filesystem::path &source_root) {
    auto data = read_catalog(source_root);
    json sources = json::array();
    for (auto source : data.at("sources")) {
        source.erase("config_patch");
        source.erase("config_parent");
        sources.push_back(std::move(source));
    }
    return {{"schema_version", 1}, {"slicer_id", "PrusaSlicer"},
            {"application_version", version}, {"source", data.at("source")},
            {"filament_slot_policy", "preserve_native_capacity"}, {"sources", sources}};
}

json options(const json &request, const json &target, const std::filesystem::path &source_root) {
    check_identity(request, target);
    const auto data = read_catalog(source_root);
    const auto &row = source_row(data, request);
    auto result = row;
    result.erase("config_patch");
    result.erase("config_parent");
    result["slicer_id"] = "PrusaSlicer";
    result["application_version"] = version;
    const auto native = source_config(data, row);
    const auto &sheet = native.at("preset").at("hw_config").at("sheet");
    result["build_plates"] = json::array({{{"build_plate_uid", "prusa-sheet:" + sheet.at("type").get<std::string>()},
        {"display_name", sheet.at("name")}, {"project_value", sheet.at("type")}}});
    result["print_profiles"] = json::array({{{"name", row.at("default_print_profile_name")}, {"available", true}}});
    result["filament_profiles"] = json::array();
    std::set<std::string> names;
    for (const auto &name : row.at("default_filament_profile_names")) {
        if (names.insert(name.get<std::string>()).second) result["filament_profiles"].push_back({{"name", name}, {"available", true}, {"material_type", "PLA"}});
    }
    result["process_settings_contract"] = target.at("process_settings_contract");
    result["material_settings_contract"] = target.at("material_settings_contract");
    return result;
}

json compose_builtin(const json &request, const json &target, const std::filesystem::path &source_root) {
    const auto data = read_catalog(source_root);
    const auto &row = source_row(data, request);
    auto result = compose(source_config(data, row), request, target);
    result["process_source"] = {{"source_slicer_id", "PrusaSlicer"},
        {"source_application_version", version}, {"source_profile_name", row.at("default_print_profile_name")}};
    result["material_source"] = {{"source_slicer_id", "PrusaSlicer"},
        {"source_application_version", version}, {"source_profile_names", row.at("default_filament_profile_names")}};
    return result;
}

namespace {
double layout_purge_volume(const json &config) {
    const auto &printer = config.at("printer_settings");
    const auto &filaments = config.at("filament_settings");
    const auto &settings = config.at("project_settings");
    const bool semm = printer.at("single_extruder_multi_material").get<bool>();
    double purge = 0.0, ramming = 0.0;
    if (semm && settings.at("wiping_volumes_use_custom_matrix").get<bool>()) {
        for (const auto &value : settings.at("wiping_volumes_matrix")) purge = std::max(purge, number(value));
    } else if (semm) {
        for (const auto &value : filaments.at("filament_purge_multiplier")) purge = std::max(purge, number(printer.at("multimaterial_purging")) * number(value) / 100.0);
    }
    for (const auto &value : filaments.at("filament_minimal_purge_on_wipe_tower")) purge = std::max(purge, number(value));
    if (semm) {
        for (const auto &value : filaments.at("filament_ramming_parameters")) {
            std::istringstream stream(value.get<std::string>());
            stream.imbue(std::locale::classic());
            double line_width, step, flow, volume = 0.0;
            if (!(stream >> line_width >> step)) invalid("invalid native ramming parameters");
            while (stream >> flow) {
                if (!std::isfinite(flow) || flow < 0.0) invalid("invalid native ramming flow");
                volume += 0.25 * flow;
            }
            ramming = std::max(ramming, volume * step / 100.0);
        }
    } else {
        for (std::size_t i = 0; i < filaments.at("filament_multitool_ramming").size(); ++i) {
            if (filaments.at("filament_multitool_ramming").at(i).get<bool>()) ramming = std::max(ramming, number(filaments.at("filament_multitool_ramming_volume").at(i)));
        }
    }
    return purge + ramming;
}
}  // namespace

json summary(const json &project) {
    const auto &box = container(project);
    const auto &config = box.at("configuration");
    const auto &hardware = box.at("preset").at("hw_config");
    auto result = config.at("print_settings");
    for (const auto &[key, values] : config.at("toolprint_settings").items()) {
        if (values.is_array() && !values.empty()) result[key] = values.front();
    }
    const auto &printer = config.at("printer_settings");
    result["printer_model"] = hardware.at("model");
    result["printer_settings_id"] = hardware.at("config_name");
    result["filament_settings_id"] = json::array();
    for (const auto &material : box.at("preset").at("materials")) result["filament_settings_id"].push_back(material.at("name"));
    result["filament_colour"] = config.at("project_settings").at("extruder_colour");
    result["filament_type"] = config.at("filament_settings").at("filament_type");
    result["printable_area"] = json::array();
    for (const auto &point : printer.at("bed_shape")) {
        result["printable_area"].push_back(decimal(number(point.at(0))) + "x" + decimal(number(point.at(1))));
    }
    result["printable_height"] = printer.at("max_print_height");
    result["nozzle_diameter"] = json::array();
    for (std::size_t i = 0; i < hardware.at("tool_count").get<std::size_t>(); ++i) {
        result["nozzle_diameter"].push_back(hardware.at("tools").at(std::to_string(i)).at("features").at("nozzle_diameter"));
    }
    const auto &height = result.at("first_layer_height");
    result["initial_layer_print_height"] = number(height) *
        (height.value("is_percent", false) ? number(result.at("layer_height")) / 100.0 : 1.0);
    result["enable_prime_tower"] = result.at("wipe_tower").get<bool>() ? "1" : "0";
    result["prime_tower_width"] = result.at("wipe_tower_width");
    // Layout consumes native purge and spacing facts, never the Orca fallback.
    result["prime_tower_brim_width"] = result.at("wipe_tower_brim_width");
    result["prime_tower_infill_gap"] = number(result.at("wipe_tower_extra_spacing")) / 100.0;
    result["prime_volume"] = layout_purge_volume(config);
    result["curr_bed_type"] = hardware.at("sheet").at("type");
    result["min_layer_height"] = printer.value("min_layer_height", json(nullptr));
    result["max_layer_height"] = printer.value("max_layer_height", json(nullptr));
    const auto &bed = box.at("beds").front();
    if (bed.contains("wipe_tower") && bed.at("wipe_tower").is_object()) {
        for (const auto &[native, output] : {std::pair{"x", "wipe_tower_x"}, {"y", "wipe_tower_y"}, {"rotation_angle", "wipe_tower_rotation_angle"}}) result[output] = bed.at("wipe_tower").at(native);
    }
    return result;
}

json compose(json project, const json &request, const json &target) {
    check_identity(request, target);
    project = normalized_project(std::move(project));
    const auto merged = request.contains("merge_sources") ? merge_materials(project, request, target) : json::object();
    if (request.contains("material_uids") || request.contains("material_uid")) invalid("use a native material config or per-slot native_settings for Prusa 3");
    auto &box = container(project);
    check_presets(box, request);
    auto &config = box.at("configuration");
    const auto &hardware = box.at("preset").at("hw_config");
    if (request.contains("machine_uid") && !request.at("machine_uid").get<std::string>().empty() &&
        request.at("machine_uid") != hardware_uid(hardware)) invalid("selected machine differs from source hardware");
    if (request.contains("nozzle_uid") && !request.at("nozzle_uid").get<std::string>().empty()) {
        const auto selected = request.at("nozzle_uid").get<std::string>();
        for (std::size_t i = 0; i < hardware.at("tool_count").get<std::size_t>(); ++i) {
            const auto diameter = hardware.at("tools").at(std::to_string(i)).at("features").at("nozzle_diameter");
            if (selected != "nozzle:" + decimal(diameter.get<double>()) + "mm") invalid("selected nozzle differs from source hardware");
        }
    }
    if (request.contains("material_mode") && request.at("material_mode") != "preserve_template") invalid("Prusa 3 requires preserve_template material mode");
    if (request.contains("hardware_mode") && request.at("hardware_mode") != "preserve_template" && request.at("hardware_mode") != "preserve_source" && request.at("hardware_mode") != "auto") invalid("Prusa 3 requires preserved native hardware");
    if (request.contains("filament_slot_mode") && request.at("filament_slot_mode") != "preserve_template" && request.at("filament_slot_mode") != "preserve" && request.at("filament_slot_mode") != "auto") invalid("Prusa 3 preserves native hardware slot capacity; slot compaction is not supported");
    if (request.contains("filament_multi_colour") && request.at("filament_multi_colour") != request.value("filament_colour", json::array())) invalid("Prusa 3 multi-colour filament metadata is not supported");
    if (request.contains("preserve_source_material_settings") && request.at("preserve_source_material_settings") != true) invalid("Prusa 3 preserves native material settings; replace them through explicit native_settings");
    if (request.contains("project_source") && request.at("project_source") != "fatcat_native") invalid("unsupported built-in source mode");
    const auto &sheet = box.at("preset").at("hw_config").at("sheet");
    if (request.contains("build_plate_uid") && !request.at("build_plate_uid").get<std::string>().empty() &&
        request.at("build_plate_uid") != "prusa-sheet:" + sheet.at("type").get<std::string>()) invalid("sheet selection differs from native config; export the selected sheet's config");
    static const std::set<std::string> allowed = {
        "slicer_id", "application_version", "project_source", "machine_uid", "nozzle_uid",
        "build_plate_uid", "hardware_mode", "source_materials", "filament_colour", "filament_multi_colour",
        "filament_slot_mode", "filament_source_slots", "material_mode", "process_settings",
        "native_print_profile_name", "native_filament_profile_names", "source_slot_colours",
        "preserve_source_material_settings", "source_profile", "consumer_type", "merge_sources",
        "enable_prime_tower", "prime_tower_width", "wipe_tower_x", "wipe_tower_y", "wipe_tower_rotation_angle"};
    json chosen = json::object();
    const auto accept = [&](const std::string &key, const json &value) {
        const auto &contract = target.at("process_settings_contract");
        const auto native = contract.at("aliases").value(key, key);
        if (!contract.at("fields").contains(native)) invalid("unsupported process override '" + key + "' [sdk_not_supported]");
        auto canonical = value;
        if (contract.contains("enum_aliases") && contract.at("enum_aliases").contains(native)) {
            const auto &aliases = contract.at("enum_aliases").at(native);
            const auto text = canonical.is_string() ? canonical.get<std::string>() : canonical.dump();
            if (aliases.contains(text)) canonical = aliases.at(text);
        }
        json parsed;
        try {
            parsed = scalar(canonical, contract.at("fields").at(native));
        } catch (const std::exception &error) {
            invalid("process field '" + native + "': " + error.what());
        }
        if (chosen.contains(native) && chosen.at(native) != parsed) invalid("conflicting aliases for native field '" + native + "'");
        chosen[native] = parsed;
        set_process(config, native, parsed, target);
    };
    for (const auto &[key, value] : request.items()) if (!allowed.count(key)) accept(key, value);
    if (request.contains("process_settings")) {
        for (const auto &[key, value] : request.at("process_settings").items()) accept(key, value);
    }
    apply_materials(project, request, target);
    json tower;
    if (request.contains("enable_prime_tower")) tower["enabled"] = scalar(request.at("enable_prime_tower"), {{"type", "Bool"}});
    if (request.contains("prime_tower_width")) tower["width_mm"] = number(request.at("prime_tower_width"));
    if (request.contains("wipe_tower_x") || request.contains("wipe_tower_y")) {
        const auto old = summary(project);
        tower["positions"] = json::array({{{"plate_index", 0},
            {"x_mm", request.contains("wipe_tower_x") ? number(request.at("wipe_tower_x")) : number(old.value("wipe_tower_x", json(15.0)))},
            {"y_mm", request.contains("wipe_tower_y") ? number(request.at("wipe_tower_y")) : number(old.value("wipe_tower_y", json(15.0)))}}});
    }
    if (request.contains("wipe_tower_rotation_angle")) tower["rotation_deg"] = number(request.at("wipe_tower_rotation_angle"));
    if (!tower.is_null()) project = patch_tower(std::move(project), tower);
    const auto effective = summary(project);
    json slot_map = json::array();
    const auto count = effective.at("filament_settings_id").size();
    for (std::size_t i = 0; i < count; ++i) slot_map.push_back(i);
    json result = {{"project_settings_json", project.dump()}, {"effective_settings", effective},
        {"metadata_defaults", {{"identify_id", 1}, {"plate_value", effective.at("curr_bed_type")},
            {"sidecar_bed_value", effective.at("curr_bed_type")}, {"plate_summary", false},
            {"geometry_layout", "core"}}},
        {"slot_projection", {{"output_slot_count", count}, {"source_to_output", slot_map}}},
        {"wipe_tower_dialect", {{"project_format", "prusa3"}}},
        {"build_item_properties", json::object()}};
    result.update(merged);
    return result;
}

json patch_tower(json project, const json &settings) {
    project = normalized_project(std::move(project));
    if (!settings.is_object()) invalid("wipe tower settings require an object");
    for (const auto &[key, value] : settings.items()) {
        if (key != "enabled" && key != "width_mm" && key != "positions" && key != "rotation_deg") invalid("unknown wipe tower field '" + key + "'");
    }
    auto &box = container(project);
    auto &print = box.at("configuration").at("print_settings");
    if (settings.contains("enabled")) {
        if (!settings.at("enabled").is_boolean()) invalid("wipe tower enabled must be a boolean");
        print["wipe_tower"] = settings.at("enabled");
    }
    if (settings.contains("width_mm")) {
        const auto width = number(settings.at("width_mm"));
        if (width <= 0) invalid("wipe tower width must be positive");
        print["wipe_tower_width"] = width;
    }
    if (settings.contains("positions")) {
        if (!settings.at("positions").is_array()) invalid("wipe tower positions must be an array");
        std::set<std::size_t> indices;
        for (const auto &position : settings.at("positions")) {
            const auto &index = position.at("plate_index");
            if (!index.is_number_integer() || index.get<long long>() < 0) invalid("wipe tower plate index must be a nonnegative integer");
            const auto plate = index.get<std::size_t>();
            if (plate >= box.at("beds").size() || !indices.insert(plate).second) invalid("wipe tower plate index is absent or duplicated");
            auto &bed = box.at("beds").at(plate);
            bed["wipe_tower"] = {{"x", number(position.at("x_mm"))}, {"y", number(position.at("y_mm"))},
                {"rotation_angle", number(settings.value("rotation_deg", json(0.0)))}};
        }
    } else if (settings.contains("rotation_deg")) {
        for (auto &bed : box.at("beds")) {
            if (bed.at("wipe_tower").is_object()) bed["wipe_tower"]["rotation_angle"] = number(settings.at("rotation_deg"));
        }
    }
    return project;
}

json material_slots(const json &project) {
    const auto effective = summary(project);
    const auto &materials = container(project).at("preset").at("materials");
    json slots = json::array();
    for (std::size_t i = 0; i < materials.size(); ++i) slots.push_back({
        {"slot_id", i}, {"slot_name", materials.at(i).at("name")},
        {"preview_color", effective.at("filament_colour").at(i)},
        {"material_id", materials.at(i).at("id")}});
    return slots;
}

namespace {
void remap_extruders(json &settings, const json &input, std::size_t capacity) {
    if (!input.contains("source_slot_output_indexes")) return;
    const auto &mapping = input.at("source_slot_output_indexes");
    for (auto &[key, value] : settings.items()) {
        if (key != "extruder" && (key.size() < 9 || key.substr(key.size() - 9) != "_extruder")) continue;
        const auto slot = slot_index(value);
        if (slot == 0) continue;
        if (slot > mapping.size()) invalid("source object references an absent material slot");
        const auto output = slot_index(mapping.at(slot - 1));
        // Unselected slots use a sentinel at the logical palette length.
        const auto used = slot_index(input.at("output_slot_count"));
        if (output >= capacity || output >= used) invalid("source object uses a material absent from the selected merge palette");
        value = output + 1;
    }
}

void apply_ranges(json &object, const json &input, const json &target) {
    const auto &fields = target.at("process_settings_contract").at("fields");
    json ranges = json::array();
    double previous_end = 0.0;
    for (const auto &range : input.at("ranges")) {
        const auto begin = number(range.at("min_z"));
        const auto end = number(range.at("max_z"));
        if (begin < previous_end || end <= begin) invalid("layer ranges must be positive, ordered and nonoverlapping");
        previous_end = end;
        json configuration = json::object();
        if (range.contains("use_default_extruder")) {
            if (range.at("use_default_extruder") != true) invalid("layer range use_default_extruder must be true");
            configuration["extruder"] = 0;
        }
        if (range.contains("layer_height_mm")) {
            const auto height = number(range.at("layer_height_mm"));
            if (height <= 0) invalid("layer-range height must be positive");
            configuration["layer_height"] = scalar(height, fields.at("layer_height"));
        }
        if (range.contains("infill_density_percent")) configuration["fill_density"] = scalar(range.at("infill_density_percent"), fields.at("fill_density"));
        for (const auto &[key, value] : range.items()) {
            if (key != "min_z" && key != "max_z" && key != "use_default_extruder" && key != "layer_height_mm" && key != "infill_density_percent") invalid("unsupported layer range business field: " + key);
        }
        ranges.push_back({{"zRange", json::array({begin, end})}, {"configuration", configuration}});
    }
    if (ranges.empty()) invalid("layer ranges must not be empty");
    object["ranges"] = std::move(ranges);
}
}  // namespace

json describe(json project, const json &request, const json &target) {
    check_identity(request, target);
    project = normalized_project(std::move(project));
    auto inputs = request.value("objects", json::array({request}));
    const auto slots = container(project).at("preset").at("materials").size();
    json objects = json::array();
    json build_items = json::array();
    std::set<long long> ids;
    for (const auto &input : inputs) {
        if (!input.at("assembly_id").is_number_integer()) invalid("assembly ID must be an integer");
        const auto assembly = input.at("assembly_id").get<long long>();
        if (assembly <= 0 || !ids.insert(assembly).second) invalid("assembly ID must be positive and unique");
        json object = {{"id", assembly}, {"object_settings", {{"extruder", 0}, {"wipe_into_objects", false}}}};
        json original = json::object();
        if (input.contains("source_model_settings_xml")) {
            const auto source = normalized_project(json::parse(input.at("source_model_settings_xml").get<std::string>()));
            for (const auto &candidate : source.at("objects")) {
                if (candidate.at("id") == input.at("source_assembly_id")) original = candidate;
            }
            if (original.empty()) invalid("source assembly metadata is missing");
            object = original;
            object["id"] = assembly;
            object.erase("instances"); // The writer owns the new build transforms.
            object.erase("object_uuid");
            if (object.contains("cutId") || object.contains("slaSupportPoints") || object.contains("slaDrainHoles")) invalid("cut/SLA geometry metadata is unsupported for FFF merge");
            const auto overrides = object_process_overrides(source, project, target);
            // Source object overrides remain authoritative over its source defaults.
            auto settings = overrides;
            settings.update(object.value("object_settings", json::object()));
            object["object_settings"] = std::move(settings);
            remap_extruders(object.at("object_settings"), input, slots);
            if (object.contains("ranges")) {
                for (auto &range : object.at("ranges")) remap_extruders(range.at("configuration"), input, slots);
            }
        }
        json volumes = json::array();
        for (const auto &part : input.at("parts")) {
            if (!part.at("part_id").is_number_integer()) invalid("part ID must be an integer");
            const auto id = part.at("part_id").get<long long>();
            if (id <= 0 || !ids.insert(id).second) invalid("part ID must be positive and unique");
            json volume;
            if (!original.empty()) {
                for (const auto &candidate : original.at("volumes")) {
                    if (candidate.at("id") == part.at("source_part_id")) volume = candidate;
                }
                if (volume.is_null()) invalid("source volume metadata is missing");
                volume["id"] = id;
                remap_extruders(volume.at("volume_settings"), input, slots);
            } else {
                const auto slot = slot_index(part.at("material_index"));
                if (slot >= slots) invalid("part material index is outside native slots");
                volume = {{"id", id}, {"type", "ModelPart"},
                    {"volume_settings", {{"extruder", slot + 1}, {"wipe_into_infill", false}}}};
            }
            volumes.push_back(std::move(volume));
        }
        if (volumes.empty()) invalid("model requires at least one part");
        object["volumes"] = std::move(volumes);
        objects.push_back(std::move(object));
        if (request.contains("objects")) {
            const auto values = input.value("transform", json::array({1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}));
            if (!values.is_array() || values.size() != 12) invalid("build transform requires twelve numeric values");
            std::string transform;
            for (const auto &value : values) {
                if (!transform.empty()) transform += ' ';
                transform += decimal(number(value));
            }
            build_items.push_back({{"model_index", input.at("model_index")}, {"transform", transform}, {"properties", json::object()}});
        }
    }
    const auto components = request.value("component_inputs", json::object());
    json parts = json::array();
    for (const auto &[key, value] : components.items()) {
        if (value.is_null()) continue;
        if (key == "layer_config_ranges") {
            const auto ranges = value.contains("objects") ? value.at("objects") : json::array({value});
            for (const auto &range : ranges) {
                const auto index = slot_index(range.at("model_index"));
                if (index == 0 || index > objects.size()) invalid("layer range model index is absent");
                apply_ranges(objects.at(index - 1), range, target);
            }
        } else if (key == "wipe_tower_placement") {
            parts.push_back({{"role", key}, {"path", "Metadata/wipe_tower_placement.json"}, {"media_type", "application/json"}, {"content", value.dump()}});
        } else invalid("unsupported model component '" + key + "'");
    }
    project["objects"] = objects;
    parts.push_back({{"role", "project_settings"}, {"path", project_path}, {"media_type", "application/json"}, {"content", project.dump()}});
    return {{"parts", parts}, {"build_items", build_items},
        {"relationships", json::array()}, {"content_types", json::array({{{"extension", "json"}, {"media_type", "application/json"}}})},
        {"root_model", {{"namespaces", json::array()}, {"metadata", json::array({
            {{"name", "Application"}, {"value", std::string("PrusaSlicer-") + version}}})},
            {"external_metadata", json::array()}}},
        {"settings_parts", {{"project_settings", project_path}, {"model_settings", project_path}, {"slice_info", ""}}}};
}
}  // namespace fatcat::detail::prusa
