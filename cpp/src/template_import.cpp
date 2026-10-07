#include "fatcat/template_import.h"
#include "source_identity.h"
#include "prusa_project.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <tinyxml2.h>

namespace fatcat {
namespace {

using json = nlohmann::json;

struct Marker {
    std::string name;
    std::string value;
};

struct Source {
    std::string slicer;
    std::optional<std::string> version;
};

[[noreturn]] void invalid(const std::string &message) {
    throw TemplateImportError(message);
}

json parse_json(std::string_view text, const char *error_message) {
    try {
        return json::parse(text.begin(), text.end());
    } catch (const json::exception &) {
        invalid(error_message);
    }
}

const json &required_object(const json &object, const char *key) {
    if (!object.is_object() || !object.contains(key) ||
        !object.at(key).is_object()) {
        invalid(std::string("Fat Cat target is missing ") + key);
    }
    return object.at(key);
}

std::string required_text(const json &object, const char *key) {
    if (!object.is_object() || !object.contains(key) ||
        !object.at(key).is_string()) {
        invalid(std::string("Fat Cat target is missing ") + key);
    }
    return object.at(key).get<std::string>();
}

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

bool contains(std::string_view text, std::string_view needle) {
    return lower(text).find(lower(needle)) != std::string::npos;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           lower(text.substr(0, prefix.size())) == lower(prefix);
}

std::string_view local_name(std::string_view name) {
    const auto separator = name.find_last_of(':');
    return separator == std::string_view::npos ? name : name.substr(separator + 1);
}

void collect_markers(const tinyxml2::XMLElement *element,
                     std::vector<Marker> &markers, std::string &marker_text) {
    for (auto *current = element; current != nullptr;
         current = current->NextSiblingElement()) {
        const auto name = local_name(current->Name());
        if (name == "metadata" || name == "header_item") {
            const char *marker_name = current->Attribute("name");
            if (marker_name == nullptr) {
                marker_name = current->Attribute("key");
            }
            const char *marker_value = current->Attribute("value");
            if (marker_value == nullptr) {
                marker_value = current->GetText();
            }
            markers.push_back({marker_name == nullptr ? "" : marker_name,
                               marker_value == nullptr ? "" : marker_value});
            for (const auto *attribute = current->FirstAttribute();
                 attribute != nullptr; attribute = attribute->Next()) {
                marker_text += " ";
                marker_text += attribute->Value();
            }
            if (const char *text = current->GetText(); text != nullptr) {
                marker_text += " ";
                marker_text += text;
            }
        }
        collect_markers(current->FirstChildElement(), markers, marker_text);
    }
}

void read_markers(std::optional<std::string_view> xml,
                  std::vector<Marker> &markers, std::string &marker_text) {
    tinyxml2::XMLDocument document;
    collect_markers(detail::read_source_xml(document, xml), markers, marker_text);
}

std::optional<std::string> declared_version(
    const std::vector<Marker> &markers, const json &target) {
    const auto &identity = required_object(
        required_object(target, "package_dialect"), "source_identity");
    const auto &contract = required_object(target, "target_contract");
    const auto slicer = required_text(contract, "slicer_id");
    const auto identity_name = required_text(identity, "name");
    const auto identity_value = required_text(identity, "value");
    const auto target_version = required_text(contract, "application_version");
    const auto application_prefix = slicer + "-";

    for (const auto &marker : markers) {
        if (lower(marker.name) == lower(identity_name) && !marker.value.empty()) {
            if (slicer == "OrcaSlicer") {
                return marker.value;
            }
            if (slicer == "ElegooSlicer" &&
                starts_with(marker.value, application_prefix) &&
                marker.value.size() > application_prefix.size()) {
                return marker.value.substr(application_prefix.size());
            }
            if (identity_value.size() > target_version.size()) {
                const auto prefix = identity_value.substr(
                    0, identity_value.size() - target_version.size());
                if (starts_with(marker.value, prefix)) {
                    return marker.value.substr(prefix.size());
                }
            }
        }
    }
    if (slicer == "OrcaSlicer") {
        for (const auto &marker : markers) {
            if (lower(marker.name) == "orcaslicer-version" &&
                !marker.value.empty()) {
                return marker.value;
            }
        }
    }
    if (slicer == "QIDIStudio") {
        for (const auto &marker : markers) {
            if (lower(marker.name) == "x-qdt-client-version" &&
                !marker.value.empty()) {
                return marker.value;
            }
        }
    }
    if (slicer == "ElegooSlicer") {
        const auto elegoo_client = std::any_of(
            markers.begin(), markers.end(), [](const Marker &marker) {
                return lower(marker.name) == "x-bbl-client-name" &&
                       lower(marker.value) == "elegooslicer";
            });
        if (elegoo_client) {
            for (const auto &marker : markers) {
                if (lower(marker.name) == "x-bbl-client-version" &&
                    !marker.value.empty()) {
                    return marker.value;
                }
            }
        }
    }
    if (slicer == "AnycubicSlicerNext") {
        for (const auto &marker : markers) {
            if (lower(marker.name) == "application" &&
                starts_with(marker.value, "ACNext-")) {
                return marker.value.substr(std::string_view("ACNext-").size());
            }
        }
        for (const auto &marker : markers) {
            if (lower(marker.name) == "x-acnext-client-version" &&
                !marker.value.empty()) {
                return marker.value;
            }
        }
    }
    for (const auto &marker : markers) {
        if (starts_with(marker.value, application_prefix)) {
            return marker.value.substr(application_prefix.size());
        }
    }
    return std::nullopt;
}

std::string source_slicer(std::string_view marker_text) {
    if (contains(marker_text, "PrusaSlicer-3.")) return "PrusaSlicer";
    if (contains(marker_text, "Snapmaker")) {
        return "SnapmakerOrca";
    }
    for (const auto *flash_marker : {"FlashStudio", "Flash Studio", "Flashforge"}) {
        if (contains(marker_text, flash_marker)) {
            return "FlashStudio";
        }
    }
    if (contains(marker_text, "Anycubic") || contains(marker_text, "ACNext")) {
        return "AnycubicSlicerNext";
    }
    if (contains(marker_text, "Elegoo")) {
        return "ElegooSlicer";
    }
    if (contains(marker_text, "QIDI") || contains(marker_text, "X-QDT")) {
        return "QIDIStudio";
    }
    if (contains(marker_text, "OrcaSlicer")) {
        return "OrcaSlicer";
    }
    if (contains(marker_text, "BambuStudio") || contains(marker_text, "Bambu Studio")) {
        return "BambuStudio";
    }
    return {};
}

Source identify_source(const std::vector<Marker> &markers,
                       const std::string &marker_text,
                       const std::vector<json> &targets) {
    const auto slicer = source_slicer(marker_text);
    if (slicer.empty()) {
        return {};
    }
    for (const auto &target : targets) {
        if (required_text(required_object(target, "target_contract"), "slicer_id") == slicer) {
            return {slicer, declared_version(markers, target)};
        }
    }
    for (const auto &marker : markers) {
        const auto prefix = slicer + "-";
        if (starts_with(marker.value, prefix)) {
            return {slicer, marker.value.substr(prefix.size())};
        }
        if (slicer == "OrcaSlicer" &&
            (lower(marker.name) == "orcaslicer" || lower(marker.name) == "orcaslicer-version")) {
            return {slicer, marker.value};
        }
    }
    return {slicer, std::nullopt};
}

bool matches_target_identity(const std::vector<Marker> &root_markers,
                             const std::vector<Marker> &headers,
                             const json &target) {
    const auto &dialect = required_object(target, "package_dialect");
    bool application_matches = false;
    for (const auto &expected : dialect.value("root_metadata", json::array())) {
        const auto name = required_text(expected, "name");
        const auto value = required_text(expected, "value");
        if (name != "Application" && name != "OrcaSlicer" &&
            name.find(":3mfVersion") == std::string::npos) {
            continue;
        }
        for (const auto &actual : root_markers) {
            if (actual.name != name) {
                continue;
            }
            if (actual.value != value) {
                return false;
            }
            if (name == "Application") {
                application_matches = true;
            }
        }
    }
    if (!application_matches) {
        return false;
    }
    for (const auto &expected : dialect.value("slice_headers", json::array())) {
        const auto name = required_text(expected, "name");
        const auto value = required_text(expected, "value");
        if (value.empty()) {
            continue;
        }
        for (const auto &actual : headers) {
            if (actual.name == name && actual.value != value) {
                return false;
            }
        }
    }
    return true;
}

bool truthy(const json &value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number()) {
        return value != 0;
    }
    if (value.is_string()) {
        return !value.get_ref<const std::string &>().empty();
    }
    return !value.empty();
}

std::optional<std::string> plate_value(
    std::optional<std::string_view> model_settings_xml) {
    if (!model_settings_xml.has_value()) {
        return std::nullopt;
    }
    tinyxml2::XMLDocument document;
    if (document.Parse(model_settings_xml->data(), model_settings_xml->size()) !=
            tinyxml2::XML_SUCCESS || document.RootElement() == nullptr) {
        invalid("Import a project 3MF containing printer and material settings, not a geometry-only 3MF");
    }
    std::optional<std::string> value;
    int plate_count = 0;
    for (const auto *plate = document.RootElement()->FirstChildElement();
         plate != nullptr; plate = plate->NextSiblingElement()) {
        if (local_name(plate->Name()) != "plate") {
            continue;
        }
        ++plate_count;
        if (plate_count > 1) {
            invalid("Save a single-plate project as the custom template");
        }
        for (const auto *item = plate->FirstChildElement(); item != nullptr;
             item = item->NextSiblingElement()) {
            const char *key = item->Attribute("key");
            if (key != nullptr && std::string_view(key) == "bed_type") {
                const char *bed = item->Attribute("value");
                value = bed == nullptr ? "" : bed;
            }
        }
    }
    return value;
}

std::optional<std::string> sidecar_bed(
    std::optional<std::string_view> plate_sidecar_json) {
    if (!plate_sidecar_json.has_value()) {
        return std::nullopt;
    }
    const auto sidecar = parse_json(
        *plate_sidecar_json,
        "Import a project 3MF containing printer and material settings, not a geometry-only 3MF");
    if (sidecar.is_object() && sidecar.contains("bed_type") &&
        sidecar.at("bed_type").is_string()) {
        return sidecar.at("bed_type").get<std::string>();
    }
    return std::nullopt;
}

}  // namespace

