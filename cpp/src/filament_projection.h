#pragma once

#include <cstddef>
#include <numeric>
#include <vector>

#include <nlohmann/json.hpp>

namespace fatcat::detail {

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
