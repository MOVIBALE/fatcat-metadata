#include "fatcat/source_project_settings.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "fatcat/project_settings.h"
#include "difference_index.h"
#include "filament_projection.h"

namespace fatcat::detail {
namespace {

using json = nlohmann::json;

std::string text(const json &value) {
    if (value.is_null()) return "";
    return value.is_string() ? value.get<std::string>() : value.dump();
}

std::string stripped(std::string value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::size_t filament_count(const json &project) {
    std::size_t count = 0;
    for (const auto *key : {"filament_type", "filament_settings_id", "filament_vendor", "filament_ids"}) {
        const auto value = project.find(key);
        if (value != project.end() && value->is_array()) count = std::max(count, value->size());
    }
    return count;
}

std::size_t default_slot(const json &project) {
    for (const auto *key : {"filament_type", "filament_settings_id"}) {
        const auto values = project.find(key);
        if (values == project.end() || !values->is_array()) continue;
        for (std::size_t index = 0; index < values->size(); ++index) {
            const auto value = upper(text(values->at(index)));
            if (value.find("PLA") != std::string::npos && value.find("PETG") == std::string::npos) return index;
        }
        for (std::size_t index = 0; index < values->size(); ++index) {
            if (upper(text(values->at(index))).find("PETG") == std::string::npos) return index;
        }
    }
    return 0;
}

json slot_value(const json &values, std::size_t index, std::size_t count) {
    if (!values.is_array()) return nullptr;
    const auto selected = count > 0 && values.size() == count * 2 ? index * 2 : index;
    return selected < values.size() ? values.at(selected) : json();
}

json default_value(const json &values, const json &source) {
    auto value = slot_value(values, default_slot(source), filament_count(source));
    if (!value.is_null()) return value;
    return values.empty() ? json("0") : values.at(0);
}

json repeat(const json &value, std::size_t count) {
    json result = json::array();
    for (std::size_t i = 0; i < count; ++i) result.push_back(value);
    return result;
}

json source_slot_colour(const json &colours, std::size_t index) {
    return colours.value("Slot " + std::to_string(index + 1),
                         colours.value(std::to_string(index), json("#C8C8C8")));
}

void rebuild_flush_matrix_from_vector(json &project) {
    const auto count = filament_count(project);
    const auto values = project.find("flush_volumes_vector");
    if (count == 0 || values == project.end() || !values->is_array() ||
        values->size() != count * 2) return;

    std::vector<double> volumes;
    for (const auto &value : *values) volumes.push_back(std::stod(text(value)));
    json matrix = json::array();
    // Flash Studio 1.7.15, PresetBundle::update_multi_material_filament_presets:
    // an unconfigured transition is source unload plus destination load;
    // the same slot needs no flush. This is a default, not color prediction.
    // https://github.com/FlashForge/Orca-Flashforge/blob/84b7074b252af144a75fec04a0db3c21e6e65536/src/libslic3r/PresetBundle.cpp#L5538
    for (std::size_t from = 0; from < count; ++from) {
        for (std::size_t to = 0; to < count; ++to) {
            matrix.push_back(from == to ? "0" :
                json(volumes[from * 2] + volumes[to * 2 + 1]).dump());
        }
    }
    project["flush_volumes_matrix"] = std::move(matrix);
}

std::string process_text(const json &value) {
    if (value.is_array()) return value.empty() ? "" : process_text(value.front());
    if (value.is_boolean()) return value.get<bool>() ? "1" : "0";
    return text(value);
}

json process_overrides(const json &request) {
    json result = json::object();
    const auto process = request.value("process_settings", json::object());
    for (const auto *key : {"layer_height", "initial_layer_height", "initial_layer_print_height", "line_width",
            "initial_layer_line_width", "wall_loops", "top_shell_layers", "bottom_shell_layers", "bottom_surface_pattern",
            "elefant_foot_compensation", "sparse_infill_density", "sparse_infill_pattern", "print_speed", "travel_speed",
            "enable_support", "single_extruder_multi_material", "precise_outer_wall", "brim_width", "brim_type"}) {
        const auto found = process.find(key);
        if (found != process.end() && !found->is_null()) {
            const auto value = process_text(*found);
            if (!value.empty()) result[key] = value;
        }
        if (request.contains(key)) result[key] = request.at(key);
    }
    return result;
}

void resize_arrays(json &project, const json &source, std::size_t count) {
    const auto old_count = filament_count(source);
    for (auto &[key, values] : project.items()) {
        if (!values.is_array() || values.empty()) continue;
        if (key == "flush_volumes_matrix") {
            if (old_count == 0 || count > old_count) continue;
            const auto pairs = old_count * old_count;
            if (values.size() % pairs != 0) {
                values.erase(values.begin() + std::min(values.size(), count * count), values.end());
                continue;
            }
            values = project_filament_matrix(values, old_count, count);
        } else if (filament_array(key) || key == "flush_volumes_vector") {
            if (values.size() == count) continue;
            const bool grouped = old_count > 0 && count <= old_count &&
                values.size() % old_count == 0 &&
                (key == "flush_volumes_vector" ||
                 (key.rfind("filament_", 0) == 0 && values.size() > old_count));
            if (grouped) {
                values.erase(values.begin() + count * (values.size() / old_count), values.end());
            } else {
                values = repeat(default_value(values, source), count);
            }
        }
    }
}

std::set<std::string> source_difference_tokens(const json &entry) {
    return detail::difference_tokens(text(entry));
}

json scoped_differences(const json &project, std::string_view difference_key,
                        std::size_t slot_offset, std::size_t trailing_count) {
    const auto found = project.find(std::string(difference_key));
    json entries = found == project.end() || found->is_null() ? json::array() :
        (found->is_array() ? *found : json::array({*found}));
    const auto count = filament_count(project);
    const auto expected = slot_offset + count + trailing_count;
    if (entries.size() > 1) {
        if (entries.size() != expected) {
            throw ProjectSettingsError(
                "source project " + std::string(difference_key) +
                " count does not match its declared filament-slot protocol");
        }
        return entries;
    }
    json scoped = repeat("", expected);
    if (entries.empty()) return scoped;
    std::set<std::string> process, filament, printer;
    for (const auto &key : source_difference_tokens(entries.at(0))) {
        if (key == "printer_settings_id") printer.insert(key);
        else if (filament_array(key)) filament.insert(key);
        else process.insert(key);
    }
    if (slot_offset > 0) scoped.at(0) = render_difference_tokens(process);
    else if (count > 0) scoped.at(0) = render_difference_tokens(process);
    for (std::size_t i = 0; i < count; ++i) {
        scoped.at(slot_offset + i) = render_difference_tokens(filament);
    }
    if (trailing_count > 0) scoped.at(expected - 1) = render_difference_tokens(printer);
    return scoped;
}

void record_differences(json &project, const std::set<std::string> &extra,
                        bool scoped, std::string_view difference_key,
                        std::size_t slot_offset, std::size_t trailing_count) {
    const auto found = project.find(std::string(difference_key));
    json entries = scoped ? scoped_differences(project, difference_key, slot_offset, trailing_count) :
        found == project.end() || found->is_null() ? json::array() :
        (found->is_array() ? *found : json::array({*found}));
    if (entries.empty() && extra.empty()) {
        if (found != project.end()) project[std::string(difference_key)] = entries;
        return;
    }
    if (entries.empty()) entries.push_back("");
    auto tokens = source_difference_tokens(entries.at(0));
    for (const auto &key : extra) {
        if (scoped && filament_array(key) &&
            entries.size() == slot_offset + filament_count(project) + trailing_count) {
            for (std::size_t i = 0; i < filament_count(project); ++i) {
                auto material_tokens = source_difference_tokens(entries.at(slot_offset + i));
                material_tokens.insert(key);
                entries.at(slot_offset + i) = render_difference_tokens(material_tokens);
            }
        } else tokens.insert(key);
    }
    entries.at(0) = render_difference_tokens(tokens);
    project[std::string(difference_key)] = std::move(entries);
}

struct MaterialSelection {
    std::string type;
    std::string preset;
    std::optional<std::size_t> source_index;
    bool petg = false;
    bool apply_override = true;
    bool allow_petg_fallback = true;
};

std::string requested_material_type(const json &material) {
    const auto raw_type = material.find("material_type");
    // Missing historical facts retain the old product compatibility choice.
    // An explicit unknown type is never interpreted as PLA.
    const auto name = text(material.value("name", json()));
    if (raw_type != material.end() && !raw_type->is_null()) return stripped(text(*raw_type));
    const auto normalized = upper(name);
    if (normalized.find("PETG") == std::string::npos) return "PLA Basic";
    std::string compact;
    for (unsigned char c : normalized) if (std::isalnum(c)) compact += static_cast<char>(c);
    if (compact.find("PETGCF") != std::string::npos) return "PETG-CF";
    if (compact.find("PETGHF") != std::string::npos) return "PETG HF";
    if (normalized.find("TRANSLUCENT") != std::string::npos || normalized.find("TRANSPARENT") != std::string::npos ||
        name.find("透明") != std::string::npos) return "PETG Translucent";
    return "PETG Basic";
}

MaterialSelection material_selection(const json &material, const json &source) {
    const auto raw_type = material.find("material_type");
    const bool explicit_type = raw_type != material.end() && !raw_type->is_null();
    const auto name = text(material.value("name", json()));
    if (!explicit_type && upper(name).find("PETG") == std::string::npos) {
        MaterialSelection result;
        result.apply_override = false;
        return result;
    }
    const auto requested = requested_material_type(material);
    if (requested.empty()) throw ProjectSettingsError("source material has no explicit material_type: " + name);
    const auto normalized = upper(requested);
    MaterialSelection result{requested, requested, std::nullopt, normalized.find("PETG") != std::string::npos};
    if (result.petg) {
        std::string compact;
        for (unsigned char c : normalized) if (std::isalnum(c)) compact += static_cast<char>(c);
        result.type = compact.find("PETGCF") != std::string::npos ? "PETG-CF" : "PETG";
        result.preset = result.type == "PETG-CF" ? "PETG-CF" :
            compact.find("PETGHF") != std::string::npos ? "PETG HF" :
            (normalized.find("TRANSLUCENT") != std::string::npos || normalized.find("TRANSPARENT") != std::string::npos ||
             requested.find("透明") != std::string::npos) ? "PETG Translucent" : "PETG Basic";
    }
    const bool specific_petg = explicit_type && result.petg && normalized != "PETG" && normalized != "PETG BASIC";
    result.allow_petg_fallback = !specific_petg;
    if (specific_petg && result.preset == "PETG Basic") result.preset = requested;
    const bool plain_pla = normalized == "PLA" || normalized == "PLA BASIC";
    for (const auto *key : {"filament_type", "filament_settings_id"}) {
        const auto values = source.find(key);
        if (values == source.end() || !values->is_array()) continue;
        for (std::size_t i = 0; i < values->size(); ++i) {
            const auto candidate = upper(text(values->at(i)));
            bool matches = result.petg ? candidate.find("PETG") != std::string::npos :
                plain_pla ? candidate == "PLA" || candidate.find("PLA BASIC") != std::string::npos :
                candidate == normalized;
            if (specific_petg) {
                const auto preset = upper(result.preset);
                const auto offset = candidate.find(preset);
                const auto end = offset == std::string::npos ? 0 : offset + preset.size();
                matches = offset != std::string::npos &&
                    (offset == 0 || !std::isalnum(static_cast<unsigned char>(candidate[offset - 1]))) &&
                    (end == candidate.size() || !std::isalnum(static_cast<unsigned char>(candidate[end])));
            }
            if (matches) {
                result.source_index = i;
                return result;
            }
        }
    }
    if (!result.petg || specific_petg) {
        throw ProjectSettingsError("source project has no material settings for " + requested);
    }
    return result;
}

void remap_material_difference_sources(
    json &project, const json &source,
    const std::vector<MaterialSelection> &selected,
    std::string_view difference_key, std::size_t slot_offset,
    std::size_t trailing_count) {
    const auto source_differences = scoped_differences(
        source, difference_key, slot_offset, trailing_count);
    auto output_differences = scoped_differences(
        project, difference_key, slot_offset, trailing_count);
    const auto output_count = filament_count(project);
    const auto copied_material_field = [](const std::string &key) {
        return filament_array(key) && key != "filament_colour" &&
               key != "filament_multi_colour";
    };
    for (std::size_t output_index = 0;
         output_index < std::min(output_count, selected.size()); ++output_index) {
        const auto &material = selected.at(output_index);
        if (!material.apply_override || !material.source_index) continue;
        const auto source_index = *material.source_index;
        if (source_index >= filament_count(source)) {
            throw ProjectSettingsError(
                "source material selection is outside the imported project");
        }
        auto tokens = source_difference_tokens(
            output_differences.at(slot_offset + output_index));
        for (auto it = tokens.begin(); it != tokens.end();) {
            if (copied_material_field(*it)) it = tokens.erase(it);
            else ++it;
        }
        for (const auto &key : source_difference_tokens(
                 source_differences.at(slot_offset + source_index))) {
            if (copied_material_field(key)) tokens.insert(key);
        }
        output_differences.at(slot_offset + output_index) =
            render_difference_tokens(tokens);
    }
    project[std::string(difference_key)] = std::move(output_differences);
}

bool compact_source_slots(const json &project, const json &request,
                          const std::string &slicer, std::size_t count) {
    const auto mode = request.value("filament_slot_mode", "auto");
    if (mode == "compact") return true;
    if (mode == "preserve") return false;
    if (mode != "auto") throw ProjectSettingsError("invalid source filament_slot_mode");
    if (request.value("preserve_source_material_settings", false)) return false;
    const auto profile = request.value("source_profile", json::object());
    const bool u1 = project.value("printer_model", "") == "Snapmaker U1";
    return !profile.value("registry", false) || u1 || slicer == "FlashStudio" || filament_count(project) <= count;
}

std::string derive_preset(std::string original, const std::string &preset) {
    original = stripped(original);
    if (original.empty()) return preset;
    for (const auto *token : {"PLA Basic", "PLA Matte", "PLA Silk", "PLA-CF", "PLA Metal", "PLA Marble",
                              "PLA Wood", "PLA Glow", "PLA Galaxy", "PLA Aero", "PLA"}) {
        const auto offset = original.find(token);
        if (offset != std::string::npos) {
            original.replace(offset, std::string(token).size(), preset);
            break;
        }
    }
    return original;
}

json petg_value(const std::string &key, const json &current, const MaterialSelection &material) {
    static const std::map<std::string, std::string> compatibility_values = {
        {"nozzle_temperature", "255"}, {"nozzle_temperature_initial_layer", "245"},
        {"nozzle_temperature_range_low", "230"}, {"nozzle_temperature_range_high", "270"},
        {"bed_temperature", "70"}, {"bed_temperature_initial_layer", "70"},
        {"textured_plate_temp", "70"}, {"textured_plate_temp_initial_layer", "70"},
        {"hot_plate_temp", "70"}, {"hot_plate_temp_initial_layer", "70"},
        {"eng_plate_temp", "70"}, {"eng_plate_temp_initial_layer", "70"},
        {"cool_plate_temp", "0"}, {"cool_plate_temp_initial_layer", "0"},
    };
    if (key == "filament_type") return material.type;
    if (key == "filament_settings_id") return derive_preset(text(current), material.preset);
    if (key == "filament_vendor") return current.is_null() || text(current).empty() ? json("Bambu Lab") : current;
    if (key == "filament_ids") return text(current).rfind("GF", 0) == 0 ? json("GFG00") : current;
    const auto found = compatibility_values.find(key);
    return found == compatibility_values.end() ? json() : json(found->second);
}

void apply_materials(json &project, const json &source,
                     const std::vector<MaterialSelection> &selected,
                     bool complete_native_defaults, bool scoped_markers,
                     std::string_view difference_key, std::size_t slot_offset,
                     std::size_t trailing_count) {
    if (!complete_native_defaults &&
        std::none_of(selected.begin(), selected.end(), [](const auto &item) { return item.apply_override; })) return;
    const auto old_count = filament_count(source);
    const auto output_count = filament_count(project);
    const auto source_differences = scoped_markers
        ? scoped_differences(project, difference_key, slot_offset, trailing_count)
        : json::array();
    json differences = source_differences;
    const bool generated_petg = std::any_of(selected.begin(), selected.end(), [](const auto &item) {
        return item.apply_override && item.petg && item.allow_petg_fallback && !item.source_index;
    });
    if (complete_native_defaults && generated_petg) {
        // Native PLA defaults fill untouched slots; petg_value supplies the
        // existing PETG compatibility range only for generated PETG slots.
        if (!project.contains("nozzle_temperature_range_low")) {
            project["nozzle_temperature_range_low"] = repeat("190", output_count);
        }
        if (!project.contains("nozzle_temperature_range_high")) {
            project["nozzle_temperature_range_high"] = repeat("240", output_count);
        }
    }
    for (auto &[key, values] : project.items()) {
        if (key == "filament_colour" || key == "filament_multi_colour" ||
            !filament_array(key) || !values.is_array()) continue;
        if (values.size() < selected.size()) values = repeat(values.empty() ? json("") : values.at(0), selected.size());
        const auto width = output_count > 0 && values.size() % output_count == 0
            ? std::max<std::size_t>(1, values.size() / output_count) : std::size_t(1);
        for (std::size_t i = 0; i < selected.size(); ++i) {
            const auto &material = selected.at(i);
            if (!material.apply_override) continue;
            for (std::size_t variant = 0; variant < width; ++variant) {
                const auto output_index = i * width + variant;
                if (output_index >= values.size()) continue;
                json value;
                if (material.source_index && source.contains(key)) {
                    const auto &original = source.at(key);
                    const auto source_width = old_count > 0 && original.size() % old_count == 0
                        ? std::max<std::size_t>(1, original.size() / old_count) : std::size_t(1);
                    const auto source_index = *material.source_index * source_width + std::min(variant, source_width - 1);
                    if (source_index < original.size()) value = original.at(source_index);
                }
                if (value.is_null() && material.petg && material.allow_petg_fallback) {
                    value = petg_value(key, values.at(output_index), material);
                }
                if (!value.is_null()) {
                    if (scoped_markers && values.at(output_index) != value) {
                        auto tokens = source_difference_tokens(differences.at(slot_offset + i));
                        tokens.insert(key);
                        differences.at(slot_offset + i) = render_difference_tokens(tokens);
                    }
                    values.at(output_index) = std::move(value);
                }
            }
        }
    }
    if (scoped_markers) project[std::string(difference_key)] = std::move(differences);
    const auto first_petg = std::find_if(selected.begin(), selected.end(), [](const auto &item) { return item.petg; });
    if (first_petg != selected.end() && project.contains("default_filament_profile")) {
        auto &value = project["default_filament_profile"];
        if (value.is_array() && !value.empty()) value.at(0) = derive_preset(text(value.at(0)), first_petg->preset);
        else if (value.is_string() && !value.empty()) value = derive_preset(value.get<std::string>(), first_petg->preset);
    }
}

void collapse_orca_arrays(json &project, const json &source, std::size_t count) {
    if (count == 0 || count > filament_count(source)) return;
    const std::set<std::string> grouped = {"flush_volumes_vector", "long_retractions_when_ec",
        "retraction_distances_when_ec", "volumetric_speed_coefficients"};
    for (auto &[key, value] : project.items()) {
        if (!value.is_array() || value.size() <= count || value.size() % count != 0 ||
            (key.rfind("filament_", 0) != 0 && grouped.count(key) == 0)) continue;
        const auto width = value.size() / count;
        json collapsed = json::array();
        for (std::size_t i = 0; i < count; ++i) append_filament_group(collapsed, value, i, width, 1);
        value = std::move(collapsed);
    }
}

void flash_routing(json &project, std::size_t count) {
    const auto first = [&](const char *key, const char *fallback) -> json {
        const auto value = project.find(key);
        return value != project.end() && value->is_array() && !value->empty() ? value->at(0) : json(fallback);
    };
    project["default_bed_type"] = text(project.value("default_bed_type", json("")));
    project["filament_colour_type"] = repeat("1", count);
    project["filament_extruder_variant"] = repeat(first("printer_extruder_variant", "Direct Drive Standard"), count);
    project["filament_map"] = repeat("1", count);
    project["filament_map_mode"] = "Auto For Flush";
    project["filament_printable"] = repeat("3", count);
    project["filament_self_index"] = json::array();
    for (std::size_t i = 0; i < count; ++i) project["filament_self_index"].push_back(std::to_string(i + 1));
    project["nozzle_volume_type"] = repeat(first("default_nozzle_volume_type", "Standard"), count);
    project["printer_agent"] = text(project.value("printer_agent", json("")));
}

void preserve_selected_slots(json &project, const json &source, const json &request,
                             std::size_t count, std::string_view difference_key = "different_settings_to_system",
                             std::size_t difference_offset = 1,
                             std::size_t difference_trailing = 1,
                             bool strict_difference_shape = false,
                             bool singleton_difference_allowed = true) {
    const auto old_count = filament_count(source);
    std::vector<std::size_t> slots;
    const auto selected = request.find("filament_source_slots");
    const bool compact = request.value("filament_slot_mode", "compact") == "compact";
    if (selected != request.end() &&
        (!selected->is_array() || selected->size() != count)) {
        throw ProjectSettingsError(
            "filament_source_slots must match the requested source material slots");
    }
    if (!compact && count > old_count) {
        throw ProjectSettingsError(
            "preserve source material mode cannot add slots beyond the imported project");
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto index = compact && count > old_count ? default_slot(source) : i;
        if (selected != request.end() && !selected->at(i).is_null()) {
            if (!selected->at(i).is_number_unsigned()) {
                throw ProjectSettingsError(
                    "filament_source_slots entries must be unsigned source indices or null");
            }
            index = selected->at(i).get<std::size_t>();
        }
        if (index >= old_count) throw ProjectSettingsError("source material slot is outside the imported project");
        slots.push_back(index);
    }
    for (auto &[key, values] : project.items()) {
        if (!values.is_array() || values.empty()) continue;
        if (key == std::string(difference_key)) {
            if (singleton_difference_allowed && values.size() == 1) continue;
            const auto expected = old_count + difference_offset + difference_trailing;
            if (values.size() != expected) {
                if (strict_difference_shape) {
                    throw ProjectSettingsError(
                        "source project " + key +
                        " count does not match its declared filament-slot protocol");
                }
                continue;
            }
            const auto &original = source.at(std::string(difference_key));
            if (compact) {
                json result = json::array();
                for (std::size_t i = 0; i < difference_offset; ++i) result.push_back(original.at(i));
                for (auto slot : slots) result.push_back(original.at(slot + difference_offset));
                for (std::size_t i = 0; i < difference_trailing; ++i) {
                    result.push_back(original.at(difference_offset + old_count + i));
                }
                values = std::move(result);
            } else {
                for (std::size_t i = 0; i < slots.size(); ++i) {
                    values.at(difference_offset + i) = original.at(slots[i] + difference_offset);
                }
            }
        } else if (key == "inherits_group") {
            if (values.size() != old_count + 2) continue;
            const auto &original = source.at("inherits_group");
            if (compact) {
                json result = json::array({original.at(0)});
                for (auto slot : slots) result.push_back(original.at(slot + 1));
                result.push_back(original.back());
                values = std::move(result);
            } else {
                for (std::size_t i = 0; i < slots.size(); ++i) {
                    values.at(i + 1) = original.at(slots[i] + 1);
                }
            }
        } else if (key == "flush_volumes_matrix") {
            if (!compact || old_count == 0 || values.size() % (old_count * old_count) != 0) continue;
            values = project_filament_matrix(values, old_count, slots);
        } else if (filament_array(key) || key == "flush_volumes_vector") {
            if (old_count == 0 || values.size() % old_count != 0) continue;
            const auto width = values.size() / old_count;
            json result = compact ? json::array() : values;
            for (std::size_t i = 0; i < slots.size(); ++i) {
                if (compact) {
                    append_filament_group(result, values, slots[i], width, width);
                    continue;
                }
                for (std::size_t variant = 0; variant < width; ++variant) {
                    result.at(i * width + variant) = values.at(slots[i] * width + variant);
                }
            }
            values = std::move(result);
        }
    }
}

}  // namespace

json prepare_source_identity(const json &base, const json &request) {
    json project = base;
    if (project.contains("filament_settings_id") || request.contains("merge_sources")) return project;
    const auto defaults = project.find("default_filament_profile");
    if (defaults == project.end()) throw ProjectSettingsError("source project has no material profile identity");
    const auto identity = defaults->is_array() && !defaults->empty() ? defaults->at(0) : *defaults;
    if (!identity.is_string() || identity.get<std::string>().empty()) {
        throw ProjectSettingsError("source project has no material profile identity");
    }
    const auto source_count = filament_count(project);
    const auto count = source_count == 0 ? request.at("source_materials").size() : source_count;
    project["filament_settings_id"] = repeat(identity, count);
    if (!project.contains("filament_type")) {
        const auto profile = upper(identity.get<std::string>());
        std::string type;
        // These are identities present in source preset names, not parameter
        // presets. No temperature or other material tuning is manufactured.
        std::vector<std::string> candidates = {"PETG-CF", "PLA-CF", "PETG", "PLA"};
        for (const auto &material : request.at("source_materials")) {
            const auto explicit_type = material.find("material_type");
            if (explicit_type != material.end() && explicit_type->is_string()) {
                const auto candidate = upper(stripped(explicit_type->get<std::string>()));
                if (!candidate.empty()) candidates.push_back(candidate);
            }
        }
        for (const auto &token : candidates) {
            const auto offset = profile.find(token);
            if (offset == std::string::npos) continue;
            const auto end = offset + token.size();
            if ((offset == 0 || !std::isalnum(static_cast<unsigned char>(profile[offset - 1]))) &&
                (end == profile.size() || !std::isalnum(static_cast<unsigned char>(profile[end])))) {
                type = token;
                break;
            }
        }
        if (!type.empty()) project["filament_type"] = repeat(type, count);
    }
    return project;
}

void apply_source_flush_defaults(json &project, const json &request, const json &target) {
    if (!request.contains("source_materials") || request.contains("merge_sources") ||
        request.value("preserve_source_material_settings", false) ||
        target.at("target_contract").value("slicer_id", "") != "FlashStudio" ||
        project.value("printer_model", "") != "Flashforge AD5X") return;
    rebuild_flush_matrix_from_vector(project);
}

void apply_source_tower_defaults(json &project, const json &target) {
    const auto &contract = target.at("target_contract");
    const auto package = target.find("package_dialect");
    if (package != target.end() && package->is_object() &&
        package->contains("wipe_tower_placement")) {
        const auto &geometry = package->at("wipe_tower_placement");
        if (!geometry.is_object()) {
            throw ProjectSettingsError("wipe tower placement_geometry must be an object");
        }
        const auto defaults = geometry.find("native_defaults");
        if (defaults != geometry.end()) {
            if (!defaults->is_object()) {
                throw ProjectSettingsError("wipe tower placement native_defaults must be an object");
            }
            for (const auto &[key, value] : defaults->items()) {
                if (!value.is_string() && !value.is_number() && !value.is_boolean()) {
                    throw ProjectSettingsError("wipe tower placement native defaults must be scalar values");
                }
                if (!project.contains(key)) project[key] = value;
            }
        }
    }
    if (contract.value("slicer_id", "") == "BambuStudio" &&
        contract.value("application_version", "") == "02.08.02.61") {
        // BambuStudio v02.08.02.61 PrintConfig/WipeTower defaults define its
        // square ribbed tower envelope even when a project omits these keys.
        // https://github.com/bambulab/BambuStudio/blob/v02.08.02.61/src/libslic3r/PrintConfig.cpp
        // https://github.com/bambulab/BambuStudio/blob/v02.08.02.61/src/libslic3r/GCode/WipeTower.cpp
        const json defaults = {{"prime_tower_brim_width", "3"},
                               {"prime_tower_rib_wall", "1"},
                               {"prime_tower_rib_width", "8"},
                               {"prime_tower_extra_rib_length", "0"},
                               {"prime_tower_fillet_wall", "1"},
                               {"prime_tower_infill_gap", "150%"}};
        for (const auto &[key, value] : defaults.items()) {
            if (!project.contains(key)) project[key] = value;
        }
        return;
    }
    if (contract.value("slicer_id", "") != "ElegooSlicer" ||
        contract.value("application_version", "") != "1.5.3.5") return;
    const auto model = project.value("printer_model", "");
    if (model != "Elegoo Centauri Carbon" && model != "Elegoo Centauri Carbon 2") return;
    // ElegooSlicer v1.5.3.5 PrintConfig.cpp: 6904-7023. These defaults
    // affect the physical envelope even when omitted from a source project.
    const json defaults = {{"prime_tower_brim_width", "3"},
                           {"wipe_tower_wall_type", "rib"},
                           {"wipe_tower_rib_width", "8"},
                           {"prime_tower_infill_gap", "150%"}};
    for (const auto &[key, value] : defaults.items()) {
        if (!project.contains(key)) project[key] = value;
    }
}

json compose_source_project(const json &base, const json &request, const json &target) {
    json project = prepare_source_identity(base, request);
    const auto slicer = target.at("target_contract").at("slicer_id").get<std::string>();
    const auto &filament_snapshot = target.at("package_dialect").at("filament_snapshot");
    const auto difference_key = filament_snapshot.at("difference_list_key").get<std::string>();
    const auto difference_offset = filament_snapshot.at("filament_slot_offset").get<std::size_t>();
    const auto difference_trailing = filament_snapshot.at("trailing_entry_count").get<std::size_t>();
    const bool complete_native_defaults = slicer == "ElegooSlicer" || slicer == "QIDIStudio";
    const bool scoped_markers = complete_native_defaults ||
        filament_snapshot.value("scoped_difference_entries", false);
    if (complete_native_defaults && !project.contains("filament_diameter")) {
        // Elegoo/QIDI normalize material vectors using this array's length.
        // 1.75 is the native schema default, not a replacement for an explicit diameter.
        project["filament_diameter"] = repeat("1.75", filament_count(project));
    }
    if (complete_native_defaults) {
        project[difference_key] = scoped_differences(
            project, difference_key, difference_offset, difference_trailing);
    }
    const bool preserve_materials = request.value("preserve_source_material_settings", false);
    const bool merging = request.contains("merge_sources");
    if (!merging) {
        const auto &materials = request.at("source_materials");
        if (!materials.is_array() || materials.empty()) throw ProjectSettingsError("source_materials must contain the output palette");
        const json source = project;
        const auto count = materials.size();
        const bool compact = compact_source_slots(source, request, slicer, count);
        json selection_request = request;
        selection_request["filament_slot_mode"] = compact ? "compact" : "preserve";
        std::vector<MaterialSelection> selected_materials;
        if (!preserve_materials) {
            selected_materials.reserve(materials.size());
            for (const auto &material : materials) {
                selected_materials.push_back(material_selection(material, source));
            }
        }
        if (preserve_materials || scoped_markers) {
            preserve_selected_slots(project, source, selection_request, count,
                difference_key, difference_offset, difference_trailing,
                scoped_markers, true);
        }
        else if (compact) resize_arrays(project, source, count);
        if (scoped_markers && !preserve_materials) {
            remap_material_difference_sources(
                project, source, selected_materials, difference_key,
                difference_offset, difference_trailing);
        }
        json colors = json::array();
        for (const auto &material : materials) colors.push_back(material.at("colour"));
        const auto output_count = compact ? count : std::max(count, filament_count(project));
        const auto source_colours = request.value("source_slot_colours", json::object());
        for (const auto *key : {"filament_colour", "filament_multi_colour"}) {
            const auto existing = project.find(key);
            auto values = !compact && existing != project.end() && existing->is_array()
                ? *existing : json::array();
            while (values.size() < output_count) {
                values.push_back(source_slot_colour(source_colours, values.size()));
            }
            for (std::size_t i = 0; i < count; ++i) values.at(i) = colors.at(i);
            project[key] = std::move(values);
        }
        if (!preserve_materials) {
            apply_materials(project, source, selected_materials, complete_native_defaults,
                scoped_markers, difference_key, difference_offset, difference_trailing);
            if (slicer == "OrcaSlicer") collapse_orca_arrays(project, source, count);
            if (slicer == "FlashStudio") flash_routing(project, count);
            if (slicer == "QIDIStudio" && project.contains("default_filament_profile")) {
                const auto &defaults = project.at("default_filament_profile");
                const auto identity = defaults.is_array() && !defaults.empty() ? defaults.at(0) : defaults;
                if (identity.is_string() && !identity.get<std::string>().empty()) {
                    auto identities = project.at("filament_settings_id");
                    for (std::size_t i = 0; i < count; ++i) {
                        const auto material_type = materials.at(i).find("material_type");
                        if (material_type == materials.at(i).end() || material_type->is_null()) {
                            identities.at(i) = identity;
                        }
                    }
                    project["filament_settings_id"] = std::move(identities);
                }
            }
        }
        if (compact && !preserve_materials) {
            for (const auto *key : {"filament_long_retractions_when_cut", "filament_retraction_distances_when_cut"}) {
                if (project.contains(key)) project[key] = repeat("nil", count);
            }
            if (project.contains("long_retractions_when_cut") && project.at("long_retractions_when_cut").is_array()) {
                project["long_retractions_when_cut"] = repeat("0", project.at("long_retractions_when_cut").size());
            }
        }
    }
    apply_source_flush_defaults(project, request, target);
    if (!project.contains("single_extruder_multi_material")) project["single_extruder_multi_material"] = "1";
    if (project.contains("printer_settings_id")) {
        project["print_compatible_printers"] = json::array({project.at("printer_settings_id")});
    }
    project["enable_prime_tower"] = "1";
    std::set<std::string> overrides = {"enable_prime_tower"};
    if ((preserve_materials || complete_native_defaults) && !merging) {
        overrides.insert("filament_colour");
        overrides.insert("filament_multi_colour");
    }
    if (slicer == "OrcaSlicer" && !preserve_materials && !merging) {
        project["precise_outer_wall"] = "0";
        overrides.insert("precise_outer_wall");
    }
    const auto process = process_overrides(request);
    for (const auto &[key, value] : process.items()) {
        project[key] = value;
        overrides.insert(key);
    }
    const bool u1 = (slicer == "OrcaSlicer" || slicer == "SnapmakerOrca") &&
                    project.value("printer_model", "") == "Snapmaker U1";
    record_differences(project, overrides, scoped_markers, difference_key,
                       difference_offset, difference_trailing);
    if (u1) {
        project.erase("inherits_group");
    }
    return project;
}

bool has_native_material_parameters(const json &binding, const json &target,
                                    std::set<std::string> &managed_keys) {
    if (binding.value("write_policy", "") != "target_native_preset" ||
        !binding.contains("material_profile_key")) return false;
    const auto profiles = target.find("material_profiles");
    const auto parameter_sets = target.find("target_parameter_sets");
    if (profiles == target.end() || parameter_sets == target.end()) return false;
    for (const auto &profile : *profiles) {
        if (profile.value("material_profile_key", json()) != binding.at("material_profile_key") ||
            !profile.contains("target_parameter_set_key")) continue;
        for (const auto &parameters : *parameter_sets) {
            if (parameters.value("target_parameter_set_key", json()) == profile.at("target_parameter_set_key") &&
                parameters.contains("managed_parameter_keys") && !parameters.at("managed_parameter_keys").empty() &&
                parameters.contains("target_parameters") && !parameters.at("target_parameters").empty()) {
                for (const auto &key : parameters.at("managed_parameter_keys")) {
                    managed_keys.insert(key.get<std::string>());
                }
                return true;
            }
        }
    }
    return false;
}

json native_source_request(const json &project, const json &request, const json &target) {
    if (request.value("preserve_source_material_settings", false) || request.contains("merge_sources")) return nullptr;
    const json *machine = nullptr;
    for (const auto &binding : target.at("machine_bindings")) {
        if (binding.at("printer_model") == project.value("printer_model", json()) &&
            binding.at("nozzle_diameter") == project.value("nozzle_diameter", json())) {
            machine = &binding;
            break;
        }
    }
    if (machine == nullptr) return nullptr;
    const auto &materials = request.at("source_materials");
    const auto slicer = target.at("target_contract").at("slicer_id").get<std::string>();
    const bool compact = compact_source_slots(project, request, slicer, materials.size());
    json selections = materials;
    if (!compact) {
        for (std::size_t i = materials.size(); i < filament_count(project); ++i) {
            const auto &types = project.at("filament_type");
            selections.push_back({{"name", ""}, {"material_type", types.at(i)}});
        }
    }
    json uids = json::array();
    std::set<std::string> managed_keys;
    for (const auto &material : selections) {
        const auto selected = requested_material_type(material);
        const auto kind = upper(selected);
        const auto uid = kind == "PLA" || kind == "PLA BASIC" ? "material:pla" :
                         kind == "PETG" || kind == "PETG BASIC" ? "material:petg" : "";
        const json *matched = nullptr;
        for (const auto &binding : target.at("material_bindings")) {
            if (binding.value("machine_uid", json()) != machine->at("machine_uid") ||
                binding.value("nozzle_uid", json()) != machine->at("nozzle_uid")) continue;
            if ((!std::string(uid).empty() && binding.value("material_uid", "") == uid) ||
                upper(binding.value("native_profile_id", "")) == kind) {
                matched = &binding;
                break;
            }
        }
        if (matched == nullptr || !has_native_material_parameters(*matched, target, managed_keys)) return nullptr;
        uids.push_back(matched->at("material_uid"));
    }
    if (compact && materials.size() != filament_count(project)) {
        auto covered = managed_keys;
        for (const auto &rule : target.at("package_dialect").at("filament_snapshot").at("array_rules")) {
            if (rule.value("merged_only", false)) continue;
            for (const auto &key : rule.at("keys")) covered.insert(key.get<std::string>());
        }
        // Native preset application rewrites these identities after selection.
        for (const auto *key : {"filament_settings_id", "filament_type", "filament_ids",
                                "filament_vendor", "filament_compatible_printers", "filament_self_index"}) {
            covered.insert(key);
        }
        for (const auto &[key, value] : project.items()) {
            if (value.is_array() && !value.empty() &&
                (filament_array(key) || key == "flush_volumes_matrix" || key == "flush_volumes_vector") &&
                covered.count(key) == 0) {
                // A mixed legacy source may carry slot arrays absent from the
                // native snapshot plan. Preserve its complete source policy.
                return nullptr;
            }
        }
    }
    json resolved = request;
    resolved["hardware_mode"] = "target_binding";
    resolved["machine_uid"] = machine->at("machine_uid");
    resolved["nozzle_uid"] = machine->at("nozzle_uid");
    resolved["material_mode"] = "target_native_preset";
    resolved["material_uids"] = std::move(uids);
    resolved["filament_slot_mode"] = compact ? "compact" : "preserve";
    if (compact) resolved["default_filament_source_slot"] = default_slot(project);
    resolved["enable_prime_tower"] = "1";
    json colours = json::array();
    for (const auto &material : materials) colours.push_back(material.at("colour"));
    if (!compact && !project.contains("filament_colour")) {
        const auto source_colours = request.value("source_slot_colours", json::object());
        for (std::size_t i = colours.size(); i < selections.size(); ++i) {
            colours.push_back(source_slot_colour(source_colours, i));
        }
    }
    resolved["filament_colour"] = colours;
    resolved["filament_multi_colour"] = colours;
    const auto process = process_overrides(request);
    for (const auto &[key, value] : process.items()) resolved[key] = value;
    if (slicer == "OrcaSlicer") resolved["precise_outer_wall"] = "0";
    return resolved;
}

json prepare_source_merge_project(const json &project, const json &filament_snapshot) {
    json result = project;
    const auto count = project.at("filament_settings_id").size();
    const auto difference_key = filament_snapshot.at("difference_list_key").get<std::string>();
    const auto difference_offset = filament_snapshot.at("filament_slot_offset").get<std::size_t>();
    const auto difference_trailing = filament_snapshot.at("trailing_entry_count").get<std::size_t>();
    const bool scoped_markers = filament_snapshot.value("scoped_difference_entries", false);
    if (filament_count(project) > count) {
        preserve_selected_slots(result, project, {{"filament_slot_mode", "compact"}}, count,
            difference_key, difference_offset, difference_trailing,
            scoped_markers, true);
    }
    return result;
}

json source_merge_dialect(const json &dialect, const json &project) {
    json result = dialect;
    auto &snapshot = result["filament_snapshot"];
    auto &rules = snapshot["array_rules"];
    rules = json::array();
    const auto count = filament_count(project);
    for (const auto &[key, values] : project.items()) {
        if (!values.is_array() || values.empty() || count == 0) continue;
        const bool matrix = key == "flush_volumes_matrix";
        if (!matrix && !filament_array(key) && key != "flush_volumes_vector") continue;
        const auto divisor = matrix ? count * count : count;
        if (values.size() % divisor != 0 && values.size() != 1) continue;
        rules.push_back({{"keys", json::array({key})}, {"selection", matrix ? "matrix" : key == "filament_self_index" ? "slot_index" : "default_or_group"},
                         {"group_size", std::max<std::size_t>(1, values.size() / divisor)}, {"broadcast_singleton", !matrix && values.size() == 1},
                         {"material_source_override", true}});
    }
    snapshot["merged_difference_list_singleton"] = true;
    snapshot["merged_preserve_default_filament_profile"] = true;
    snapshot["merged_preserve_first_source_keys"] = json::array();
    return result;
}

}  // namespace fatcat::detail
