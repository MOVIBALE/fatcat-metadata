#include "project_settings_overrides.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include "fatcat/source_project_settings.h"
#include "fatcat/wipe_tower.h"
#include "json_object.h"
#include "project_settings_internal.h"

namespace fatcat::detail {
using namespace project_settings_internal;
namespace {

double parse_numeric_token(const std::string &value, std::string_view field) {
    double number = 0.0;
    if (!detail::parse_finite_decimal_string(value, number)) {
        invalid("request." + std::string(field) +
                " must be a finite numeric string");
    }
    return number;
}

double numeric_value(const std::string &value, std::string_view field) {
    return parse_numeric_token(value, field);
}

json normalized_numeric_value(const std::string &raw, std::string_view field) {
    const double number = parse_numeric_token(raw, field);
    if (number == 0.0 && !raw.empty() && raw.front() == '-') {
        // Keep the sign until A2 formats the value so -0 follows its contract.
        return json::parse("-0.0");
    }
    return json(number);
}

const json &process_contract(const json &target) {
    return required_member(target, "process_settings_contract", "target data");
}

std::string target_label(const json &target) {
    const auto &contract = required_member(target, "target_contract", "target data");
    return required_string(contract, "slicer_id", "target contract") + " " +
           required_string(contract, "application_version", "target contract");
}

std::string scalar_text(const json &value, const std::string &path) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "1" : "0";
    if (value.is_number()) return value.dump();
    invalid(path + " must be a scalar string, number or boolean");
}

std::string validate_process_value(const json &value, const json &field,
                                   const json &contract, const std::string &key,
                                   const std::string &path, const json &target) {
    std::string raw = scalar_text(value, path);
    const auto &value_aliases = contract.at("value_aliases");
    if (value_aliases.contains(key) && value_aliases.at(key).contains(raw)) {
        raw = value_aliases.at(key).at(raw).get<std::string>();
    }
    const auto type = required_string(field, "type", "process field");
    if (value.is_boolean() && type != "bool") invalid(path + " does not accept a boolean");
    if (type == "enum") {
        const auto &values = field.at("values");
        if (std::find(values.begin(), values.end(), json(raw)) == values.end()) {
            invalid(path + "='" + raw + "' is unsupported by " + target_label(target) +
                    "; supported values: " + values.dump());
        }
        return raw;
    }
    if (type == "bool") {
        if (raw == "true") return "1";
        if (raw == "false") return "0";
        if (raw != "0" && raw != "1") invalid(path + " must be 0 or 1 (or boolean)");
        return raw;
    }
    const bool percent = !raw.empty() && raw.back() == '%';
    if (percent && type != "percent" && type != "float_or_percent") {
        invalid(path + " does not accept a percentage in " + target_label(target));
    }
    double number = 0.0;
    const auto token = percent ? raw.substr(0, raw.size() - 1) : raw;
    if (!detail::parse_finite_decimal_string(token, number)) {
        invalid(path + " must be a finite numeric value");
    }
    if (type == "int" && std::trunc(number) != number) {
        invalid(path + " must be a whole number");
    }
    if (field.contains("minimum") && number < field.at("minimum").get<double>()) {
        invalid(path + " must be at least " + field.at("minimum").dump());
    }
    if (field.contains("exclusive_minimum") &&
        number <= field.at("exclusive_minimum").get<double>()) {
        invalid(path + " must be greater than " + field.at("exclusive_minimum").dump());
    }
    if (field.contains("maximum") && number > field.at("maximum").get<double>()) {
        invalid(path + " must be at most " + field.at("maximum").dump());
    }
    if (type == "int") return std::to_string(static_cast<std::int64_t>(number));
    if (type == "percent" && !percent) raw += '%';
    return raw;
}

bool equivalent_process_values(const json &left, const json &right) {
    if (left == right) return true;
    double a = 0.0, b = 0.0;
    return detail::parse_finite_decimal_string(left.get<std::string>(), a) &&
           detail::parse_finite_decimal_string(right.get<std::string>(), b) && a == b;
}

json resolve_process_level(const json &values, const json &target,
                           const std::string &path, bool nested) {
    const auto &contract = process_contract(target);
    const auto &fields = contract.at("fields");
    const auto &aliases = contract.at("aliases");
    const auto &unsupported = contract.at("unsupported_fields");
    json result = json::object();
    for (const auto &[input_key, value] : values.items()) {
        const auto key = aliases.value(input_key, input_key);
        if (unsupported.contains(input_key)) {
            invalid(path + "." + input_key + " is unsupported by " + target_label(target) +
                    "; " + unsupported.at(input_key).get<std::string>());
        }
        if (!fields.contains(key)) {
            if (nested) invalid(path + " contains unsupported field '" + input_key +
                                "' for " + target_label(target));
            continue;
        }
        const auto normalized = validate_process_value(value, fields.at(key), contract,
                                                        key, path + "." + input_key, target);
        if (result.contains(key) && !equivalent_process_values(result.at(key), normalized)) {
            invalid(path + " supplies conflicting aliases for '" + key + "'");
        }
        result[key] = normalized;
    }
    return result;
}

}  // namespace

