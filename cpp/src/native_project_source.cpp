#include "fatcat/native_project_source.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "filament_projection.h"
#include "filesystem_path.h"
#include "native_material_selection.h"
#include "prusa_project.h"
#include "fatcat/project_settings.h"
#include "fatcat/metadata_components.h"
#include "fatcat/source_project_settings.h"
#include "fatcat/template_assembly.h"

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
    std::ifstream stream(detail::io_path(path), std::ios::binary);
    if (!stream) invalid("native project source file is missing or unreadable: " + path.u8string());
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

std::filesystem::path safe_path(const std::filesystem::path &root,
                                const std::string &relative) {
    const auto rel = std::filesystem::u8path(relative);
    if (relative.empty() || rel.is_absolute() || rel.has_root_name() ||
        rel.has_root_directory()) {
        invalid("native source path must be relative: " + relative);
    }
    for (const auto &part : rel) {
        if (part == "." || part == "..") {
            invalid("native source path contains traversal: " + relative);
        }
    }
    const auto canonical_root = detail::canonical_io_path(root);
    const auto candidate = detail::canonical_io_path(root / rel);
    const auto beneath = candidate.lexically_relative(canonical_root);
    if (beneath.empty() || beneath.is_absolute() ||
        *beneath.begin() == std::filesystem::path("..")) {
        invalid("native source path escapes its data root: " + relative);
    }
    std::error_code access_error;
    if (!std::filesystem::is_regular_file(candidate, access_error)) {
        if (access_error && access_error != std::errc::no_such_file_or_directory) {
            invalid("native source profile access failed: " + relative +
                    " (" + access_error.category().name() + ":" +
                    std::to_string(access_error.value()) + ": " + access_error.message() + ")");
        }
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

class NativeProfileCatalog {
public:
    explicit NativeProfileCatalog(const std::filesystem::path &data_root)
        : data_root_(data_root), index_(read_index(data_root)) {}

    const json &index() const { return index_; }

    const json &read(const std::string &profile_path) {
        const auto found = profiles_.find(profile_path);
        if (found != profiles_.end()) return found->second;
        return profiles_.emplace(profile_path, read_json(
            safe_path(data_root_, profile_path), "native profile")).first->second;
    }

    json resolve(const std::string &profile_path) {
        std::vector<std::string> ancestry;
        return resolve(profile_path, ancestry);
    }

private:
    // Raw files live only for this public call. Resolve each chain normally so
    // cycle/depth checks and include precedence remain independent of reuse.
    std::filesystem::path data_root_;
    json index_;
    std::map<std::string, json> profiles_;

    json resolve(const std::string &profile_path,
                 std::vector<std::string> &ancestry) {
        if (std::find(ancestry.begin(), ancestry.end(), profile_path) != ancestry.end() ||
            ancestry.size() >= 48) {
            invalid("cyclic or excessively deep native profile chain at " + profile_path);
        }
        ancestry.push_back(profile_path);
        const json &source = read(profile_path);
        json result = json::object();

        const auto inherits = source.find("inherits");
        if (inherits != source.end() && !inherits->is_null() &&
            !(inherits->is_string() && inherits->get<std::string>().empty())) {
            if (!inherits->is_string()) {
                invalid("native profile inherits field must be text: " + profile_path);
            }
            const auto name = inherits->get<std::string>();
            result.update(resolve(
                linked_profile_path(index_, profile_path, "inherits", name), ancestry));
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
                result.update(resolve(
                    linked_profile_path(index_, profile_path, "include", name), ancestry));
            }
        }

        result.update(source);
        ancestry.pop_back();
        return result;
    }
};

json public_source_record(const json &source, NativeProfileCatalog &catalog) {
    const auto machine_path = required_string(
        source, "machine_profile_path", "native source");
    const auto machine = catalog.resolve(machine_path);
    json hardware = json::object();
    for (const auto *key : {"printer_model", "nozzle_diameter"}) {
        if (machine.contains(key)) hardware[key] = machine.at(key);
    }
    return json{
        {"machine_uid", required_string(source, "machine_uid", "native source")},
        {"nozzle_uid", required_string(source, "nozzle_uid", "native source")},
        {"source_machine_profile_name", required_string(
            source, "source_machine_profile_name", "native source")},
        {"availability", source.value("availability", json::object())},
        {"default_print_profile_name", source.value(
            "default_print_profile_name", std::string())},
        {"hardware_settings", std::move(hardware)},
        {"compatibility_project", source.value("compatibility_project", json())},
    };
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
            const json *resolved_variants = found == profiles[slot].end()
                ? native_schema_default(snapshot, "filament_extruder_variant")
                : &*found;
            if (resolved_variants == nullptr || !resolved_variants->is_array() ||
                resolved_variants->empty()) {
                invalid("native material profile '" + profile_names[slot] +
                        "' has no resolved filament_extruder_variant list");
            }
            std::vector<std::string> variants;
            for (const auto &variant : *resolved_variants) {
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
    if (const auto defaults = snapshot.find("native_schema_defaults");
        defaults != snapshot.end() && defaults->contains("values")) {
        for (const auto &[key, value] : defaults->at("values").items()) {
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
            std::size_t selected_slot = 0;
            bool missing = false;
            for (std::size_t slot = 0; slot < profiles.size(); ++slot) {
                const auto found = profiles[slot].find(key);
                if (found == profiles[slot].end()) {
                    missing = true;
                    continue;
                }
                if (selected == nullptr) {
                    selected = &*found;
                    selected_slot = slot;
                } else if (*selected != *found) {
                    invalid("native material profiles disagree on non-material setting '" + key +
                            "' between '" + profile_names[selected_slot] + "' and '" +
                            profile_names[slot] + "'; select profiles with a shared value");
                }
            }
            if (selected == nullptr) {
                if (const auto *default_value = native_schema_default(snapshot, key)) {
                    combined[key] = *default_value;
                }
                continue;
            }
            if (missing) {
                const auto *default_value = native_schema_default(snapshot, key);
                if (default_value == nullptr || *selected != *default_value) {
                    invalid("native material profile '" + profile_names[selected_slot] +
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

    // Preset identity is structural in the profile but a per-material vector
    // in the project. Native startup G-code may index it directly. Resolve it
    // from each inherited preset, independently of its parameter variants.
    if (target_material_fields.find("filament_ids") != target_material_fields.end()) {
        json ids = json::array();
        for (std::size_t slot = 0; slot < profiles.size(); ++slot) {
            const auto found = profiles[slot].find("filament_id");
            if (found != profiles[slot].end() && !found->is_string()) {
                invalid("native material profile '" + profile_names[slot] +
                        "' has a non-text filament_id");
            }
            ids.push_back(found == profiles[slot].end() ? json("") : *found);
        }
        combined["filament_ids"] = std::move(ids);
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

using detail::material_compatible_with_machine;
using detail::material_supports_plate;
using detail::native_profile_material_type;
using detail::native_type_matches_request;

json offered_build_plates(const json &canonical, const json &target,
                          const std::string &machine_uid);
json material_choices(const json &source, const json &target, const json &plates,
                      const json &selected_plate, NativeProfileCatalog &catalog,
                      const std::string &machine_name);

json retained_material_choices(const json &project, const json &target,
                               const json &plates) {
    const auto &names = required_array(project, "filament_settings_id", "retained source");
    const auto &types = required_array(project, "filament_type", "retained source");
    if (names.size() != types.size()) invalid("retained material identities and types differ");
    json choices = json::array();
    std::set<std::string> seen;
    for (std::size_t slot = 0; slot < names.size(); ++slot) {
        const auto name = names.at(slot).get<std::string>();
        if (!seen.insert(name).second) continue;
        json profile = {{"filament_type", json::array({types.at(slot)})}};
        for (const auto &plate : target.at("build_plate_bindings")) {
            const auto key = plate.at("sidecar_value").get<std::string>() + "_temp";
            const auto values = project.find(key);
            if (values != project.end() && values->is_array() && values->size() == names.size()) {
                profile[key] = json::array({values->at(slot)});
            }
        }
        json supported = json::array();
        for (const auto &plate : target.at("build_plate_bindings")) {
            if (std::any_of(plates.begin(), plates.end(), [&](const json &choice) {
                    return choice.at("build_plate_uid") == plate.at("build_plate_uid");
                }) && material_supports_plate(profile, plate)) {
                supported.push_back(plate.at("build_plate_uid"));
            }
        }
        choices.push_back({{"name", name}, {"material_type", native_profile_material_type(profile, name)},
            {"vendor", ""}, {"available", true}, {"machine_compatible", true},
            {"supported_build_plate_uids", std::move(supported)}});
    }
    return choices;
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
        if (source.contains("compatibility_project")) {
            return required_string(source.at("compatibility_project"),
                                   "source_profile_name", "compatibility source");
        }
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
                             const json &target, const std::filesystem::path &data_root,
                             bool material_selection_only = false) {
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

    NativeProfileCatalog catalog(data_root);
    const json &index = catalog.index();
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
    const bool per_material_selection = std::any_of(materials.begin(), materials.end(),
        [](const json &material) { return material.contains("native_filament_profile_name"); });
    const std::string process_name = source_process_name(input_request, source);
    const bool compatibility = availability.value("process", std::string()) != "available" &&
                               source.contains("compatibility_project");
    bool retained_materials = compatibility &&
        availability.value("material", std::string()) != "available" &&
        !per_material_selection &&
        !input_request.contains("native_filament_profile_names") &&
        !input_request.contains("material_uid") && !input_request.contains("material_uids");
    json process;
    json provenance;
    if (compatibility) {
        const auto &record = source.at("compatibility_project");
        if (process_name != required_string(record, "source_profile_name", "compatibility source")) {
            invalid("requested process does not match the recorded compatibility source");
        }
        const auto &payload = catalog.read(required_string(record, "path", "compatibility source"));
        provenance = payload.at("source");
        for (const auto *key : {"source_slicer_id", "source_application_version", "source_profile_name"}) {
            if (provenance.at(key) != record.at(key)) invalid("compatibility provenance differs from source index");
        }
        if (provenance.at("machine_uid") != machine_uid || provenance.at("nozzle_uid") != nozzle_uid) {
            invalid("compatibility source hardware identity differs from selected native source");
        }
        process = payload.at("project_settings");
        // Preserve the recorded project's slots through the shared composer.
        // Exact native material profiles replace them only when available.
        process = detail::compose_source_project(process,
            {{"source_materials", materials}, {"filament_slot_mode", "compact"},
             {"preserve_source_material_settings", true}}, target);
    } else {
        const auto &options = profile_options(source, "print_profile_options", "native source");
        const auto &option = find_profile_option(options, process_name, "native process source");
        process = catalog.resolve(required_string(option, "path", "native process source"));
    }

    const auto machine_path = required_string(source, "machine_profile_path", "native source");
    const json machine = catalog.resolve(machine_path);
    json plate_project = json::object();
    if (compatibility) merge_profile_settings(plate_project, process);
    merge_profile_settings(plate_project, machine, true);
    if (!compatibility) merge_profile_settings(plate_project, process);
    const auto defaults = target.find("native_project_defaults");
    if (!plate_project.contains("curr_bed_type") && defaults != target.end() &&
            defaults->contains("curr_bed_type")) {
        plate_project["curr_bed_type"] = defaults->at("curr_bed_type");
    }
    const json plate = detail::resolve_source_plate(plate_project, input_request, target);

    std::vector<std::string> material_names;
    std::vector<json> material_profiles;
    material_names.reserve(materials.size());
    material_profiles.reserve(materials.size());
    const auto plates = offered_build_plates(canonical, target, machine_uid);
    auto choices = material_choices(source, target, plates, plate, catalog,
                                    source_machine_profile_name);
    auto selection = detail::resolve_native_material_selection(
        input_request, source, target, materials, plate, choices, plates);
    if (retained_materials || (compatibility && !selection.at("ready").get<bool>())) {
        const auto &identities = required_array(process, "filament_settings_id", "retained material source");
        const auto &types = required_array(process, "filament_type", "retained material source");
        if (identities.size() != materials.size() || types.size() != materials.size()) {
            invalid("retained material source slots differ from the selected palette");
        }
        auto retained_source = source;
        retained_source["default_filament_profile_names"] = identities;
        auto retained_selection = detail::resolve_native_material_selection(
            input_request, retained_source, target, materials, plate,
            retained_material_choices(process, target, plates), plates);
        // An unsuccessful fallback must not hide native alternatives or the
        // compatible plates that can resolve the original selection.
        if (retained_materials || retained_selection.at("ready").get<bool>()) {
            retained_materials = true;
            selection = std::move(retained_selection);
        }
    }
    json material_settings = json::object();
    std::vector<std::vector<std::string>> variant_groups;
    if (selection.at("ready").get<bool>() && retained_materials) {
        const auto &identities = process.at("filament_settings_id");
        for (const auto &name : identities) material_names.push_back(name.get<std::string>());
    } else if (selection.at("ready").get<bool>()) {
        for (const auto &selected : selection.at("materials")) {
            const auto name = selected.at("selected_profile_name").get<std::string>();
            const auto &option = find_profile_option(
                profile_options(source, "filament_profile_options", "native source"),
                name, "native material source");
            material_names.push_back(name);
            material_profiles.push_back(catalog.resolve(
                required_string(option, "path", "native material source")));
        }
        // A compatible preset in each slot is insufficient: their shared
        // settings must also compose. Prepare once for preflight and export.
        try {
            material_settings = compose_material_settings(
                material_profiles, material_names, target, variant_groups);
        } catch (const ProjectSettingsError &error) {
            selection["ready"] = false;
            selection["issues"].push_back({
                {"code", "native_material_settings_incompatible"},
                {"material_index", nullptr}, {"material_name", ""}, {"material_type", ""},
                {"build_plate_uid", plate.at("build_plate_uid")},
                {"message", std::string("Selected native filament profiles cannot be combined: ") +
                    error.what() + ". Select profiles with compatible shared settings or "
                    "provide an exported native project configuration for these materials."},
            });
        }
    }
    if (material_selection_only) return selection;
    if (!selection.at("ready").get<bool>()) {
        throw ProjectSettingsError(selection.at("issues").front().at("message").get<std::string>(),
                                   selection.dump());
    }

    json project = std::move(plate_project);
    project.update(material_settings);

    for (const auto *key : {"printer_model", "nozzle_diameter"}) {
        if (!machine.contains(key) || !project.contains(key) ||
            machine.at(key) != project.at(key)) {
            invalid(std::string("selected process/material source conflicts with native hardware at '") + key + "'");
        }
    }
    project["name"] = "project_settings";
    project["from"] = "project";
    if (compatibility && process.contains("version")) project["version"] = process.at("version");
    else if (contract.contains("saved_project_format_version")) project["version"] = contract.at("saved_project_format_version");
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

    project = json::parse(assemble_project_template(project.dump(), std::nullopt, "{}"));
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
    if (!retained_materials) apply_builtin_native_variant_identity(composed, material_names, variant_groups);
    if (compatibility) composed["process_source"] = provenance;
    if (retained_materials) {
        auto material_provenance = provenance;
        material_provenance["filament_settings_id"] = material_names;
        composed["material_source"] = std::move(material_provenance);
    } else {
        composed["material_source"] = {{"source_slicer_id", slicer_id},
            {"source_application_version", application_version},
            {"filament_settings_id", material_names}};
    }
    return composed;
}

json offered_build_plates(const json &canonical, const json &target,
                           const std::string &machine_uid) {
    json plates = json::array();
    for (const auto &machine : canonical.at("machines")) {
        if (machine.at("machine_uid") != machine_uid) continue;
        const auto &supported = machine.at("supported_build_plate_uids");
        for (const auto &plate : target.at("build_plate_bindings")) {
            if (std::find(supported.begin(), supported.end(), plate.at("build_plate_uid")) != supported.end()) {
                plates.push_back({{"build_plate_uid", plate.at("build_plate_uid")},
                                  {"name", plate.at("project_value")}});
            }
        }
    }
    return plates;
}

bool contains_plate(const json &plates, const json &uid) {
    return std::any_of(plates.begin(), plates.end(), [&](const json &plate) {
        return plate.at("build_plate_uid") == uid;
    });
}

json process_choices(const json &source, NativeProfileCatalog &catalog) {
    json choices = json::array();
    for (const auto &option : profile_options(source, "print_profile_options", "native source")) {
        json choice = {{"name", option.at("name")}, {"available", option.contains("path")}};
        if (option.contains("path")) {
            try {
                const auto profile = catalog.resolve(required_string(option, "path", "native process"));
                if (profile.contains("layer_height")) choice["layer_height"] = profile.at("layer_height");
            } catch (const ProjectSettingsError &error) {
                choice["available"] = false;
                choice["unavailable_reason"] = error.what();
            }
        } else {
            choice["unavailable_reason"] = option.value("unavailable_reason", "native profile file is unavailable");
        }
        choices.push_back(std::move(choice));
    }
    return choices;
}

json material_choices(const json &source, const json &target, const json &plates,
                      const json &selected_plate, NativeProfileCatalog &catalog,
                      const std::string &machine_name) {
    json choices = json::array();
    for (const auto &option : profile_options(source, "filament_profile_options", "native source")) {
        const auto name = required_string(option, "name", "native material");
        json choice = {{"name", name}, {"available", false}};
        if (!option.contains("path")) {
            choice["unavailable_reason"] = option.value("unavailable_reason", "native profile file is unavailable");
            choice["source_unavailable"] = true;
            choices.push_back(std::move(choice));
            continue;
        }
        json profile;
        std::string type;
        bool machine_compatible = false;
        try {
            profile = catalog.resolve(required_string(option, "path", "native material"));
            type = native_profile_material_type(profile, name);
            machine_compatible = material_compatible_with_machine(profile, machine_name);
        } catch (const ProjectSettingsError &error) {
            choice["unavailable_reason"] = error.what();
            choice["source_unavailable"] = true;
            choices.push_back(std::move(choice));
            continue;
        }
        choice["material_type"] = type;
        const auto vendors = profile.value("filament_vendor", json::array());
        choice["vendor"] = vendors.is_array() && !vendors.empty() && vendors.front().is_string()
            ? vendors.front().get<std::string>() : std::string();
        choice["machine_compatible"] = machine_compatible && !type.empty();
        choice["supported_build_plate_uids"] = json::array();
        for (const auto &plate : target.at("build_plate_bindings")) {
            const auto uid = plate.at("build_plate_uid");
            if (contains_plate(plates, uid) && material_supports_plate(profile, plate)) {
                choice["supported_build_plate_uids"].push_back(uid);
            }
        }
        if (!machine_compatible) {
            choice["unavailable_reason"] = "native profile is incompatible with the selected machine";
        } else if (type.empty()) {
            choice["unavailable_reason"] = "native profile has no material type";
        } else if (!selected_plate.is_null() && !material_supports_plate(profile, selected_plate)) {
            choice["unavailable_reason"] = "native profile does not support the selected build plate";
        } else {
            choice["available"] = true;
        }
        choices.push_back(std::move(choice));
    }
    return choices;
}

json resolve_prusa_materials(const json &input, const json &target,
                            const std::filesystem::path &source_root) {
    auto request = input;
    detail::prusa::validate_builtin_material_selection(request, target, source_root);
    auto source = detail::prusa::options(request, target, source_root);
    const auto &materials = required_array(request, "source_materials", "material selection");
    const auto &plates = source.at("build_plates");
    const auto uid = request.value("build_plate_uid",
        plates.front().at("build_plate_uid").get<std::string>());
    if (!contains_plate(plates, uid)) invalid("build_plate_uid is not offered by this native configuration");
    json plate = {{"build_plate_uid", uid}, {"project_value", uid}};
    json profiles = source.at("filament_profiles");
    for (auto &profile : profiles) {
        profile["vendor"] = "";
        profile["machine_compatible"] = profile.at("available");
        profile["supported_build_plate_uids"] = json::array({uid});
    }
    // Prusa's existing preset selection accepts one name or native capacity.
    // Preserve that public contract while resolving the caller's logical slots.
    if (request.contains("native_filament_profile_names")) {
        const auto names = request.at("native_filament_profile_names");
        if (names.is_array() && !names.empty() && names.size() != materials.size()) {
            json logical = json::array();
            for (std::size_t slot = 0; slot < materials.size(); ++slot) {
                logical.push_back(names.at(names.size() == 1 ? 0 : slot));
            }
            request["native_filament_profile_names"] = std::move(logical);
        }
    }
    return detail::resolve_native_material_selection(
        request, source, target, materials, plate, profiles, plates);
}

}  // namespace

std::string resolve_project_materials(std::string_view request_json,
                                     const std::filesystem::path &data_root) {
    try {
        const auto request = json::parse(request_json);
        const auto target = json::parse(metadata_target_data(
            required_string(request, "slicer_id", "material selection"),
            required_string(request, "application_version", "material selection"), data_root));
        const auto source_root = data_root / "native_project_sources";
        if (detail::prusa::is_target(target)) {
            return resolve_prusa_materials(request, target, source_root).dump();
        }
        const auto canonical = read_json(safe_path(data_root, "translations/canonical.json"),
                                         "canonical data");
        return compose_builtin_project(request, canonical, target, source_root, true).dump();
    } catch (const ProjectSettingsError &) {
        throw;
    } catch (const json::exception &error) {
        invalid(std::string("invalid material selection JSON: ") + error.what());
    } catch (const std::filesystem::filesystem_error &error) {
        invalid(std::string("native material selection file access failed: ") + error.what());
    }
}

std::string metadata_target_catalog(const std::filesystem::path &data_root) {
    const auto index = read_json(safe_path(data_root, "translations/supported-targets.json"),
                                 "supported targets");
    json targets = json::array();
    for (const auto &target : required_array(index, "targets", "supported targets")) {
        targets.push_back({
            {"slicer_id", required_string(target, "slicer_id", "target")},
            {"application_version", required_string(target, "application_version", "target")},
        });
    }
    return json{{"schema_version", 1}, {"targets", std::move(targets)}}.dump();
}

std::string native_project_options(std::string_view request_json,
                                   const std::filesystem::path &data_root) {
    try {
        const auto request = json::parse(request_json);
        if (!request.is_object()) invalid("options request must be a JSON object");
        const auto slicer = required_string(request, "slicer_id", "options request");
        const auto version = required_string(request, "application_version", "options request");
        const auto machine_uid = required_string(request, "machine_uid", "options request");
        const auto nozzle_uid = required_string(request, "nozzle_uid", "options request");
        const auto target = json::parse(metadata_target_data(slicer, version, data_root));
        if (detail::prusa::is_target(target)) {
            return detail::prusa::options(request, target, data_root / "native_project_sources").dump();
        }
        const auto canonical = read_json(safe_path(data_root, "translations/canonical.json"),
                                         "canonical data");
        NativeProfileCatalog catalog(data_root / "native_project_sources");
        const auto matches = matching_source_rows(catalog.index(), slicer, version,
                                                  machine_uid, nozzle_uid);
        if (matches.size() != 1) invalid("no unique native source for requested machine/nozzle");
        const auto &source = *matches.front();
        const auto machine_name = required_string(source, "source_machine_profile_name", "native source");
        require_target_machine_binding(canonical, target, slicer, version, machine_uid,
                                       nozzle_uid, machine_name);

        auto plates = offered_build_plates(canonical, target, machine_uid);
        json selected_plate;
        if (request.contains("build_plate_uid")) {
            const auto uid = required_string(request, "build_plate_uid", "options request");
            if (!contains_plate(plates, uid)) invalid("build_plate_uid is not supported by the requested target/machine");
            selected_plate = detail::resolve_source_plate(json::object(), request, target);
        }
        auto processes = process_choices(source, catalog);
        auto materials = material_choices(source, target, plates, selected_plate, catalog, machine_name);
        json result = {{"schema_version", 1}, {"slicer_id", slicer},
            {"application_version", version}, {"machine_uid", machine_uid},
            {"nozzle_uid", nozzle_uid}, {"source_machine_profile_name", machine_name},
            {"availability", source.at("availability")},
            {"process_settings_contract", target.at("process_settings_contract")},
            {"build_plates", std::move(plates)},
            {"print_profiles", std::move(processes)}, {"filament_profiles", std::move(materials)},
            {"default_print_profile_name", source.value("default_print_profile_name", std::string())},
            {"default_filament_profile_names", source.value("default_filament_profile_names", json::array())}};
        if (source.contains("compatibility_project")) {
            const auto &record = source.at("compatibility_project");
            result["compatibility_source"] = {
                {"source_slicer_id", record.at("source_slicer_id")},
                {"source_application_version", record.at("source_application_version")},
                {"source_profile_name", record.at("source_profile_name")}};
        }
        return result.dump();
    } catch (const ProjectSettingsError &) {
        throw;
    } catch (const json::exception &error) {
        invalid(std::string("invalid native options JSON: ") + error.what());
    } catch (const std::filesystem::filesystem_error &error) {
        invalid(std::string("native options file access failed: ") + error.what());
    }
}

std::string native_project_source(std::string_view request_json,
                                  const std::filesystem::path &data_root) {
    try {
        const auto request = json::parse(request_json);
        const auto slicer = required_string(request, "slicer_id", "native source request");
        const auto version = required_string(request, "application_version", "native source request");
        const auto nozzle = required_string(request, "nozzle_uid", "native source request");
        const bool by_uid = request.contains("machine_uid");
        const bool by_name = request.contains("source_machine_profile_name");
        if (by_uid == by_name) {
            invalid("native source request requires exactly one machine_uid or source_machine_profile_name");
        }
        const char *selector = by_uid ? "machine_uid" : "source_machine_profile_name";
        const auto identity = required_string(request, selector, "native source request");
        auto matches = [&](const json &source) {
            return source.is_object() && source.value(selector, std::string()) == identity &&
                source.value("nozzle_uid", std::string()) == nozzle;
        };
        if (slicer == "PrusaSlicer") {
            if (version != "3.0.0-alpha12") invalid("unsupported PrusaSlicer version");
            const auto catalog = detail::prusa::catalog(data_root);
            json selected;
            for (const auto &source : catalog.at("sources")) {
                if (!matches(source)) continue;
                if (!selected.is_null()) invalid("no unique native source for requested machine/nozzle");
                selected = source;
            }
            if (selected.is_null()) invalid("no unique native source for requested machine/nozzle");
            selected["filament_slot_policy"] = catalog.at("filament_slot_policy");
            return selected.dump();
        }
        NativeProfileCatalog catalog(data_root);
        const json *selected = nullptr;
        for (const auto &source : catalog.index().at("sources")) {
            if (!matches(source) || source.value("slicer_id", std::string()) != slicer ||
                source.value("application_version", std::string()) != version) continue;
            if (selected) invalid("no unique native source for requested machine/nozzle");
            selected = &source;
        }
        if (!selected) invalid("no unique native source for requested machine/nozzle");
        return public_source_record(*selected, catalog).dump();
    } catch (const ProjectSettingsError &) {
        throw;
    } catch (const json::exception &error) {
        invalid(std::string("invalid native source request JSON: ") + error.what());
    } catch (const std::filesystem::filesystem_error &error) {
        invalid(std::string("native source file access failed: ") + error.what());
    }
}

std::string native_project_source_catalog(
    std::string_view slicer_id,
    std::string_view application_version,
    const std::filesystem::path &data_root) {
    try {
        if (slicer_id == "PrusaSlicer") {
            if (application_version != "3.0.0-alpha12") invalid("unsupported PrusaSlicer version");
            return detail::prusa::catalog(data_root).dump();
        }
        NativeProfileCatalog catalog(data_root);
        const auto &index = catalog.index();
        json sources = json::array();
        for (const auto &source : index.at("sources")) {
            if (!source.is_object() ||
                source.value("slicer_id", std::string()) != slicer_id ||
                source.value("application_version", std::string()) != application_version) {
                continue;
            }
            sources.push_back(public_source_record(source, catalog));
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
        if (detail::prusa::is_target(target)) {
            if (request.contains("source_materials") && !request.at("source_materials").empty()) {
                const auto selection = resolve_prusa_materials(request, target, data_root);
                if (!selection.at("ready").get<bool>()) {
                    throw ProjectSettingsError(
                        selection.at("issues").front().at("message").get<std::string>(), selection.dump());
                }
            }
            return detail::prusa::compose_builtin(request, target, data_root).dump();
        }
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

std::string metadata_target_data(std::string_view slicer_id,
                                 std::string_view application_version,
                                 const std::filesystem::path &data_root) {
    const auto index = read_json(safe_path(data_root, "translations/supported-targets.json"),
                                 "supported targets");
    json selected;
    for (const auto &entry : index.at("targets")) {
        if (entry.at("slicer_id") != std::string(slicer_id) ||
            (!application_version.empty() && entry.at("application_version") != std::string(application_version))) continue;
        if (!selected.is_null()) invalid("packaged slicer/version selection is not unique");
        selected = entry;
    }
    if (selected.is_null()) invalid("unsupported metadata target version: " + std::string(slicer_id) + " " + std::string(application_version));
    const auto target = read_json(safe_path(data_root,
        "translations/targets/" + required_string(selected, "filename", "supported target")), "packaged target");
    const auto &contract = target.at("target_contract");
    if (contract.at("slicer_id") != selected.at("slicer_id") ||
        contract.at("application_version") != selected.at("application_version")) {
        invalid("packaged target identity does not match its index entry");
    }
    return target.dump();
}

std::string native_field_inventory(std::string_view slicer_id,
                                    std::string_view application_version,
                                    const std::filesystem::path &data_root) {
    const auto target = json::parse(metadata_target_data(slicer_id, application_version, data_root));
    const auto &identity = target.at("target_contract");
    const auto &contract = target.at("process_settings_contract");
    if (detail::prusa::is_target(target)) {
        const auto schema = read_json(data_root / "native_project_sources/prusa-3/schema.json", "Prusa native schema");
        json fields = json::object();
        std::size_t supported = 0;
        for (const auto *group : {"print", "printer", "filament"}) {
            for (const auto &definition : schema.at(group).at("items")) {
                const auto name = definition.at("name").get<std::string>();
                const bool accepted = std::string(group) == "print" && contract.at("fields").contains(name);
                if (accepted) ++supported;
                fields[std::string(group) + "." + name] = {
                    {"definitions", json::array({definition})},
                    {"profile_groups", json::array({group})},
                    {"process_override_status", accepted ? "supported" : "sdk_not_supported"}};
                if (std::string(group) == "filament") {
                    fields[std::string(group) + "." + name]["material_override_status"] =
                        target.at("material_settings_contract").at("fields").contains(name) ? "supported" : "sdk_not_supported";
                }
            }
        }
        return json{{"slicer_id", identity.at("slicer_id")},
            {"application_version", identity.at("application_version")},
            {"source", contract.at("source")}, {"fields", fields},
            {"process_settings_contract", contract},
            {"material_settings_contract", target.at("material_settings_contract")},
            {"summary", {{"observed_native_fields", fields.size()},
                {"supported", supported}, {"sdk_not_supported", fields.size() - supported}, {"unverified", 0}}}}.dump();
    }
    const auto index = read_json(safe_path(data_root, "translations/supported-targets.json"),
                                 "supported targets");
    json result;
    for (const auto &row : index.at("targets")) {
        if (row.at("slicer_id") == identity.at("slicer_id") &&
            row.at("application_version") == identity.at("application_version")) {
            result = read_json(safe_path(data_root, "translations/native-fields/" +
                required_string(row, "filename", "target")), "native field inventory");
            break;
        }
    }
    if (result.is_null()) invalid("native field inventory is unavailable for selected target");
    if (result.at("slicer_id") != identity.at("slicer_id") ||
        result.at("application_version") != identity.at("application_version") ||
        result.at("source") != contract.at("source")) {
        invalid("native field inventory provenance differs from process contract; refresh both together");
    }
    json summary = {{"observed_native_fields", result.at("fields").size()},
        {"supported", 0}, {"sdk_not_supported", 0}, {"unverified", 0},
        {"process_profile_observed_fields", 0}, {"process_profile_supported_fields", 0}};
    for (auto &[key, field] : result.at("fields").items()) {
        bool native_present = field.contains("native_export_shape");
        if (contract.at("source").value("exact_application_definitions", false) &&
            field.contains("definitions")) {
            for (const auto &definition : field.at("definitions")) {
                if (!definition.contains("conditions")) native_present = true;
            }
        }
        const bool supported = contract.at("fields").contains(key);
        const std::string status = supported ? "supported" :
            native_present ? "sdk_not_supported" : "unverified";
        field["process_override_status"] = status;
        summary[status] = summary.at(status).get<std::size_t>() + 1;
        const auto groups = field.value("profile_groups", json::array());
        if (std::find(groups.begin(), groups.end(), json("process")) != groups.end()) {
            summary["process_profile_observed_fields"] =
                summary.at("process_profile_observed_fields").get<std::size_t>() + 1;
            if (supported) summary["process_profile_supported_fields"] =
                summary.at("process_profile_supported_fields").get<std::size_t>() + 1;
        }
    }
    result["summary"] = std::move(summary);
    result["process_settings_contract"] = contract;
    result["input_aliases"] = json::object();
    for (const auto &[alias, key] : contract.at("aliases").items()) {
        result["input_aliases"][alias] = {{"status", "converted"}, {"native_field", key}};
    }
    result["status_definitions"] = {
        {"supported", "Accepted native field in FatCat's explicit process override API"},
        {"converted", "Reviewed input alias or value mapping in process_settings_contract"},
        {"sdk_not_supported", "Native evidence exists, but this process override API does not accept the field; other FatCat APIs may own it"},
        {"target_not_supported", "Used only for a canonical enum token excluded by an exact native parse map; legacy replacements and feature interactions remain outside scope"},
        {"unverified", "Native support is not established for this version; absent keys have this status too"}};
    return result.dump();
}

std::string compose_builtin_project_settings(std::string_view request_json,
                                             const std::filesystem::path &data_root) {
    const auto request = json::parse(request_json);
    const auto target = metadata_target_data(request.at("slicer_id").get<std::string>(),
        request.at("application_version").get<std::string>(), data_root);
    return compose_builtin_project_settings(request_json,
        read_text(safe_path(data_root, "translations/canonical.json")), target,
        data_root / "native_project_sources");
}

std::string compose_project_settings_from_data(std::string_view project_json,
    std::string_view request_json, const std::filesystem::path &data_root) {
    auto request = json::parse(request_json);
    const auto target_text = metadata_target_data(request.at("slicer_id").get<std::string>(),
        request.at("application_version").get<std::string>(), data_root);
    const auto target = json::parse(target_text);
    if (detail::prusa::is_target(target)) {
        return detail::prusa::compose(json::parse(project_json), request, target).dump();
    }
    if (request.contains("merge_sources") && request.contains("source_materials")) {
        const auto &palette = required_array(request, "source_materials", "merged request");
        std::set<std::size_t> present;
        std::set<std::string> source_ids;
        for (const auto &source : request.at("merge_sources")) {
            source_ids.insert(source.at("source_id").get<std::string>());
            for (const auto &slot : source.at("slots")) {
                const auto id = slot.at("source_slot_id").get<std::size_t>();
                if (id >= palette.size()) invalid("merged source slot is outside the selected palette");
                present.insert(id);
            }
        }
        bool missing_pair = false;
        for (const auto row : present) {
            for (const auto column : present) {
                bool supplied = false;
                for (const auto &source : request.at("merge_sources")) {
                    bool has_row = false, has_column = false;
                    for (const auto &slot : source.at("slots")) {
                        has_row = has_row || slot.at("source_slot_id") == row;
                        has_column = has_column || slot.at("source_slot_id") == column;
                    }
                    supplied = supplied || (has_row && has_column);
                }
                missing_pair = missing_pair || !supplied;
            }
        }
        const bool native_transition_defaults =
            target.at("package_dialect").contains("flush_volume_defaults");
        if (present.size() != palette.size() ||
            (missing_pair && !native_transition_defaults)) {
            // Complete absent selections with the same packaged native-source
            // synthesis used by ordinary exports. Actual source slots remain
            // authoritative; the carrier has no geometry and retains the
            // source process instead of introducing a replacement process.
            json native_request = {
                {"project_source", "fatcat_native"},
                {"slicer_id", request.at("slicer_id")},
                {"application_version", request.at("application_version")},
                {"machine_uid", request.at("machine_uid")},
                {"nozzle_uid", request.at("nozzle_uid")},
                {"source_materials", palette}, {"filament_slot_mode", "compact"}
            };
            if (request.contains("build_plate_uid")) native_request["build_plate_uid"] = request.at("build_plate_uid");
            const auto native_result = json::parse(compose_builtin_project_settings(native_request.dump(), data_root));
            const auto defaults = json::parse(native_result.at("project_settings_json").get<std::string>());
            const auto first = json::parse(project_json);
            for (const auto *key : {"printer_model", "nozzle_diameter"}) {
                if (first.at(key) != defaults.at(key)) invalid("missing palette completion hardware differs from source");
            }
            const auto &snapshot = target.at("package_dialect").at("filament_snapshot");
            const auto normalized = detail::prepare_source_merge_project(first, snapshot);
            // Retain the source process and hardware, including unknown fields.
            // Only fields declared by the target as material-slot data come
            // from the native defaults; this is not another process authority.
            json carrier = normalized;
            const auto merge_dialect = detail::source_merge_dialect(
                target.at("package_dialect"), normalized);
            for (const auto &rule : merge_dialect.at("filament_snapshot").at("array_rules")) {
                for (const auto &key_value : rule.at("keys")) {
                    const auto key = key_value.get<std::string>();
                    if (defaults.contains(key)) carrier[key] = defaults.at(key);
                    else carrier.erase(key);
                }
            }
            const auto difference_key = snapshot.at("difference_list_key").get<std::string>();
            if (defaults.contains(difference_key)) carrier[difference_key] = defaults.at(difference_key);
            const auto offset = snapshot.at("filament_slot_offset").get<std::size_t>();
            const auto tail = snapshot.at("trailing_entry_count").get<std::size_t>();
            if (normalized.contains(difference_key) && carrier.contains(difference_key)) {
                auto &entries = carrier.at(difference_key);
                const auto &original = normalized.at(difference_key);
                for (std::size_t i = 0; i < offset; ++i) entries.at(i) = original.at(i);
                for (std::size_t i = 0; i < tail; ++i) entries.at(entries.size() - tail + i) = original.at(original.size() - tail + i);
            }
            if (first.contains("inherits_group")) {
                const auto &original = first.at("inherits_group");
                if (!original.is_array() || original.size() != first.at("filament_settings_id").size() + 2) {
                    invalid("source inherits_group does not match its material slots");
                }
                json inheritance = json::array({original.front()});
                for (const auto &material : defaults.at("filament_settings_id")) inheritance.push_back(material);
                inheritance.push_back(original.back());
                carrier["inherits_group"] = std::move(inheritance);
            }
            std::string id = "native-material-defaults";
            while (source_ids.count(id)) id += "-";
            json slots = json::array();
            for (std::size_t index = 0; index < palette.size(); ++index) {
                if (present.count(index)) continue;
                slots.push_back({{"source_slot_id", index}, {"source_slot_index", index},
                    {"slot_name", palette.at(index).value("name", std::string())},
                    {"preview_color", palette.at(index).at("colour")},
                    {"material_id", defaults.at("filament_settings_id").at(index)}});
            }
            if (!slots.empty()) request["merge_sources"].push_back({{"source_id", id}, {"project_settings", carrier}, {"slots", slots}});
            // Native transition defaults supply only pairs absent from every
            // source, while source transition values and conflicts survive.
            if (!native_transition_defaults) request["merge_default_project"] = defaults;
        }
    }
    return compose_project_settings(project_json, request.dump(),
        read_text(safe_path(data_root, "translations/canonical.json")), target_text);
}

std::string compose_model_metadata_from_data(std::string_view project_json,
    std::string_view request_json, const std::filesystem::path &data_root) {
    const auto request = json::parse(request_json);
    return compose_model_metadata(project_json, request_json,
        metadata_target_data(request.at("slicer_id").get<std::string>(),
            request.at("application_version").get<std::string>(), data_root));
}

}  // namespace fatcat
