#include "fatcat/project_settings.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "fatcat/wipe_tower.h"
#include "fatcat/source_project_settings.h"
#include "difference_index.h"
#include "filament_projection.h"

namespace fatcat {
namespace {

using json = nlohmann::json;
using detail::difference_tokens;
using detail::render_difference_tokens;

[[noreturn]] void invalid(std::string message) {
    throw ProjectSettingsError(std::move(message));
}

json parse_json(std::string_view text, std::string_view name) {
    bool duplicate_key = false;
    std::string duplicate_name;
    std::vector<std::set<std::string>> object_keys;
    const json::parser_callback_t callback =
        [&](int, json::parse_event_t event, json &parsed) {
            if (event == json::parse_event_t::object_start) {
                object_keys.emplace_back();
            } else if (event == json::parse_event_t::object_end) {
                if (!object_keys.empty()) {
                    object_keys.pop_back();
                }
            } else if (event == json::parse_event_t::key &&
                       !object_keys.empty()) {
                const auto key = parsed.get<std::string>();
                if (!object_keys.back().insert(key).second) {
                    duplicate_key = true;
                    duplicate_name = key;
                    return false;
                }
            }
            return true;
        };
    try {
        json parsed = json::parse(text, callback, true, false);
        if (duplicate_key) {
            invalid("duplicate JSON field in " + std::string(name) + ": " +
                    duplicate_name);
        }
        if (!parsed.is_object()) {
            invalid(std::string(name) + " must be a JSON object");
        }
        return parsed;
    } catch (const ProjectSettingsError &) {
        throw;
    } catch (const json::exception &error) {
        invalid("invalid " + std::string(name) + " JSON: " + error.what());
    }
}

const json &required_member(const json &object, std::string_view key,
                            std::string_view context) {
    if (!object.contains(key)) {
        invalid(std::string(context) + " is missing required field '" +
                std::string(key) + "'");
    }
    return object.at(key);
}

std::string required_string(const json &object, std::string_view key,
                            std::string_view context) {
    const json &value = required_member(object, key, context);
    if (!value.is_string() || value.get<std::string>().empty()) {
        invalid(std::string(context) + "." + std::string(key) +
                " must be non-empty text");
    }
    return value.get<std::string>();
}

bool contains_string(const json &array, const std::string &value) {
    if (!array.is_array()) {
        return false;
    }
    return std::any_of(array.begin(), array.end(), [&](const json &item) {
        return item.is_string() && item.get<std::string>() == value;
    });
}

const json &find_binding(const json &root, std::string_view array_key,
                         const std::string &first_key, const std::string &first,
                         const std::optional<std::pair<std::string, std::string>>
                             &second = std::nullopt,
                         const std::optional<std::pair<std::string, std::string>>
                             &third = std::nullopt,
                         bool reject_duplicates = false) {
    const json &items = required_member(root, array_key, "target data");
    if (!items.is_array()) {
        invalid("target data." + std::string(array_key) + " must be an array");
    }
    const json *match = nullptr;
    for (const json &item : items) {
        if (!item.is_object() || !item.contains(first_key) ||
            !item.at(first_key).is_string() ||
            item.at(first_key).get<std::string>() != first) {
            continue;
        }
        const auto matches = [&](const std::optional<std::pair<std::string, std::string>> &condition) {
            return !condition.has_value() ||
                (item.contains(condition->first) && item.at(condition->first).is_string() &&
                 item.at(condition->first).get<std::string>() == condition->second);
        };
        if (matches(second) && matches(third)) {
            if (reject_duplicates && match != nullptr) {
                invalid("target data has multiple " + std::string(array_key) +
                        " bindings for " + first_key + "='" + first + "'");
            }
            match = &item;
        }
    }
    if (match != nullptr) return *match;
    invalid("target data has no " + std::string(array_key) + " binding for " +
            first_key + "='" + first + "'");
}

void validate_request_keys(const json &request) {
    static const std::set<std::string> allowed = {
        "slicer_id",
        "application_version",
        "machine_uid",
        "nozzle_uid",
        "build_plate_uid",
        "material_uid",
        "material_uids",
        "material_mode",
        "layer_height",
        "initial_layer_print_height",
        "initial_layer_height",
        "line_width",
        "initial_layer_line_width",
        "inner_wall_line_width",
        "outer_wall_line_width",
        "top_surface_line_width",
        "sparse_infill_line_width",
        "internal_solid_infill_line_width",
        "support_line_width",
        "wall_loops",
        "top_shell_layers",
        "bottom_shell_layers",
        "bottom_surface_pattern",
        "elefant_foot_compensation",
        "sparse_infill_density",
        "sparse_infill_pattern",
        "print_speed",
        "travel_speed",
        "enable_support",
        "single_extruder_multi_material",
        "precise_outer_wall",
        "brim_type",
        "brim_width",
        "wall_generator",
        "skirt_loops",
        "skirt_distance",
        "skirt_height",
        "draft_shield",
        "enable_prime_tower",
        "prime_tower_width",
        "wipe_tower_rotation_angle",
        "wipe_tower_x",
        "wipe_tower_y",
        "wipe_tower_positions",
        "filament_colour",
        "filament_multi_colour",
        "filament_slot_mode",
        "default_filament_source_slot",
        "filament_source_slots",
        "disable_cut_retraction",
        "merge_sources",
        "merge_default_project",
        "hardware_mode",
        "source_materials",
        "process_settings",
        "preserve_source_material_settings",
        "source_profile",
        "consumer_type",  // Accepted for compatibility; does not affect composition.
        "source_slot_colours",
    };
    for (const auto &[key, ignored] : request.items()) {
        if (allowed.find(key) == allowed.end()) {
            invalid("request contains unsupported field '" + key + "'");
        }
    }
}

std::vector<std::string> string_array(const json &value,
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

std::vector<std::string> required_string_array(const json &object,
                                               std::string_view key,
                                               std::string_view context) {
    return string_array(required_member(object, key, context),
                        std::string(context) + "." + std::string(key), false);
}

void validate_slots(const json &project, std::size_t &slot_count) {
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

void prepare_registry_only_native_project(json &project,
                                             const json &machine,
                                             const std::string &material_mode,
                                             const json &request) {
    if (material_mode != "target_native_preset") return;
    const auto source_profile = request.value("source_profile", json::object());
    const bool registry_source = project.value("type", std::string()) == "machine" ||
                                 source_profile.value("registry", false);
    const auto expand_difference_index = [&]() {
        // Registry overlays retain one shared process entry even when the
        // selected project already supplied its material identities.
        if (registry_source && project.contains("different_settings_to_system") &&
            project.at("different_settings_to_system").is_array() &&
            project.at("different_settings_to_system").size() == 1) {
            const auto count = project.at("filament_settings_id").size();
            for (std::size_t index = 0; index <= count; ++index) {
                project["different_settings_to_system"].push_back("");
            }
        }
    };
    if (project.contains("filament_settings_id")) {
        expand_difference_index();
        return;
    }
    if (!registry_source) {
        invalid("native source without filament_settings_id must be a registry machine template");
    }
    project = detail::prepare_source_identity(project, request);
    const auto types = required_string_array(project, "filament_type", "project");
    if (types.empty()) {
        invalid("registry machine template has no filament slots");
    }
    const auto &defaults = required_member(project, "default_filament_profile", "project");
    if (!defaults.is_array() || defaults.empty() || !defaults.at(0).is_string()) {
        invalid("registry machine template has no default material identity");
    }
    project["filament_settings_id"] = json::array();
    for (std::size_t slot = 0; slot < types.size(); ++slot) {
        project["filament_settings_id"].push_back(defaults.at(0));
    }
    for (const auto *key : {"type", "instantiation", "inherits", "setting_id"}) {
        project.erase(key);
    }
    project["name"] = "project_settings";
    project["from"] = "project";
    if (!project.contains("version") && machine.contains("source_version")) {
        project["version"] = required_string(machine, "source_version", "machine binding");
    }
    const auto project_defaults = machine.value("native_project_defaults", json::object());
    if (!project_defaults.is_object()) {
        invalid("native project defaults must be a source-bound object");
    }
    for (const auto &[key, value] : project_defaults.items()) {
        if (!value.is_string()) {
            invalid("native project default must be a string: " + key);
        }
        if (!project.contains(key)) {
            project[key] = value;
        }
    }
    // The registry machine preset carries only process differences, whereas
    // the project snapshot uses process + N material entries + machine tail.
    expand_difference_index();
}

void validate_source_hardware(const json &project, const json &machine) {
    if (!project.contains("printer_model") ||
        project.at("printer_model") != required_member(machine, "printer_model", "machine")) {
        invalid("source project machine model does not match the exact target machine");
    }
    if (!project.contains("nozzle_diameter") ||
        project.at("nozzle_diameter") != required_member(machine, "nozzle_diameter", "machine")) {
        invalid("source project machine/nozzle does not match the exact target nozzle");
    }
}

void select_first_slot_variants(json &project, const json &machine,
                                 std::size_t slot_count) {
    if (!machine.contains("first_variant_per_slot_keys")) {
        return;
    }
    // Only hardware whose native profile declares a single variant selects
    // standard values. Other machines and unknown arrays keep their shape.
    for (const auto &key : string_array(machine.at("first_variant_per_slot_keys"),
                                        "machine.first_variant_per_slot_keys")) {
        auto found = project.find(key);
        if (found == project.end() || !found->is_array() ||
            found->size() != slot_count * 2) {
            continue;
        }
        json selected = json::array();
        for (std::size_t slot = 0; slot < slot_count; ++slot) {
            selected.push_back(found->at(slot * 2));
        }
        *found = std::move(selected);
    }
}

struct FilamentSelection {
    bool compact = false;
    bool disable_cut_retraction = false;
    std::size_t source_count = 0;
    std::size_t output_count = 0;
    std::size_t default_source = 0;
    std::vector<std::optional<std::size_t>> material_sources;
};

std::size_t source_slot(const json &value, std::size_t source_count,
                         const std::string &context) {
    if (!value.is_number_unsigned() || value.get<std::uint64_t>() >= source_count) {
        invalid(context + " must be an unsigned source slot less than the source filament count");
    }
    return value.get<std::size_t>();
}

FilamentSelection filament_selection(const json &request, std::size_t source_count) {
    FilamentSelection selection;
    selection.source_count = source_count;
    selection.output_count = source_count;
    if (request.contains("filament_slot_mode")) {
        const auto mode = required_string(request, "filament_slot_mode", "request");
        if (mode != "preserve" && mode != "compact") {
            invalid("request.filament_slot_mode must be preserve or compact");
        }
        selection.compact = mode == "compact";
    }
    std::size_t requested_count = 0;
    if (request.contains("filament_colour")) {
        requested_count = string_array(request.at("filament_colour"), "request.filament_colour", false).size();
    }
    if (selection.compact) {
        if (requested_count == 0) {
            invalid("compact request.filament_colour must contain at least one slot");
        }
        selection.output_count = requested_count;
        required_member(request, "default_filament_source_slot", "compact request");
        if (request.contains("filament_multi_colour") &&
            string_array(request.at("filament_multi_colour"), "request.filament_multi_colour", false).size() != requested_count) {
            invalid("compact request.filament_multi_colour must match filament_colour slots");
        }
    }
    if (request.contains("default_filament_source_slot")) {
        selection.default_source = source_slot(request.at("default_filament_source_slot"), source_count,
                                               "request.default_filament_source_slot");
    }
    selection.material_sources.resize(selection.output_count);
    if (request.contains("filament_source_slots")) {
        const auto &sources = request.at("filament_source_slots");
        if (!sources.is_array() || requested_count == 0 ||
            (!selection.compact && requested_count > source_count) || sources.size() != requested_count) {
            invalid("request.filament_source_slots must match the nonempty filament_colour slots");
        }
        for (std::size_t i = 0; i < sources.size(); ++i) {
            if (!sources.at(i).is_null()) {
                selection.material_sources[i] = source_slot(sources.at(i), source_count,
                                                            "request.filament_source_slots[" + std::to_string(i) + "]");
            }
        }
    }
    if (request.contains("disable_cut_retraction")) {
        if (!request.at("disable_cut_retraction").is_boolean()) {
            invalid("request.disable_cut_retraction must be a boolean");
        }
        selection.disable_cut_retraction = request.at("disable_cut_retraction").get<bool>();
        if (selection.disable_cut_retraction && !selection.compact) {
            invalid("request.disable_cut_retraction requires compact filament_slot_mode");
        }
    }
    return selection;
}

using FilamentSources =
    std::map<std::string, std::vector<std::optional<std::size_t>>>;

void remap_filament_differences(json &project, const json &source, const json &snapshot,
                                const FilamentSelection &selection,
                                const FilamentSources &field_sources,
                                const std::set<std::string> &known_fields,
                                const std::vector<std::set<std::string>> &generated_fields) {
    const auto key = required_string(snapshot, "difference_list_key", "filament snapshot");
    const auto &offset_value = required_member(snapshot, "filament_slot_offset", "filament snapshot");
    const auto &trailing_value = required_member(snapshot, "trailing_entry_count", "filament snapshot");
    if (!offset_value.is_number_unsigned() || !trailing_value.is_number_unsigned()) {
        invalid("filament snapshot entry counts must be non-negative integers");
    }
    const auto offset = offset_value.get<std::size_t>();
    const auto trailing = trailing_value.get<std::size_t>();
    const auto expected = offset + selection.source_count + trailing;
    std::vector<std::string> entries;
    if (source.contains(key)) {
        entries = string_array(source.at(key), "project." + key);
    }
    if (entries.empty()) {
        entries.resize(expected);
    } else if (entries.size() != expected) {
        invalid("filament snapshot difference-list entry count does not match the source slot layout");
    }
    json result = json::array();
    for (std::size_t i = 0; i < offset; ++i) {
        result.push_back(entries[i]);
    }
    for (std::size_t slot = 0; slot < selection.output_count; ++slot) {
        if (!selection.compact && !selection.material_sources.at(slot)) {
            const auto &original = entries.at(offset + slot);
            if (generated_fields.at(slot).empty()) {
                result.push_back(original);
            } else {
                auto retained = difference_tokens(original);
                retained.insert(generated_fields.at(slot).begin(), generated_fields.at(slot).end());
                result.push_back(render_difference_tokens(retained));
            }
            continue;
        }
        std::set<std::size_t> adopted_sources;
        for (const auto &[field, sources] : field_sources) {
            if (sources.at(slot)) {
                adopted_sources.insert(*sources.at(slot));
            }
        }
        auto retained = generated_fields.at(slot);
        for (const auto adopted : adopted_sources) {
            for (const auto &token : difference_tokens(entries.at(offset + adopted))) {
                const auto found = field_sources.find(token);
                if (known_fields.count(token) == 0 ||
                    (found != field_sources.end() && found->second.at(slot) == adopted)) {
                    retained.insert(token);
                }
            }
        }
        result.push_back(render_difference_tokens(retained));
    }
    for (std::size_t i = 0; i < trailing; ++i) {
        result.push_back(entries.at(offset + selection.source_count + i));
    }
    project[key] = std::move(result);
}

std::size_t filament_group_width(const json &values, const FilamentSelection &selection,
                                  std::size_t declared_width, bool variable_group,
                                  const std::string &key) {
    if (values.size() == selection.source_count) {
        return 1;
    }
    if (values.size() == selection.source_count * declared_width) {
        return declared_width;
    }
    if (variable_group && selection.source_count > 0 && !values.empty() &&
        values.size() % selection.source_count == 0) {
        return values.size() / selection.source_count;
    }
    if (selection.compact && values.size() == selection.output_count) {
        return 1;
    }
    invalid("project." + key + " does not match its declared filament array shape");
}

void record_material_value_overrides(const json &project, const json &source,
                                      const json &rules, const FilamentSelection &selection,
                                      const FilamentSources &field_sources,
                                      std::vector<std::set<std::string>> &generated_fields) {
    const auto &identity_sources = field_sources.at("filament_settings_id");
    for (const auto &rule : rules) {
        if (!required_member(rule, "material_source_override", "filament array rule").get<bool>()) {
            continue;
        }
        const auto declared_width = required_member(rule, "group_size", "filament array rule").get<std::size_t>();
        const bool variable_group = required_string(rule, "selection", "filament array rule") ==
                                    "default_or_group";
        for (const auto &key : required_string_array(rule, "keys", "filament array rule")) {
            if (field_sources.count(key) == 0) {
                continue;
            }
            const auto &original = source.at(key);
            const auto &final = project.at(key);
            const auto source_width = filament_group_width(
                original, selection, declared_width, variable_group, key);
            const auto output_width = final.size() / selection.output_count;
            for (std::size_t slot = 0; slot < selection.output_count; ++slot) {
                const auto identity_start = *identity_sources.at(slot) * source_width;
                bool differs = identity_start + output_width > original.size();
                for (std::size_t variant = 0; !differs && variant < output_width; ++variant) {
                    differs = final.at(slot * output_width + variant) != original.at(identity_start + variant);
                }
                if (differs) {
                    generated_fields[slot].insert(key);
                }
            }
        }
    }
}

void compose_filament_arrays(json &project, const json &request, const json &dialect,
                             const json &machine, const FilamentSelection &selection,
                             std::vector<std::string> &changed_keys) {
    const json source = project;
    const auto &snapshot = required_member(dialect, "filament_snapshot", "package dialect");
    const auto &rules = required_member(snapshot, "array_rules", "filament snapshot");
    const json first_variants = machine.value("first_variant_per_slot_keys", json::array());
    const json default_groups = machine.value("compact_default_group_keys", json::array());
    FilamentSources field_sources;
    std::set<std::string> known_fields;
    std::vector<std::set<std::string>> generated_fields(selection.output_count);
    for (const auto &rule : rules) {
        if (rule.value("merged_only", false)) continue;
        const auto rule_selection = required_string(rule, "selection", "filament array rule");
        const auto declared_width = required_member(rule, "group_size", "filament array rule").get<std::size_t>();
        const bool material_copy = required_member(rule, "material_source_override", "filament array rule").get<bool>();
        for (const auto &key : required_string_array(rule, "keys", "filament array rule")) {
            known_fields.insert(key);
            const auto found = source.find(key);
            if (found == source.end() || !found->is_array() || found->empty()) {
                continue;
            }
            const auto &values = *found;
            if (rule_selection == "matrix") {
                if (!selection.compact || selection.output_count > selection.source_count) {
                    continue;
                }
                const auto pairs = selection.source_count * selection.source_count;
                const auto matrix_count = values.size() == pairs ? 1 : declared_width;
                if (values.size() != pairs * matrix_count) {
                    invalid("project." + key + " does not match its declared filament matrix shape");
                }
                project[key] = detail::project_filament_matrix(
                    values, selection.source_count, selection.output_count);
            } else {
                const auto source_width = filament_group_width(
                    values, selection, declared_width,
                    material_copy && rule_selection == "default_or_group", key);
                const bool first_variant = source_width == 2 && contains_string(first_variants, key);
                // A source K*N group is still grouped when its raw length
                // coincides with the requested slot count (for example M=2N).
                const bool already_output = source_width == 1 &&
                    values.size() == selection.output_count;
                const bool retain_groups = rule_selection == "group" || rule_selection == "slot_index" ||
                    (rule_selection == "default_or_group" && source_width > 1);
                const auto output_width = first_variant ? 1 :
                    (!selection.compact || retain_groups || contains_string(default_groups, key) ? source_width : 1);
                json resized = json::array();
                std::vector<std::optional<std::size_t>> sources;
                for (std::size_t slot = 0; slot < selection.output_count; ++slot) {
                    if (selection.compact && rule_selection == "slot_index") {
                        // The slicer uses this 1-based output slot to split
                        // variant groups, independently of material selection.
                        sources.push_back(slot < selection.source_count ? std::make_optional(slot) : std::nullopt);
                        for (std::size_t variant = 0; variant < output_width; ++variant) {
                            resized.push_back(std::to_string(slot + 1));
                        }
                        continue;
                    }
                    const bool retain_source_slot = !selection.compact || already_output ||
                        (retain_groups && selection.output_count <= selection.source_count);
                    auto from = retain_source_slot ? slot : selection.default_source;
                    if (material_copy && selection.material_sources.at(slot)) {
                        from = *selection.material_sources.at(slot);
                    }
                    if (from * source_width + output_width > values.size()) {
                        invalid("project." + key + " does not contain the requested material source slot");
                    }
                    sources.push_back(from);
                    detail::append_filament_group(resized, values, from, source_width, output_width);
                }
                // Source provenance is independent of whether a caller may
                // redirect this field with an explicit material selection.
                field_sources[key] = std::move(sources);
                project[key] = std::move(resized);
            }
            if (project.at(key) != values) {
                changed_keys.push_back(key);
            }
        }
    }
    // A retained group can originate from a different preset than the slot's
    // selected identity. Register those actual value differences even when the
    // source preset had no overrides of its own.
    record_material_value_overrides(project, source, rules, selection,
                                    field_sources, generated_fields);
    // Colours are independent caller input, not material-preset selection.
    for (const auto *key : {"filament_colour", "filament_multi_colour"}) {
        if (!request.contains(key)) {
            continue;
        }
        const auto requested = string_array(request.at(key), std::string("request.") + key, false);
        for (std::size_t slot = 0; slot < requested.size() && slot < selection.output_count; ++slot) {
            if (!project.contains(key) || project.at(key).at(slot) != requested[slot]) {
                generated_fields[slot].insert(key);
                const auto found = field_sources.find(key);
                if (found != field_sources.end()) {
                    found->second.at(slot).reset();
                }
            }
        }
    }
    remap_filament_differences(project, source, snapshot, selection, field_sources,
                               known_fields, generated_fields);
}

void validate_canonical_selection(const json &canonical,
                                  const std::string &machine_uid,
                                  const std::string &nozzle_uid,
                                  const std::string &plate_uid) {
    const json &machine = find_binding(canonical, "machines", "machine_uid", machine_uid);
    if (!contains_string(required_member(machine, "supported_nozzle_uids", "canonical machine"),
                         nozzle_uid)) {
        invalid("canonical machine does not support the requested nozzle '" + nozzle_uid + "'");
    }
    if (!contains_string(required_member(machine, "supported_build_plate_uids", "canonical machine"),
                         plate_uid)) {
        invalid("canonical machine does not support the requested build plate '" + plate_uid + "'");
    }
    find_binding(canonical, "nozzles", "nozzle_uid", nozzle_uid);
    find_binding(canonical, "build_plates", "build_plate_uid", plate_uid);
}

void validate_preserved_build_plate(const json &project, const json &plate) {
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

double parse_numeric_token(const std::string &value, std::string_view field) {
    double number = 0.0;
    if (!detail::parse_finite_decimal_string(value, number)) {
        invalid("request." + std::string(field) +
                " must be a finite numeric string");
    }
    return number;
}

double numeric_value(const std::string &value, std::string_view field) {
    return parse_numeric_token(value, field);
}

json normalized_numeric_value(const std::string &raw, std::string_view field) {
    const double number = parse_numeric_token(raw, field);
    if (number == 0.0 && !raw.empty() && raw.front() == '-') {
        // Keep the sign until A2 formats the value so -0 follows its contract.
        return json::parse("-0.0");
    }
    return json(number);
}

void validate_positive_number(const json &request, const std::string &key) {
    if (!request.at(key).is_string()) {
        invalid("request." + key + " must be text");
    }
    if (!(numeric_value(request.at(key).get<std::string>(), key) > 0.0)) {
        invalid("request." + key + " must be finite and positive");
    }
}

void validate_density(const json &request) {
    const std::string value = request.at("sparse_infill_density").get<std::string>();
    if (value.empty()) {
        invalid("request.sparse_infill_density must be a finite percentage");
    }
    const bool percent = value.back() == '%';
    const std::string numeric = percent ? value.substr(0, value.size() - 1) : value;
    const double density = numeric_value(numeric, "sparse_infill_density");
    if (density < 0.0 || density > 100.0) {
        invalid("request.sparse_infill_density must be between 0% and 100%");
    }
}

void apply_scalar_overrides(json &project, const json &request,
                            std::vector<std::string> &changed_keys) {
    static const std::vector<std::string> scalar_keys = {
        "layer_height",
        "initial_layer_print_height",
        "initial_layer_height",
        "line_width",
        "initial_layer_line_width",
        "inner_wall_line_width",
        "outer_wall_line_width",
        "top_surface_line_width",
        "sparse_infill_line_width",
        "internal_solid_infill_line_width",
        "support_line_width",
        "wall_loops",
        "top_shell_layers",
        "bottom_shell_layers",
        "bottom_surface_pattern",
        "elefant_foot_compensation",
        "sparse_infill_density",
        "sparse_infill_pattern",
        "print_speed",
        "travel_speed",
        "enable_support",
        "single_extruder_multi_material",
        "precise_outer_wall",
        "brim_type",
        "brim_width",
        "wall_generator",
        "skirt_loops",
        "skirt_distance",
        "skirt_height",
        "draft_shield",
    };
    for (const auto &key : scalar_keys) {
        if (!request.contains(key)) {
            continue;
        }
        if (!request.at(key).is_string()) {
            invalid("request." + key + " must be text");
        }
        if (key == "layer_height" || key == "initial_layer_print_height" ||
            key == "initial_layer_height") {
            validate_positive_number(request, key);
        } else if (key == "sparse_infill_density") {
            validate_density(request);
        } else if (key == "sparse_infill_pattern" &&
                   request.at(key).get<std::string>().empty()) {
            invalid("request.sparse_infill_pattern must be non-empty text");
        } else if (key == "brim_type" && request.at(key).get<std::string>().empty()) {
            invalid("request.brim_type must be non-empty text");
        } else if (key == "brim_width") {
            const double width = numeric_value(request.at(key).get<std::string>(), key);
            if (width < 0.0) {
                invalid("request.brim_width must be finite and non-negative");
            }
        }
        project[key] = request.at(key);
        changed_keys.push_back(key);
    }
}

json target_tower_dialect(const json &target) {
    const json &tower = required_member(
        required_member(target, "package_dialect", "target data"),
        "wipe_tower", "package dialect");
    return {
        {"enabled_key", required_string(tower, "enabled_key", "wipe tower dialect")},
        {"x_key", required_string(tower, "x_key", "wipe tower dialect")},
        {"y_key", required_string(tower, "y_key", "wipe tower dialect")},
        {"width_key", required_string(tower, "width_key", "wipe tower dialect")},
        {"rotation_key", required_string(tower, "rotation_key", "wipe tower dialect")},
        {"process_difference_key",
         required_string(tower, "process_difference_key", "wipe tower dialect")},
    };
}

std::string scalar_tower_settings(const json &request) {
    json settings = json::object();
    if (request.contains("enable_prime_tower")) {
        const json &value = request.at("enable_prime_tower");
        if (!value.is_string()) {
            invalid("request.enable_prime_tower must be text");
        }
        const auto enabled = value.get<std::string>();
        if (enabled != "0" && enabled != "1") {
            invalid("request.enable_prime_tower must be exactly 0 or 1");
        }
        settings["enabled"] = enabled == "1";
    }
    if (request.contains("prime_tower_width")) {
        const auto &value = request.at("prime_tower_width");
        if (!value.is_string()) {
            invalid("request.prime_tower_width must be text");
        }
        const auto raw = value.get<std::string>();
        if (!(numeric_value(raw, "prime_tower_width") > 0.0)) {
            invalid("request.prime_tower_width must be finite and positive");
        }
        settings["width_mm"] = normalized_numeric_value(raw, "prime_tower_width");
    }
    if (request.contains("wipe_tower_rotation_angle")) {
        const auto &value = request.at("wipe_tower_rotation_angle");
        if (!value.is_string()) {
            invalid("request.wipe_tower_rotation_angle must be text");
        }
        const auto raw = value.get<std::string>();
        settings["rotation_deg"] =
            normalized_numeric_value(raw, "wipe_tower_rotation_angle");
    }
    return settings.dump();
}

std::pair<std::size_t, std::size_t> coordinate_sizes(const json &project,
                                                     const json &dialect) {
    const std::string x_key = required_string(dialect, "x_key", "wipe tower dialect");
    const std::string y_key = required_string(dialect, "y_key", "wipe tower dialect");
    const bool has_x = project.contains(x_key);
    const bool has_y = project.contains(y_key);
    if (has_x != has_y) {
        invalid("wipe tower x/y coordinate arrays must be provided together");
    }
    if (!has_x) {
        return {0, 0};
    }
    if (!project.at(x_key).is_array() || !project.at(y_key).is_array()) {
        invalid("wipe tower x/y coordinate fields must be arrays");
    }
    const auto x_size = project.at(x_key).size();
    const auto y_size = project.at(y_key).size();
    if (x_size != y_size) {
        invalid("wipe tower x/y coordinate plate counts differ");
    }
    return {x_size, y_size};
}

std::string position_tower_settings(std::size_t plate_index,
                                    const std::string &x_value,
                                    const std::string &y_value) {
    const json settings = {
        {"positions", json::array({{{"plate_index", plate_index},
                                     {"x_mm", normalized_numeric_value(x_value, "wipe_tower_x")},
                                     {"y_mm", normalized_numeric_value(y_value, "wipe_tower_y")}}})}};
    return settings.dump();
}

void prepare_tower_difference_index(json &project, const json &target,
                                    std::size_t slot_count, bool merging_sources) {
    const json &snapshot = required_member(
        required_member(target, "package_dialect", "target data"),
        "filament_snapshot", "package dialect");
    const std::string key =
        required_string(snapshot, "difference_list_key", "filament snapshot");
    const json &offset_value =
        required_member(snapshot, "filament_slot_offset", "filament snapshot");
    const json &trailing_value =
        required_member(snapshot, "trailing_entry_count", "filament snapshot");
    if (!offset_value.is_number_unsigned() || !trailing_value.is_number_unsigned()) {
        invalid("filament snapshot entry counts must be non-negative integers");
    }
    const std::size_t expected =
        merging_sources && snapshot.value("merged_difference_list_singleton", false) &&
                (!project.contains(key) || project.at(key).size() <= 1)
            ? 1
            : offset_value.get<std::size_t>() + slot_count +
                  trailing_value.get<std::size_t>();
    if (!project.contains(key)) {
        project[key] = json::array();
        for (std::size_t index = 0; index < expected; ++index) {
            project[key].push_back("");
        }
        return;
    }
    if (project.at(key).is_array() && project.at(key).empty()) {
        project[key] = json::array();
        for (std::size_t index = 0; index < expected; ++index) {
            project[key].push_back("");
        }
        return;
    }
    const auto entries = string_array(project.at(key), "project." + key);
    if (entries.size() != expected) {
        invalid("filament snapshot difference-list entry count does not match the slot layout");
    }
}

bool is_signed_integer_zero(const json &value) {
    return value.is_number_integer() && !value.is_number_unsigned() &&
           value.get<std::int64_t>() == 0;
}

void normalize_typed_positions(json &positions) {
    if (!positions.is_array()) {
        invalid("request.wipe_tower_positions must be an array");
    }
    for (auto &position : positions) {
        if (!position.is_object()) {
            continue;
        }
        const auto plate_it = position.find("plate_index");
        if (plate_it != position.end() && is_signed_integer_zero(*plate_it)) {
            // JSON 0 is stored as unsigned by nlohmann/json; signed integer 0
            // here is the lexical -0 that A2 intentionally rejects.
            invalid("wipe tower request: wipe tower plate_index must be a nonnegative integer");
        }
        for (const auto key : {std::string("x_mm"), std::string("y_mm")}) {
            const auto value_it = position.find(key);
            if (value_it != position.end() && is_signed_integer_zero(*value_it)) {
                *value_it = json::parse("-0.0");
            }
        }
    }
}

void apply_tower_patch(json &project, const std::string &settings_json,
                       const json &dialect) {
    try {
        project = parse_json(
            patch_wipe_tower(project.dump(), settings_json, dialect.dump()),
            "patched project settings");
    } catch (const WipeTowerError &error) {
        invalid(std::string("wipe tower request: ") + error.what());
    }
}

void apply_tower_overrides(json &project, const json &request,
                           const json &target, bool record_process_differences = true) {
    detail::apply_source_tower_defaults(project, target);
    json dialect = target_tower_dialect(target);
    if (!record_process_differences) dialect["record_process_differences"] = false;
    const bool has_scalar = request.contains("enable_prime_tower") ||
                            request.contains("prime_tower_width") ||
                            request.contains("wipe_tower_rotation_angle");
    const bool has_xy = request.contains("wipe_tower_x") ||
                        request.contains("wipe_tower_y");
    const bool has_positions = request.contains("wipe_tower_positions");
    if (has_xy && has_positions) {
        invalid("request cannot combine wipe_tower_positions with wipe_tower_x/y");
    }

    if ((has_scalar || has_xy || has_positions) &&
        request.value("hardware_mode", "target_binding") != "preserve_source") {
        std::size_t slot_count = 0;
        validate_slots(project, slot_count);
        prepare_tower_difference_index(project, target, slot_count,
                                       request.contains("merge_sources"));
    }

    if (has_scalar) {
        apply_tower_patch(project, scalar_tower_settings(request), dialect);
    }

    const auto [existing_x_size, existing_y_size] = coordinate_sizes(project, dialect);
    if (has_xy) {
        if (!request.contains("wipe_tower_x") || !request.contains("wipe_tower_y")) {
            invalid("request wipe_tower_x and wipe_tower_y must be provided together");
        }
        const auto x_values = string_array(request.at("wipe_tower_x"),
                                           "request.wipe_tower_x");
        const auto y_values = string_array(request.at("wipe_tower_y"),
                                           "request.wipe_tower_y");
        if (x_values.size() != y_values.size()) {
            invalid("request wipe tower x/y coordinate counts differ");
        }
        if (existing_x_size != existing_y_size ||
            (existing_x_size != x_values.size() && existing_x_size != 0) ||
            (existing_x_size == 0 && x_values.size() > 1)) {
            invalid("request wipe tower coordinate count must match existing plates");
        }
        for (std::size_t index = 0; index < x_values.size(); ++index) {
            // Validate every requested token, including untouched tail slots. Only
            // changed slots are sent to A2 so their original text is retained.
            parse_numeric_token(x_values[index], "wipe_tower_x");
            parse_numeric_token(y_values[index], "wipe_tower_y");
            const auto current_x = existing_x_size == 0
                                       ? std::string()
                                       : project.at(dialect.at("x_key").get<std::string>())
                                             .at(index)
                                             .get<std::string>();
            const auto current_y = existing_y_size == 0
                                       ? std::string()
                                       : project.at(dialect.at("y_key").get<std::string>())
                                             .at(index)
                                             .get<std::string>();
            if (existing_x_size == 0 || x_values[index] != current_x ||
                y_values[index] != current_y) {
                apply_tower_patch(project,
                                  position_tower_settings(index, x_values[index],
                                                          y_values[index]),
                                  dialect);
            }
        }
    }

    if (has_positions) {
        json positions = request.at("wipe_tower_positions");
        normalize_typed_positions(positions);
        json settings = json::object();
        settings["positions"] = positions;
        apply_tower_patch(project, settings.dump(), dialect);
    }
}

void apply_colour_overrides(json &project, const json &request,
                            std::size_t slot_count,
                            std::vector<std::string> &changed_keys) {
    for (const auto &key : {std::string("filament_colour"),
                            std::string("filament_multi_colour")}) {
        if (!request.contains(key)) {
            continue;
        }
        const auto requested = string_array(request.at(key), "request." + key, false);
        if (requested.empty()) {
            invalid("request." + key + " must contain at least one slot");
        }
        if (requested.size() > slot_count) {
            invalid("request." + key + " has more slots than filament_settings_id");
        }
        if (!project.contains(key)) {
            if (key == "filament_colour" && requested.size() != slot_count) {
                invalid("project.filament_colour is missing; provide all material slots");
            }
            project[key] = json::array();
            for (std::size_t index = 0; index < slot_count; ++index) {
                project[key].push_back("");
            }
        }
        json &destination = project[key];
        if (!destination.is_array() || destination.size() != slot_count) {
            invalid("project." + key + " slots must match filament_settings_id");
        }
        for (std::size_t index = 0; index < requested.size(); ++index) {
            destination.at(index) = requested[index];
        }
        changed_keys.push_back(key);
    }
}

struct NativeMaterialSlot {
    const json *binding;
    const json *parameter_set;
    std::string context;
    std::set<std::string> managed_keys;
};

std::vector<NativeMaterialSlot> resolve_native_materials(const json &request,
                                                         const json &canonical,
                                                         const json &target,
                                                         std::size_t slot_count) {
    std::vector<std::string> uids;
    if (request.contains("material_uids")) {
        uids = string_array(request.at("material_uids"), "request.material_uids", false);
        if (uids.size() != slot_count) {
            invalid("request.material_uids must match the final logical slot count " + std::to_string(slot_count));
        }
    } else {
        uids.assign(slot_count, required_string(request, "material_uid", "native request"));
    }
    const auto slicer = required_string(request, "slicer_id", "request");
    const auto version = required_string(request, "application_version", "request");
    const auto machine = required_string(request, "machine_uid", "request");
    const auto nozzle = required_string(request, "nozzle_uid", "request");
    std::vector<NativeMaterialSlot> slots;
    for (std::size_t slot = 0; slot < slot_count; ++slot) {
        const auto context = slicer + " " + version + " " + machine + " " + nozzle +
            " " + uids[slot] + " slot " + std::to_string(slot);
        try {
            find_binding(canonical, "materials", "material_uid", uids[slot],
                         std::nullopt, std::nullopt, true);
            const auto &binding = find_binding(target, "material_bindings", "material_uid", uids[slot],
                std::make_pair(std::string("machine_uid"), machine),
                std::make_pair(std::string("nozzle_uid"), nozzle), true);
            if (required_string(binding, "write_policy", "material") != "target_native_preset") {
                invalid("material write_policy is not target_native_preset");
            }
            const auto profile_key = required_string(binding, "material_profile_key", "material binding");
            const auto &profile = find_binding(target, "material_profiles", "material_profile_key",
                                               profile_key, std::nullopt, std::nullopt, true);
            if (required_string(profile, "material_uid", "material profile") != uids[slot] ||
                required_string(profile, "native_profile_id", "material profile") !=
                    required_string(binding, "native_profile_id", "material binding")) {
                invalid("material profile identity disagrees with its binding");
            }
            if (profile.contains("target_parameters") || profile.contains("managed_parameter_keys")) {
                invalid("material profile duplicates a shared target parameter body");
            }
            const auto parameter_set_key = required_string(
                profile, "target_parameter_set_key", "material profile");
            const auto &parameter_set = find_binding(
                target, "target_parameter_sets", "target_parameter_set_key", parameter_set_key,
                std::nullopt, std::nullopt, true);
            const auto keys = required_string_array(
                parameter_set, "managed_parameter_keys", "material parameter set");
            std::set<std::string> managed(keys.begin(), keys.end());
            if (managed.empty() || managed.size() != keys.size()) {
                invalid("material parameter set managed keys must be non-empty unique text");
            }
            const auto &parameters = required_member(
                parameter_set, "target_parameters", "material parameter set");
            if (!parameters.is_object() || parameters.empty()) {
                invalid("material parameter set target_parameters must be a non-empty object");
            }
            for (const auto &[key, parameter] : parameters.items()) {
                if (managed.count(key) == 0 || !parameter.is_object()) {
                    invalid("material parameter set key is not declared as managed: " + key);
                }
                const auto cardinality = required_string(parameter, "cardinality", "target parameter " + key);
                const auto values = string_array(required_member(parameter, "values", "target parameter " + key),
                                                  "target parameter " + key + ".values");
                if ((cardinality != "per_slot" && cardinality != "per_slot_group") ||
                    values.empty() || (cardinality == "per_slot" && values.size() != 1)) {
                    invalid("target parameter cardinality has invalid values: " + key);
                }
            }
            slots.push_back({&binding, &parameter_set, context, std::move(managed)});
        } catch (const ProjectSettingsError &error) {
            invalid("native material " + context + ": " + error.what());
        }
    }
    return slots;
}

void apply_target_native_materials(json &project, const std::vector<NativeMaterialSlot> &slots,
                                   const json &machine) {
    std::set<std::string> managed_keys;
    for (const auto &slot : slots) {
        managed_keys.insert(slot.managed_keys.begin(), slot.managed_keys.end());
    }
    for (const auto &[key, ignored] : project.items()) {
        std::string normalized = key;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (key.rfind("filament_", 0) == 0 &&
            (normalized.find("gcode") != std::string::npos ||
             normalized.find("plugin_config") != std::string::npos) &&
            managed_keys.find(key) == managed_keys.end()) {
            invalid("native material source override is non-portable: " + key);
        }
    }
    for (const auto &key : managed_keys) {
        // Routing is generated from final output slots after preset application.
        if (key == "filament_self_index") continue;
        json expanded = json::array();
        const NativeMaterialSlot *first_missing_slot = nullptr;
        bool has_evidenced_value = false;
        for (const auto &slot : slots) {
            const auto &parameters = slot.parameter_set->at("target_parameters");
            if (!parameters.contains(key)) {
                if (first_missing_slot == nullptr) first_missing_slot = &slot;
                continue;
            }
            has_evidenced_value = true;
            for (const auto &value : parameters.at(key).at("values")) {
                expanded.push_back(value);
            }
        }
        if (!has_evidenced_value) {
            // The source profile explicitly manages this field, but this target
            // has no evidenced value. Remove stale template data and let the
            // slicer's own default apply; never synthesize a value here.
            project.erase(key);
            continue;
        }
        if (first_missing_slot != nullptr) {
            invalid("native material " + first_missing_slot->context +
                    " has no evidenced target parameter '" + key + "'");
        }
        project[key] = std::move(expanded);
    }
    const std::string target_machine =
        required_string(machine, "source_profile_name", "machine");
    project["filament_settings_id"] = json::array();
    project["filament_ids"] = json::array();
    project["filament_type"] = json::array();
    project["filament_vendor"] = json::array();
    project["filament_compatible_printers"] = json::array();
    project["filament_self_index"] = json::array();
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const auto &slot = slots[index];
        const auto &material = *slot.binding;
        project["filament_settings_id"].push_back(required_string(material, "native_profile_id", slot.context));
        project["filament_ids"].push_back(required_string(material, "native_filament_id", slot.context));
        project["filament_type"].push_back(required_string(material, "native_material_type", slot.context));
        project["filament_vendor"].push_back(required_string(material, "native_vendor", slot.context));
        project["filament_compatible_printers"].push_back(target_machine);
        const auto &parameters = slot.parameter_set->at("target_parameters");
        const auto variants = parameters.find("filament_extruder_variant");
        const std::size_t variant_count = variants == parameters.end()
            ? 1 : variants->at("values").size();
        for (std::size_t variant = 0; variant < variant_count; ++variant) {
            project["filament_self_index"].push_back(std::to_string(index + 1));
        }
    }
}

void clear_native_differences(json &project, const json &dialect,
                              std::size_t slot_count,
                              const std::vector<NativeMaterialSlot> &materials,
                              const json &request) {
    const json &snapshot = required_member(dialect, "filament_snapshot", "package dialect");
    const std::string key = required_string(snapshot, "difference_list_key", "filament snapshot");
    const auto &offset_value = required_member(snapshot, "filament_slot_offset", "filament snapshot");
    const auto &trailing_value = required_member(snapshot, "trailing_entry_count", "filament snapshot");
    if (!offset_value.is_number_unsigned() || !trailing_value.is_number_unsigned()) {
        invalid("filament snapshot entry counts must be non-negative integers");
    }
    const std::size_t offset = offset_value.get<std::size_t>();
    const std::size_t trailing = trailing_value.get<std::size_t>();
    const std::size_t expected = offset + slot_count + trailing;
    std::vector<std::string> entries;
    if (project.contains(key)) {
        entries = string_array(project.at(key), "project." + key);
        if (!entries.empty() && entries.size() != expected) {
            invalid("filament snapshot difference-list entry count does not match the slot layout");
        }
    }
    if (entries.empty()) {
        entries.assign(expected, "");
    }
    std::vector<std::string> result;
    result.insert(result.end(), entries.begin(), entries.begin() + offset);
    for (std::size_t slot = 0; slot < slot_count; ++slot) {
        std::vector<std::string> retained;
        std::stringstream tokens(entries.at(offset + slot));
        std::string token;
        while (std::getline(tokens, token, ';')) {
            const auto &managed_keys = materials.at(slot).managed_keys;
            static const std::set<std::string> identities = {
                "filament_settings_id", "filament_ids", "filament_type", "filament_vendor",
                "filament_compatible_printers", "filament_self_index",
            };
            if (token.empty() || managed_keys.count(token) || identities.count(token)) {
                continue;
            }
            std::string normalized = token;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](unsigned char character) {
                               return static_cast<char>(std::tolower(character));
                           });
            if (normalized.find("gcode") != std::string::npos ||
                normalized.find("plugin_config") != std::string::npos) {
                invalid("native material snapshot contains non-portable source override '" +
                        token + "'");
            }
            const bool requested_colour =
                (token == "filament_colour" || token == "filament_multi_colour") &&
                request.contains(token);
            if (!project.contains(token) && !requested_colour) {
                invalid("native material snapshot contains unclassified source override '" +
                        token + "'");
            }
            if (std::find(retained.begin(), retained.end(), token) == retained.end()) {
                retained.push_back(token);
            }
        }
        std::ostringstream rendered;
        for (std::size_t index = 0; index < retained.size(); ++index) {
            if (index != 0) {
                rendered << ';';
            }
            rendered << retained.at(index);
        }
        result.push_back(rendered.str());
    }
    if (trailing != 0) {
        result.insert(result.end(), entries.end() - trailing, entries.end());
    }
    project[key] = result;
}

void update_differences(json &project, const json &dialect,
                        std::size_t slot_count,
                        const std::vector<std::string> &changed_keys,
                        bool merging_sources) {
    if (changed_keys.empty()) {
        return;
    }
    const json &snapshot = required_member(dialect, "filament_snapshot", "package dialect");
    const std::string key = required_string(snapshot, "difference_list_key", "filament snapshot");
    std::vector<std::string> entries;
    if (project.contains(key)) {
        entries = string_array(project.at(key), "project." + key);
    }
    const auto &offset_value = required_member(snapshot, "filament_slot_offset", "filament snapshot");
    const auto &trailing_value = required_member(snapshot, "trailing_entry_count", "filament snapshot");
    if (!offset_value.is_number_unsigned() || !trailing_value.is_number_unsigned()) {
        invalid("filament snapshot entry counts must be non-negative integers");
    }
    const std::size_t expected =
        merging_sources && snapshot.value("merged_difference_list_singleton", false) &&
                entries.size() <= 1
            ? 1
            : offset_value.get<std::size_t>() + slot_count +
                  trailing_value.get<std::size_t>();
    if (entries.empty()) {
        entries.assign(expected, "");
    } else if (entries.size() != expected) {
        invalid("filament snapshot difference-list entry count does not match the slot layout");
    }
    std::set<std::string> current;
    std::stringstream tokens(entries.front());
    std::string token;
    while (std::getline(tokens, token, ';')) {
        if (!token.empty()) {
            current.insert(token);
        }
    }
    for (const auto &changed : changed_keys) {
        current.insert(changed);
    }
    std::ostringstream rendered;
    for (auto iterator = current.begin(); iterator != current.end(); ++iterator) {
        if (iterator != current.begin()) {
            rendered << ';';
        }
        rendered << *iterator;
    }
    entries.front() = rendered.str();
    project[key] = entries;
}

void record_machine_override(json &project, const json &snapshot,
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

void apply_selected_default_material(json &project, const json &dialect,
                                     const FilamentSelection &selection) {
    const auto selected = std::find_if(selection.material_sources.begin(), selection.material_sources.end(),
                                       [](const auto &slot) { return slot.has_value(); });
    if (selected == selection.material_sources.end() || !project.contains("default_filament_profile")) {
        return;
    }
    const auto output_slot = static_cast<std::size_t>(std::distance(selection.material_sources.begin(), selected));
    const auto &identity = project.at("filament_settings_id").at(output_slot);
    auto &value = project["default_filament_profile"];
    const auto original = value;
    if (value.is_array() && !value.empty()) {
        value.at(0) = identity;
    } else if (value.is_string() && !value.get<std::string>().empty()) {
        value = identity;
    }
    if (value != original) {
        record_machine_override(project, required_member(dialect, "filament_snapshot", "package dialect"),
                                  selection.output_count, "default_filament_profile");
    }
}

void apply_native_default_material(json &project, const json &dialect,
                                   std::size_t slot_count) {
    const auto identity = project.at("filament_settings_id").at(0);
    const auto original = project.value("default_filament_profile", json());
    if (original.is_string()) {
        project["default_filament_profile"] = identity;
    } else if (original.is_array() && !original.empty()) {
        project["default_filament_profile"][0] = identity;
    } else {
        project["default_filament_profile"] = json::array({identity});
    }
    if (project.at("default_filament_profile") != original) {
        record_machine_override(project, required_member(dialect, "filament_snapshot", "package dialect"),
                                slot_count, "default_filament_profile");
    }
}

void record_native_colour_overrides(json &project, const json &request,
                                     const json &dialect) {
    const auto &snapshot = required_member(dialect, "filament_snapshot", "package dialect");
    const auto key = required_string(snapshot, "difference_list_key", "filament snapshot");
    const auto offset = required_member(snapshot, "filament_slot_offset", "filament snapshot").get<std::size_t>();
    for (const auto *colour : {"filament_colour", "filament_multi_colour"}) {
        if (!request.contains(colour)) continue;
        for (std::size_t slot = 0; slot < request.at(colour).size(); ++slot) {
            auto &entry = project.at(key).at(offset + slot);
            auto tokens = difference_tokens(entry.get<std::string>());
            tokens.insert(colour);
            entry = render_difference_tokens(tokens);
        }
    }
}

void apply_cut_retraction_policy(json &project, const json &dialect,
                                 std::size_t slot_count,
                                 std::vector<std::string> &changed_keys) {
    const auto &snapshot = required_member(dialect, "filament_snapshot", "package dialect");
    const auto difference_key = required_string(snapshot, "difference_list_key", "filament snapshot");
    const auto offset = required_member(snapshot, "filament_slot_offset", "filament snapshot").get<std::size_t>();
    auto &differences = project.at(difference_key);
    json disabled = json::array();
    for (std::size_t slot = 0; slot < slot_count; ++slot) {
        disabled.push_back("nil");
    }
    for (const auto &key : required_string_array(snapshot, "cut_retraction_filament_keys", "filament snapshot")) {
        if (!project.contains(key)) {
            continue;
        }
        if (project.at(key) != disabled) {
            changed_keys.push_back(key);
        }
        project[key] = disabled;
        for (std::size_t slot = 0; slot < slot_count; ++slot) {
            auto tokens = difference_tokens(differences.at(offset + slot).get<std::string>());
            tokens.insert(key);
            differences.at(offset + slot) = render_difference_tokens(tokens);
        }
    }
    for (const auto &key : required_string_array(snapshot, "cut_retraction_machine_keys", "filament snapshot")) {
        const auto found = project.find(key);
        if (found == project.end() || !found->is_array()) {
            continue;
        }
        const auto original = *found;
        for (auto &value : *found) {
            value = "0";
        }
        if (*found != original) {
            record_machine_override(project, snapshot, slot_count, key);
        }
    }
}

struct MergeSourceSlot {
    std::uint64_t source_slot_id = 0;
    std::size_t source_slot_index = 0;
    std::uint64_t output_slot_id = 0;
    std::size_t output_slot_index = 0;
    std::string slot_name;
    std::string material_id;
    std::string preview_color;
};

struct MergeProjectSource {
    std::string source_id;
    json project;
    std::size_t slot_count = 0;
    std::vector<MergeSourceSlot> slots;
};

struct MergedLogicalSlot {
    std::uint64_t slot_id = 0;
    std::size_t output_index = 0;
    std::string slot_name;
    std::string material_id;
    std::string preview_color;
};

struct MergedProjectResult {
    json project;
    json logical_slots;
    std::size_t slot_count = 0;
};

struct MergeProjectInputs {
    std::vector<MergeProjectSource> sources;
    std::map<std::uint64_t, MergedLogicalSlot> logical_by_id;
    std::map<std::size_t, std::uint64_t> id_by_output_index;
    std::vector<std::map<std::uint64_t, std::size_t>> source_index_by_slot;
};

json source_slot_mappings(const MergeProjectInputs &inputs) {
    json mappings = json::array();
    for (const auto &source : inputs.sources) {
        json mapping = {{"source_id", source.source_id}, {"slots", json::array()}};
        for (const auto &slot : source.slots) {
            mapping["slots"].push_back({{"source_slot_id", slot.source_slot_id},
                {"source_slot_index", slot.source_slot_index},
                {"output_slot_id", slot.output_slot_id},
                {"output_slot_index", slot.output_slot_index}});
        }
        mappings.push_back(std::move(mapping));
    }
    return mappings;
}

const json &merge_required_array(const json &object, std::string_view key,
                                std::string_view context) {
    const json &value = required_member(object, key, context);
    if (!value.is_array() || value.empty()) {
        invalid(std::string(context) + "." + std::string(key) +
                " must be a non-empty array");
    }
    return value;
}

const json &merge_required_object(const json &object, std::string_view key,
                                 std::string_view context) {
    const json &value = required_member(object, key, context);
    if (!value.is_object()) {
        invalid(std::string(context) + "." + std::string(key) + " must be an object");
    }
    return value;
}

std::uint64_t merge_required_id(const json &object, std::string_view key,
                               std::string_view context) {
    const json &value = required_member(object, key, context);
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value >= 0) return static_cast<std::uint64_t>(signed_value);
    }
    invalid(std::string(context) + "." + std::string(key) +
            " must be a non-negative integer");
}

MergeProjectInputs collect_merge_project_inputs(
    const json &first_project, const json &request, const json &dialect,
    const json &machine, const json &plate) {
    const json &source_inputs = merge_required_array(request, "merge_sources", "request");

    MergeProjectInputs result;
    result.sources.reserve(source_inputs.size());
    result.source_index_by_slot.resize(source_inputs.size());
    for (std::size_t source_index = 0; source_index < source_inputs.size(); ++source_index) {
        const json &source_input = source_inputs.at(source_index);
        const std::string context = "request.merge_sources[" +
                                    std::to_string(source_index) + "]";
        if (!source_input.is_object()) invalid(context + " must be an object");
        if ((source_index == 0) == source_input.contains("project_settings")) {
            invalid(context + (source_index == 0
                                   ? " must use the base project argument"
                                   : " must include project_settings"));
        }

        MergeProjectSource source;
        source.source_id = required_string(source_input, "source_id", context);
        if (source.source_id.empty()) invalid(context + ".source_id must not be empty");
        for (const auto &existing : result.sources) {
            if (existing.source_id == source.source_id) {
                invalid("request.merge_sources contains duplicate source_id '" +
                        source.source_id + "'");
            }
        }
        source.project = source_index == 0
                             ? first_project
                             : merge_required_object(source_input, "project_settings", context);
        {
            // Recorded scalar-per-slot fields may retain inactive template
            // tails. Normalize their declared shape for any source hardware.
            const std::size_t native_slot_count = required_string_array(
                source.project, "filament_settings_id", context).size();
            const json &snapshot = required_member(dialect, "filament_snapshot",
                                                   "package dialect");
            for (const auto &rule : merge_required_array(snapshot, "array_rules",
                                                         "filament snapshot")) {
                if (required_string(rule, "selection", "filament array rule") != "default" ||
                    required_member(rule, "group_size", "filament array rule") != 1) {
                    continue;
                }
                for (const auto &key : required_string_array(rule, "keys", "filament array rule")) {
                    if (source.project.contains(key) && source.project.at(key).is_array() &&
                        source.project.at(key).size() > native_slot_count) {
                        source.project[key].erase(source.project[key].begin() + native_slot_count,
                                                  source.project[key].end());
                    }
                }
            }
            const std::string difference_key = required_string(
                snapshot, "difference_list_key", "filament snapshot");
            if (source.project.contains(difference_key) &&
                source.project.at(difference_key).is_array() &&
                source.project.at(difference_key).size() == 1) {
                const auto expected_entries = native_slot_count +
                    snapshot.value("filament_slot_offset", std::size_t(0)) +
                    snapshot.value("trailing_entry_count", std::size_t(0));
                for (std::size_t index = 1; index < expected_entries; ++index) {
                    source.project[difference_key].push_back("");
                }
            }
        }
        validate_slots(source.project, source.slot_count);
        validate_source_hardware(source.project, machine);
        validate_preserved_build_plate(source.project, plate);

        const json &slot_inputs = merge_required_array(source_input, "slots", context);
        std::set<std::uint64_t> source_slot_ids;
        std::set<std::size_t> source_slot_indexes;
        for (std::size_t slot_index = 0; slot_index < slot_inputs.size(); ++slot_index) {
            const json &slot_input = slot_inputs.at(slot_index);
            const std::string slot_context = context + ".slots[" +
                                             std::to_string(slot_index) + "]";
            if (!slot_input.is_object()) invalid(slot_context + " must be an object");
            MergeSourceSlot slot;
            slot.source_slot_id = merge_required_id(slot_input, "source_slot_id", slot_context);
            slot.source_slot_index = slot_input.contains("source_slot_index")
                                         ? static_cast<std::size_t>(merge_required_id(
                                               slot_input, "source_slot_index", slot_context))
                                         : slot_index;
            slot.output_slot_id = slot.source_slot_id;
            if (slot.source_slot_index >= source.slot_count) {
                invalid(slot_context + " source slot index is outside the project arrays");
            }
            slot.slot_name = required_string(slot_input, "slot_name", slot_context);
            slot.material_id = required_string(slot_input, "material_id", slot_context);
            slot.preview_color = required_string(slot_input, "preview_color", slot_context);
            if (!source_slot_ids.insert(slot.source_slot_id).second ||
                !source_slot_indexes.insert(slot.source_slot_index).second) {
                invalid(context + " contains a duplicate source or output slot mapping");
            }

            const MergedLogicalSlot identity{
                slot.output_slot_id, 0, slot.slot_name,
                slot.material_id, slot.preview_color};
            const auto existing = result.logical_by_id.find(slot.output_slot_id);
            if (existing == result.logical_by_id.end()) {
                result.logical_by_id.emplace(slot.output_slot_id, identity);
            } else if (existing->second.material_id != slot.material_id ||
                       existing->second.preview_color != slot.preview_color) {
                invalid("merged output slot " + std::to_string(slot.output_slot_id) +
                        " has conflicting material_id or color in source " +
                        source.source_id);
            }
            result.source_index_by_slot[source_index][slot.output_slot_id] =
                slot.source_slot_index;
            source.slots.push_back(std::move(slot));
        }
        result.sources.push_back(std::move(source));
    }

    const std::size_t output_count = result.logical_by_id.size();
    std::size_t output_index = 0;
    for (auto &[slot_id, identity] : result.logical_by_id) {
        identity.output_index = output_index;
        result.id_by_output_index.emplace(output_index++, slot_id);
    }
    for (auto &source : result.sources) {
        for (auto &slot : source.slots) {
            slot.output_slot_index = result.logical_by_id.at(slot.output_slot_id).output_index;
        }
    }

    const json &snapshot = required_member(dialect, "filament_snapshot", "package dialect");
    const json &rules = merge_required_array(snapshot, "array_rules", "filament snapshot");
    std::set<std::string> slot_array_keys;
    for (const auto &rule : rules) {
        for (const auto &key : required_string_array(rule, "keys", "filament array rule")) {
            if (!slot_array_keys.insert(key).second) {
                invalid("target filament array rules contain duplicate key '" + key + "'");
            }
        }
    }
    const std::string difference_key =
        required_string(snapshot, "difference_list_key", "filament snapshot");
    return result;
}

std::size_t merge_group_width(const json &project, const std::string &key,
                              std::size_t slot_count, std::size_t group_size,
                              bool matrix, bool variable_group,
                              const std::string &source_id) {
    const json &value = required_member(project, key, "merge source " + source_id);
    if (!value.is_array()) {
        invalid("merge source " + source_id + "." + key + " must be an array");
    }
    const std::size_t base_width = matrix ? slot_count * slot_count : slot_count;
    if (value.size() == base_width) return 1;
    if (group_size > 1 && value.size() == base_width * group_size) return group_size;
    if (variable_group && base_width > 0 && !value.empty() &&
        value.size() % base_width == 0) {
        return value.size() / base_width;
    }
    invalid("merge source " + source_id + "." + key +
            " does not match the target's declared slot shape");
}

json merge_transition_defaults(const json &request, const json &machine) {
    if (!request.contains("merge_default_project")) return json();
    const auto &defaults = merge_required_object(request, "merge_default_project", "request");
    validate_source_hardware(defaults, machine);
    return defaults;
}

MergedProjectResult compose_merged_slot_arrays(const MergeProjectInputs &inputs,
                                               const json &dialect,
                                               const json &defaults = json()) {
    const std::size_t output_count = inputs.logical_by_id.size();
    json project = inputs.sources.front().project;
    std::vector<std::set<std::string>> colour_overrides(output_count);
    const json &snapshot = required_member(dialect, "filament_snapshot", "package dialect");
    const json &rules = merge_required_array(snapshot, "array_rules", "filament snapshot");
    const json merged_preserve_first_source_keys = snapshot.value(
        "merged_preserve_first_source_keys", json::array());

    for (const auto &rule : rules) {
        const std::string selection = required_string(rule, "selection", "filament array rule");
        const auto group_value = required_member(rule, "group_size", "filament array rule");
        if (!group_value.is_number_unsigned() || group_value.get<std::size_t>() == 0) {
            invalid("filament array rule group_size must be a positive integer");
        }
        const std::size_t group_size = group_value.get<std::size_t>();
        if (selection != "default" && selection != "default_or_group" &&
            selection != "group" && selection != "matrix" && selection != "slot_index") {
            invalid("unsupported target filament array selection '" + selection + "'");
        }

        for (const auto &key : required_string_array(rule, "keys", "filament array rule")) {
            const bool material_group = required_member(
                rule, "material_source_override", "filament array rule").get<bool>();
            if (contains_string(merged_preserve_first_source_keys, key)) {
                continue;
            }
            bool any_source_has_key = false;
            for (const auto &source : inputs.sources) {
                any_source_has_key = any_source_has_key || source.project.contains(key);
            }
            if (key == "filament_colour") {
                json colors = json::array();
                for (std::size_t output_index = 0; output_index < output_count; ++output_index) {
                    const auto slot_id = inputs.id_by_output_index.at(output_index);
                    const auto &identity = inputs.logical_by_id.at(slot_id);
                    colors.push_back(identity.preview_color);
                    for (std::size_t source_index = 0;
                         source_index < inputs.sources.size(); ++source_index) {
                        const auto mapped = inputs.source_index_by_slot[source_index].find(slot_id);
                        if (mapped == inputs.source_index_by_slot[source_index].end()) continue;
                        const auto &source = inputs.sources[source_index];
                        if (!source.project.contains(key)) {
                            colour_overrides[output_index].insert(key);
                            continue;
                        }
                        const json &source_colors = source.project.at(key);
                        if (!source_colors.is_array() || mapped->second >= source_colors.size()) {
                            invalid("merge source " + source.source_id +
                                    "." + key + " does not contain a mapped source slot");
                        }
                        if (source_colors.at(mapped->second) != identity.preview_color) {
                            colour_overrides[output_index].insert(key);
                        }
                    }
                }
                project[key] = std::move(colors);
                continue;
            }
            if (!any_source_has_key) {
                project.erase(key);
                continue;
            }

            if (selection == "slot_index") {
                std::optional<std::size_t> output_width;
                for (std::size_t output_index = 0; output_index < output_count; ++output_index) {
                    const auto slot_id = inputs.id_by_output_index.at(output_index);
                    for (std::size_t source_index = 0;
                         source_index < inputs.sources.size(); ++source_index) {
                        const auto mapped = inputs.source_index_by_slot[source_index].find(slot_id);
                        if (mapped == inputs.source_index_by_slot[source_index].end()) continue;
                        const auto &source = inputs.sources[source_index];
                        if (!source.project.contains(key)) {
                            invalid("merged source " + source.source_id +
                                    " is missing slot index field " + key);
                        }
                        const auto width = merge_group_width(
                            source.project, key, source.slot_count, group_size, false,
                            material_group && selection == "default_or_group",
                            source.source_id);
                        if (output_width && *output_width != width) {
                            invalid("merged source slot field " + key +
                                    " has different variant counts");
                        }
                        output_width = width;
                    }
                }
                const std::size_t width = output_width.value_or(group_size);
                json values = json::array();
                for (std::size_t output_index = 0; output_index < output_count; ++output_index) {
                    for (std::size_t variant = 0; variant < width; ++variant) {
                        values.push_back(std::to_string(output_index + 1));
                    }
                }
                project[key] = std::move(values);
                continue;
            }

            if (selection == "matrix") {
                std::optional<std::size_t> output_width;
                json values = json::array();
                for (std::size_t row = 0; row < output_count; ++row) {
                    const auto row_id = inputs.id_by_output_index.at(row);
                    for (std::size_t column = 0; column < output_count; ++column) {
                        const auto column_id = inputs.id_by_output_index.at(column);
                        std::optional<std::vector<json>> chosen;
                        std::string chosen_source;
                        for (std::size_t source_index = 0;
                             source_index < inputs.sources.size(); ++source_index) {
                            const auto row_source = inputs.source_index_by_slot[source_index].find(row_id);
                            const auto column_source = inputs.source_index_by_slot[source_index].find(column_id);
                            if (row_source == inputs.source_index_by_slot[source_index].end() ||
                                column_source == inputs.source_index_by_slot[source_index].end()) {
                                continue;
                            }
                            const auto &source = inputs.sources[source_index];
                            if (!source.project.contains(key)) {
                                invalid("merged source " + source.source_id +
                                        " is missing matrix field " + key);
                            }
                            const auto width = merge_group_width(
                                source.project, key, source.slot_count, group_size, true, false,
                                source.source_id);
                            if (output_width && *output_width != width) {
                                invalid("merged source matrix field " + key +
                                        " has different variant counts");
                            }
                            if (!output_width) {
                                values = json::array_t(output_count * output_count * width);
                            }
                            output_width = width;
                            const json &matrix = source.project.at(key);
                            std::vector<json> candidate;
                            for (std::size_t nozzle = 0; nozzle < width; ++nozzle) {
                                const auto index = nozzle * source.slot_count * source.slot_count +
                                                   row_source->second * source.slot_count + column_source->second;
                                candidate.push_back(matrix.at(index));
                            }
                            if (chosen && *chosen != candidate) {
                                invalid("merged source conflict for " + key +
                                        " at global slots " + std::to_string(row_id) +
                                        " and " + std::to_string(column_id) + " between " +
                                        chosen_source + " and " + source.source_id);
                            }
                            chosen = std::move(candidate);
                            chosen_source = source.source_id;
                        }
                        if (!chosen && defaults.contains(key)) {
                            const auto width = merge_group_width(defaults, key, output_count,
                                group_size, true, false, "native transition defaults");
                            if (output_width && *output_width != width) invalid("native transition default width differs from source");
                            if (!output_width) values = json::array_t(output_count * output_count * width);
                            output_width = width;
                            std::vector<json> candidate;
                            for (std::size_t nozzle = 0; nozzle < width; ++nozzle) {
                                candidate.push_back(defaults.at(key).at(nozzle * output_count * output_count + row * output_count + column));
                            }
                            chosen = std::move(candidate);
                        }
                        if (!chosen) {
                            invalid("no source contains both global slots needed for merged matrix " + key);
                        }
                        for (std::size_t nozzle = 0; nozzle < chosen->size(); ++nozzle) {
                            const auto index = nozzle * output_count * output_count + row * output_count + column;
                            values.at(index) = chosen->at(nozzle);
                        }
                    }
                }
                project[key] = std::move(values);
                continue;
            }

            std::optional<std::size_t> output_width;
            std::vector<std::vector<json>> merged_values(output_count);
            std::vector<std::string> chosen_sources(output_count);
            for (std::size_t output_index = 0; output_index < output_count; ++output_index) {
                const auto output_id = inputs.id_by_output_index.at(output_index);
                for (std::size_t source_index = 0;
                     source_index < inputs.sources.size(); ++source_index) {
                    const auto mapped = inputs.source_index_by_slot[source_index].find(output_id);
                    if (mapped == inputs.source_index_by_slot[source_index].end()) continue;
                    const auto &source = inputs.sources[source_index];
                    if (!source.project.contains(key)) {
                        invalid("merged source " + source.source_id +
                                " is missing slot field " + key + " for global slot " +
                                std::to_string(output_id));
                    }
                    const json &array = source.project.at(key);
                    const bool broadcast_singleton =
                        rule.value("broadcast_singleton", false) &&
                        array.is_array() && array.size() == 1;
                    const auto width = broadcast_singleton ? 1 : merge_group_width(
                        source.project, key, source.slot_count, group_size, false,
                        material_group && selection == "default_or_group",
                        source.source_id);
                    if (output_width && *output_width != width) {
                        invalid("merged source slot field " + key +
                                " has different variant counts");
                    }
                    output_width = width;
                    std::vector<json> candidate;
                    for (std::size_t variant = 0; variant < width; ++variant) {
                        json value = array.at(
                            broadcast_singleton ? 0 : mapped->second * width + variant);
                        // A source may store a real multi-colour description here.
                        // Only a mirror of its plain preview colour follows a new
                        // global preview colour; independent source values survive.
                        if (key == "filament_multi_colour" &&
                            source.project.contains("filament_colour") &&
                            value == source.project.at("filament_colour").at(mapped->second)) {
                            const auto &colour = inputs.logical_by_id.at(output_id).preview_color;
                            if (value != colour) colour_overrides[output_index].insert(key);
                            value = colour;
                        }
                        candidate.push_back(std::move(value));
                    }
                    if (!merged_values[output_index].empty() &&
                        merged_values[output_index] != candidate) {
                        invalid("merged source conflict for " + key + " at global slot " +
                                std::to_string(output_id) + " between " +
                                chosen_sources[output_index] + " and " + source.source_id);
                    }
                    if (merged_values[output_index].empty()) {
                        merged_values[output_index] = std::move(candidate);
                        chosen_sources[output_index] = source.source_id;
                    }
                }
                if (merged_values[output_index].empty()) {
                    invalid("no source supplies " + key + " for merged global slot " +
                            std::to_string(output_id));
                }
            }
            json values = json::array();
            for (const auto &slot_values : merged_values) {
                for (const auto &value : slot_values) values.push_back(value);
            }
            project[key] = std::move(values);
        }
    }

    const auto offset_value = required_member(snapshot, "filament_slot_offset",
                                              "filament snapshot");
    const auto trailing_value = required_member(snapshot, "trailing_entry_count",
                                                "filament snapshot");
    if (!offset_value.is_number_unsigned() || !trailing_value.is_number_unsigned()) {
        invalid("filament snapshot offsets must be non-negative integers");
    }
    const std::size_t offset = offset_value.get<std::size_t>();
    const std::size_t trailing = trailing_value.get<std::size_t>();
    const std::string difference_key =
        required_string(snapshot, "difference_list_key", "filament snapshot");
    bool any_differences = false;
    for (const auto &source : inputs.sources) {
        any_differences = any_differences || source.project.contains(difference_key);
    }
    // Older Flash projects use one shared entry; native exports retain the
    // process + material slots + machine tail and use the regular remapping.
    const bool singleton_differences =
        snapshot.value("merged_difference_list_singleton", false) &&
        std::all_of(inputs.sources.begin(), inputs.sources.end(),
                    [&](const MergeProjectSource &source) {
                        const auto entries = source.project.find(difference_key);
                        return entries != source.project.end() && entries->is_array() &&
                               entries->size() == 1;
                    });
    if (any_differences && singleton_differences) {
        project[difference_key] = inputs.sources.front().project.at(difference_key);
    } else if (any_differences) {
        std::vector<std::string> prefix;
        std::vector<std::string> tail;
        bool have_baseline = false;
        std::vector<std::optional<std::set<std::string>>> slot_differences(output_count);
        for (const auto &source : inputs.sources) {
            if (!source.project.contains(difference_key)) {
                invalid("merged source " + source.source_id +
                        " is missing slot difference registrations");
            }
            const auto entries = string_array(source.project.at(difference_key),
                                              "merge source " + difference_key);
            if (entries.size() != offset + source.slot_count + trailing) {
                invalid("merge source " + source.source_id + " has a mismatched " +
                        difference_key + " slot count");
            }
            std::vector<std::string> current_prefix(entries.begin(), entries.begin() + offset);
            std::vector<std::string> current_tail(entries.end() - trailing, entries.end());
            if (!have_baseline) {
                prefix = std::move(current_prefix);
                tail = std::move(current_tail);
                have_baseline = true;
            } else if (prefix != current_prefix || tail != current_tail) {
                invalid("merged sources have different process or machine-tail difference entries");
            }
            for (const auto &slot : source.slots) {
                auto &chosen = slot_differences[slot.output_slot_index];
                const auto tokens = difference_tokens(entries.at(offset + slot.source_slot_index));
                if (chosen && *chosen != tokens) {
                    invalid("merged source conflict for " + difference_key +
                            " at global slot " + std::to_string(slot.output_slot_id));
                }
                chosen = tokens;
            }
        }
        json differences = json::array();
        for (const auto &entry : prefix) differences.push_back(entry);
        for (std::size_t output_index = 0; output_index < output_count; ++output_index) {
            if (!slot_differences[output_index]) {
                invalid("no source supplies difference registrations for merged slot " +
                        std::to_string(inputs.id_by_output_index.at(output_index)));
            }
            auto tokens = *slot_differences[output_index];
            tokens.insert(colour_overrides[output_index].begin(),
                          colour_overrides[output_index].end());
            differences.push_back(render_difference_tokens(tokens));
        }
        for (const auto &entry : tail) differences.push_back(entry);
        project[difference_key] = std::move(differences);
    }

    if (!snapshot.value("merged_preserve_default_filament_profile", false) &&
        project.contains("default_filament_profile") &&
        project.contains("filament_settings_id") &&
        project.at("filament_settings_id").is_array() &&
        !project.at("filament_settings_id").empty()) {
        const json original = project.at("default_filament_profile");
        const json identity = project.at("filament_settings_id").at(0);
        if (original.is_array() && !original.empty()) {
            project["default_filament_profile"][0] = identity;
        } else if (original.is_string()) {
            project["default_filament_profile"] = identity;
        }
        if (project.at("default_filament_profile") != original && any_differences) {
            record_machine_override(project, snapshot, output_count,
                                    "default_filament_profile");
        }
    }

    json logical_slots = json::array();
    for (std::size_t index = 0; index < output_count; ++index) {
        const auto &slot = inputs.logical_by_id.at(inputs.id_by_output_index.at(index));
        logical_slots.push_back({{"slot_id", slot.slot_id},
                                 {"slot_name", slot.slot_name},
                                 {"preview_color", slot.preview_color},
                                 {"material_id", slot.material_id}});
    }
    return {std::move(project), std::move(logical_slots), output_count};
}

json effective_summary(const json &project, const std::string &material_mode,
                       const std::string &machine_uid,
                       const std::string &nozzle_uid,
                       const std::string &plate_uid) {
    json summary = {
        {"material_mode", material_mode},
        {"machine_uid", machine_uid},
        {"nozzle_uid", nozzle_uid},
        {"build_plate_uid", plate_uid},
    };
    static const std::vector<std::string> keys = {
        "version",
        "printer_settings_id",
        "printer_model",
        "printer_variant",
        "nozzle_diameter",
        "printable_area",
        "printable_height",
        "print_settings_id",
        "curr_bed_type",
        "layer_height",
        "initial_layer_print_height",
        "initial_layer_height",
        "sparse_infill_density",
        "sparse_infill_pattern",
        "brim_type",
        "brim_width",
        "enable_prime_tower",
        "prime_tower_width",
        "prime_tower_brim_width",
        "prime_tower_rib_wall",
        "prime_tower_rib_width",
        "prime_tower_extra_rib_length",
        "prime_tower_fillet_wall",
        "prime_tower_infill_gap",
        "wipe_tower_wall_type",
        "wipe_tower_rib_width",
        "wipe_tower_extra_rib_length",
        "wipe_tower_fillet_wall",
        "wipe_tower_cone_angle",
        "wipe_tower_rotation_angle",
        "wipe_tower_x",
        "wipe_tower_y",
        "bed_exclude_area",
        "extruder_printable_area",
        "filament_prime_volume",
        "prime_volume",
        "flush_volumes_vector",
        "filament_change_length",
        "filament_diameter",
    };
    for (const auto &key : keys) {
        if (project.contains(key)) {
            summary[key] = project.at(key);
        }
    }
    return summary;
}

json metadata_result(const json &project, const json &dialect,
                     const json &plate, json wipe_tower_dialect, json summary) {
    json defaults = {
        {"plate_value", required_string(plate, "plate_value", "build plate")},
        {"sidecar_bed_value", required_string(plate, "sidecar_value", "build plate")},
        {"identify_id", "1"}, {"plate_summary", false},
    };
    json properties = json::object();
    const auto components = dialect.find("metadata_components");
    if (components != dialect.end() && components->is_object()) {
        defaults["identify_id"] = components->value("identify_id", "1");
        defaults["plate_summary"] = components->contains("plate_summary");
        if (components->value("slice_uuid", false)) {
            defaults["slice_uuid_seed_prefix"] = required_string(
                *components, "slice_uuid_seed_prefix", "metadata components");
        }
        if (components->contains("build_item_properties")) {
            properties = components->at("build_item_properties");
            if (!properties.is_object()) {
                invalid("package dialect metadata_components.build_item_properties must be an object");
            }
        }
    }
    return {{"project_settings_json", project.dump()},
            {"wipe_tower_dialect", std::move(wipe_tower_dialect)},
            {"build_item_properties", std::move(properties)},
            {"metadata_defaults", std::move(defaults)},
            {"effective_settings", std::move(summary)}};
}

json source_plate(const json &project, const json &request, const json &target) {
    if (request.contains("build_plate_uid")) {
        return find_binding(target, "build_plate_bindings", "build_plate_uid",
                            required_string(request, "build_plate_uid", "request"));
    }
    const auto value = project.value("curr_bed_type", std::string());
    for (const auto &binding : target.at("build_plate_bindings")) {
        if (binding.value("project_value", std::string()) == value) return binding;
    }
    // Legacy source exports retain unknown source bed names and use Textured
    // PEI only when the source itself has no bed selection.
    json fallback;
    for (const auto &binding : target.at("build_plate_bindings")) {
        if (binding.value("project_value", std::string()) == "Textured PEI Plate") {
            fallback = binding;
            break;
        }
    }
    if (value.empty() && !fallback.is_null()) return fallback;
    const auto bed = value.empty() ? std::string("Textured PEI Plate") : value;
    return {{"build_plate_uid", ""}, {"project_value", bed},
            {"plate_value", bed}, {"sidecar_value", fallback.is_null() ? "textured_plate" : fallback.at("sidecar_value")}};
}

json compose_preserved_source(json project, const json &request, const json &target) {
    if (request.value("material_mode", "preserve_template") != "preserve_template" ||
        request.contains("material_uid") || request.contains("material_uids")) {
        invalid("preserve_source hardware requires source materials rather than native material bindings");
    }
    json plate = source_plate(project, request, target);
    const bool merging = request.contains("merge_sources");
    const auto &dialect = required_member(target, "package_dialect", "target data");
    json mappings = json::array();
    json logical_slots = json::array();
    if (merging) {
        const auto &filament_snapshot = required_member(
            dialect, "filament_snapshot", "package dialect");
        // Compare actual source hardware, including every physical nozzle and
        // bed field; a canonical machine binding is deliberately not involved.
        for (const auto &source : request.at("merge_sources")) {
            if (!source.contains("project_settings")) continue;
            const auto &other = source.at("project_settings");
            for (const auto *key : {"printer_model", "nozzle_diameter", "printable_area",
                                    "printable_height", "curr_bed_type"}) {
                if (project.value(key, json()) != other.value(key, json())) {
                    invalid(std::string("merged source hardware differs at ") + key);
                }
            }
        }
        const json machine = {{"printer_model", required_member(project, "printer_model", "source project")},
                              {"nozzle_diameter", required_member(project, "nozzle_diameter", "source project")}};
        project = detail::prepare_source_merge_project(project, filament_snapshot);
        json merge_request = request;
        for (auto &source : merge_request.at("merge_sources")) {
            if (source.contains("project_settings")) {
                source["project_settings"] = detail::prepare_source_merge_project(
                    source.at("project_settings"), filament_snapshot);
            }
        }
        const json merge_dialect = detail::source_merge_dialect(dialect, project);
        const auto inputs = collect_merge_project_inputs(project, merge_request, merge_dialect,
                                                         machine, plate);
        auto merged = compose_merged_slot_arrays(inputs, merge_dialect,
            merge_transition_defaults(request, machine));
        project = std::move(merged.project);
        logical_slots = std::move(merged.logical_slots);
        mappings = source_slot_mappings(inputs);
    }
    project = detail::compose_source_project(project, request, target);
    if (request.contains("build_plate_uid")) project["curr_bed_type"] = plate.at("project_value");
    std::vector<std::string> changed;
    apply_scalar_overrides(project, request, changed);
    json tower_dialect = required_member(dialect, "wipe_tower", "package dialect");
    apply_tower_overrides(project, request, target);
    auto summary = effective_summary(project, "preserve_template", "", "",
                                      plate.value("build_plate_uid", ""));
    summary["hardware_mode"] = "preserve_source";
    json result = metadata_result(project, dialect, plate, std::move(tower_dialect),
                                   std::move(summary));
    if (merging) {
        result["merged_slots"] = std::move(logical_slots);
        result["source_slot_mappings"] = std::move(mappings);
    }
    return result;
}

}  // namespace