json detail::read_source_identity(const tinyxml2::XMLElement *source_model,
                                 const tinyxml2::XMLElement *slice_info,
                                 const json &target) {
    std::vector<Marker> root_markers, headers;
    std::string marker_text;
    collect_markers(source_model, root_markers, marker_text);
    collect_markers(slice_info, headers, marker_text);
    auto markers = root_markers;
    markers.insert(markers.end(), headers.begin(), headers.end());
    const auto source = identify_source(markers, marker_text, {target});
    return {{"source_slicer", source.slicer.empty() ? json(nullptr) : json(source.slicer)},
            {"source_version", source.version.has_value() ? json(*source.version) : json(nullptr)},
            {"matches_selected_target_identity", matches_target_identity(root_markers, headers, target)}};
}

std::string template_import_parts(std::string_view target_json) {
    const auto target = parse_json(target_json, "Fat Cat target is invalid JSON");
    const auto &dialect = required_object(target, "package_dialect");
    if (detail::prusa::is_target(target)) {
        return json{{"source_model", "3D/3dmodel.model"},
            {"project_settings", "Metadata/PrusaSlicer3_project.json"},
            {"model_settings", "Metadata/PrusaSlicer3_project.json"},
            {"slice_info", ""}, {"plate_sidecar", ""},
            {"wipe_tower_placement", "Metadata/wipe_tower_placement.json"}}.dump();
    }
    const auto &settings = required_object(dialect, "settings_parts");
    const auto &relationships = required_object(
        required_object(dialect, "metadata_components"), "model_relationships");
    const auto &placement = required_object(
        required_object(dialect, "metadata_components"), "wipe_tower_placement");
    return json{{"source_model", required_text(relationships, "source")},
                {"project_settings", required_text(settings, "project_settings")},
                {"model_settings", required_text(settings, "model_settings")},
                {"slice_info", required_text(settings, "slice_info")},
                {"plate_sidecar", required_text(settings, "plate_sidecar")},
                {"wipe_tower_placement", required_text(placement, "path")}}
        .dump();
}

