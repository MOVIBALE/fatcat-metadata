#include "fatcat/native_project_source.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "filament_projection.h"
#include "fatcat/project_settings.h"
#include "fatcat/source_project_settings.h"

namespace fatcat {
namespace {

using json = nlohmann::json;

[[noreturn]] void invalid(const std::string &message) {
    throw ProjectSettingsError(message);
}

std::string required_string(const json &object, const char *key,
                            const std::string &context) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() || found->get<std::string>().empty()) {
        invalid(context + " requires non-empty '" + key + "'");
    }
    return found->get<std::string>();
}

std::string read_text(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) invalid("native project source file is missing: " + path.string());
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

std::filesystem::path safe_path(const std::filesystem::path &root,
                                const std::string &relative) {
    const std::filesystem::path rel(relative);
    if (relative.empty() || rel.is_absolute() || rel.has_root_name() ||
        rel.has_root_directory()) {
        invalid("native source path must be relative: " + relative);
    }
    for (const auto &part : rel) {
        if (part == "." || part == "..") {
            invalid("native source path contains traversal: " + relative);
        }
    }
    const auto canonical_root = std::filesystem::weakly_canonical(root);
    const auto candidate = std::filesystem::weakly_canonical(root / rel);
    const auto beneath = candidate.lexically_relative(canonical_root);
    if (beneath.empty() || beneath.is_absolute() ||
        *beneath.begin() == std::filesystem::path("..")) {
        invalid("native source path escapes its data root: " + relative);
    }
    if (!std::filesystem::is_regular_file(candidate)) {
        invalid("native source profile is missing: " + relative);
    }
    return candidate;
}

json read_json(const std::filesystem::path &path, const std::string &context) {
    try {
        auto value = json::parse(read_text(path));
        if (!value.is_object()) invalid(context + " must be a JSON object");
        return value;
    } catch (const ProjectSettingsError &) {
        throw;
    } catch (const json::exception &error) {
        invalid("invalid " + context + " JSON: " + error.what());
    }
}

json read_index(const std::filesystem::path &data_root) {
    const auto index = read_json(data_root / "source-index.json", "native source index");
    if (!index.contains("schema_version") || index.at("schema_version") != 1 ||
        !index.contains("sources") || !index.at("sources").is_array() ||
        !index.contains("references") || !index.at("references").is_object()) {
        invalid("native source index has an unsupported schema");
    }
    return index;
}

const json &required_array(const json &object, const char *key,
                           const std::string &context) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_array()) {
        invalid(context + " requires an array '" + key + "'");
    }
    return *found;
}

json read_profile(const std::filesystem::path &data_root, const std::string &path) {
    return read_json(safe_path(data_root, path), "native profile");
}

std::string linked_profile_path(const json &index, const std::string &profile_path,
                                const std::string &kind,
                                const std::string &profile_name) {
    const auto references = index.find("references");
    if (references == index.end() || !references->is_object() ||
        !references->contains(profile_path) || !references->at(profile_path).is_object()) {
        invalid("native profile source has no recorded inheritance/include map: " + profile_path);
    }
    const std::string reference_key = kind + ":" + profile_name;
    const auto &edges = references->at(profile_path);
    if (!edges.contains(reference_key) || !edges.at(reference_key).is_string()) {
        invalid("native profile source has no exact recorded " + kind +
                " edge for '" + profile_name + "' in " + profile_path);
    }
    return edges.at(reference_key).get<std::string>();
}

json resolve_profile(const std::filesystem::path &data_root, const json &index,
                     const std::string &profile_path,
                     std::vector<std::string> &ancestry) {
    if (std::find(ancestry.begin(), ancestry.end(), profile_path) != ancestry.end() ||
        ancestry.size() >= 48) {
        invalid("cyclic or excessively deep native profile chain at " + profile_path);
    }
    ancestry.push_back(profile_path);
    const json source = read_profile(data_root, profile_path);
    json result = json::object();

    const auto inherits = source.find("inherits");
    if (inherits != source.end() && !inherits->is_null() &&
        !(inherits->is_string() && inherits->get<std::string>().empty())) {
        if (!inherits->is_string()) {
            invalid("native profile inherits field must be text: " + profile_path);
        }
        const auto name = inherits->get<std::string>();
        result.update(resolve_profile(
            data_root, index,
            linked_profile_path(index, profile_path, "inherits", name), ancestry));
    }

    const auto includes = source.find("include");
    if (includes != source.end() && !includes->is_null()) {
        std::vector<std::string> names;
        if (includes->is_string()) {
            names.push_back(includes->get<std::string>());
        } else if (includes->is_array()) {
            for (const auto &include : *includes) {
                if (!include.is_string() || include.get<std::string>().empty()) {
                    invalid("native profile include must contain preset names: " + profile_path);
                }
                names.push_back(include.get<std::string>());
            }
        } else {
            invalid("native profile include must be text or an array: " + profile_path);
        }
        for (const auto &name : names) {
            result.update(resolve_profile(
                data_root, index,
                linked_profile_path(index, profile_path, "include", name), ancestry));
        }
    }

    result.update(source);
    ancestry.pop_back();
    return result;
}