std::string compose_project_settings(std::string_view base_project_json,
                                     std::string_view request_json,
                                     std::string_view canonical_json,
                                     std::string_view target_json) {
    json project = parse_json(base_project_json, "base project settings");
    json request = parse_json(request_json, "project settings request");
    const json canonical = parse_json(canonical_json, "canonical data");
    const json target = parse_json(target_json, "target data");
    validate_request_keys(request);
    if (request.value("hardware_mode", "target_binding") == "auto" &&
        project.contains("printer_settings_id")) {
        project["print_compatible_printers"] = json::array({project.at("printer_settings_id")});
    }

    const std::string slicer_id = required_string(request, "slicer_id", "request");
    const std::string application_version =
        required_string(request, "application_version", "request");
    const json &contract = required_member(target, "target_contract", "target data");
    if (required_string(contract, "slicer_id", "target contract") != slicer_id ||
        required_string(contract, "application_version", "target contract") != application_version) {
        invalid("request slicer/version does not match the target data");
    }
    auto hardware_mode = request.value("hardware_mode", "target_binding");
    if (hardware_mode == "auto") {
        auto resolved = detail::native_source_request(project, request, target);
        const auto plate = source_plate(project, request, target);
        if (!resolved.is_null() && !plate.value("build_plate_uid", "").empty()) {
            resolved["build_plate_uid"] = plate.at("build_plate_uid");
            request = std::move(resolved);
            hardware_mode = "target_binding";
        } else {
            request["hardware_mode"] = "preserve_source";
            hardware_mode = "preserve_source";
        }
    }
    if (hardware_mode == "preserve_source") {
        return compose_preserved_source(std::move(project), request, target).dump();
    }
    if (hardware_mode != "target_binding") invalid("unsupported request.hardware_mode");
    const std::string machine_uid = required_string(request, "machine_uid", "request");
    const std::string nozzle_uid = required_string(request, "nozzle_uid", "request");
    const std::string plate_uid = required_string(request, "build_plate_uid", "request");
    const std::string material_mode = required_string(request, "material_mode", "request");
    if (material_mode != "preserve_template" && material_mode != "target_native_preset") {
        invalid("request.material_mode must be preserve_template or target_native_preset");
    }
    const bool merging_sources = request.contains("merge_sources");
    if (merging_sources && material_mode != "preserve_template") {
        invalid("merged source projects must preserve already-composed materials");
    }
    if (merging_sources &&
        (request.contains("filament_slot_mode") ||
         request.contains("default_filament_source_slot") ||
         request.contains("filament_source_slots"))) {
        invalid("merge_sources already contains the explicit source-to-output slot mapping");
    }
    if (request.contains("material_uid") && request.contains("material_uids")) {
        invalid("request.material_uid and request.material_uids are mutually exclusive");
    }
    if (material_mode == "preserve_template" &&
        (request.contains("material_uid") || request.contains("material_uids"))) {
        invalid("preserve_template does not accept material_uid or material_uids native selections");
    }
    validate_canonical_selection(canonical, machine_uid, nozzle_uid, plate_uid);

    const json &machine = find_binding(
        target, "machine_bindings", "machine_uid", machine_uid,
        std::make_pair(std::string("nozzle_uid"), nozzle_uid), std::nullopt, true);
    const json &plate = find_binding(target, "build_plate_bindings", "build_plate_uid", plate_uid);
    if (machine.contains("supported_build_plate_uids") &&
        !contains_string(machine.at("supported_build_plate_uids"), plate_uid)) {
        invalid("target machine/nozzle does not support the requested build plate '" + plate_uid + "'");
    }
    const json &dialect = required_member(target, "package_dialect", "target data");
    json merged_logical_slots = json::array();
    json mappings = json::array();
    std::size_t slot_count = 0;
    if (merging_sources) {
        // Hardware binding does not change the already-composed material shape.
        // Derive the merge plan from actual source fields, as preserve_source does.
        const auto &snapshot = required_member(dialect, "filament_snapshot", "package dialect");
        project = detail::prepare_source_merge_project(project, snapshot);
        json merge_request = request;
        for (auto &source : merge_request.at("merge_sources")) {
            if (source.contains("project_settings")) {
                source["project_settings"] = detail::prepare_source_merge_project(
                    source.at("project_settings"), snapshot);
            }
        }
        const json merge_dialect = detail::source_merge_dialect(dialect, project);
        const auto inputs = collect_merge_project_inputs(
            project, merge_request, merge_dialect, machine, plate);
        auto merged = compose_merged_slot_arrays(inputs, merge_dialect,
            merge_transition_defaults(request, machine));
        project = std::move(merged.project);
        if (!project.contains("curr_bed_type")) {
            project["curr_bed_type"] = required_string(
                plate, "project_value", "build plate");
        }
        merged_logical_slots = std::move(merged.logical_slots);
        slot_count = merged.slot_count;
        mappings = source_slot_mappings(inputs);
    } else {
        validate_source_hardware(project, machine);
        prepare_registry_only_native_project(project, machine, material_mode, request);
        validate_slots(project, slot_count);
    }
    const auto selection = filament_selection(request, slot_count);
    if (material_mode == "preserve_template" && !merging_sources) {
        validate_preserved_build_plate(project, plate);
    }
    const auto materials = material_mode == "target_native_preset"
        ? resolve_native_materials(request, canonical, target, selection.output_count)
        : std::vector<NativeMaterialSlot>();

    std::vector<std::string> changed_keys;
    const bool material_source_selected = std::any_of(
        selection.material_sources.begin(), selection.material_sources.end(),
        [](const auto &slot) { return slot.has_value(); });
    if (!merging_sources && (selection.compact || material_source_selected)) {
        compose_filament_arrays(project, request, dialect, machine, selection, changed_keys);
        slot_count = selection.output_count;
    } else if (!merging_sources) {
        select_first_slot_variants(project, machine, slot_count);
    }

    if (material_mode == "target_native_preset") {
        // The requested plate is explicit; material selection does not change
        // printer/process identity, geometry, or the source format version.
        project["curr_bed_type"] = required_string(plate, "project_value", "build plate");
    }
    apply_scalar_overrides(project, request, changed_keys);
    if (material_mode == "target_native_preset") {
        apply_target_native_materials(project, materials, machine);
        clear_native_differences(project, dialect, slot_count, materials, request);
        apply_native_default_material(project, dialect, slot_count);
    } else if (material_source_selected) {
        apply_selected_default_material(project, dialect, selection);
    }
    apply_tower_overrides(project, request, target);
    apply_colour_overrides(project, request, slot_count, changed_keys);
    if (material_mode == "target_native_preset") {
        record_native_colour_overrides(project, request, dialect);
    }
    if (selection.disable_cut_retraction) {
        apply_cut_retraction_policy(project, dialect, slot_count, changed_keys);
    }
    update_differences(project, dialect, slot_count, changed_keys, merging_sources);

    json wipe_tower_dialect = required_member(dialect, "wipe_tower", "package dialect");

    detail::apply_source_flush_defaults(project, request, target);
    json result = metadata_result(project, dialect, plate,
        std::move(wipe_tower_dialect),
        effective_summary(project, material_mode, machine_uid, nozzle_uid, plate_uid));
    result["effective_settings"]["hardware_mode"] = "target_binding";
    if (merging_sources) {
        result["merged_slots"] = std::move(merged_logical_slots);
        result["source_slot_mappings"] = std::move(mappings);
    }
    return result.dump();
}

}  // namespace fatcat
