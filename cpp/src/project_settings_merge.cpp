#include "project_settings_merge.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "fatcat/source_project_settings.h"
#include "project_settings_internal.h"

namespace fatcat::detail {
namespace {

using namespace project_settings_internal;

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

json merged_colour_values(const MergeProjectInputs &inputs, const std::string &key,
                          std::vector<std::set<std::string>> &colour_overrides) {
    const std::size_t output_count = inputs.logical_by_id.size();
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
    return colors;
}

json merged_slot_indices(const MergeProjectInputs &inputs, const std::string &key,
                         std::size_t group_size) {
    const std::size_t output_count = inputs.logical_by_id.size();
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
                source.project, key, source.slot_count, group_size, false, false,
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
    return values;
}

json merged_transition_matrix(const MergeProjectInputs &inputs, const std::string &key,
                              std::size_t group_size, const json &defaults) {
    const std::size_t output_count = inputs.logical_by_id.size();
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
    return values;
}

json merged_material_values(const MergeProjectInputs &inputs, const std::string &key,
                            const json &rule, std::size_t group_size, bool variable_group,
                            std::vector<std::set<std::string>> &colour_overrides) {
    const std::size_t output_count = inputs.logical_by_id.size();
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
                variable_group,
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
    return values;
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
                project[key] = merged_colour_values(inputs, key, colour_overrides);
                continue;
            }

            if (!any_source_has_key) {
                project.erase(key);
                continue;
            }

            if (selection == "slot_index") {
                project[key] = merged_slot_indices(inputs, key, group_size);
                continue;
            }

            if (selection == "matrix") {
                project[key] = merged_transition_matrix(inputs, key, group_size, defaults);
                continue;
            }

            project[key] = merged_material_values(
                inputs, key, rule, group_size,
                material_group && selection == "default_or_group", colour_overrides);
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

}  // namespace

MergedProjectSettings merge_project_settings(
    json project, const json &request, const json &dialect,
    const json &machine, const json &plate) {
    const auto &snapshot = required_member(dialect, "filament_snapshot", "package dialect");
    project = prepare_source_merge_project(project, snapshot);
    json merge_request = request;
    for (auto &source : merge_request.at("merge_sources")) {
        if (source.contains("project_settings")) {
            source["project_settings"] = prepare_source_merge_project(
                source.at("project_settings"), snapshot);
        }
    }
    const json merge_dialect = source_merge_dialect(dialect, project);
    const auto inputs = collect_merge_project_inputs(
        project, merge_request, merge_dialect, machine, plate);
    auto merged = compose_merged_slot_arrays(inputs, merge_dialect,
        merge_transition_defaults(request, machine));
    return {std::move(merged.project), std::move(merged.logical_slots),
            source_slot_mappings(inputs), merged.slot_count};
}

}  // namespace fatcat::detail