json resolve_profile(const std::filesystem::path &data_root, const json &index,
                     const std::string &profile_path) {
    std::vector<std::string> ancestry;
    return resolve_profile(data_root, index, profile_path, ancestry);
}

bool structural_profile_key(const std::string &key) {
    static const std::set<std::string> keys = {
        "type", "instantiation", "inherits", "include", "setting_id", "name",
        "from", "compatible_printers", "compatible_prints",
        "compatible_printers_condition", "compatible_prints_condition",
        "default_print_profile",
        "default_filament_profile", "printer_settings_id", "print_settings_id",
        "filament_settings_id", "filament_id", "filament_notes", "description", "version",
        "filament_self_index", "is_custom_defined",
    };
    return keys.find(key) != keys.end();
}

bool ignored_native_profile_key(const std::string &key, const json &snapshot) {
    const auto ignored = snapshot.find("ignored_native_profile_keys");
    if (ignored == snapshot.end()) return false;
    if (!ignored->is_array()) {
        invalid("filament snapshot ignored_native_profile_keys must be an array");
    }
    for (const auto &value : *ignored) {
        if (value.is_string() && value.get<std::string>() == key) return true;
    }
    return false;
}

const json *native_schema_default(const json &snapshot, const std::string &key) {
    const auto source = snapshot.find("native_schema_defaults");
    if (source == snapshot.end()) return nullptr;
    if (!source->is_object()) {
        invalid("filament snapshot native_schema_defaults must be an object");
    }
    const auto values = source->find("values");
    if (values == source->end()) return nullptr;
    if (!values->is_object()) {
        invalid("filament snapshot native_schema_defaults.values must be an object");
    }
    const auto value = values->find(key);
    return value == values->end() ? nullptr : &*value;
}

json material_default_group(const json &value, const std::string &key) {
    if (value.is_array()) {
        if (value.empty()) invalid("native schema default for '" + key + "' must not be empty");
        return value;
    }
    return json::array({value});
}

void merge_profile_settings(json &project, const json &profile,
                             bool keep_profile_version = false) {
    for (const auto &[key, value] : profile.items()) {
        if (structural_profile_key(key) && !(keep_profile_version && key == "version")) {
            continue;
        }
        project[key] = value;
    }
}

