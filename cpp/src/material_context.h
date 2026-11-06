#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "fatcat/project_settings.h"

namespace fatcat::detail {

struct MaterialRange {
    std::size_t begin;
    std::size_t size;
};

// All readers consume ranges, independently of how their boundaries were found.
struct MaterialLayout {
    std::vector<MaterialRange> slots;
    bool native_groups = false;

    nlohmann::json group(const nlohmann::json &values, std::size_t slot) const {
        const auto range = slots.at(slot);
        return nlohmann::json::array_t(values.begin() + range.begin,
                                      values.begin() + range.begin + range.size);
    }
};

struct MaterialFieldRule {
    bool material_source_override;
    std::string selection;
    std::size_t group_size;
};

// Per-composition knowledge. Never serialized into a target or exported project.
class MaterialContext {
public:
    explicit MaterialContext(const nlohmann::json &snapshot);
    bool is_material(std::string_view key) const;
    bool copies_material(std::string_view key) const;
    const std::map<std::string, MaterialFieldRule, std::less<>> &rules() const { return rules_; }
    void record_profile(const nlohmann::json &profile, const std::string &name);
    MaterialLayout layout(const nlohmann::json &project, const std::string &key,
                          std::size_t count, bool broadcast = false,
                          std::size_t declared_width = 1, bool variable = true) const;

private:
    std::map<std::string, MaterialFieldRule, std::less<>> rules_;
    std::map<std::string, std::map<std::string, std::size_t>> native_widths_;
    nlohmann::json defaults_;
};

// Private overloads carry runtime knowledge while installed API signatures stay stable.
std::string compose_project_with_context(nlohmann::json project, nlohmann::json request,
    const nlohmann::json &canonical, const nlohmann::json &target,
    const MaterialContext &materials);
nlohmann::json compose_source_project(const nlohmann::json &project,
    const nlohmann::json &request, const nlohmann::json &target, const MaterialContext &materials);
nlohmann::json prepare_source_merge_project(const nlohmann::json &project,
    const nlohmann::json &snapshot, const MaterialContext &materials);
nlohmann::json source_merge_dialect(const nlohmann::json &dialect,
    const nlohmann::json &project, const MaterialContext &materials);

}  // namespace fatcat::detail
