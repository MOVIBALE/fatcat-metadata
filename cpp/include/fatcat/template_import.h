#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fatcat {

/// Error raised while interpreting a supported user project template.
/// 解析受支持的用户项目模板时抛出的错误。
class TemplateImportError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Return the target-described text member paths needed by template import.
/// 返回目标描述中模板导入所需的文本成员路径。
std::string template_import_parts(std::string_view target_json);

/// Resolve one effective template plate from target bindings and source values.
/// 根据目标打印板映射与来源值确定一个有效模板打印板。
std::string resolve_template_build_plate(
    std::string_view target_json,
    std::string_view default_build_plate_uid,
    std::optional<std::string_view> model_plate_value,
    std::optional<std::string_view> sidecar_bed_value,
    std::optional<std::string_view> project_bed_value);

/// Compare an imported template with the selected slicer and complete hardware.
/// 比较导入模板与所选切片软件、打印机和完整喷嘴配置。
void validate_template_hardware(std::string_view template_json,
                                std::string_view expected_project_json,
                                std::string_view source_slicer,
                                std::string_view selected_slicer);

/// Identify a supported source slicer and its file-declared version.
/// 识别受支持的来源切片软件及文件实际声明的版本。
std::string detect_template_source(
    std::optional<std::string_view> source_model_xml,
    std::optional<std::string_view> slice_info_xml,
    std::string_view bambu_target_json,
    std::string_view orca_target_json,
    std::string_view qidi_target_json,
    std::string_view elegoo_target_json,
    std::string_view anycubic_target_json,
    std::string_view snapmaker_target_json);

/// Identify source markers using the selected dialect for declared versions.
/// 识别来源标记，并用所选方言解析文件声明的版本。
std::string detect_template_source(
    std::optional<std::string_view> source_model_xml,
    std::optional<std::string_view> slice_info_xml,
    std::string_view target_json);

/// Interpret project settings and plate values without changing user tuning.
/// 解释项目设置和板级值，同时保留用户调校原值。
std::string import_template_metadata(
    std::string_view project_settings_json,
    std::optional<std::string_view> source_model_xml,
    std::optional<std::string_view> slice_info_xml,
    std::optional<std::string_view> model_settings_xml,
    std::optional<std::string_view> plate_sidecar_json,
    std::string_view selected_target_json,
    std::string_view bambu_target_json,
    std::string_view orca_target_json,
    std::string_view qidi_target_json,
    std::string_view elegoo_target_json,
    std::string_view anycubic_target_json,
    std::string_view snapmaker_target_json,
    bool explicit_slicer_hint);

}  // namespace fatcat
