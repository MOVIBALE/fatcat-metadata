#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fatcat {

/// Assemble selected source templates, retaining their material parameters.
/// 合成已选择的源模板，并保留其材料参数。
/// Compatibility API for existing explicit template callers. Current built-in
/// exports use compose_builtin_project_settings and packaged source data.
/// 此入口仅兼容现有显式模板调用；当前内置导出使用来源数据与内置合成入口。
///
/// base_project_json is the selected built-in project template. An absent
/// registry_machine_json keeps that project; project_template_kind="none" with
/// a registry input uses the registry preset directly. The existing purge-chute
/// G-code normalization applies in all cases. Machine-only material completion
/// belongs to the project-settings composer, not this function.
/// base_project_json 是已选择的内置项目模板；无注册表输入时保留它，若有注册表
/// 输入且 project_template_kind 为 none，则直接使用注册表预设。所有路径均执行
/// 既有废料口 G-code 规范化；纯机器预设的材料补全由项目设置合成器负责。
///
/// options_json contains source facts, not vendor-specific operation switches:
/// - slicer_id: selected software ID, e.g. FlashStudio or OrcaSlicer.
/// - source_version: the registry's source version, empty when unavailable.
/// - project_template_kind: printer (the selected printer's own project),
///   compatibility (a compatible printer-family project), none (no project),
///   or minimal (the existing generic defaults when no template is available).
/// - legacy_nozzle_size: nozzle used to select the built-in project template.
/// - registry_nozzle_size: nozzle of the selected registry device.
/// - device_brand / device_display_name: the selected registry device's original
///   brand and display name retained as source context; composition follows
///   the source slot shape and target format rather than a machine-name rule.
/// Optional text fields default to empty; project_template_kind defaults to
/// printer. Minimal mode uses Generic PLA identity with the existing temperature
/// defaults, without adding hardware identity, file paths or geometry.
/// options_json 仅传递来源事实：软件 ID、来源版本、项目模板来源类别、内置模板与
/// 注册表设备各自的喷嘴尺寸，以及设备的原始品牌和显示名称。可选文本缺省为空，
/// 模板类别缺省为 printer；minimal 使用 Generic PLA 身份及既有温度默认，
/// 不补机器身份。
/// 所有模式均不合成文件路径或几何。
std::string assemble_project_template(
    std::string_view base_project_json,
    std::optional<std::string_view> registry_machine_json,
    std::string_view options_json);

/// Compose the offline registry template from selected process and source facts.
/// 根据已选择的产品工艺与来源事实合成离线注册表模板。
/// Compatibility API; current built-in exports do not invoke this legacy
/// registry/default factory. Its explicit historical options remain supported.
/// 兼容入口；当前内置导出不调用此旧注册表与默认值工厂，历史显式选项仍受支持。
/// options_json may contain protected_process_settings, line_width_settings,
/// slot_count, source_profile_name and slicer_id for process composition.
/// source_version, default_application_metadata and nozzle_size additionally
/// request the existing registry identity fields. The result contains settings
/// and application_metadata (null when no source_version was supplied).
/// 工艺输入包含保护工艺、线宽、槽数、配置名和软件标识；来源版本、默认应用标识
/// 与喷嘴尺寸用于生成既有身份字段。返回 settings 与可选 application_metadata。
std::string assemble_machine_registry_template(
    std::string_view source_settings_json,
    std::string_view options_json);

}  // namespace fatcat