bool is_process_override_key(const json &target, const std::string &key) {
    const auto &contract = process_contract(target);
    return contract.at("fields").contains(key) || contract.at("aliases").contains(key) ||
           contract.at("unsupported_fields").contains(key);
}

json resolve_process_overrides(const json &request, const json &target) {
    json result = json::object();
    if (request.contains("process_settings")) {
        const auto &process = request.at("process_settings");
        if (!process.is_object()) invalid("request.process_settings must be an object");
        result = resolve_process_level(process, target, "request.process_settings", true);
    }
    // Explicit top-level values retain priority over nested process defaults.
    result.update(resolve_process_level(request, target, "request", false));
    return result;
}

void apply_scalar_overrides(json &project, const json &request, const json &target,
                            std::vector<std::string> &changed_keys) {
    const auto overrides = resolve_process_overrides(request, target);
    const auto &fields = process_contract(target).at("fields");
    for (const auto &[key, value] : overrides.items()) {
        if (fields.at(key).value("array", false)) {
            const auto existing = project.find(key);
            const auto nozzles = project.find("nozzle_diameter");
            std::size_t count = 1;
            if (existing != project.end() && existing->is_array() && !existing->empty()) {
                count = existing->size();
            } else if (nozzles != project.end() && nozzles->is_array() && !nozzles->empty()) {
                count = nozzles->size();
            }
            project[key] = json::array();
            for (std::size_t index = 0; index < count; ++index) project[key].push_back(value);
        } else {
            project[key] = value;
        }
        changed_keys.push_back(key);
    }
}