std::string resolve_template_build_plate(
    std::string_view target_json,
    std::string_view default_build_plate_uid,
    std::optional<std::string_view> model_plate_value,
    std::optional<std::string_view> sidecar_bed_value,
    std::optional<std::string_view> project_bed_value) {
    const auto target = parse_json(target_json, "Fat Cat target is invalid JSON");
    if (detail::prusa::is_target(target)) {
        if (!project_bed_value.has_value() || project_bed_value->empty()) {
            invalid("Prusa sheet resolution requires the native project's sheet type");
        }
        const auto sheet = std::string(*project_bed_value);
        const auto uid = "prusa-sheet:" + sheet;
        if (!default_build_plate_uid.empty() && default_build_plate_uid != uid) {
            invalid("Prusa template sheet differs from the selected native sheet");
        }
        return json{{"build_plate_uid", uid}, {"project_value", sheet},
            {"plate_value", sheet}, {"sidecar_value", sheet}}.dump();
    }
    const auto &bindings = target.at("build_plate_bindings");
    const auto result = [](const json &binding) {
        return json{{"build_plate_uid", required_text(binding, "build_plate_uid")},
                    {"project_value", required_text(binding, "project_value")},
                    {"plate_value", required_text(binding, "plate_value")},
                    {"sidecar_value", required_text(binding, "sidecar_value")}}
            .dump();
    };
    for (const auto &[value, key] : {
             std::pair{model_plate_value, "plate_value"},
             std::pair{sidecar_bed_value, "sidecar_value"},
             std::pair{project_bed_value, "project_value"}}) {
        if (!value.has_value() || value->empty() || *value == "Default") {
            continue;
        }
        for (const auto &binding : bindings) {
            if (required_text(binding, key) == *value) {
                return result(binding);
            }
        }
        invalid("Custom 3MF template has unsupported build plate '" +
                std::string(*value) + "'");
    }
    for (const auto &binding : bindings) {
        const bool matches_default = default_build_plate_uid.empty()
            ? required_text(binding, "project_value") == "Textured PEI Plate"
            : required_text(binding, "build_plate_uid") == default_build_plate_uid;
        if (matches_default) {
            return result(binding);
        }
    }
    invalid("Fat Cat target is missing its default build plate");
}

