#include "fatcat/template_assembly.h"
#include "difference_index.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace fatcat {
namespace {

using json = nlohmann::json;

const std::set<std::string> identity_overlay_keys = {
    "printer_settings_id", "printer_variant", "nozzle_diameter",
    "printable_area", "printable_height", "bed_exclude_area",
};

const std::set<std::string> machine_overlay_keys = {
    "best_object_pos", "change_filament_gcode", "default_filament_profile",
    "default_nozzle_volume_type", "default_print_profile", "default_wipe_tower_pos",
    "deretract_speed_extruder_change", "deretraction_speed", "extruder_colour",
    "fan_direction", "gcode_flavor", "grab_length", "group_algo_with_time",
    "head_wrap_detect_zone", "hotend_cooling_rate", "hotend_heating_rate",
    "layer_change_gcode", "long_retractions_when_cut", "master_extruder_id",
    "nozzle_diameter", "nozzle_flush_dataset", "nozzle_height", "nozzle_type",
    "nozzle_volume", "physical_extruder_map", "printable_area", "printable_height",
    "printer_extruder_id", "printer_extruder_variant", "printer_model",
    "printer_settings_id", "printer_structure", "printer_technology", "printer_variant",
    "retract_before_wipe", "retract_length_toolchange", "retract_lift_above",
    "retract_lift_below", "retract_restart_extra", "retract_restart_extra_toolchange",
    "retract_when_changing_layer", "retraction_distances_when_cut", "retraction_length",
    "retraction_minimum_travel", "retraction_speed", "scan_first_layer", "silent_mode",
    "single_extruder_multi_material", "support_air_filtration",
    "support_chamber_temp_control", "support_cooling_filter", "support_fast_purge_mode",
    "support_object_skip_flush", "time_lapse_gcode", "upward_compatible_machine",
    "wipe", "wipe_distance", "wrapping_detection_gcode", "wrapping_exclude_area",
    "z_hop", "z_hop_types",
};

const std::set<std::string> project_profile_keys = {
    "default_filament_profile", "default_print_profile",
};

const std::set<std::string> nozzle_label_keys = {
    "compatible_printers", "default_filament_profile", "default_print_profile",
    "filament_compatible_printers", "filament_settings_id", "inherits_group",
    "print_compatible_printers", "print_settings_id",
};

std::string text(const json &value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return "None";
    if (value.is_boolean()) return value.get<bool>() ? "True" : "False";
    return value.dump();
}

bool has_value(const json &value) {
    if (value.is_null()) return false;
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number()) return value != 0;
    if (value.is_string()) return !value.get_ref<const std::string &>().empty();
    return !value.empty();
}

std::string text_or_empty(const json &value) {
    return has_value(value) ? text(value) : std::string();
}

std::string trim(std::string value) {
    const auto whitespace = [](unsigned char c) { return std::isspace(c) != 0; };
    const auto first = std::find_if_not(value.begin(), value.end(), whitespace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), whitespace).base();
    return first < last ? std::string(first, last) : std::string();
}

std::string option(const json &options, const char *key) {
    return text_or_empty(options.value(key, json()));
}

json clean_overlay_value(const std::string &key, const json &value) {
    if (key != "printable_area" || !value.is_array()) return value;
    json result = json::array();
    for (const auto &item : value) result.push_back(trim(text(item)));
    return result;
}

void copy_keys(json &project, const json &machine, const std::set<std::string> &keys) {
    for (const auto &key : keys) {
        if (machine.contains(key)) project[key] = clean_overlay_value(key, machine.at(key));
    }
}

void collect_difference_keys(const json &value, std::set<std::string> &keys) {
    if (value.is_null()) return;
    if (value.is_array()) {
        for (const auto &item : value) {
            const auto entry = text(item);
            for (const auto &token : detail::split_difference_tokens(entry)) {
                const auto key = trim(token);
                if (!key.empty()) keys.insert(key);
            }
        }
        return;
    }
    collect_difference_keys(json::array({value}), keys);
}

