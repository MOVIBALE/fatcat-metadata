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

/// Load one exact target from the public packaged data root. An empty version
/// selects the unique installed version. The library owns the file layout.
/// 从公开数据根加载唯一目标，内部文件布局由库维护。
std::string metadata_target_data(std::string_view slicer_id,
                                 std::string_view application_version,
                                 const std::filesystem::path &data_root);

/// Compose bundled native/recorded compatibility sources without private JSON
/// inputs. 保持同一合成路径，调用者无需读取内部目标或 canonical 文件。
std::string compose_builtin_project_settings(
    std::string_view request_json, const std::filesystem::path &data_root);

/// Compose an explicit source using library-owned target/canonical loading.
/// 显式用户来源共用库拥有的目标加载规则。
std::string compose_project_settings_from_data(
    std::string_view project_json, std::string_view request_json,
    const std::filesystem::path &data_root);

/// Describe final model metadata using the same library-owned target lookup.
/// 最终对象元数据使用相同的公共目标选择。
std::string compose_model_metadata_from_data(
    std::string_view project_json, std::string_view request_json,
    const std::filesystem::path &data_root);

}  // namespace fatcat
