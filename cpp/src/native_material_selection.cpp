#include "native_material_selection.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <vector>

#include "fatcat/project_settings.h"
#include "fatcat/source_project_settings.h"

namespace fatcat::detail {
using json = nlohmann::json;

namespace {
[[noreturn]] void invalid(const std::string &message) {
    throw ProjectSettingsError(message);
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

template <typename Predicate>
std::optional<std::string> unique_profile_name(const std::vector<std::string> &names,
                                              Predicate matches) {
    std::optional<std::string> result;
    for (const auto &name : names) {
        if (!matches(name)) continue;
        if (result) return std::nullopt;
        result = name;
    }
    return result;
}

// Rank only already-compatible candidates; this never invents a preset.
std::optional<std::string> preferred_material_profile(
    const std::vector<std::string> &names, const std::string &requested_name,
    const std::string &requested_type, const std::string &native_default) {
    const auto exact = unique_profile_name(names, [&](const std::string &name) {
        return !requested_name.empty() && normalized_text(requested_name) == normalized_text(name);
    });
    if (exact) return exact;
    if (std::find(names.begin(), names.end(), native_default) != names.end()) return native_default;

    const auto default_alias = unique_profile_name(names, [&](const std::string &name) {
        return !native_default.empty() &&
            compact_text(normalized_text(name.substr(0, name.find('@')))) ==
            compact_text(normalized_text(native_default));
    });
    if (default_alias) return default_alias;

    const auto family = material_family(requested_type);
    if (requested_type == family || requested_type == family + " BASIC") {
        const auto basic = unique_profile_name(names, [&](const std::string &name) {
            return compact_text(normalized_text(name)).find(compact_text(family + " BASIC")) != std::string::npos;
        });
        if (basic) return basic;
        const auto generic = unique_profile_name(names, [&](const std::string &name) {
            return compact_text(normalized_text(name.substr(0, name.find('@')))) ==
                   compact_text(normalized_text("Generic " + family));
        });
        if (generic) return generic;
    }
    const auto requested_key = compact_text(requested_type);
    if (!requested_key.empty() && requested_type != family) {
        const auto variant = unique_profile_name(names, [&](const std::string &name) {
            return compact_text(normalized_text(name)).find(requested_key) != std::string::npos;
        });
        if (variant) return variant;
    }
    if (names.size() == 1) return names.front();
    return std::nullopt;
}

}  // namespace

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
    const auto profile_key = compact_text(normalized_text(profile_name));
    const auto requested_key = compact_text(requested);
    if (!requested_key.empty() && profile_key.find(requested_key) != std::string::npos) return true;
    return normalized_text(material_name) == normalized_text(profile_name);
}

bool material_compatible_with_machine(const json &profile, const std::string &machine_name) {
    const auto printers = profile.find("compatible_printers");
    if (printers == profile.end()) return true;
    if (!printers->is_array()) invalid("native compatible_printers must be an array");
    return printers->empty() || std::find(printers->begin(), printers->end(), machine_name) != printers->end();
}

bool material_supports_plate(const json &profile, const json &plate) {
    const auto temp = profile.find(plate.value("sidecar_value", std::string()) + "_temp");
    if (temp == profile.end() || !temp->is_array() || temp->empty()) return true;
    return !std::all_of(temp->begin(), temp->end(), [](const json &value) {
        return value == "0" || value == 0;
    });
}


namespace {

std::vector<std::string> explicit_material_profiles(
    const json &request, const json &target, const json &materials) {
    const bool names = request.contains("native_filament_profile_names");
    const bool uid = request.contains("material_uid");
    const bool uids = request.contains("material_uids");
    const bool per_slot = std::any_of(materials.begin(), materials.end(),
        [](const json &material) { return material.contains("native_filament_profile_name"); });
    if (static_cast<int>(names) + static_cast<int>(uid) + static_cast<int>(uids) +
            static_cast<int>(per_slot) > 1) {
        invalid("select native materials through one explicit identity field");
    }
    std::vector<std::string> selected(materials.size());
    if (names) {
        const auto &values = request.at("native_filament_profile_names");
        if (!values.is_array() || values.size() != materials.size()) {
            invalid("native_filament_profile_names must match source_materials slots");
        }
        for (std::size_t slot = 0; slot < selected.size(); ++slot) {
            if (!values.at(slot).is_string() || values.at(slot).get<std::string>().empty()) {
                invalid("native_filament_profile_names must contain non-empty preset names");
            }
            selected[slot] = values.at(slot).get<std::string>();
        }
    } else if (uid || uids) {
        const auto ids = uids ? request.at("material_uids") :
            json(std::vector<json>(materials.size(), request.at("material_uid")));
        if (!ids.is_array() || ids.size() != materials.size()) {
            invalid("material_uids must match source_materials slots");
        }
        const auto bindings = target.value("material_bindings", json::array());
        for (std::size_t slot = 0; slot < selected.size(); ++slot) {
            if (!ids.at(slot).is_string() || ids.at(slot).get<std::string>().empty()) {
                invalid("material_uid(s) must contain non-empty identities");
            }
            for (const auto &binding : bindings) {
                if (binding.value("machine_uid", std::string()) != request.at("machine_uid") ||
                    binding.value("nozzle_uid", std::string()) != request.at("nozzle_uid") ||
                    binding.value("material_uid", std::string()) != ids.at(slot)) continue;
                const auto name = binding.value("native_profile_id", std::string());
                if (!selected[slot].empty() && selected[slot] != name) {
                    invalid("target has ambiguous native material preset bindings");
                }
                selected[slot] = name;
            }
            if (selected[slot].empty()) invalid("target has no exact native material binding for '" +
                                               ids.at(slot).get<std::string>() + "'");
        }
    } else if (per_slot) {
        for (std::size_t slot = 0; slot < selected.size(); ++slot) {
            const auto name = materials.at(slot).find("native_filament_profile_name");
            if (name == materials.at(slot).end()) continue;
            if (!name->is_string() || name->get<std::string>().empty()) {
                invalid("native_filament_profile_name must contain a non-empty preset name");
            }
            selected[slot] = name->get<std::string>();
        }
    }
    return selected;
}

bool profile_supports_plate(const json &profile, const json &plate_uid) {
    const auto &plates = profile.at("supported_build_plate_uids");
    return std::find(plates.begin(), plates.end(), plate_uid) != plates.end();
}

json selection_issue(const std::string &code, std::size_t slot,
                     const std::string &name, const std::string &type,
                     const json &plate_uid, const std::string &message) {
    return {{"code", code}, {"material_index", slot}, {"material_name", name},
            {"material_type", type}, {"build_plate_uid", plate_uid}, {"message", message}};
}

}  // namespace

json resolve_native_material_selection(
    const json &request, const json &source, const json &target,
    const json &materials, const json &plate, const json &profiles,
    const json &build_plates) {
    if (!materials.is_array() || materials.empty()) {
        invalid("source_materials must contain at least one selected material");
    }
    const auto explicit_names = explicit_material_profiles(request, target, materials);
    auto defaults = source.value("default_filament_profile_names", json::array());
    if (defaults.empty() && source.contains("default_filament_profile_name")) {
        defaults.push_back(source.at("default_filament_profile_name"));
    }
    const auto plate_uid = plate.at("build_plate_uid");
    json result = {{"schema_version", 1}, {"slicer_id", request.at("slicer_id")},
        {"machine_uid", request.at("machine_uid")}, {"nozzle_uid", request.at("nozzle_uid")},
        {"build_plate_uid", plate_uid}, {"ready", true},
        {"materials", json::array()}, {"issues", json::array()},
        {"build_plates", build_plates}};
    std::vector<json> plate_candidates;

    for (std::size_t slot = 0; slot < materials.size(); ++slot) {
        const auto &material = materials.at(slot);
        const auto name = material.value("name", std::string());
        auto type = normalized_text(requested_material_type(material));
        const std::string native_default = defaults.empty() ? "" :
            defaults.at(slot < defaults.size() ? slot : 0).get<std::string>();
        const auto type_profile = explicit_names[slot].empty() ? native_default : explicit_names[slot];
        if (type.empty() && !type_profile.empty()) {
            for (const auto &profile : profiles) {
                if (profile.at("name") == type_profile) {
                    type = profile.value("material_type", std::string());
                    break;
                }
            }
        }

        json candidates = json::array();
        std::vector<std::string> available_names;
        const json *explicit_profile = nullptr;
        for (const auto &profile : profiles) {
            if (profile.at("name") == explicit_names[slot]) explicit_profile = &profile;
            if (!profile.value("machine_compatible", profile.value("available", false))) continue;
            const auto native_type = profile.value("material_type", std::string());
            if (native_type.empty() || (!type.empty() &&
                    !native_type_matches_request(type, native_type,
                        profile.at("name").get<std::string>(), name))) continue;
            json candidate = profile;
            candidate.erase("machine_compatible");
            candidate.erase("unavailable_reason");
            candidate["available"] = true;
            candidates.push_back(std::move(candidate));
            if (profile_supports_plate(profile, plate_uid)) {
                available_names.push_back(profile.at("name").get<std::string>());
            }
        }

        const bool explicit_matches = explicit_profile &&
            explicit_profile->value("machine_compatible", explicit_profile->value("available", false)) &&
            (type.empty() || native_type_matches_request(type,
                explicit_profile->value("material_type", std::string()), explicit_names[slot], name));
        std::string selected;
        json issue;
        if (!explicit_names[slot].empty()) {
            const auto requested = explicit_names[slot];
            if (!explicit_matches) {
                issue = selection_issue("native_material_selection_invalid", slot, name, type, plate_uid,
                    "Selected native filament profile '" + requested +
                    "' does not match this machine and material; select a compatible native preset.");
            } else if (!profile_supports_plate(*explicit_profile, plate_uid)) {
                issue = selection_issue("native_material_plate_incompatible", slot, name, type, plate_uid,
                    "Native filament profile '" + requested + "' does not support selected build plate '" +
                    plate.value("project_value", plate_uid.get<std::string>()) + "'.");
            } else {
                selected = requested;
            }
        } else if (available_names.empty()) {
            const bool plate_conflict = !candidates.empty();
            issue = selection_issue(
                plate_conflict ? "native_material_plate_incompatible" : "native_material_unavailable",
                slot, name, type, plate_uid,
                plate_conflict ?
                    "Source material slot " + std::to_string(slot) + " type '" + type +
                        "' does not support selected build plate '" +
                        plate.value("project_value", plate_uid.get<std::string>()) + "'." :
                    "no available native filament profile matches source material slot " +
                        std::to_string(slot) + " type '" + type +
                        "'; select a compatible native_filament_profile_names entry");
        } else {
            const auto preferred = preferred_material_profile(available_names, name, type, native_default);
            if (preferred) {
                selected = *preferred;
            } else {
                std::string choices;
                for (const auto &candidate : available_names) {
                    if (!choices.empty()) choices += ", ";
                    choices += candidate;
                }
                issue = selection_issue("native_material_ambiguous", slot, name, type, plate_uid,
                    "source material slot " + std::to_string(slot) + " type '" + type +
                    "' matches multiple native profiles (" + choices +
                    "); select native_filament_profile_names explicitly");
            }
        }

        json alternatives = candidates;
        if (explicit_matches) alternatives = json::array({*explicit_profile});
        plate_candidates.push_back(std::move(alternatives));
        json material_result = {{"name", name}, {"material_type", type},
            {"selected_profile_name", selected.empty() ? json(nullptr) : json(selected)},
            {"profiles", std::move(candidates)}};
        if (!issue.is_null()) {
            result["ready"] = false;
            result["issues"].push_back(issue);
        }
        result["materials"].push_back(std::move(material_result));
    }

    for (auto &candidate_plate : result["build_plates"]) {
        const auto uid = candidate_plate.at("build_plate_uid");
        candidate_plate["compatible"] = std::all_of(plate_candidates.begin(), plate_candidates.end(),
            [&](const json &candidates) {
                return std::any_of(candidates.begin(), candidates.end(),
                    [&](const json &profile) { return profile_supports_plate(profile, uid); });
            });
    }
    return result;
}

}  // namespace fatcat::detail