void merge_difference_index(json &project, const json &machine) {
    if (!machine.contains("different_settings_to_system")) return;
    std::set<std::string> keys;
    collect_difference_keys(machine.at("different_settings_to_system"), keys);
    const auto existing = project.value("different_settings_to_system", json());
    json slots = json::array();
    if (existing.is_array()) {
        for (const auto &slot : existing) slots.push_back(text_or_empty(slot));
        if (slots.empty() && keys.empty()) {
            project["different_settings_to_system"] = slots;
            return;
        }
        if (slots.empty()) slots.push_back("");
        collect_difference_keys(slots.at(0), keys);
        slots[0] = detail::render_difference_tokens(keys);
    } else {
        collect_difference_keys(existing, keys);
        if (!keys.empty()) slots.push_back(detail::render_difference_tokens(keys));
    }
    project["different_settings_to_system"] = std::move(slots);
}

void align_default_filament(json &project, const json &machine) {
    auto profiles = machine.value("filament_settings_id", json());
    if (!has_value(profiles)) profiles = machine.value("default_filament_profile", json());
    std::string profile;
    if (profiles.is_array() && !profiles.empty()) profile = trim(text_or_empty(profiles.at(0)));
    else if (profiles.is_string()) profile = trim(profiles.get<std::string>());
    if (profile.empty()) return;
    auto existing = project.find("filament_settings_id");
    if (existing == project.end()) return;
    if (existing->is_array() && !existing->empty()) {
        for (auto &slot : *existing) slot = profile;
    } else if (existing->is_string() && !trim(existing->get<std::string>()).empty()) {
        *existing = profile;
    }
}

void replace_nozzle_label(json &value, const std::string &old_label,
                          const std::string &new_label) {
    if (value.is_string()) {
        auto changed = value.get<std::string>();
        std::size_t offset = 0;
        while ((offset = changed.find(old_label, offset)) != std::string::npos) {
            changed.replace(offset, old_label.size(), new_label);
            offset += new_label.size();
        }
        value = std::move(changed);
    } else if (value.is_array() || value.is_object()) {
        for (auto &item : value) replace_nozzle_label(item, old_label, new_label);
    }
}

void retarget_nozzle_labels(json &project, const json &machine, const json &options) {
    const auto previous = option(options, "legacy_nozzle_size");
    const auto selected = option(options, "registry_nozzle_size");
    if (previous == selected) return;
    auto model = text_or_empty(machine.value("printer_model", json()));
    if (model.empty()) model = text_or_empty(project.value("printer_model", json()));
    model = trim(model);
    if (model.empty()) return;
    copy_keys(project, machine, project_profile_keys);
    const auto old_label = model + " " + previous + " nozzle";
    const auto new_label = model + " " + selected + " nozzle";
    for (const auto &key : nozzle_label_keys) {
        if (project.contains(key)) replace_nozzle_label(project[key], old_label, new_label);
    }
    align_default_filament(project, machine);
}

std::vector<std::string> version_parts(const json &value) {
    const auto version = text_or_empty(value);
    std::vector<std::string> parts;
    std::size_t start = 0;
    do {
        const auto end = version.find('.', start);
        auto part = version.substr(start, end - start);
        if (part.empty() || !std::all_of(part.begin(), part.end(),
                [](unsigned char c) { return std::isdigit(c) != 0; })) return {};
        const auto significant = part.find_first_not_of('0');
        parts.push_back(significant == std::string::npos ? "0" : part.substr(significant));
        if (end == std::string::npos) break;
        start = end + 1;
    } while (start <= version.size());
    return parts;
}

