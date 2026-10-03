#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace fatcat {

/// Error raised when metadata-component inputs or target data are invalid.
/// 元数据部件输入或目标数据无效时抛出。
class MetadataComponentsError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Serialize existing layer-range business data using the shared XML writer.
/// 使用共享 XML 写入器序列化现有层高区间业务数据。
std::string serialize_layer_config_ranges(std::string_view data_json,
                                          double fine_layer_height_mm);

/// Build the target's complete metadata parts, relationships, content types,
/// and structured root description for a final project-settings document.
/// 根据最终项目设置生成目标软件的完整元数据部件、关系、内容类型和结构化根描述。
///
/// `project_json` must already be the final project-settings result. This API
/// uses those settings without reapplying native material presets or copying
/// objects from a template.
/// `project_json` 必须是最终项目设置；本接口不会重新应用原生材料预设或复制模板对象。
std::string compose_model_metadata(std::string_view project_json,
                                   std::string_view request_json,
                                   std::string_view target_json);

}  // namespace fatcat
