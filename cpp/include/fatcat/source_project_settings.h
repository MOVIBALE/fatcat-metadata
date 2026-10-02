#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace fatcat::detail {

// Resolve an explicit material_type first, then infer a supported preset type from the name.
std::string requested_material_type(const nlohmann::json &material);

// Material identity for a machine-only source comes from its actual default preset.
nlohmann::json prepare_source_identity(const nlohmann::json &project,
                                      const nlohmann::json &request);

// Compose from actual source settings without selecting a native hardware preset.
nlohmann::json compose_source_project(const nlohmann::json &project,
                                      const nlohmann::json &request,
                                      const nlohmann::json &target);

// Derive new-project flush defaults from the selected source's slot vectors.
void apply_source_flush_defaults(nlohmann::json &project,
                                 const nlohmann::json &request,
                                 const nlohmann::json &target);

// Resolve omitted native tower geometry for both composition and layout reads.
void apply_source_tower_defaults(nlohmann::json &project,
                                 const nlohmann::json &target);

// Use the source's actual array widths for merged slot remapping.
nlohmann::json source_merge_dialect(const nlohmann::json &dialect,
                                  const nlohmann::json &project);

// Contract wider saved palettes to their actual active source identities.
nlohmann::json prepare_source_merge_project(const nlohmann::json &project,
                                            const nlohmann::json &filament_snapshot);

// Return a fully resolved native request, or null when source settings are needed.
nlohmann::json native_source_request(const nlohmann::json &project,
                                     const nlohmann::json &request,
                                     const nlohmann::json &target);

}  // namespace fatcat::detail