bool use_registry_version(const json &current, const std::string &source) {
    const auto selected = version_parts(source);
    const auto previous = version_parts(current);
    if (selected.empty()) return false;
    if (previous.empty()) return true;
    const auto less = [](const std::string &left, const std::string &right) {
        return left.size() != right.size() ? left.size() < right.size() : left < right;
    };
    return !std::lexicographical_compare(selected.begin(), selected.end(),
                                          previous.begin(), previous.end(), less);
}

void mark_purge_chute_flush(json &project) {
    const auto found = project.find("change_filament_gcode");
    if (found == project.end() || !found->is_string()) return;
    const auto &gcode = found->get_ref<const std::string &>();
    constexpr std::string_view marker = "Compensate for filament spillage during waiting temperature";
    if (gcode.find(marker) == std::string::npos) return;
    std::string normalized;
    bool active = false;
    std::size_t start = 0;
    while (start < gcode.size()) {
        auto end = gcode.find_first_of("\r\n", start);
        if (end == std::string::npos) end = gcode.size();
        else if (gcode[end] == '\r' && end + 1 < gcode.size() && gcode[end + 1] == '\n') end += 2;
        else ++end;
        const auto line = gcode.substr(start, end - start);
        const auto stripped = trim(line);
        if (stripped == "; FLUSH_START") active = true;
        else if (stripped == "; FLUSH_END") active = false;
        if (line.find(marker) != std::string::npos && !active) {
            const auto newline = line.size() >= 2 && line.compare(line.size() - 2, 2, "\r\n") == 0
                ? "\r\n" : "\n";
            normalized += std::string("; FLUSH_START") + newline;
            normalized += line;
            normalized += std::string("; FLUSH_END") + newline;
        } else normalized += line;
        start = end;
    }
    *found = std::move(normalized);
}

json merge_templates(json project, const json &machine, const json &options,
                      const std::string &kind) {
    const bool flash = trim(option(options, "slicer_id")) == "FlashStudio";
    const bool compatibility = kind == "compatibility";
    copy_keys(project, machine, identity_overlay_keys);
    if (flash || compatibility) {
        for (const auto &[key, value] : machine.items()) {
            if (flash || machine_overlay_keys.count(key) ||
                key.rfind("extruder_", 0) == 0 || key.rfind("machine_", 0) == 0) {
                project[key] = clean_overlay_value(key, value);
            }
        }
        merge_difference_index(project, machine);
        align_default_filament(project, machine);
    }
    retarget_nozzle_labels(project, machine, options);
    if (compatibility && machine.contains("printer_model")) project["printer_model"] = machine.at("printer_model");
    const auto source_version = flash ? std::string() : option(options, "source_version");
    if (!source_version.empty() &&
        (compatibility || use_registry_version(project.value("version", json()), source_version))) {
        project["version"] = source_version;
    }
    for (const auto *key : {"type", "instantiation", "inherits"}) project.erase(key);
    project["name"] = "project_settings";
    project["from"] = "project";
    return project;
}

bool meaningful_profile_value(const json &value) {
    const auto formatted = trim(text(value));
    return !value.is_null() && !formatted.empty() && formatted != "null" && formatted != "None";
}