namespace {

json target_tower_dialect(const json &target) {
    const json &tower = required_member(
        required_member(target, "package_dialect", "target data"),
        "wipe_tower", "package dialect");
    return {
        {"enabled_key", required_string(tower, "enabled_key", "wipe tower dialect")},
        {"x_key", required_string(tower, "x_key", "wipe tower dialect")},
        {"y_key", required_string(tower, "y_key", "wipe tower dialect")},
        {"width_key", required_string(tower, "width_key", "wipe tower dialect")},
        {"rotation_key", required_string(tower, "rotation_key", "wipe tower dialect")},
        {"process_difference_key",
         required_string(tower, "process_difference_key", "wipe tower dialect")},
    };
}

std::string scalar_tower_settings(const json &request) {
    json settings = json::object();
    if (request.contains("enable_prime_tower")) {
        const json &value = request.at("enable_prime_tower");
        if (!value.is_string()) {
            invalid("request.enable_prime_tower must be text");
        }
        const auto enabled = value.get<std::string>();
        if (enabled != "0" && enabled != "1") {
            invalid("request.enable_prime_tower must be exactly 0 or 1");
        }
        settings["enabled"] = enabled == "1";
    }
    if (request.contains("prime_tower_width")) {
        const auto &value = request.at("prime_tower_width");
        if (!value.is_string()) {
            invalid("request.prime_tower_width must be text");
        }
        const auto raw = value.get<std::string>();
        if (!(numeric_value(raw, "prime_tower_width") > 0.0)) {
            invalid("request.prime_tower_width must be finite and positive");
        }
        settings["width_mm"] = normalized_numeric_value(raw, "prime_tower_width");
    }
    if (request.contains("wipe_tower_rotation_angle")) {
        const auto &value = request.at("wipe_tower_rotation_angle");
        if (!value.is_string()) {
            invalid("request.wipe_tower_rotation_angle must be text");
        }
        const auto raw = value.get<std::string>();
        settings["rotation_deg"] =
            normalized_numeric_value(raw, "wipe_tower_rotation_angle");
    }
    return settings.dump();
}

std::pair<std::size_t, std::size_t> coordinate_sizes(const json &project,
                                                     const json &dialect) {
    const std::string x_key = required_string(dialect, "x_key", "wipe tower dialect");
    const std::string y_key = required_string(dialect, "y_key", "wipe tower dialect");
    const bool has_x = project.contains(x_key);
    const bool has_y = project.contains(y_key);
    if (has_x != has_y) {
        invalid("wipe tower x/y coordinate arrays must be provided together");
    }
    if (!has_x) {
        return {0, 0};
    }
    if (!project.at(x_key).is_array() || !project.at(y_key).is_array()) {
        invalid("wipe tower x/y coordinate fields must be arrays");
    }
    const auto x_size = project.at(x_key).size();
    const auto y_size = project.at(y_key).size();
    if (x_size != y_size) {
        invalid("wipe tower x/y coordinate plate counts differ");
    }
    return {x_size, y_size};
}

std::string position_tower_settings(std::size_t plate_index,
                                    const std::string &x_value,
                                    const std::string &y_value) {
    const json settings = {
        {"positions", json::array({{{"plate_index", plate_index},
                                     {"x_mm", normalized_numeric_value(x_value, "wipe_tower_x")},
                                     {"y_mm", normalized_numeric_value(y_value, "wipe_tower_y")}}})}};
    return settings.dump();
}

void prepare_tower_difference_index(json &project, const json &target,
                                    std::size_t slot_count, bool merging_sources) {
    const json &snapshot = required_member(
        required_member(target, "package_dialect", "target data"),
        "filament_snapshot", "package dialect");
    const std::string key =
        required_string(snapshot, "difference_list_key", "filament snapshot");
    const json &offset_value =
        required_member(snapshot, "filament_slot_offset", "filament snapshot");
    const json &trailing_value =
        required_member(snapshot, "trailing_entry_count", "filament snapshot");
    if (!offset_value.is_number_unsigned() || !trailing_value.is_number_unsigned()) {
        invalid("filament snapshot entry counts must be non-negative integers");
    }
    const std::size_t expected =
        merging_sources && snapshot.value("merged_difference_list_singleton", false) &&
                (!project.contains(key) || project.at(key).size() <= 1)
            ? 1
            : offset_value.get<std::size_t>() + slot_count +
                  trailing_value.get<std::size_t>();
    if (!project.contains(key)) {
        project[key] = json::array();
        for (std::size_t index = 0; index < expected; ++index) {
            project[key].push_back("");
        }
        return;
    }
    if (project.at(key).is_array() && project.at(key).empty()) {
        project[key] = json::array();
        for (std::size_t index = 0; index < expected; ++index) {
            project[key].push_back("");
        }
        return;
    }
    const auto entries = string_array(project.at(key), "project." + key);
    if (entries.size() != expected) {
        invalid("filament snapshot difference-list entry count does not match the slot layout");
    }
}

bool is_signed_integer_zero(const json &value) {
    return value.is_number_integer() && !value.is_number_unsigned() &&
           value.get<std::int64_t>() == 0;
}

void normalize_typed_positions(json &positions) {
    if (!positions.is_array()) {
        invalid("request.wipe_tower_positions must be an array");
    }
    for (auto &position : positions) {
        if (!position.is_object()) {
            continue;
        }
        const auto plate_it = position.find("plate_index");
        if (plate_it != position.end() && is_signed_integer_zero(*plate_it)) {
            // JSON 0 is stored as unsigned by nlohmann/json; signed integer 0
            // here is the lexical -0 that A2 intentionally rejects.
            invalid("wipe tower request: wipe tower plate_index must be a nonnegative integer");
        }
        for (const auto key : {std::string("x_mm"), std::string("y_mm")}) {
            const auto value_it = position.find(key);
            if (value_it != position.end() && is_signed_integer_zero(*value_it)) {
                *value_it = json::parse("-0.0");
            }
        }
    }
}

void apply_tower_patch(json &project, const std::string &settings_json,
                       const json &dialect) {
    try {
        project = detail::parse_json_object<ProjectSettingsError>(
            patch_wipe_tower(project.dump(), settings_json, dialect.dump()),
            "patched project settings");
    } catch (const WipeTowerError &error) {
        invalid(std::string("wipe tower request: ") + error.what());
    }
}

}  // namespace

