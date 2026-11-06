#include "material_context.h"

#include <set>

namespace fatcat::detail {
namespace {
// Compatibility for imported fields absent from the target contract.
bool legacy_material_field(std::string_view key) {
    if (key.rfind("filament_", 0) == 0) return true;
    static const std::set<std::string_view> resizable_keys = {
        "nozzle_temperature", "nozzle_temperature_initial_layer",
        "nozzle_temperature_range_low", "nozzle_temperature_range_high",
        "bed_temperature", "bed_temperature_initial_layer", "activate_air_filtration",
        "additional_cooling_fan_speed", "additional_fan_full_speed_layer",
        "chamber_temperatures", "circle_compensation_speed",
        "close_additional_fan_first_x_layers", "close_fan_the_first_x_layers",
        "complete_print_exhaust_fan_speed", "during_print_exhaust_fan_speed",
        "cool_plate_temp", "cool_plate_temp_initial_layer",
        "cooling_perimeter_transition_distance", "cooling_slowdown_logic",
        "counter_coef_1", "counter_coef_2", "counter_coef_3", "counter_limit_max",
        "counter_limit_min", "default_filament_colour", "diameter_limit",
        "enable_overhang_bridge_fan", "enable_pressure_advance", "eng_plate_temp",
        "eng_plate_temp_initial_layer", "fan_cooling_layer_time", "fan_max_speed",
        "fan_min_speed", "first_x_layer_fan_speed", "first_x_layer_part_fan_speed",
        "full_fan_speed_layer", "hole_coef_1", "hole_coef_2", "hole_coef_3",
        "hole_limit_max", "hole_limit_min", "hot_plate_temp",
        "hot_plate_temp_initial_layer", "impact_strength_z", "ironing_fan_speed",
        "no_slow_down_for_cooling_on_outwalls", "overhang_fan_speed",
        "overhang_fan_threshold", "overhang_threshold_participating_cooling",
        "pre_start_fan_time", "pressure_advance", "reduce_fan_stop_start_freq",
        "required_nozzle_HRC", "slow_down_for_layer_cooling", "slow_down_layer_time",
        "slow_down_min_speed",
        "supertack_plate_temp", "supertack_plate_temp_initial_layer",
        "temperature_vitrification", "textured_plate_temp",
        "textured_plate_temp_initial_layer",
    };
    return resizable_keys.count(key) != 0;
}

}  // namespace

MaterialContext::MaterialContext(const nlohmann::json &snapshot) {
    for (const auto &rule : snapshot.at("array_rules")) {
        if (rule.value("merged_only", false)) continue;
        for (const auto &key : rule.at("keys")) {
            rules_[key.get<std::string>()] = {
                rule.value("material_source_override", false),
                rule.value("selection", std::string()), rule.value("group_size", std::size_t{1})};
        }
    }
    const auto defaults = snapshot.find("native_schema_defaults");
    if (defaults != snapshot.end()) defaults_ = defaults->value("values", nlohmann::json::object());
}

bool MaterialContext::is_material(std::string_view key) const {
    const auto rule = rules_.find(key);
    if (rule != rules_.end()) {
        return rule->second.material_source_override || rule->second.selection == "slot_index" ||
               key == "filament_colour" || key == "filament_multi_colour";
    }
    return legacy_material_field(key);
}

bool MaterialContext::copies_material(std::string_view key) const {
    const auto rule = rules_.find(key);
    return rule == rules_.end() ? legacy_material_field(key) : rule->second.material_source_override;
}

void MaterialContext::record_profile(const nlohmann::json &profile, const std::string &name) {
    auto resolved = defaults_.is_object() ? defaults_ : nlohmann::json::object();
    for (const auto &[key, value] : profile.items()) {
        if (!value.is_array() || !value.empty()) resolved[key] = value;
    }
    for (const auto &[key, value] : resolved.items()) {
        if (is_material(key) && value.is_array() && !value.empty()) {
            native_widths_[key][name] = value.size();
        }
    }
}

MaterialLayout MaterialContext::layout(const nlohmann::json &project, const std::string &key,
    std::size_t count, bool broadcast, std::size_t declared_width, bool variable) const {
    const auto value = project.find(key);
    const auto fail = [&]() -> MaterialLayout {
        throw ProjectSettingsError("material field " + key +
            " has an unresolved slot shape; provide complete native preset identities or equal per-slot groups");
    };
    if (value == project.end() || !value->is_array() || value->empty() || count == 0) return fail();
    const auto widths = native_widths_.find(key);
    const auto identities = project.find("filament_settings_id");
    if (widths != native_widths_.end() && identities != project.end() &&
        identities->is_array() && identities->size() == count) {
        MaterialLayout result;
        std::size_t offset = 0;
        for (const auto &identity : *identities) {
            if (!identity.is_string()) break;
            const auto width = widths->second.find(identity.get<std::string>());
            if (width == widths->second.end() || width->second > value->size() - offset) break;
            result.slots.push_back({offset, width->second});
            offset += width->second;
        }
        if (result.slots.size() == count && offset == value->size()) {
            result.native_groups = true;
            return result;
        }
    }
    const bool singleton = broadcast && value->size() == 1;
    if (!singleton && (value->size() % count != 0 ||
        (!variable && value->size() / count != 1 && value->size() / count != declared_width))) return fail();
    const auto width = singleton ? 1 : value->size() / count;
    MaterialLayout result;
    for (std::size_t slot = 0; slot < count; ++slot) {
        result.slots.push_back({singleton ? 0 : slot * width, width});
    }
    return result;
}

}  // namespace fatcat::detail