void validate_template_hardware(std::string_view template_json,
                                std::string_view expected_project_json,
                                std::string_view source_slicer,
                                std::string_view selected_slicer) {
    const auto project = parse_json(template_json, "Custom 3MF template is invalid JSON");
    const auto expected = parse_json(expected_project_json, "Expected project is invalid JSON");
    if (source_slicer != selected_slicer) {
        invalid("Custom 3MF template slicer does not match the selected slicer");
    }
    if (selected_slicer == "PrusaSlicer") {
        const auto actual_project = detail::prusa::normalized_project(project);
        const auto wanted_project = detail::prusa::normalized_project(expected);
        const auto &actual = actual_project.at("config_containers").at(0).at("preset").at("hw_config");
        const auto &wanted = wanted_project.at("config_containers").at(0).at("preset").at("hw_config");
        // Material assignments and instance UUIDs are not hardware identities.
        const auto hardware = [](const json &config) {
            json tools = json::object();
            for (std::size_t i = 0; i < config.at("tool_count").get<std::size_t>(); ++i) {
                const auto id = std::to_string(i);
                const auto &tool = config.at("tools").at(id);
                tools[id] = {{"id", tool.at("id")}, {"type", tool.at("type")},
                    {"features", tool.at("features")}, {"feeder", tool.value("feeder", json(nullptr))}};
            }
            return json{{"model", config.at("model")}, {"tool_count", config.at("tool_count")},
                {"tools", tools}};
        };
        if (hardware(actual) != hardware(wanted)) {
            invalid("Custom Prusa template hardware differs from selected native configuration");
        }
        return;
    }
    if (project.value("printer_model", json(nullptr)) != expected.value("printer_model", json(nullptr))) {
        invalid("Custom 3MF template printer does not match the selected printer");
    }
    if (project.value("nozzle_diameter", json(nullptr)) != expected.value("nozzle_diameter", json(nullptr))) {
        invalid("Custom 3MF template nozzles do not match the selected nozzles");
    }
}