void apply_tower_overrides(json &project, const json &request,
                           const json &target) {
    detail::apply_source_tower_defaults(project, target);
    json dialect = target_tower_dialect(target);
    const bool has_scalar = request.contains("enable_prime_tower") ||
                            request.contains("prime_tower_width") ||
                            request.contains("wipe_tower_rotation_angle");
    const bool has_xy = request.contains("wipe_tower_x") ||
                        request.contains("wipe_tower_y");
    const bool has_positions = request.contains("wipe_tower_positions");
    if (has_xy && has_positions) {
        invalid("request cannot combine wipe_tower_positions with wipe_tower_x/y");
    }

    if ((has_scalar || has_xy || has_positions) &&
        request.value("hardware_mode", "target_binding") != "preserve_source") {
        std::size_t slot_count = 0;
        validate_slots(project, slot_count);
        prepare_tower_difference_index(project, target, slot_count,
                                       request.contains("merge_sources"));
    }

    if (has_scalar) {
        apply_tower_patch(project, scalar_tower_settings(request), dialect);
    }

    const auto [existing_x_size, existing_y_size] = coordinate_sizes(project, dialect);
    if (has_xy) {
        if (!request.contains("wipe_tower_x") || !request.contains("wipe_tower_y")) {
            invalid("request wipe_tower_x and wipe_tower_y must be provided together");
        }
        const auto x_values = string_array(request.at("wipe_tower_x"),
                                           "request.wipe_tower_x");
        const auto y_values = string_array(request.at("wipe_tower_y"),
                                           "request.wipe_tower_y");
        if (x_values.size() != y_values.size()) {
            invalid("request wipe tower x/y coordinate counts differ");
        }
        if (existing_x_size != existing_y_size ||
            (existing_x_size != x_values.size() && existing_x_size != 0) ||
            (existing_x_size == 0 && x_values.size() > 1)) {
            invalid("request wipe tower coordinate count must match existing plates");
        }
        for (std::size_t index = 0; index < x_values.size(); ++index) {
            // Validate every requested token, including untouched tail slots. Only
            // changed slots are sent to A2 so their original text is retained.
            parse_numeric_token(x_values[index], "wipe_tower_x");
            parse_numeric_token(y_values[index], "wipe_tower_y");
            const auto current_x = existing_x_size == 0
                                       ? std::string()
                                       : project.at(dialect.at("x_key").get<std::string>())
                                             .at(index)
                                             .get<std::string>();
            const auto current_y = existing_y_size == 0
                                       ? std::string()
                                       : project.at(dialect.at("y_key").get<std::string>())
                                             .at(index)
                                             .get<std::string>();
            if (existing_x_size == 0 || x_values[index] != current_x ||
                y_values[index] != current_y) {
                apply_tower_patch(project,
                                  position_tower_settings(index, x_values[index],
                                                          y_values[index]),
                                  dialect);
            }
        }
    }

    if (has_positions) {
        json positions = request.at("wipe_tower_positions");
        normalize_typed_positions(positions);
        json settings = json::object();
        settings["positions"] = positions;
        apply_tower_patch(project, settings.dump(), dialect);
    }
}

void apply_colour_overrides(json &project, const json &request,
                            std::size_t slot_count,
                            std::vector<std::string> &changed_keys) {
    for (const auto &key : {std::string("filament_colour"),
                            std::string("filament_multi_colour")}) {
        if (!request.contains(key)) {
            continue;
        }
        const auto requested = string_array(request.at(key), "request." + key, false);
        if (requested.empty()) {
            invalid("request." + key + " must contain at least one slot");
        }
        if (requested.size() > slot_count) {
            invalid("request." + key + " has more slots than filament_settings_id");
        }
        if (!project.contains(key)) {
            if (key == "filament_colour" && requested.size() != slot_count) {
                invalid("project.filament_colour is missing; provide all material slots");
            }
            project[key] = json::array();
            for (std::size_t index = 0; index < slot_count; ++index) {
                project[key].push_back("");
            }
        }
        json &destination = project[key];
        if (!destination.is_array() || destination.size() != slot_count) {
            invalid("project." + key + " slots must match filament_settings_id");
        }
        for (std::size_t index = 0; index < requested.size(); ++index) {
            destination.at(index) = requested[index];
        }
        changed_keys.push_back(key);
    }
}

}  // namespace fatcat::detail
