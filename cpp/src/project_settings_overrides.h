#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace fatcat::detail {

// Apply explicit caller values after material selection, in composition order.
void apply_scalar_overrides(nlohmann::json &project,
                            const nlohmann::json &request,
                            std::vector<std::string> &changed_keys);
void apply_tower_overrides(nlohmann::json &project,
                           const nlohmann::json &request,
                           const nlohmann::json &target);
void apply_colour_overrides(nlohmann::json &project,
                            const nlohmann::json &request,
                            std::size_t slot_count,
                            std::vector<std::string> &changed_keys);

}  // namespace fatcat::detail