std::string detect_template_source(
    std::optional<std::string_view> source_model_xml,
    std::optional<std::string_view> slice_info_xml,
    std::string_view bambu_target_json,
    std::string_view orca_target_json,
    std::string_view qidi_target_json,
    std::string_view elegoo_target_json,
    std::string_view anycubic_target_json,
    std::string_view snapmaker_target_json) {
    const auto bambu = parse_json(bambu_target_json, "Fat Cat target is invalid JSON");
    const auto orca = parse_json(orca_target_json, "Fat Cat target is invalid JSON");
    const auto qidi = parse_json(qidi_target_json, "Fat Cat target is invalid JSON");
    const auto elegoo = parse_json(elegoo_target_json, "Fat Cat target is invalid JSON");
    const auto anycubic = parse_json(anycubic_target_json, "Fat Cat target is invalid JSON");
    const auto snapmaker = parse_json(snapmaker_target_json, "Fat Cat target is invalid JSON");
    std::vector<Marker> markers;
    std::string marker_text;
    read_markers(source_model_xml, markers, marker_text);
    read_markers(slice_info_xml, markers, marker_text);
    const auto source = identify_source(markers, marker_text,
                                       {bambu, orca, qidi, elegoo, anycubic, snapmaker});
    return json{{"source_slicer", source.slicer.empty() ? json(nullptr)
                                                    : json(source.slicer)},
                {"source_version", source.version.has_value() ? json(*source.version)
                                                                : json(nullptr)}}
        .dump();
}

std::string detect_template_source(
    std::optional<std::string_view> source_model_xml,
    std::optional<std::string_view> slice_info_xml,
    std::string_view target_json) {
    const auto target = parse_json(target_json, "Fat Cat target is invalid JSON");
    tinyxml2::XMLDocument model_document, slice_document;
    const auto *model = detail::read_source_xml(model_document, source_model_xml);
    const auto *slice = detail::read_source_xml(slice_document, slice_info_xml);
    return detail::read_source_identity(model, slice, target).dump();
}

