#pragma once

#include <cstddef>
#include <numeric>
#include <set>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace fatcat::detail {

inline bool filament_array(std::string_view key) {
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

// Shape validation and fallback selection remain with the calling policy.
inline void append_filament_group(nlohmann::json &output, const nlohmann::json &values,
                                  std::size_t slot, std::size_t source_width,
                                  std::size_t output_width) {
    for (std::size_t variant = 0; variant < output_width; ++variant) {
        output.push_back(values.at(slot * source_width + variant));
    }
}

inline nlohmann::json project_filament_matrix(
    const nlohmann::json &values, std::size_t source_count,
    const std::vector<std::size_t> &slots) {
    auto result = nlohmann::json::array();
    const auto pairs = source_count * source_count;
    // Each nozzle stores a complete row-major material matrix.
    for (std::size_t nozzle = 0; nozzle < values.size() / pairs; ++nozzle) {
        for (const auto row : slots) {
            for (const auto col : slots) {
                result.push_back(values.at(nozzle * pairs + row * source_count + col));
            }
        }
    }
    return result;
}

inline nlohmann::json project_filament_matrix(
    const nlohmann::json &values, std::size_t source_count, std::size_t output_count) {
    std::vector<std::size_t> slots(output_count);
    std::iota(slots.begin(), slots.end(), 0);
    return project_filament_matrix(values, source_count, slots);
}

}  // namespace fatcat::detail
