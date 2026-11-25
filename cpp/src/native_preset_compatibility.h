#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace fatcat::detail {

// Native system presets use an explicit printer list, or a condition when the
// list is empty. A system machine's inherited parent is not an alternate ID.
bool preset_compatible_with_printer(const nlohmann::json &profile,
                                    const nlohmann::json &machine,
                                    const std::string &machine_name,
                                    const nlohmann::json &field_definitions);

}  // namespace fatcat::detail
