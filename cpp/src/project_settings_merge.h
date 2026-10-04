#pragma once

#include <cstddef>
#include <nlohmann/json.hpp>

namespace fatcat::detail {

// Only the completed merge crosses this boundary; source indices stay private.
struct MergedProjectSettings {
    nlohmann::json project;
    nlohmann::json logical_slots;
    nlohmann::json source_slot_mappings;
    std::size_t slot_count;
};

MergedProjectSettings merge_project_settings(
    nlohmann::json project, const nlohmann::json &request,
    const nlohmann::json &dialect, const nlohmann::json &machine,
    const nlohmann::json &plate);

}  // namespace fatcat::detail
