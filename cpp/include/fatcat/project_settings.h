#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace fatcat {

/// Error raised when project-settings inputs or target data are invalid.
/// 项目设置输入或目标数据无效时抛出。
class ProjectSettingsError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Compose one final project-settings object and its effective-settings summary.
/// 合成最终项目设置对象，并返回由同一结果生成的有效配置摘要。
///
/// The returned JSON object contains `project_settings_json` and
/// `effective_settings`, plus target-owned `metadata_defaults` for the selected
/// plate, single-object identity, and optional metadata inputs.
/// 返回值另含目标定义的 metadata_defaults，描述所选打印板、单对象身份和可选元数据输入。
/// `request_json` explicitly selects either
/// `preserve_template` or `target_native_preset`; the two data JSON arguments
/// are interpreted by the C++ core rather than by a Python/Rust helper.
/// Optional slot selection defaults to preserve; compact requests supply an
/// explicit default source slot and may select material sources per output slot.
/// 槽位模式默认保留；compact 请求显式提供默认来源，并可逐槽选择材料来源。
/// Native mode accepts per-final-slot `material_uids` or scalar `material_uid`,
/// exclusively; preserve mode accepts neither and needs no native bindings.
/// 原生模式二选一接收逐最终槽 material_uids 或同料 material_uid；保留模式均不接收，
/// 且无需原生材料绑定。材料选择不修改打印机或工艺身份。
/// `hardware_mode=preserve_source` retains the actual source hardware and uses
/// `source_materials` facts, `process_settings`, and an auto/preserve/compact
/// slot policy. Imported tuning is retained with `preserve_source_material_settings`.
/// `hardware_mode=auto` selects exact native hardware/material bindings when
/// available and otherwise uses that source mode; imported tuning always uses source mode.
/// preserve_source 保留来源硬件，按材料事实、工艺选择和槽位策略合成；auto 自动选择
/// 完整匹配的原生绑定，无法匹配或使用已导入调校时采用来源设置。
std::string compose_project_settings(std::string_view base_project_json,
                                     std::string_view request_json,
                                     std::string_view canonical_json,
                                     std::string_view target_json);

}  // namespace fatcat