std::string import_template_metadata(
    std::string_view project_settings_json,
    std::optional<std::string_view> source_model_xml,
    std::optional<std::string_view> slice_info_xml,
    std::optional<std::string_view> model_settings_xml,
    std::optional<std::string_view> plate_sidecar_json,
    std::string_view selected_target_json,
    std::string_view bambu_target_json,
    std::string_view orca_target_json,
    std::string_view qidi_target_json,
    std::string_view elegoo_target_json,
    std::string_view anycubic_target_json,
    std::string_view snapmaker_target_json,
    bool explicit_slicer_hint) {
    const auto project = parse_json(
        project_settings_json,
        "Import a project 3MF containing printer and material settings, not a geometry-only 3MF");
    if (!project.is_object()) {
        invalid("3MF project settings must be an object");
    }
    const auto selected = parse_json(selected_target_json, "Fat Cat target is invalid JSON");
    if (detail::prusa::is_target(selected)) {
        const auto native = detail::prusa::normalized_project(project);
        const auto effective = detail::prusa::summary(native);
        tinyxml2::XMLDocument document;
        const auto *root = detail::read_source_xml(document, source_model_xml);
        const auto identity = detail::read_source_identity(root, nullptr, selected);
        if (identity.at("source_slicer") != "PrusaSlicer" ||
            identity.at("source_version") != "3.0.0-alpha12" ||
            !identity.at("matches_selected_target_identity").get<bool>()) invalid("Prusa template requires exact 3.0.0-alpha12 project identity");
        return json{{"settings", native}, {"source_slicer", "PrusaSlicer"},
            {"source_version", "3.0.0-alpha12"}, {"selected_slicer", "PrusaSlicer"},
            {"matches_selected_target_identity", true},
            {"printer_model", effective.at("printer_model")},
            {"nozzle_diameter", effective.at("nozzle_diameter")},
            {"filament_count", effective.at("filament_settings_id").size()},
            {"plate_value", effective.at("curr_bed_type")}, {"sidecar_bed", nullptr}}.dump();
    }
    const auto bambu = parse_json(bambu_target_json, "Fat Cat target is invalid JSON");
    const auto orca = parse_json(orca_target_json, "Fat Cat target is invalid JSON");
    const auto qidi = parse_json(qidi_target_json, "Fat Cat target is invalid JSON");
    const auto elegoo = parse_json(elegoo_target_json, "Fat Cat target is invalid JSON");
    const auto anycubic = parse_json(anycubic_target_json, "Fat Cat target is invalid JSON");
    const auto snapmaker = parse_json(snapmaker_target_json, "Fat Cat target is invalid JSON");
    std::vector<Marker> root_markers, headers;
    std::string marker_text;
    read_markers(source_model_xml, root_markers, marker_text);
    read_markers(slice_info_xml, headers, marker_text);
    auto markers = root_markers;
    markers.insert(markers.end(), headers.begin(), headers.end());
    const auto source = identify_source(markers, marker_text,
                                       {bambu, orca, qidi, elegoo, anycubic, snapmaker, selected});
    if (source.slicer.empty()) {
        invalid("Save the template as a project in a supported slicer first");
    }
    const auto selected_slicer = required_text(
        required_object(selected, "target_contract"), "slicer_id");
    const auto selected_identity_matches = matches_target_identity(
        root_markers, headers, selected);
    if (source.slicer != selected_slicer &&
        !(explicit_slicer_hint &&
          (selected_identity_matches || source.slicer == "BambuStudio"))) {
        invalid("Template source slicer does not match the selected slicer");
    }

    const auto plate = plate_value(model_settings_xml);
    const auto sidecar = sidecar_bed(plate_sidecar_json);
    for (const auto *key : {
             "printer_model", "printer_settings_id", "printable_area",
             "printable_height", "nozzle_diameter", "filament_settings_id",
             "filament_type", "filament_colour"}) {
        if (!project.contains(key) || !truthy(project.at(key))) {
            invalid(std::string("3MF template is missing ") + key +
                    "; save a complete slicer project");
        }
    }
    const auto &nozzles = project.at("nozzle_diameter");
    if (!nozzles.is_array() || nozzles.empty() ||
        !std::all_of(nozzles.begin(), nozzles.end(),
                     [](const json &value) { return value.is_string(); })) {
        invalid("Template nozzle_diameter must be a nonempty array");
    }
    const auto &slots = project.at("filament_settings_id");
    if (!slots.is_array() || slots.empty()) {
        invalid("Template must contain material slots");
    }
    for (const auto *key : {"filament_type", "filament_colour"}) {
        if (!project.at(key).is_array() || project.at(key).size() != slots.size()) {
            invalid(std::string("Template ") + key +
                    " must match its material slot count");
        }
    }
    return json{{"settings", project},
                {"source_slicer", source.slicer},
                {"source_version", source.version.has_value() ? json(*source.version)
                                                                : json(nullptr)},
                {"selected_slicer", selected_slicer},
                {"matches_selected_target_identity", selected_identity_matches},
                {"printer_model", project.at("printer_model")},
                {"nozzle_diameter", nozzles},
                {"filament_count", slots.size()},
                {"project_bed_type", project.value("curr_bed_type", json(nullptr))},
                {"plate_value", plate.has_value() ? json(*plate) : json(nullptr)},
                {"sidecar_bed", sidecar.has_value() ? json(*sidecar) : json(nullptr)}}
        .dump();
}

}  // namespace fatcat
