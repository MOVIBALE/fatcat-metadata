#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace fatcat {

/// Read public machine-source identities and effective hardware facts for a
/// packaged target, without exposing the installed source-file layout.
std::string native_project_source_catalog(
    std::string_view slicer_id,
    std::string_view application_version,
    const std::filesystem::path &data_root);

/// Compose a project from an explicitly selected, bundled native profile source.
/// The existing four-JSON-argument composer remains the source-project entry.
std::string compose_builtin_project_settings(
    std::string_view request_json,
    std::string_view canonical_json,
    std::string_view target_json,
    const std::filesystem::path &data_root);

}  // namespace fatcat
