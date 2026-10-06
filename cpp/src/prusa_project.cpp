#include "prusa_project.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <locale>
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
                const auto type = material.at("material_type").get<std::string>();
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
            {"application_version", version}, {"source", data.at("source")}, {"sources", sources}};
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
    if (request.contains("merge_sources")) invalid("merged source tuning is not yet supported for Prusa 3; no settings will be silently discarded");
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
    if (request.contains("hardware_mode") && request.at("hardware_mode") != "preserve_template") invalid("Prusa 3 requires preserve_template hardware mode; compose the selected native hardware instead");
    if (request.contains("filament_slot_mode") && request.at("filament_slot_mode") != "preserve_template") invalid("Prusa 3 preserves native hardware slot capacity; slot compaction is not supported");
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
        "preserve_source_material_settings", "source_profile", "consumer_type",
        "enable_prime_tower", "prime_tower_width", "wipe_tower_x", "wipe_tower_y", "wipe_tower_rotation_angle"};
    json chosen = json::object();
    const auto accept = [&](const std::string &key, const json &value) {
        const auto &contract = target.at("process_settings_contract");
        const auto native = contract.at("aliases").value(key, key);
        if (!contract.at("fields").contains(native)) invalid("unsupported process override '" + key + "' [sdk_not_supported]");
        auto canonical = value;
        if (canonical.is_string() && contract.contains("enum_aliases") && contract.at("enum_aliases").contains(native)) {
            const auto &aliases = contract.at("enum_aliases").at(native);
            canonical = aliases.value(canonical.get<std::string>(), canonical.get<std::string>());
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
    return {{"project_settings_json", project.dump()}, {"effective_settings", effective},
        {"metadata_defaults", {{"identify_id", 1}, {"plate_value", effective.at("curr_bed_type")},
            {"sidecar_bed_value", effective.at("curr_bed_type")}, {"plate_summary", false}}},
        {"slot_projection", {{"output_slot_count", count}, {"source_to_output", slot_map}}},
        {"wipe_tower_dialect", {{"project_format", "prusa3"}}},
        {"build_item_properties", json::object()}};
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

json describe(json project, const json &request, const json &target) {
    check_identity(request, target);
    project = normalized_project(std::move(project));
    auto inputs = request.value("objects", json::array({request}));
    const auto slots = container(project).at("preset").at("materials").size();
    json objects = json::array();
    std::set<long long> ids;
    for (const auto &input : inputs) {
        if (!input.at("assembly_id").is_number_integer()) invalid("assembly ID must be an integer");
        const auto assembly = input.at("assembly_id").get<long long>();
        if (assembly <= 0 || !ids.insert(assembly).second) invalid("assembly ID must be positive and unique");
        json volumes = json::array();
        for (const auto &part : input.at("parts")) {
            if (!part.at("part_id").is_number_integer() || !part.at("material_index").is_number_integer()) invalid("part ID and material index must be integers");
            const auto id = part.at("part_id").get<long long>();
            const auto slot = part.at("material_index").get<long long>();
            if (id <= 0 || !ids.insert(id).second) invalid("part ID must be positive and unique");
            if (slot < 0 || static_cast<std::size_t>(slot) >= slots) invalid("part material index is outside native slots");
            volumes.push_back({{"id", id}, {"type", "ModelPart"},
                {"volume_settings", {{"extruder", slot + 1}, {"wipe_into_infill", false}}}});
        }
        if (volumes.empty()) invalid("model requires at least one part");
        objects.push_back({{"id", assembly}, {"volumes", volumes},
            {"object_settings", {{"extruder", 0}, {"wipe_into_objects", false}}}});
    }
    const auto components = request.value("component_inputs", json::object());
    for (const auto &[key, value] : components.items()) {
        if (!value.is_null()) invalid("unsupported model component '" + key + "'; it will not be silently dropped (apply native tower settings to the project first)");
    }
    project["objects"] = objects;
    return {{"parts", json::array({{{"role", "project_settings"}, {"path", project_path},
                {"media_type", "application/json"}, {"content", project.dump()}}})},
        {"relationships", json::array()}, {"content_types", json::array({{{"extension", "json"}, {"media_type", "application/json"}}})},
        {"root_model", {{"namespaces", json::array()}, {"metadata", json::array({
            {{"name", "Application"}, {"value", std::string("PrusaSlicer-") + version}}})},
            {"external_metadata", json::array()}}},
        {"settings_parts", {{"project_settings", project_path}, {"model_settings", project_path}, {"slice_info", ""}}}};
}
}  // namespace fatcat::detail::prusa