void apply_registry_process(json &project, const json &options) {
    const auto &process = options.at("protected_process_settings");
    const auto widths = options.value("line_width_settings", json::object());
    project.update(process);
    project.update(widths);
    auto profile_name = options.value("source_profile_name", json());
    if (!has_value(profile_name)) {
        profile_name = project.value("printer_model", json());
    }
    if (meaningful_profile_value(profile_name)) {
        project["printer_settings_id"] = text(profile_name);
    }
    const auto slot_count = std::max<long long>(0, options.value("slot_count", 8LL));
    std::set<std::string> differences;
    const auto existing = project.value("different_settings_to_system", json());
    if (existing.is_array() || meaningful_profile_value(existing)) {
        collect_difference_keys(existing, differences);
    }
    for (const auto &[key, value] : process.items()) differences.insert(key);
    for (const auto &[key, value] : widths.items()) differences.insert(key);
    differences.insert("printer_settings_id");
    for (const auto &[key, value] : std::vector<std::pair<std::string, std::string>>{
             {"nozzle_temperature", "220"}, {"nozzle_temperature_initial_layer", "220"},
             {"bed_temperature", "60"}, {"bed_temperature_initial_layer", "60"},
             {"textured_plate_temp", "60"}, {"textured_plate_temp_initial_layer", "60"},
             {"hot_plate_temp", "60"}, {"hot_plate_temp_initial_layer", "60"},
             {"eng_plate_temp", "60"}, {"eng_plate_temp_initial_layer", "60"},
             {"cool_plate_temp", "0"}, {"cool_plate_temp_initial_layer", "0"},
             {"filament_type", "PLA"}}) {
        project[key] = std::vector<std::string>(static_cast<std::size_t>(slot_count), value);
        differences.insert(key);
    }
    if (option(options, "slicer_id") == "OrcaSlicer") {
        project["precise_outer_wall"] = "0";
        differences.insert("precise_outer_wall");
    }
    project["different_settings_to_system"] = json::array({detail::render_difference_tokens(differences)});
}

}  // namespace

std::string assemble_project_template(
    std::string_view base_project_json,
    std::optional<std::string_view> registry_machine_json,
    std::string_view options_json) {
    json project = json::parse(base_project_json);
    const json options = json::parse(options_json);
    if (!registry_machine_json.has_value() && project.empty() &&
        options.value("project_template_kind", "printer") == "minimal") {
        project = {
            {"layer_height", "0.08"}, {"initial_layer_height", "0.08"},
            {"wall_loops", "1"}, {"top_shell_layers", "0"}, {"bottom_shell_layers", "1"},
            {"bottom_surface_pattern", "monotonicline"}, {"elefant_foot_compensation", "0"},
            {"sparse_infill_density", "100%"}, {"sparse_infill_pattern", "zig-zag"},
            {"filament_settings_id", std::vector<std::string>(8, "Generic PLA")},
            {"filament_type", std::vector<std::string>(8, "PLA")},
            {"nozzle_temperature", std::vector<std::string>(8, "220")},
            {"nozzle_temperature_initial_layer", std::vector<std::string>(8, "220")},
        };
    }
    if (registry_machine_json.has_value()) {
        const json machine = json::parse(*registry_machine_json);
        const auto kind = options.value("project_template_kind", std::string("printer"));
        project = kind == "none" ? machine : merge_templates(std::move(project), machine, options, kind);
    }
    mark_purge_chute_flush(project);
    return project.dump();
}

std::string assemble_machine_registry_template(
    std::string_view source_settings_json,
    std::string_view options_json) {
    auto settings = json::parse(source_settings_json);
    const auto options = json::parse(options_json);
    if (options.contains("protected_process_settings")) {
        apply_registry_process(settings, options);
    }
    json application = nullptr;
    if (options.contains("source_version")) {
        const auto slicer = option(options, "slicer_id");
        const auto version = option(options, "source_version");
        const auto explicit_application = options.value("default_application_metadata", json());
        const auto identity = !explicit_application.is_null()
                                  ? text(explicit_application)
                                  : (slicer == "ElegooSlicer" ? "ElegooSlicer-" : "BambuStudio-") + version;
        application = identity;
        if (slicer == "FlashStudio" || slicer == "QIDIStudio") {
            const std::string prefix = "BambuStudio-";
            settings["version"] = identity.rfind(prefix, 0) == 0
                                      ? identity.substr(prefix.size()) : version;
        }
        if (slicer == "QIDIStudio") {
            settings["printer_variant"] = option(options, "nozzle_size");
            if (!settings.contains("printer_extruder_id")) {
                settings["printer_extruder_id"] = json::array({"1"});
            }
        }
    }
    return json{{"settings", settings}, {"application_metadata", application}}.dump();
}

}  // namespace fatcat
