#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace fatcat::detail::prusa {

bool is_target(const nlohmann::json &target);
nlohmann::json catalog(const std::filesystem::path &source_root);
nlohmann::json options(const nlohmann::json &request, const nlohmann::json &target,
                       const std::filesystem::path &source_root);
nlohmann::json compose_builtin(const nlohmann::json &request,
                               const nlohmann::json &target,
                               const std::filesystem::path &source_root);
nlohmann::json compose(nlohmann::json project, const nlohmann::json &request,
                       const nlohmann::json &target);
nlohmann::json describe(nlohmann::json project, const nlohmann::json &request,
                        const nlohmann::json &target);
nlohmann::json normalized_project(nlohmann::json source);
nlohmann::json summary(const nlohmann::json &project);
nlohmann::json material_slots(const nlohmann::json &project);
nlohmann::json patch_tower(nlohmann::json project, const nlohmann::json &settings);

}  // namespace fatcat::detail::prusa
