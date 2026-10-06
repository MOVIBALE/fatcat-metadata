#include "fatcat/project_settings.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "json_object.h"

#include "fatcat/source_project_settings.h"
#include "difference_index.h"
#include "filament_projection.h"
#include "project_settings_internal.h"
#include "project_settings_merge.h"
#include "project_settings_overrides.h"
#include "prusa_project.h"

namespace fatcat {
namespace {

using namespace detail::project_settings_internal;
using detail::difference_tokens;
using detail::render_difference_tokens;

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

void validate_request_keys(const json &request, const json &target) {
    static const std::set<std::string> allowed = {
        "slicer_id",
        "application_version",
        "machine_uid",
        "nozzle_uid",
        "build_plate_uid",
        "material_uid",
        "material_uids",
        "material_mode",
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
        if (allowed.find(key) == allowed.end() && !detail::is_process_override_key(target, key)) {
            invalid("request contains unsupported field '" + key +
                    "' in FatCat's request API [sdk_not_supported]; native compatibility has not been checked");
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
        required_member(dialect, "filament_snapshot", "package dialect");
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
        auto merged = detail::merge_project_settings(
            std::move(project), request, dialect, machine, plate);
        project = std::move(merged.project);
        logical_slots = std::move(merged.logical_slots);
        mappings = std::move(merged.source_slot_mappings);
    }
    project = detail::compose_source_project(project, request, target);
    if (request.contains("build_plate_uid")) project["curr_bed_type"] = plate.at("project_value");
    std::vector<std::string> changed;
    detail::apply_scalar_overrides(project, request, target, changed);
    json tower_dialect = required_member(dialect, "wipe_tower", "package dialect");
    detail::apply_tower_overrides(project, request, target);
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

nlohmann::json detail::resolve_source_plate(const json &project, const json &request, const json &target) {
    return source_plate(project, request, target);
}

std::string compose_project_settings(std::string_view base_project_json,
                                     std::string_view request_json,
                                     std::string_view canonical_json,
                                     std::string_view target_json) {
    json project = detail::parse_json_object<ProjectSettingsError>(base_project_json, "base project settings");
    json request = detail::parse_json_object<ProjectSettingsError>(request_json, "project settings request");
    const json canonical = detail::parse_json_object<ProjectSettingsError>(canonical_json, "canonical data");
    const json target = detail::parse_json_object<ProjectSettingsError>(target_json, "target data");
    if (detail::prusa::is_target(target)) return detail::prusa::compose(project, request, target).dump();
    validate_request_keys(request, target);
    detail::resolve_process_overrides(request, target);
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
        auto merged = detail::merge_project_settings(
            std::move(project), request, dialect, machine, plate);
        project = std::move(merged.project);
        if (!project.contains("curr_bed_type")) {
            project["curr_bed_type"] = required_string(
                plate, "project_value", "build plate");
        }
        merged_logical_slots = std::move(merged.logical_slots);
        slot_count = merged.slot_count;
        mappings = std::move(merged.source_slot_mappings);
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
    detail::apply_scalar_overrides(project, request, target, changed_keys);
    if (material_mode == "target_native_preset") {
        apply_target_native_materials(project, materials, machine);
        clear_native_differences(project, dialect, slot_count, materials, request);
        apply_native_default_material(project, dialect, slot_count);
    } else if (material_source_selected) {
        apply_selected_default_material(project, dialect, selection);
    }
    detail::apply_tower_overrides(project, request, target);
    detail::apply_colour_overrides(project, request, slot_count, changed_keys);
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
