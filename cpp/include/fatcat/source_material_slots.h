#pragma once

#include <stdexcept>
#include <optional>
#include <string>
#include <string_view>

namespace fatcat {

/// Error raised while reading supported source material-slot metadata.
/// 读取受支持源文件耗材槽元数据时抛出的错误。
class SourceMaterialSlotsError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Return the existing target roles and package paths for source settings.
/// 返回目标数据中现有的源设置 role 与包内路径。
/// @param target_json Registered target data from the packaged slicer/version.
///                    随包提供的切片软件/版本目标数据。
/// @return JSON object containing only the `project_settings` and
///         `model_settings` roles and paths. (只含两个 role/path 的 JSON 对象。)
std::string source_material_settings_parts(std::string_view target_json);

/// Extract canonical source material slots from project settings and model XML.
/// 从项目设置与模型 XML 中提取规范化的源耗材槽记录。
///
/// The returned JSON array keeps the source slot order and contains only the
/// four sidecar fields: `slot_id`, `slot_name`, `preview_color`, and
/// `material_id`.
/// 返回的 JSON 数组保留源槽顺序，且只包含 sidecar 的四个字段。
/// @param project_json UTF-8 `Metadata/project_settings.config` text.
///                     UTF-8 项目设置文本。
/// @param model_settings_xml UTF-8 `Metadata/model_settings.config` text.
///                           UTF-8 模型设置文本。
/// @param target_json Registered target data from the packaged slicer/version.
///                    随包提供的切片软件/版本目标数据。
/// @return JSON array matching the four-field sidecar contract.
///         符合四字段 sidecar 合同的 JSON 数组。
std::string extract_source_material_slots(std::string_view project_json,
                                         std::string_view model_settings_xml,
                                         std::string_view target_json);

/// Read source metadata without interpreting mesh geometry or hardware bindings.
/// 读取来源元数据，不解析网格几何，也不要求来源硬件具有原生绑定。
/// @return JSON with project_settings, slots, source_root_metadata,
///         model_settings (objects/plates), slice_headers, layout and warnings.
///         返回项目、槽位、根元数据、模型/盘元数据、切片头、布局事实及警告。
std::string read_source_metadata(
    std::string_view project_json,
    std::string_view model_settings_xml,
    std::string_view target_json,
    std::optional<std::string_view> source_model_xml = std::nullopt,
    std::optional<std::string_view> slice_info_xml = std::nullopt,
    std::optional<std::string_view> placement_json = std::nullopt);

/// Decode slicer project fields into numeric layout inputs; does not place models.
/// 将切片软件项目字段解码为数值布局输入，不计算模型摆放。
std::string read_project_layout(std::string_view project_json,
                                std::string_view target_json);

/// Read Lumina placement warning codes from an optional metadata JSON part.
/// 从可选摆放元数据 JSON 部件中读取 Lumina 警告代码。
std::string read_placement_warnings(
    std::optional<std::string_view> placement_json);

/// Read external model object metadata, leaving mesh contents uninterpreted.
/// 读取外部模型对象元数据，不解释网格内容。
std::string read_model_object_metadata(std::string_view model_xml);

}  // namespace fatcat
