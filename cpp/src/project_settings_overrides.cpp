#include "project_settings_overrides.h"

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

void validate_positive_number(const json &request, const std::string &key) {
    if (!request.at(key).is_string()) {
        invalid("request." + key + " must be text");
    }
    if (!(numeric_value(request.at(key).get<std::string>(), key) > 0.0)) {
        invalid("request." + key + " must be finite and positive");
    }
}

void validate_density(const json &request) {
    const std::string value = request.at("sparse_infill_density").get<std::string>();
    if (value.empty()) {
        invalid("request.sparse_infill_density must be a finite percentage");
    }
    const bool percent = value.back() == '%';
    const std::string numeric = percent ? value.substr(0, value.size() - 1) : value;
    const double density = numeric_value(numeric, "sparse_infill_density");
    if (density < 0.0 || density > 100.0) {
        invalid("request.sparse_infill_density must be between 0% and 100%");
    }
}

}  // namespace

void apply_scalar_overrides(json &project, const json &request,
                            std::vector<std::string> &changed_keys) {
    static const std::vector<std::string> scalar_keys = {
        "layer_height",
        "initial_layer_print_height",
        "initial_layer_height",
        "line_width",
        "initial_layer_line_width",
        "inner_wall_line_width",
        "outer_wall_line_width",
        "top_surface_line_width",
        "sparse_infill_line_width",
        "internal_solid_infill_line_width",
        "support_line_width",
        "wall_loops",
        "top_shell_layers",
        "bottom_shell_layers",
        "bottom_surface_pattern",
        "elefant_foot_compensation",
        "sparse_infill_density",
        "sparse_infill_pattern",
        "print_speed",
        "travel_speed",
        "enable_support",
        "single_extruder_multi_material",
        "precise_outer_wall",
        "brim_type",
        "brim_width",
        "wall_generator",
        "skirt_loops",
        "skirt_distance",
        "skirt_height",
        "draft_shield",
    };
    for (const auto &key : scalar_keys) {
        if (!request.contains(key)) {
            continue;
        }
        if (!request.at(key).is_string()) {
            invalid("request." + key + " must be text");
        }
        if (key == "layer_height" || key == "initial_layer_print_height" ||
            key == "initial_layer_height") {
            validate_positive_number(request, key);
        } else if (key == "sparse_infill_density") {
            validate_density(request);
        } else if (key == "sparse_infill_pattern" &&
                   request.at(key).get<std::string>().empty()) {
            invalid("request.sparse_infill_pattern must be non-empty text");
        } else if (key == "brim_type" && request.at(key).get<std::string>().empty()) {
            invalid("request.brim_type must be non-empty text");
        } else if (key == "brim_width") {
            const double width = numeric_value(request.at(key).get<std::string>(), key);
            if (width < 0.0) {
                invalid("request.brim_width must be finite and non-negative");
            }
        }
        project[key] = request.at(key);
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
