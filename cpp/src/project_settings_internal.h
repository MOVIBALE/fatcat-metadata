#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

#include "fatcat/project_settings.h"
#include "difference_index.h"

namespace fatcat::detail::project_settings_internal {

using json = nlohmann::json;

// Shared project-field contracts; selection and composition belong to callers.
[[noreturn]] inline void invalid(std::string message) {
    throw ProjectSettingsError(std::move(message));
}

inline const json &required_member(const json &object, std::string_view key,
                            std::string_view context) {
    if (!object.contains(key)) {
        invalid(std::string(context) + " is missing required field '" +
                std::string(key) + "'");
    }
    return object.at(key);
}

inline std::string required_string(const json &object, std::string_view key,
                            std::string_view context) {
    const json &value = required_member(object, key, context);
    if (!value.is_string() || value.get<std::string>().empty()) {
        invalid(std::string(context) + "." + std::string(key) +
                " must be non-empty text");
    }
    return value.get<std::string>();
}

inline bool contains_string(const json &array, const std::string &value) {
    if (!array.is_array()) {
        return false;
    }
    return std::any_of(array.begin(), array.end(), [&](const json &item) {
        return item.is_string() && item.get<std::string>() == value;
    });
}

inline std::vector<std::string> string_array(const json &value,
                                      std::string_view context,
                                      bool allow_empty = true) {
    if (!value.is_array()) {
        invalid(std::string(context) + " must be an array");
    }
    std::vector<std::string> values;
    values.reserve(value.size());
    for (const json &item : value) {
        if (!item.is_string() || (!allow_empty && item.get<std::string>().empty())) {
            invalid(std::string(context) + " must contain text values");
        }
        values.push_back(item.get<std::string>());
    }
    return values;
}

inline std::vector<std::string> required_string_array(const json &object,
                                               std::string_view key,
                                               std::string_view context) {
    return string_array(required_member(object, key, context),
                        std::string(context) + "." + std::string(key), false);
}

inline void validate_slots(const json &project, std::size_t &slot_count) {
    const auto ids = required_string_array(project, "filament_settings_id", "project");
    const auto types = required_string_array(project, "filament_type", "project");
    slot_count = ids.size();
    if (slot_count == 0) {
        invalid("project filament_settings_id must contain at least one slot");
    }
    if (types.size() != slot_count) {
        invalid("project.filament_type slots must match filament_settings_id");
    }

    // Only fields with an explicit per-slot contract are aligned here. Vendor
    // and compatible-printer descriptions are allowed to be absent or empty.
    for (const auto &key : {std::string("filament_ids"),
                            std::string("filament_vendor"),
                            std::string("filament_compatible_printers"),
                            std::string("filament_colour"),
                            std::string("filament_multi_colour")}) {
        if (!project.contains(key)) {
            continue;
        }
        const auto values = string_array(
            project.at(key), "project." + key, key != "filament_ids");
        if (values.size() != slot_count) {
            invalid("project." + key + " slots must match filament_settings_id");
        }
    }
}

inline void validate_source_hardware(const json &project, const json &machine) {
    if (!project.contains("printer_model") ||
        project.at("printer_model") != required_member(machine, "printer_model", "machine")) {
        invalid("source project machine model does not match the exact target machine");
    }
    if (!project.contains("nozzle_diameter") ||
        project.at("nozzle_diameter") != required_member(machine, "nozzle_diameter", "machine")) {
        invalid("source project machine/nozzle does not match the exact target nozzle");
    }
}

inline void validate_preserved_build_plate(const json &project, const json &plate) {
    if (!project.contains("curr_bed_type")) {
        return;
    }
    if (!project.at("curr_bed_type").is_string()) {
        invalid("project.curr_bed_type must be text in preserve_template mode");
    }
    const std::string target_plate = required_string(plate, "project_value", "build plate");
    if (project.at("curr_bed_type").get<std::string>() != target_plate) {
        invalid("preserve_template build plate conflicts with the selected target plate");
    }
}

inline void record_machine_override(json &project, const json &snapshot,
                              std::size_t slot_count, const std::string &field) {
    const auto key = required_string(snapshot, "difference_list_key", "filament snapshot");
    const auto offset = required_member(snapshot, "filament_slot_offset", "filament snapshot").get<std::size_t>();
    const auto trailing = required_member(snapshot, "trailing_entry_count", "filament snapshot").get<std::size_t>();
    if (trailing == 0) {
        invalid("filament snapshot has no machine entry for the requested machine override");
    }
    auto &entry = project.at(key).at(offset + slot_count);
    auto tokens = difference_tokens(entry.get<std::string>());
    tokens.insert(field);
    entry = render_difference_tokens(tokens);
}

}  // namespace fatcat::detail::project_settings_internal
