#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace fatcat {

/// List installed slicer/version identities without exposing source paths.
/// 查询已安装的软件与版本标识，不暴露内部文件布局。
std::string metadata_target_catalog(const std::filesystem::path &data_root);

/// Query versioned native-field evidence and process-override coverage.
/// 查询指定版本的原生字段证据与工艺覆盖范围；清单外字段不等于软件不支持。
std::string native_field_inventory(std::string_view slicer_id,
                                    std::string_view application_version,
                                    const std::filesystem::path &data_root);

/// List exact process/material/plate choices for the requested machine/nozzle.
/// 查询指定机型与喷嘴的工艺、材料和板型；不可用候选保留原因。
/// Optional build_plate_uid filters material availability using the composer.
/// 可选 build_plate_uid 使用合成器的同一规则判断材料可用性。
std::string native_project_options(std::string_view request_json,
                                   const std::filesystem::path &data_root);

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