json compose_material_settings(const std::vector<json> &profiles,
                               const std::vector<std::string> &profile_names,
                               const json &target,
                               std::vector<std::vector<std::string>> &variant_groups) {
    if (profiles.empty() || profiles.size() != profile_names.size()) {
        invalid("native source must select one exact filament profile for every source material");
    }

    struct MaterialFieldRule {
        bool material_source_override;
        std::string selection;
        std::size_t group_size;
    };
    std::map<std::string, MaterialFieldRule> target_material_fields;
    const auto &snapshot = target.at("package_dialect").at("filament_snapshot");
    const auto &rules = snapshot.at("array_rules");
    for (const auto &rule : rules) {
        if (!rule.is_object() || rule.value("merged_only", false)) continue;
        const bool material_override = rule.value("material_source_override", false);
        const auto selection = rule.value("selection", std::string());
        const auto group_size = rule.value("group_size", std::size_t{1});
        for (const auto &key : rule.at("keys")) {
            if (!key.is_string()) continue;
            const auto name = key.get<std::string>();
            target_material_fields[name] = {material_override, selection, group_size};
        }
    }

    const auto variant_rule = target_material_fields.find("filament_extruder_variant");
    const auto self_index_rule = target_material_fields.find("filament_self_index");
    // The native variant list maps each material preset's parameter rows to
    // the slicer's variant axis; it is independent of physical nozzle count.
    const bool has_native_variant_identity =
        variant_rule != target_material_fields.end() &&
        variant_rule->second.material_source_override &&
        self_index_rule != target_material_fields.end() &&
        self_index_rule->second.selection == "slot_index";
    variant_groups.clear();
    if (has_native_variant_identity) {
        variant_groups.reserve(profiles.size());
        for (std::size_t slot = 0; slot < profiles.size(); ++slot) {
            const auto found = profiles[slot].find("filament_extruder_variant");
            if (found == profiles[slot].end() || !found->is_array() || found->empty()) {
                invalid("native material profile '" + profile_names[slot] +
                        "' has no resolved filament_extruder_variant list");
            }
            std::vector<std::string> variants;
            for (const auto &variant : *found) {
                if (!variant.is_string() || variant.get<std::string>().empty()) {
                    invalid("native material profile '" + profile_names[slot] +
                            "' has an invalid filament_extruder_variant entry");
                }
                variants.push_back(variant.get<std::string>());
            }
            variant_groups.push_back(std::move(variants));
        }
    }

    std::set<std::string> keys;
    for (const auto &profile : profiles) {
        for (const auto &[key, ignored] : profile.items()) {
            if (!structural_profile_key(key) && !ignored_native_profile_key(key, snapshot)) {
                keys.insert(key);
            }
        }
    }

    json combined = json::object();
    for (const auto &key : keys) {
        const auto target_field = target_material_fields.find(key);
        const bool material_field = target_field != target_material_fields.end()
            ? target_field->second.material_source_override
            : detail::filament_array(key);
        if (!material_field) {
            const json *selected = nullptr;
            bool missing = false;
            for (std::size_t slot = 0; slot < profiles.size(); ++slot) {
                const auto found = profiles[slot].find(key);
                if (found == profiles[slot].end()) {
                    missing = true;
                    continue;
                }
                if (selected == nullptr) {
                    selected = &*found;
                } else if (*selected != *found) {
                    invalid("native material profiles disagree on non-material setting '" + key +
                            "' between '" + profile_names.front() + "' and '" +
                            profile_names[slot] + "'; select profiles with a shared value");
                }
            }
            if (selected == nullptr) continue;
            if (missing) {
                const auto *default_value = native_schema_default(snapshot, key);
                if (default_value == nullptr || *selected != *default_value) {
                    invalid("native material profile '" + profile_names.front() +
                            "' defines non-material setting '" + key +
                            "' while another selected profile omits it; no matching native schema default is recorded");
                }
            }
            combined[key] = *selected;
            continue;
        }

        std::vector<json> groups;
        for (std::size_t slot = 0; slot < profiles.size(); ++slot) {
            const auto &profile = profiles[slot];
            const auto found = profile.find(key);
            if (found == profile.end()) {
                const auto *default_value = native_schema_default(snapshot, key);
                if (default_value == nullptr) {
                    invalid("native material profile '" + profile_names[slot] +
                            "' omits slice setting '" + key +
                            "' and the exact target has no recorded native schema default");
                }
                groups.push_back(material_default_group(*default_value, key));
                continue;
            }
            json group = found->is_array() ? *found : json::array({*found});
            if (group.empty()) {
                const auto *default_value = native_schema_default(snapshot, key);
                if (default_value == nullptr) {
                    invalid("native material profile '" + profile_names[slot] +
                            "' has an empty slice setting '" + key +
                            "' and the exact target has no recorded native schema default");
                }
                group = material_default_group(*default_value, key);
            }
            groups.push_back(std::move(group));
        }

        const std::size_t group_width = groups.front().size();
        json joined = json::array();
        for (std::size_t slot = 0; slot < groups.size(); ++slot) {
            if (groups[slot].size() != group_width) {
                invalid("native material field '" + key + "' has incompatible group widths in '" +
                        profile_names.front() + "' and '" + profile_names[slot] + "'");
            }
            const bool declared_group = target_field != target_material_fields.end() &&
                target_field->second.material_source_override &&
                target_field->second.selection == "default_or_group";
            const auto profile_variants = profiles[slot].find("filament_extruder_variant");
            // Parameters on a declared native variant axis use one value per
            // listed material variant, including temperature arrays.
            const bool matches_variant_axis = has_native_variant_identity &&
                profile_variants != profiles[slot].end() && profile_variants->is_array() &&
                group_width == profile_variants->size();
            const bool matches_declared_width = target_field != target_material_fields.end() &&
                group_width == target_field->second.group_size;
            if (group_width > 1 && !declared_group && !matches_variant_axis &&
                !matches_declared_width) {
                invalid("native material field '" + key + "' in '" + profile_names[slot] +
                        "' has unsupported group width " + std::to_string(group_width) +
                        " for its native role");
            }
            for (const auto &item : groups[slot]) joined.push_back(item);
        }
        combined[key] = std::move(joined);
    }

    json types = combined.value("filament_type", json());
    if (!types.is_array() || types.size() != profiles.size()) {
        invalid("selected native filament profiles must each provide one material type per slot");
    }
    return combined;
}

void apply_builtin_native_variant_identity(
    json &composed, const std::vector<std::string> &profile_names,
    const std::vector<std::vector<std::string>> &variant_groups) {
    if (variant_groups.empty()) return;
    if (variant_groups.size() != profile_names.size() ||
        !composed.contains("project_settings_json") ||
        !composed.at("project_settings_json").is_string()) {
        invalid("native material variant mapping does not match the composed project");
    }

    std::map<std::string, std::vector<std::string>> variants_by_profile;
    for (std::size_t slot = 0; slot < profile_names.size(); ++slot) {
        const auto [entry, inserted] = variants_by_profile.emplace(
            profile_names[slot], variant_groups[slot]);
        if (!inserted && entry->second != variant_groups[slot]) {
            invalid("native material profile '" + profile_names[slot] +
                    "' has inconsistent variant groups");
        }
    }

    auto project = json::parse(composed.at("project_settings_json").get<std::string>());
    const auto identities = project.find("filament_settings_id");
    const auto actual_variants = project.find("filament_extruder_variant");
    if (identities == project.end() || !identities->is_array() ||
        actual_variants == project.end() || !actual_variants->is_array()) {
        invalid("native variant-aware project has no final material identities or variant list");
    }

    json expected_variants = json::array();
    json self_indexes = json::array();
    for (std::size_t slot = 0; slot < identities->size(); ++slot) {
        const auto &identity = identities->at(slot);
        if (!identity.is_string()) {
            invalid("native variant-aware project has a non-text material identity");
        }
        const auto found = variants_by_profile.find(identity.get<std::string>());
        if (found == variants_by_profile.end()) {
            invalid("native variant-aware project has no selected profile map for '" +
                    identity.get<std::string>() + "'");
        }
        for (const auto &variant : found->second) {
            expected_variants.push_back(variant);
            self_indexes.push_back(std::to_string(slot + 1));
        }
    }
    if (*actual_variants != expected_variants) {
        invalid("native material variant fields were not projected in the same order as their identities");
    }
    project["filament_self_index"] = std::move(self_indexes);
    composed["project_settings_json"] = project.dump();
}

