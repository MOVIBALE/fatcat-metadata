#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace fatcat {

/// Error raised when a wipe-tower request is not valid for its dialect.
class WipeTowerError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace detail {

/// Parse one locale-independent finite decimal string using the wipe-tower
/// numeric grammar.
/// 使用擦料塔相同的数字语法，按与区域设置无关的方式解析有限小数字符串。
bool parse_finite_decimal_string(std::string_view text, double &number);

}  // namespace detail

/// Apply one caller-computed wipe-tower request to project JSON.
///
/// The JSON text boundary intentionally mirrors the existing Python API. The
/// core owns parsing, validation, merging and serialization; bindings only
/// translate arguments and exceptions.
/// Optional wall_type selects only native styles declared by the dialect.
/// 可选 wall_type 仅选择目标方言已声明的原生塔壁形状，不关闭擦料塔。
std::string patch_wipe_tower(std::string_view project_json,
                             std::string_view settings_json,
                             std::string_view dialect_json);

}  // namespace fatcat
