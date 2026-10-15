#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace fatcat::detail {

// One native material policy shared by options, preflight and composition.
std::string native_profile_material_type(const nlohmann::json &profile,
                                         const std::string &profile_name);
bool native_type_matches_request(const std::string &requested_type,
                                 const std::string &native_type,
                                 const std::string &profile_name,
                                 const std::string &material_name);
bool material_compatible_with_machine(const nlohmann::json &profile,
                                     const std::string &machine_name);
bool material_supports_plate(const nlohmann::json &profile,
                             const nlohmann::json &plate);

nlohmann::json resolve_native_material_selection(
    const nlohmann::json &request, const nlohmann::json &source,
    const nlohmann::json &target, const nlohmann::json &materials,
    const nlohmann::json &plate, const nlohmann::json &profiles,
    const nlohmann::json &build_plates);

}  // namespace fatcat::detail