std::vector<const json *> matching_source_rows(const json &index,
                                               const std::string &slicer_id,
                                               const std::string &application_version,
                                               const std::string &machine_uid,
                                               const std::string &nozzle_uid) {
    std::vector<const json *> matches;
    for (const auto &source : index.at("sources")) {
        if (source.is_object() &&
            source.value("slicer_id", std::string()) == slicer_id &&
            source.value("application_version", application_version) == application_version &&
            source.value("machine_uid", std::string()) == machine_uid &&
            source.value("nozzle_uid", std::string()) == nozzle_uid) {
            matches.push_back(&source);
        }
    }
    return matches;
}

const json &profile_options(const json &source, const char *key,
                            const std::string &context) {
    return required_array(source, key, context);
}

const json &find_profile_option(const json &options, const std::string &name,
                                const std::string &context) {
    const json *match = nullptr;
    for (const auto &option : options) {
        if (!option.is_object() || option.value("name", std::string()) != name) continue;
        if (match != nullptr) invalid(context + " has duplicate native profile name '" + name + "'");
        match = &option;
    }
    if (match == nullptr) invalid(context + " has no exact native profile named '" + name + "'");
    if (!match->contains("path") || !match->at("path").is_string()) {
        const std::string reason = match->value("unavailable_reason", "source preset is unavailable");
        invalid(context + " native profile '" + name + "' is unavailable: " + reason);
    }
    return *match;
}

std::string normalized_text(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::string compact_text(const std::string &value) {
    std::string compact;
    for (const unsigned char ch : value) {
        if (std::isalnum(ch)) compact.push_back(static_cast<char>(ch));
    }
    return compact;
}

std::string material_family(const std::string &value) {
    const auto normalized = normalized_text(value);
    static const std::vector<std::string> families = {
        "PETG", "PLA", "ABS", "ASA", "TPU", "PVA", "PC", "PA", "HIPS", "TPE", "PP", "PVB",
    };
    for (const auto &family : families) {
        auto offset = normalized.find(family);
        while (offset != std::string::npos) {
            const bool left_boundary = offset == 0 ||
                !std::isalnum(static_cast<unsigned char>(normalized[offset - 1]));
            const std::size_t end = offset + family.size();
            const bool right_boundary = end == normalized.size() ||
                !std::isalnum(static_cast<unsigned char>(normalized[end]));
            if (left_boundary && right_boundary) return family;
            offset = normalized.find(family, offset + 1);
        }
    }
    return normalized;
}

std::string native_profile_material_type(const json &profile,
                                         const std::string &profile_name) {
    const auto value = profile.find("filament_type");
    if (value == profile.end()) return {};
    std::vector<std::string> types;
    if (value->is_array()) {
        for (const auto &entry : *value) {
            if (!entry.is_string() || normalized_text(entry.get<std::string>()).empty()) return {};
            types.push_back(normalized_text(entry.get<std::string>()));
        }
    } else if (value->is_string()) {
        types.push_back(normalized_text(value->get<std::string>()));
    } else {
        return {};
    }
    if (types.empty() || std::any_of(types.begin(), types.end(), [&](const auto &type) {
            return type != types.front();
        })) {
        invalid("native filament profile '" + profile_name +
                "' does not declare one consistent filament_type");
    }
    return types.front();
}

bool native_type_matches_request(const std::string &requested_type,
                                 const std::string &native_type,
                                 const std::string &profile_name,
                                 const std::string &material_name) {
    const auto requested = normalized_text(requested_type);
    const auto native = normalized_text(native_type);
    if (requested.empty() || native.empty()) return false;
    if (requested == native) return true;
    const auto requested_family = material_family(requested);
    const auto native_family = material_family(native);
    if (requested_family.empty() || requested_family != native_family) return false;
    if (requested == requested_family || requested == requested_family + " BASIC") return true;
    const auto profile_key = compact_text(profile_name);
    const auto requested_key = compact_text(requested);
    if (!requested_key.empty() && profile_key.find(requested_key) != std::string::npos) return true;
    return normalized_text(material_name) == normalized_text(profile_name);
}

struct SelectedMaterialProfile {
    std::string name;
    json profile;
};

std::vector<SelectedMaterialProfile> select_material_profiles(
    const json &request, const json &target, const json &source, const json &index,
    const std::filesystem::path &data_root, const std::string &machine_uid,
    const std::string &nozzle_uid, const json &materials) {
    const std::size_t material_count = materials.size();
    if (request.contains("native_filament_profile_names") &&
        (request.contains("material_uid") || request.contains("material_uids"))) {
        invalid("built-in source cannot combine native_filament_profile_names with material_uid(s)");
    }
    if (request.contains("material_uid") && request.contains("material_uids")) {
        invalid("built-in source cannot combine material_uid and material_uids");
    }

    const auto &material_options = profile_options(source, "filament_profile_options", "native source");
    std::map<std::string, json> resolved_profiles;
    auto profile_for_name = [&](const std::string &name) -> const json & {
        const auto cached = resolved_profiles.find(name);
        if (cached != resolved_profiles.end()) return cached->second;
        const auto &option = find_profile_option(material_options, name, "native material source");
        const auto path = required_string(option, "path", "native material source");
        return resolved_profiles.emplace(name, resolve_profile(data_root, index, path)).first->second;
    };

    std::vector<std::string> defaults;
    const auto default_names = source.find("default_filament_profile_names");
    if (default_names != source.end()) {
        if (!default_names->is_array()) {
            invalid("native default_filament_profile_names must be an array");
        }
        for (const auto &name : *default_names) {
            if (!name.is_string() || name.get<std::string>().empty()) {
                invalid("native default_filament_profile_names must contain non-empty preset names");
            }
            defaults.push_back(name.get<std::string>());
        }
    }
    if (defaults.empty()) {
        const auto default_name = source.find("default_filament_profile_name");
        if (default_name != source.end() && default_name->is_string() &&
            !default_name->get<std::string>().empty()) {
            defaults.push_back(default_name->get<std::string>());
        }
    }
    // AnycubicSlicerNext and Orca's PresetBundle use the ordered native
    // defaults by filament slot, then fall back to the first default:
    // https://raw.githubusercontent.com/ANYCUBIC-3D/AnycubicSlicerNext/main/src/libslic3r/PresetBundle.cpp
    // https://github.com/SoftFever/OrcaSlicer/blob/main/src/libslic3r/PresetBundle.cpp
    const auto default_for_slot = [&](std::size_t slot) -> std::string {
        if (defaults.empty()) return {};
        return defaults[slot < defaults.size() ? slot : 0];
    };

    std::vector<std::string> requested_names;
    const auto explicit_names = request.find("native_filament_profile_names");
    if (explicit_names != request.end()) {
        if (!explicit_names->is_array() || explicit_names->size() != material_count) {
            invalid("native_filament_profile_names must match source_materials slots");
        }
        for (const auto &name : *explicit_names) {
            if (!name.is_string() || name.get<std::string>().empty()) {
                invalid("native_filament_profile_names must contain non-empty preset names");
            }
            requested_names.push_back(name.get<std::string>());
        }
    } else if (request.contains("material_uids")) {
        const auto &ids = required_array(request, "material_uids", "built-in request");
        if (ids.size() != material_count) {
            invalid("material_uids must match source_materials slots for a built-in source");
        }
        for (const auto &id : ids) {
            if (!id.is_string() || id.get<std::string>().empty()) {
                invalid("material_uids must contain non-empty identities");
            }
        }
    } else if (request.contains("material_uid")) {
        const auto &id = request.at("material_uid");
        if (!id.is_string() || id.get<std::string>().empty()) {
            invalid("material_uid must be non-empty text for a built-in source");
        }
    }

    if (request.contains("material_uid") || request.contains("material_uids")) {
        std::vector<std::string> selected_material_ids;
        if (request.contains("material_uids")) {
            for (const auto &id : request.at("material_uids")) {
                selected_material_ids.push_back(id.get<std::string>());
            }
        } else {
            selected_material_ids.assign(material_count,
                                         request.at("material_uid").get<std::string>());
        }
        const auto bindings = target.find("material_bindings");
        if (bindings == target.end() || !bindings->is_array()) {
            invalid("target data has no native material bindings for the selected source");
        }
        for (const auto &material_uid : selected_material_ids) {
            std::string profile_name;
            for (const auto &binding : *bindings) {
                if (!binding.is_object() ||
                    binding.value("machine_uid", std::string()) != machine_uid ||
                    binding.value("nozzle_uid", std::string()) != nozzle_uid ||
                    binding.value("material_uid", std::string()) != material_uid) {
                    continue;
                }
                if (profile_name.empty()) {
                    profile_name = binding.value("native_profile_id", std::string());
                } else if (profile_name != binding.value("native_profile_id", std::string())) {
                    invalid("target has ambiguous native material preset bindings for '" + material_uid + "'");
                }
            }
            if (profile_name.empty()) {
                invalid("target has no exact native material binding for '" + material_uid + "'");
            }
            requested_names.push_back(profile_name);
        }
    } else if (explicit_names == request.end()) {
        for (std::size_t slot = 0; slot < material_count; ++slot) {
            const auto requested_type = normalized_text(
                detail::requested_material_type(materials.at(slot)));
            if (requested_type.empty()) {
                const auto native_default = default_for_slot(slot);
                if (native_default.empty()) {
                    invalid("built-in source has no native default for source material slot " +
                            std::to_string(slot) + "; select native_filament_profile_names");
                }
                requested_names.push_back(native_default);
                continue;
            }

            std::vector<std::string> matching_names;
            for (const auto &option : material_options) {
                if (!option.is_object() || !option.contains("path") || !option.at("path").is_string()) continue;
                const auto name = required_string(option, "name", "native material source");
                const auto &profile = profile_for_name(name);
                const auto native_type = native_profile_material_type(profile, name);
                const auto source_name = materials.at(slot).value("name", std::string());
                if (native_type_matches_request(requested_type, native_type, name, source_name)) {
                    matching_names.push_back(name);
                }
            }
            if (matching_names.empty()) {
                invalid("no available native filament profile matches source material slot " +
                        std::to_string(slot) + " type '" + requested_type +
                        "'; select a compatible native_filament_profile_names entry");
            }

            std::vector<std::string> exact_name_matches;
            const auto requested_name = materials.at(slot).value("name", std::string());
            for (const auto &name : matching_names) {
                if (!requested_name.empty() &&
                    normalized_text(requested_name) == normalized_text(name)) {
                    exact_name_matches.push_back(name);
                }
            }
            if (exact_name_matches.size() == 1) {
                requested_names.push_back(exact_name_matches.front());
                continue;
            }

            const auto requested_key = compact_text(requested_type);
            const auto family = material_family(requested_type);
            const bool specific_type = !requested_key.empty() && requested_type != family;
            if (specific_type) {
                std::vector<std::string> variant_matches;
                for (const auto &name : matching_names) {
                    if (compact_text(name).find(requested_key) != std::string::npos) {
                        variant_matches.push_back(name);
                    }
                }
                if (variant_matches.size() == 1) {
                    requested_names.push_back(variant_matches.front());
                    continue;
                }
            }

            const auto native_default = default_for_slot(slot);
            if (std::find(matching_names.begin(), matching_names.end(), native_default) !=
                matching_names.end()) {
                requested_names.push_back(native_default);
            } else if (matching_names.size() == 1) {
                requested_names.push_back(matching_names.front());
            } else {
                std::string choices;
                for (const auto &name : matching_names) {
                    if (!choices.empty()) choices += ", ";
                    choices += name;
                }
                invalid("source material slot " + std::to_string(slot) + " type '" +
                        requested_type + "' matches multiple native profiles (" + choices +
                        "); select native_filament_profile_names explicitly");
            }
        }
    }

    if (requested_names.size() != material_count) {
        invalid("native material profile selection must match source_materials slots");
    }
    std::vector<SelectedMaterialProfile> selected;
    selected.reserve(material_count);
    for (std::size_t slot = 0; slot < material_count; ++slot) {
        const auto &name = requested_names[slot];
        const auto &profile = profile_for_name(name);
        const auto native_type = native_profile_material_type(profile, name);
        if (native_type.empty()) {
            invalid("native filament profile '" + name + "' does not provide a filament_type; "
                    "select a source with an explicit material type");
        }
        const auto requested_type = normalized_text(
            detail::requested_material_type(materials.at(slot)));
        if (!requested_type.empty() && !native_type_matches_request(
                requested_type, native_type, name,
                materials.at(slot).value("name", std::string()))) {
            invalid("source material slot " + std::to_string(slot) + " declares type '" +
                    requested_type + "' but native profile '" + name + "' defines '" +
                    native_type + "'; select a matching native_filament_profile_names entry "
                    "or correct the source material type");
        }
        selected.push_back({name, profile});
    }
    return selected;
}

std::string source_process_name(const json &request, const json &source) {
    const auto requested = request.find("native_print_profile_name");
    if (requested != request.end()) {
        if (!requested->is_string() || requested->get<std::string>().empty()) {
            invalid("native_print_profile_name must be non-empty text");
        }
        return requested->get<std::string>();
    }
    const auto default_name = source.find("default_print_profile_name");
    if (default_name == source.end() || !default_name->is_string() ||
        default_name->get<std::string>().empty()) {
        invalid("built-in source has no unique native default process; select exact "
                "native_print_profile_name from print_profile_options");
    }
    return default_name->get<std::string>();
}

void require_target_machine_binding(const json &canonical, const json &target,
                                    const std::string &slicer_id,
                                    const std::string &application_version,
                                    const std::string &machine_uid,
                                    const std::string &nozzle_uid,
                                    const std::string &source_machine_profile_name) {
    const std::string selection = slicer_id + " " + application_version + " " +
        machine_uid + " " + nozzle_uid;
    bool has_canonical_machine_nozzle = false;
    const auto machines = canonical.find("machines");
    if (machines != canonical.end() && machines->is_array()) {
        for (const auto &machine : *machines) {
            if (!machine.is_object() ||
                machine.value("machine_uid", std::string()) != machine_uid) {
                continue;
            }
            const auto nozzles = machine.find("supported_nozzle_uids");
            has_canonical_machine_nozzle = nozzles != machine.end() && nozzles->is_array() &&
                std::find(nozzles->begin(), nozzles->end(), nozzle_uid) != nozzles->end();
            break;
        }
    }
    if (!has_canonical_machine_nozzle) {
        invalid("no FatCat canonical machine/nozzle identity for " + selection);
    }

    bool has_exact_target_binding = false;
    const auto bindings = target.find("machine_bindings");
    if (bindings != target.end() && bindings->is_array()) {
        for (const auto &binding : *bindings) {
            if (binding.is_object() &&
                binding.value("machine_uid", std::string()) == machine_uid &&
                binding.value("nozzle_uid", std::string()) == nozzle_uid &&
                binding.value("source_profile_name", std::string()) ==
                    source_machine_profile_name) {
                has_exact_target_binding = true;
                break;
            }
        }
    }
    if (!has_exact_target_binding) {
        invalid("no FatCat target machine binding for " + selection +
                " (native profile '" + source_machine_profile_name + "')");
    }
}

json compose_builtin_project(const json &input_request, const json &canonical,
                             const json &target, const std::filesystem::path &data_root) {
    if (input_request.value("project_source", std::string()) != "fatcat_native") {
        invalid("a missing project_json requires project_source='fatcat_native'");
    }
    const std::string slicer_id = required_string(input_request, "slicer_id", "request");
    const std::string application_version =
        required_string(input_request, "application_version", "request");
    const std::string machine_uid = required_string(input_request, "machine_uid", "request");
    const std::string nozzle_uid = required_string(input_request, "nozzle_uid", "request");
    const auto &contract = target.at("target_contract");
    if (contract.value("slicer_id", std::string()) != slicer_id ||
        contract.value("application_version", std::string()) != application_version) {
        invalid("request slicer/version does not match the pinned target data");
    }

    const json index = read_index(data_root);
    const auto matches = matching_source_rows(index, slicer_id, application_version,
                                              machine_uid, nozzle_uid);
    if (matches.size() != 1) {
        invalid("no unique built-in native source for " + slicer_id + " " +
                application_version + " " + machine_uid + " " + nozzle_uid);
    }
    const json &source = *matches.front();
    const std::string source_machine_profile_name = required_string(
        source, "source_machine_profile_name", "built-in native source");
    require_target_machine_binding(canonical, target, slicer_id, application_version,
                                   machine_uid, nozzle_uid,
                                   source_machine_profile_name);
    const auto &availability = source.at("availability");
    if (availability.value("machine", std::string()) != "available") {
        invalid("built-in native machine source is unavailable for " +
                source.value("source_machine_profile_name", machine_uid) + ": " +
                availability.value("reason", std::string("exact native machine source missing")));
    }
    const auto &materials = required_array(input_request, "source_materials", "built-in request");
    if (materials.empty()) invalid("source_materials must contain at least one selected material");
    const std::string process_name = source_process_name(input_request, source);
    const auto &process_options = profile_options(source, "print_profile_options", "native source");
    const auto &process_option = find_profile_option(process_options, process_name, "native process source");
    const std::string process_path = required_string(process_option, "path", "native process source");

    const auto selected_materials = select_material_profiles(
        input_request, target, source, index, data_root, machine_uid, nozzle_uid, materials);
    std::vector<std::string> material_names;
    std::vector<json> material_profiles;
    material_names.reserve(selected_materials.size());
    material_profiles.reserve(selected_materials.size());
    for (const auto &selected : selected_materials) {
        material_names.push_back(selected.name);
        material_profiles.push_back(selected.profile);
    }

    const auto machine_path = required_string(source, "machine_profile_path", "native source");
    const json machine = resolve_profile(data_root, index, machine_path);
    const json process = resolve_profile(data_root, index, process_path);
    json project = json::object();
    std::vector<std::vector<std::string>> variant_groups;
    merge_profile_settings(project, machine, true);
    merge_profile_settings(project, process);
    project.update(compose_material_settings(
        material_profiles, material_names, target, variant_groups));

    for (const auto *key : {"printer_model", "nozzle_diameter"}) {
        if (!machine.contains(key) || !project.contains(key) ||
            machine.at(key) != project.at(key)) {
            invalid(std::string("selected process/material source conflicts with native hardware at '") + key + "'");
        }
    }
    project["name"] = "project_settings";
    project["from"] = "project";
    project["printer_settings_id"] = source.at("source_machine_profile_name");
    project["print_settings_id"] = process_name;
    project["filament_settings_id"] = material_names;
    project["default_print_profile"] = process_name;
    project["default_filament_profile"] = material_names;
    project["print_compatible_printers"] = json::array({source.at("source_machine_profile_name")});
    for (const auto *key : {"type", "instantiation", "inherits", "include", "setting_id",
                            "compatible_printers", "compatible_prints"}) {
        project.erase(key);
    }

    // This target-version value mirrors QIDIStudio 02.07.02.60's compiled
    // PrintConfig default (curr_bed_type=btPC, which maps to Cool Plate) and
    // applies across that version's machine/nozzle bindings. Apply it only to
    // built-in native sources that omitted the active plate; explicit source
    // values remain authoritative and request plate UIDs are resolved later.
    const auto native_project_defaults = target.find("native_project_defaults");
    if (native_project_defaults != target.end()) {
        if (!native_project_defaults->is_object()) {
            invalid("target-version native project defaults must be an object");
        }
        const auto default_plate = native_project_defaults->find("curr_bed_type");
        if (default_plate != native_project_defaults->end()) {
            if (!default_plate->is_string() || default_plate->get<std::string>().empty()) {
                invalid("target-version native project default curr_bed_type must be non-empty text");
            }
            if (!project.contains("curr_bed_type")) {
                project["curr_bed_type"] = *default_plate;
            }
        }
    }

    json request = input_request;
    if (slicer_id == "OrcaSlicer") {
        auto process_settings = request.value("process_settings", json::object());
        if (!process_settings.is_object()) {
            invalid("request.process_settings must be an object");
        }
        if (!request.contains("precise_outer_wall") &&
            !process_settings.contains("precise_outer_wall")) {
            process_settings["precise_outer_wall"] = "0";
        }
        request["process_settings"] = std::move(process_settings);
    }
    request.erase("project_source");
    request.erase("native_print_profile_name");
    request.erase("native_filament_profile_names");
    request.erase("material_uid");
    request.erase("material_uids");
    if (request.contains("hardware_mode") && request.at("hardware_mode") != "preserve_source") {
        invalid("built-in native sources require hardware_mode='preserve_source'");
    }
    request["hardware_mode"] = "preserve_source";
    if (request.contains("material_mode") && request.at("material_mode") != "preserve_template") {
        invalid("built-in native sources require material_mode='preserve_template'");
    }
    request["material_mode"] = "preserve_template";
    if (request.contains("preserve_source_material_settings") &&
        request.at("preserve_source_material_settings") != true) {
        invalid("built-in native sources preserve the explicitly selected native material settings");
    }
    request["preserve_source_material_settings"] = true;
    auto composed = json::parse(compose_project_settings(
        project.dump(), request.dump(), canonical.dump(), target.dump()));
    apply_builtin_native_variant_identity(composed, material_names, variant_groups);
    return composed;
}

}  // namespace

std::string native_project_source_catalog(
    std::string_view slicer_id,
    std::string_view application_version,
    const std::filesystem::path &data_root) {
    try {
        const auto index = read_index(data_root);
        json sources = json::array();
        for (const auto &source : index.at("sources")) {
            if (!source.is_object() ||
                source.value("slicer_id", std::string()) != slicer_id ||
                source.value("application_version", std::string()) != application_version) {
                continue;
            }
            const auto machine_path = required_string(
                source, "machine_profile_path", "native source");
            const auto machine = resolve_profile(data_root, index, machine_path);
            json hardware = json::object();
            for (const auto *key : {"printer_model", "nozzle_diameter"}) {
                if (machine.contains(key)) hardware[key] = machine.at(key);
            }
            sources.push_back({
                {"machine_uid", required_string(source, "machine_uid", "native source")},
                {"nozzle_uid", required_string(source, "nozzle_uid", "native source")},
                {"source_machine_profile_name", required_string(
                    source, "source_machine_profile_name", "native source")},
                {"availability", source.value("availability", json::object())},
                {"default_print_profile_name", source.value(
                    "default_print_profile_name", std::string())},
                {"hardware_settings", std::move(hardware)},
            });
        }
        return json{{"schema_version", 1},
                    {"slicer_id", slicer_id},
                    {"application_version", application_version},
                    {"sources", std::move(sources)}}.dump();
    } catch (const ProjectSettingsError &) {
        throw;
    } catch (const json::exception &error) {
        invalid(std::string("invalid native source catalog JSON: ") + error.what());
    } catch (const std::filesystem::filesystem_error &error) {
        invalid(std::string("native source catalog file access failed: ") + error.what());
    }
}

std::string compose_builtin_project_settings(
    std::string_view request_json,
    std::string_view canonical_json,
    std::string_view target_json,
    const std::filesystem::path &data_root) {
    try {
        const auto request = json::parse(request_json);
        const auto canonical = json::parse(canonical_json);
        const auto target = json::parse(target_json);
        if (!request.is_object() || !canonical.is_object() || !target.is_object()) {
            invalid("built-in project source inputs must be JSON objects");
        }
        return compose_builtin_project(request, canonical, target, data_root).dump();
    } catch (const ProjectSettingsError &) {
        throw;
    } catch (const json::exception &error) {
        invalid(std::string("invalid built-in project source JSON: ") + error.what());
    } catch (const std::filesystem::filesystem_error &error) {
        invalid(std::string("native project source file access failed: ") + error.what());
    }
}

}  // namespace fatcat
