#include "fatcat/metadata_components.h"
#include "supported_targets.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "json_object.h"
#include <tinyxml2.h>

#include "fatcat/wipe_tower.h"

namespace fatcat {
namespace {

using json = nlohmann::json;

[[noreturn]] void invalid(std::string message) {
    throw MetadataComponentsError(std::move(message));
}

const json &required_member(const json &object, std::string_view key,
                            std::string_view context) {
    if (!object.contains(key)) {
        invalid(std::string(context) + " is missing required field '" +
                std::string(key) + "'");
    }
    return object.at(key);
}

std::string required_text(const json &object, std::string_view key,
                          std::string_view context, bool allow_empty = false) {
    const json &value = required_member(object, key, context);
    if (!value.is_string() || (!allow_empty && value.get<std::string>().empty())) {
        invalid(std::string(context) + "." + std::string(key) +
                " must be non-empty text");
    }
    const std::string text = value.get<std::string>();
    if (text.find('\0') != std::string::npos) {
        invalid(std::string(context) + "." + std::string(key) +
                " must not contain a NUL character");
    }
    return text;
}

std::string optional_text(const json &object, std::string_view key,
                          std::string_view context) {
    if (!object.contains(key)) {
        return {};
    }
    const json &value = object.at(key);
    if (!value.is_string()) {
        invalid(std::string(context) + "." + std::string(key) +
                " must be text");
    }
    const auto text = value.get<std::string>();
    if (text.find('\0') != std::string::npos) {
        invalid(std::string(context) + "." + std::string(key) +
                " must not contain a NUL character");
    }
    return text;
}

std::uint64_t required_id(const json &object, std::string_view key,
                          std::string_view context) {
    const json &value = required_member(object, key, context);
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        invalid(std::string(context) + "." + std::string(key) +
                " must be a non-negative integer ID");
    }
    try {
        if (value.is_number_unsigned()) {
            return value.get<std::uint64_t>();
        }
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            invalid(std::string(context) + "." + std::string(key) +
                    " must be a non-negative integer ID");
        }
        return static_cast<std::uint64_t>(signed_value);
    } catch (const json::exception &) {
        invalid(std::string(context) + "." + std::string(key) +
                " is outside the supported ID range");
    }
}

std::string id_text(std::uint64_t value) {
    return std::to_string(value);
}

std::string required_identifier(const json &object, std::string_view key,
                                std::string_view context) {
    const json &value = required_member(object, key, context);
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (text.empty() || text.find('\0') != std::string::npos) {
            invalid(std::string(context) + "." + std::string(key) +
                    " must be a non-empty identifier");
        }
        return text;
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return id_text(required_id(object, key, context));
    }
    invalid(std::string(context) + "." + std::string(key) +
            " must be text or a non-negative integer identifier");
}

const json &required_object(const json &object, std::string_view key,
                            std::string_view context) {
    const json &value = required_member(object, key, context);
    if (!value.is_object()) {
        invalid(std::string(context) + "." + std::string(key) +
                " must be an object");
    }
    return value;
}

const json &required_array(const json &object, std::string_view key,
                           std::string_view context, bool allow_empty = false) {
    const json &value = required_member(object, key, context);
    if (!value.is_array() || (!allow_empty && value.empty())) {
        invalid(std::string(context) + "." + std::string(key) +
                " must be a non-empty array");
    }
    return value;
}

std::string bool_text(const json &object, std::string_view key,
                      std::string_view context) {
    const json &value = required_member(object, key, context);
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_string() &&
        (value.get<std::string>() == "true" || value.get<std::string>() == "false")) {
        return value.get<std::string>();
    }
    invalid(std::string(context) + "." + std::string(key) +
            " must be boolean or the text true/false");
}

bool is_descriptor_whitespace(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

void validate_finite_descriptor(std::string_view text, std::size_t expected_count,
                                std::string_view field) {
    std::vector<std::string_view> tokens;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        while (cursor < text.size() && is_descriptor_whitespace(text[cursor])) {
            ++cursor;
        }
        if (cursor == text.size()) {
            break;
        }
        const std::size_t start = cursor;
        while (cursor < text.size() && !is_descriptor_whitespace(text[cursor])) {
            ++cursor;
        }
        tokens.push_back(text.substr(start, cursor - start));
    }
    if (tokens.size() != expected_count) {
        invalid(std::string(field) + " must contain exactly " +
                std::to_string(expected_count) + " finite numeric values");
    }
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        double number = 0.0;
        if (!detail::parse_finite_decimal_string(tokens[index], number)) {
            invalid(std::string(field) + " contains an invalid finite numeric value at " +
                    std::to_string(index));
        }
    }
}

bool is_xml10_character(std::uint32_t code_point) {
    return code_point == 0x9 || code_point == 0xA || code_point == 0xD ||
           (code_point >= 0x20 && code_point <= 0xD7FF) ||
           (code_point >= 0xE000 && code_point <= 0xFFFD) ||
           (code_point >= 0x10000 && code_point <= 0x10FFFF);
}

void validate_xml10_text(std::string_view text, std::string_view field) {
    std::size_t index = 0;
    while (index < text.size()) {
        const std::size_t byte_index = index;
        const auto first = static_cast<unsigned char>(text[index]);
        std::uint32_t code_point = 0;
        std::size_t length = 0;
        std::uint32_t minimum = 0;
        if (first <= 0x7F) {
            code_point = first;
            length = 1;
            minimum = 0;
        } else if (first >= 0xC2 && first <= 0xDF) {
            code_point = first & 0x1F;
            length = 2;
            minimum = 0x80;
        } else if (first >= 0xE0 && first <= 0xEF) {
            code_point = first & 0x0F;
            length = 3;
            minimum = 0x800;
        } else if (first >= 0xF0 && first <= 0xF4) {
            code_point = first & 0x07;
            length = 4;
            minimum = 0x10000;
        } else {
            invalid(std::string(field) + " contains an invalid XML 1.0 character at byte " +
                    std::to_string(byte_index));
        }
        if (index + length > text.size()) {
            invalid(std::string(field) + " contains an invalid XML 1.0 character at byte " +
                    std::to_string(byte_index));
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0) != 0x80) {
                invalid(std::string(field) +
                        " contains an invalid XML 1.0 character at byte " +
                        std::to_string(byte_index));
            }
            code_point = (code_point << 6) | (continuation & 0x3F);
        }
        if (code_point < minimum || code_point > 0x10FFFF ||
            (code_point >= 0xD800 && code_point <= 0xDFFF) ||
            !is_xml10_character(code_point)) {
            invalid(std::string(field) + " contains an invalid XML 1.0 character at byte " +
                    std::to_string(byte_index));
        }
        index += length;
    }
}

void validate_reference(const std::string &value, std::string_view field) {
    const bool windows_absolute = value.size() >= 2 &&
                                  ((value.front() >= 'A' && value.front() <= 'Z') ||
                                   (value.front() >= 'a' && value.front() <= 'z')) &&
                                  value[1] == ':';
    if (value.empty() || value.front() == '/' || windows_absolute ||
        value.find('\\') != std::string::npos || value.find('\0') != std::string::npos) {
        invalid(std::string(field) + " must be a non-empty relative resource path");
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto segment = value.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (segment == "..") {
            invalid(std::string(field) + " must not contain a parent directory segment");
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
}

void add_metadata(tinyxml2::XMLDocument &document, tinyxml2::XMLElement *parent,
                  const char *key, const std::string &value,
                  std::string_view field = {}) {
    validate_xml10_text(value, field.empty() ? std::string_view(key) : field);
    auto *element = document.NewElement("metadata");
    element->SetAttribute("key", key);
    element->SetAttribute("value", value.c_str());
    parent->InsertEndChild(element);
}

std::string serialize_xml(tinyxml2::XMLDocument &document) {
    tinyxml2::XMLPrinter printer(nullptr, false, 0);
    document.Print(&printer);
    const std::string raw(printer.CStr(), printer.CStrSize() - 1);
    // XML 1.0 normalizes literal line breaks/tabs in attributes to spaces.
    // Encode them as character references so an independent parser restores
    // caller-provided names exactly.
    std::string result;
    result.reserve(raw.size());
    bool in_tag = false;
    char quote = '\0';
    for (const char character : raw) {
        if (!in_tag) {
            result.push_back(character);
            if (character == '<') {
                in_tag = true;
            }
            continue;
        }
        if (quote != '\0') {
            if (character == quote) {
                quote = '\0';
                result.push_back(character);
            } else if (character == '\n') {
                result += "&#xA;";
            } else if (character == '\r') {
                result += "&#xD;";
            } else if (character == '\t') {
                result += "&#x9;";
            } else {
                result.push_back(character);
            }
            continue;
        }
        result.push_back(character);
        if (character == '>' ) {
            in_tag = false;
        } else if (character == '"' || character == '\'') {
            quote = character;
        }
    }
    return result;
}

struct PartInput {
    std::string context;
    std::uint64_t part_id;
    std::string name;
    std::uint64_t material_index;
    std::uint64_t source_object_id;
    std::uint64_t source_volume_id;
    std::string matrix;
    std::string offset_x;
    std::string offset_y;
    std::string offset_z;
    std::uint64_t source_part_id = 0;
};

struct ModelInput {
    std::string context = "request";
    std::uint64_t model_index = 1;
    std::uint64_t assembly_id = 0;
    std::uint64_t source_assembly_id = 0;
    std::uint64_t source_model_index = 1;
    std::string source_file;
    std::string source_model_settings_xml;
    std::string source_layer_config_ranges_xml;
    std::string instance_id;
    std::string identify_id;
    std::vector<PartInput> parts;
    std::vector<std::size_t> source_slot_output_indexes;
    std::size_t default_slot_output_index = 0;
    json source_slots = json::array();
    json source_root_metadata = json::object();
    std::string assemble_transform;
    std::string assemble_offset;
    std::uint64_t face_count = 0;
};

std::string format_layer_range_number(double value, std::string_view field);

struct ResourcePartPlan {
    std::string role;
    std::string path;
    std::string media_type;
    std::string model_field;
    json relationship;
};

class MetadataParts {
public:
    void add_text(const std::string &role, const std::string &path,
                  const std::string &media_type, const std::string &content) {
        add(role, path, media_type, "content", content);
    }

    void add_resource(const ResourcePartPlan &resource) {
        add(resource.role, resource.path, resource.media_type,
            "resource_role", resource.role);
    }

    const std::string *path(const std::string &role) const {
        const auto found = paths_by_role_.find(role);
        return found == paths_by_role_.end() ? nullptr : &found->second;
    }

    json take_parts() { return std::move(parts_); }

private:
    json parts_ = json::array();
    std::map<std::string, std::string> paths_by_role_;
    std::set<std::string> paths_;

    void add(const std::string &role, const std::string &path,
             const std::string &media_type, const char *payload_key,
             const std::string &payload) {
        if (!paths_by_role_.emplace(role, path).second ||
            !paths_.insert(path).second) {
            invalid("metadata component plan contains a duplicate part role or path");
        }
        parts_.push_back({{"role", role}, {"path", path},
                          {"media_type", media_type}, {payload_key, payload}});
    }
};

bool component_is_included(const json &plan, const std::string &name,
                           std::size_t object_count) {
    return plan.contains(name) &&
           object_count >= plan.at(name).value("min_objects", std::size_t{1});
}

json build_metadata_relationships(const json &plan, std::size_t object_count,
                                 const std::vector<ResourcePartPlan> &resources,
                                 const MetadataParts &parts) {
    std::set<std::string> defined_roles = {
        "model_settings", "project_settings", "slice_info", "wipe_tower_placement"};
    for (const auto *name : {"plate_summary", "filament_sequence",
                             "cut_information", "layer_config_ranges"}) {
        if (component_is_included(plan, name, object_count)) defined_roles.insert(name);
    }
    if (plan.contains("custom_gcode_per_layer")) defined_roles.insert("custom_gcode_per_layer");
    for (const auto &resource : resources) defined_roles.insert(resource.role);

    const json &relationship_plan = required_object(
        plan, "model_relationships", "target metadata components");
    const auto source = required_text(relationship_plan, "source", "target model relationships", true);
    const auto type = required_text(relationship_plan, "type", "target model relationships");
    const auto id_prefix = required_text(relationship_plan, "id_prefix", "target model relationships");
    const json &related_roles = required_array(
        relationship_plan, "part_roles", "target model relationships", true);
    json relationships = json::array();
    std::size_t index = 0;
    for (const auto &role_value : related_roles) {
        if (!role_value.is_string()) {
            invalid("target model relationship part_roles entries must be text");
        }
        const std::string role = role_value.get<std::string>();
        if (plan.contains(role) && !component_is_included(plan, role, object_count)) continue;
        if (defined_roles.count(role) == 0) {
            invalid("target model relationship references an unknown metadata part role");
        }
        const auto *path = parts.path(role);
        if (path == nullptr) continue;
        relationships.push_back({{"source", source}, {"id", id_prefix + std::to_string(index++)},
                                 {"type", type}, {"target", "/" + *path}});
    }
    for (const auto &resource : resources) {
        if (!resource.relationship.is_null()) relationships.push_back(resource.relationship);
    }
    return relationships;
}

json build_metadata_build_items(const std::vector<ModelInput> &models, const json &plan) {
    json properties = json::object();
    if (plan.contains("build_item_properties")) {
        properties = required_object(plan, "build_item_properties", "target metadata components");
        for (const auto &[key, value] : properties.items()) {
            if (!value.is_string()) invalid("target build_item_properties values must be text");
            validate_xml10_text(value.get<std::string>(), "target build_item_properties." + key);
        }
    }
    json items = json::array();
    for (const auto &model : models) {
        json object_ids = json::array();
        for (const auto &part : model.parts) object_ids.push_back(part.part_id);
        items.push_back({{"model_index", model.model_index}, {"assembly_id", model.assembly_id},
                         {"object_ids", std::move(object_ids)}, {"transform", model.assemble_transform},
                         {"offset", model.assemble_offset}, {"properties", properties}});
    }
    return items;
}

std::vector<ResourcePartPlan> parse_resource_part_plan(const json &components,
                                                       const json &request) {
    const json &resources = required_array(
        components, "resources", "target metadata components");
    std::vector<ResourcePartPlan> result;
    std::set<std::string> roles;
    std::set<std::string> model_fields;
    std::set<std::string> available_roles;
    std::map<std::string, std::string> required_model_fields;
    if (components.contains("required_model_fields")) {
        const json &declared_fields = required_object(
            components, "required_model_fields", "target metadata components");
        for (auto it = declared_fields.begin(); it != declared_fields.end(); ++it) {
            if (!it.value().is_string() || it.value().get<std::string>().empty()) {
                invalid("target metadata components.required_model_fields must map roles to non-empty text");
            }
            required_model_fields.emplace(it.key(), it.value().get<std::string>());
        }
    }
    const bool explicit_resources = request.contains("resource_roles");
    if (explicit_resources) {
        for (const auto &role : required_array(request, "resource_roles", "request", true)) {
            if (!role.is_string() || role.get<std::string>().empty()) {
                invalid("request.resource_roles must contain non-empty text");
            }
            available_roles.insert(role.get<std::string>());
        }
    }
    result.reserve(resources.size());
    for (std::size_t index = 0; index < resources.size(); ++index) {
        const auto &resource = resources.at(index);
        const std::string context = "target metadata components.resources[" +
                                    std::to_string(index) + "]";
        if (!resource.is_object()) {
            invalid(context + " must be an object");
        }
        ResourcePartPlan plan{
            required_text(resource, "role", context),
            required_text(resource, "path", context),
            required_text(resource, "media_type", context),
            {},
            nullptr,
        };
        validate_reference(plan.path, context + ".path");
        if (!roles.insert(plan.role).second) {
            invalid("target metadata components contain duplicate resource roles");
        }
        const auto expected = required_model_fields.find(plan.role);
        if (!resource.contains("model_field")) {
            if (expected != required_model_fields.end()) {
                invalid(context + ".model_field is required for role " + plan.role +
                        " (expected " + expected->second + ")");
            }
        } else {
            plan.model_field = required_text(resource, "model_field", context);
            if (expected != required_model_fields.end() &&
                plan.model_field != expected->second) {
                invalid(context + ".model_field for role " + plan.role +
                        " must be " + expected->second);
            }
        }
        if (explicit_resources && available_roles.erase(plan.role) == 0) continue;
        if (!plan.model_field.empty()) {
            if (!model_fields.insert(plan.model_field).second) {
                invalid("target metadata components contain duplicate model fields");
            }
        }
        if (resource.contains("relationship")) {
            const json &relationship = required_object(
                resource, "relationship", context);
            plan.relationship = {
                {"source", required_text(relationship, "source", context, true)},
                {"id", required_text(relationship, "id", context)},
                {"type", required_text(relationship, "type", context)},
                {"target", "/" + plan.path},
            };
        }
        result.push_back(std::move(plan));
    }
    if (!available_roles.empty()) {
        invalid("request.resource_roles contains an unknown target resource role");
    }
    for (const auto &[role, field] : required_model_fields) {
        if (roles.count(role) == 0) {
            invalid("target metadata components.required_model_fields references unknown role " +
                    role + " (expected " + field + ")");
        }
    }
    return result;
}

json build_model_resource_paths(const std::vector<ResourcePartPlan> &resources) {
    json result = json::object();
    for (const auto &resource : resources) {
        if (!resource.model_field.empty()) {
            result[resource.model_field] = resource.path;
        }
    }
    return result;
}

json validate_content_types(const json &components) {
    const json &content_types = required_array(
        components, "content_types", "target metadata components");
    std::set<std::string> extensions;
    for (std::size_t index = 0; index < content_types.size(); ++index) {
        const auto &item = content_types.at(index);
        const std::string context = "target metadata components.content_types[" +
                                    std::to_string(index) + "]";
        if (!item.is_object()) {
            invalid(context + " must be an object");
        }
        const auto extension = required_text(item, "extension", context);
        required_text(item, "media_type", context);
        if (!extensions.insert(extension).second) {
            invalid("target metadata components contain duplicate content-type extensions");
        }
    }
    return content_types;
}

std::vector<PartInput> parse_parts(const json &request, std::size_t slot_count,
                                   std::uint64_t assembly_id) {
    const json &parts = required_array(request, "parts", "request");
    std::vector<PartInput> result;
    std::set<std::uint64_t> part_ids;
    result.reserve(parts.size());
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const json &part = parts.at(index);
        if (!part.is_object()) {
            invalid("request.parts entries must be objects");
        }
        const std::string context = "request.parts[" + std::to_string(index) + "]";
        PartInput input{
            context,
            required_id(part, "part_id", context),
            required_text(part, "name", context),
            required_id(part, "material_index", context),
            required_id(part, "source_object_id", context),
            required_id(part, "source_volume_id", context),
            required_text(part, "matrix", context),
            required_text(part, "source_offset_x", context),
            required_text(part, "source_offset_y", context),
            required_text(part, "source_offset_z", context),
        };
        validate_finite_descriptor(input.matrix, 16, context + ".matrix");
        validate_finite_descriptor(input.offset_x, 1, context + ".source_offset_x");
        validate_finite_descriptor(input.offset_y, 1, context + ".source_offset_y");
        validate_finite_descriptor(input.offset_z, 1, context + ".source_offset_z");
        if (input.part_id == assembly_id) {
            invalid("request assembly_id conflicts with a part_id");
        }
        if (!part_ids.insert(input.part_id).second) {
            invalid("request.parts contains a duplicate part_id");
        }
        if (input.material_index >= slot_count) {
            invalid("request.parts material_index is outside the project slot range");
        }
        result.push_back(std::move(input));
    }
    return result;
}

std::vector<PartInput> parse_merged_parts(const json &object,
                                         const std::string &context) {
    const auto &parts = required_array(object, "parts", context);
    std::vector<PartInput> result;
    result.reserve(parts.size());
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto &part = parts.at(index);
        const auto part_context = context + ".parts[" + std::to_string(index) + "]";
        PartInput input{};
        input.context = part_context;
        input.part_id = required_id(part, "part_id", part_context);
        input.source_part_id = required_id(part, "source_part_id", part_context);
        result.push_back(std::move(input));
    }
    return result;
}

std::string numeric_vector_text(const json &value, std::size_t expected_size,
                               std::string_view context) {
    if (!value.is_array() || value.size() != expected_size) {
        invalid(std::string(context) + " must contain exactly " +
                std::to_string(expected_size) + " numeric values");
    }
    std::string text;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto &entry = value.at(index);
        if (!entry.is_number()) {
            invalid(std::string(context) + " must contain only numeric values");
        }
        const double number = entry.get<double>();
        if (!std::isfinite(number)) {
            invalid(std::string(context) + " must contain only finite values");
        }
        if (!text.empty()) text.push_back(' ');
        text += format_layer_range_number(number, context);
    }
    return text;
}

std::pair<bool, std::vector<ModelInput>> parse_model_inputs(
    const json &request, std::size_t slot_count,
    bool emit_lumina_merged_slots_json) {
    const bool multi_model = request.contains("objects");
    std::vector<ModelInput> models;
    std::set<std::uint64_t> assembly_ids;
    std::set<std::uint64_t> all_part_ids;
    const json empty_objects = json::array();
    const json &input_objects = multi_model
                                   ? required_array(request, "objects", "request")
                                   : empty_objects;
    const std::size_t count = multi_model ? input_objects.size() : 1;
    models.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        const json &object = multi_model ? input_objects.at(index) : request;
        const std::string context = multi_model
                                        ? "request.objects[" + std::to_string(index) + "]"
                                        : "request";
        if (!object.is_object()) invalid(context + " must be an object");
        ModelInput model;
        model.context = context;
        model.model_index = multi_model
                                ? required_id(object, "model_index", context)
                                : 1;
        if (model.model_index != index + 1) {
            invalid(context + ".model_index must match final build order");
        }
        model.assembly_id = required_id(object, "assembly_id", context);
        if (!assembly_ids.insert(model.assembly_id).second) {
            invalid("request.objects contains duplicate assembly_id");
        }
        model.instance_id = required_identifier(object, "instance_id", context);
        model.identify_id = required_identifier(object, "identify_id", context);
        if (multi_model) {
            model.source_assembly_id = required_id(object, "source_assembly_id", context);
            if (object.contains("source_model_index")) {
                model.source_model_index = required_id(object, "source_model_index", context);
                if (model.source_model_index == 0) {
                    invalid(context + ".source_model_index must be a one-based build index");
                }
            }
            model.source_model_settings_xml = required_text(
                object, "source_model_settings_xml", context);
            if (object.contains("source_layer_config_ranges_xml")) {
                model.source_layer_config_ranges_xml = required_text(
                    object, "source_layer_config_ranges_xml", context, true);
            }
            model.face_count = required_id(object, "face_count", context);
            model.parts = parse_merged_parts(object, context);
            const auto &slot_mapping = required_array(
                object, "source_slot_output_indexes", context);
            for (const auto &output_index : slot_mapping) {
                if (!output_index.is_number_unsigned()) {
                    invalid(context + ".source_slot_output_indexes must contain indexes");
                }
                model.source_slot_output_indexes.push_back(output_index.get<std::size_t>());
            }
            if (object.contains("default_slot_output_index")) {
                model.default_slot_output_index = static_cast<std::size_t>(required_id(
                    object, "default_slot_output_index", context));
            } else if (!model.source_slot_output_indexes.empty()) {
                model.default_slot_output_index =
                    model.source_slot_output_indexes.front();
            } else {
                model.default_slot_output_index = slot_count;
            }
            if (object.contains("source_slots")) {
                model.source_slots = required_array(object, "source_slots", context);
            } else if (emit_lumina_merged_slots_json) {
                required_array(object, "source_slots", context);
            }
        } else {
            model.source_file = required_text(object, "source_file", context);
            validate_reference(model.source_file, context + ".source_file");
            model.parts = parse_parts(object, slot_count, model.assembly_id);
        }
        for (const auto &part : model.parts) {
            if (!all_part_ids.insert(part.part_id).second) {
                invalid("request.objects contains duplicate part_id across models");
            }
        }

        if (multi_model) {
            model.source_root_metadata = required_object(
                object, "source_root_metadata", context);
            for (const auto &[name, value] : model.source_root_metadata.items()) {
                if (!value.is_string()) {
                    invalid(context + ".source_root_metadata values must be text");
                }
                validate_xml10_text(name, context + ".source_root_metadata name");
                validate_xml10_text(value.get<std::string>(),
                                    context + ".source_root_metadata value");
            }
            model.assemble_transform = numeric_vector_text(
                required_member(object, "transform", context), 12, context + ".transform");
            model.assemble_offset = numeric_vector_text(
                required_member(object, "offset_mm", context), 3, context + ".offset_mm");
        }
        models.push_back(std::move(model));
    }
    for (const auto assembly_id : assembly_ids) {
        if (all_part_ids.count(assembly_id) != 0) {
            invalid("request.objects assembly_id conflicts with a part_id");
        }
    }
    return {multi_model, std::move(models)};
}

std::string build_model_settings(const json &request,
                                 const std::vector<PartInput> &parts,
                                 const std::string &bed_type,
                                 std::string_view bed_type_field,
                                 const json &resources,
                                 const json &component_plan,
                                 bool qidi_studio) {
    const auto assembly_id = required_id(request, "assembly_id", "request");
    const auto source_file = required_text(request, "source_file", "request");
    const json &plate = required_object(request, "plate", "request");
    const std::string plater_id = required_text(plate, "plater_id", "request.plate");
    const std::string plater_name =
        required_text(plate, "plater_name", "request.plate", true);
    const std::string locked = bool_text(plate, "locked", "request.plate");
    validate_reference(source_file, "request.source_file");

    tinyxml2::XMLDocument document;
    document.InsertEndChild(
        document.NewDeclaration("xml version=\"1.0\" encoding=\"UTF-8\""));
    auto *config = document.NewElement("config");
    document.InsertEndChild(config);
    auto *object = document.NewElement("object");
    object->SetAttribute("id", id_text(assembly_id).c_str());
    config->InsertEndChild(object);
    if (qidi_studio || component_plan.value("object_name_from_source_file", false)) {
        std::string name = source_file;
        const auto extension = name.find_last_of('.');
        if (extension != std::string::npos) name.erase(extension);
        add_metadata(document, object, "name", name);
    }
    // Slicers use the object setting when the object has only one volume.
    add_metadata(document, object, "extruder",
                 id_text(parts.size() == 1 ? parts.front().material_index + 1 : 1));
    for (const auto &part_input : parts) {
            auto *part = document.NewElement("part");
            part->SetAttribute("id", id_text(part_input.part_id).c_str());
            part->SetAttribute("subtype", "normal_part");
            object->InsertEndChild(part);
            add_metadata(document, part, "name", part_input.name,
                         part_input.context + ".name");
            add_metadata(document, part, "matrix", part_input.matrix,
                         part_input.context + ".matrix");
            add_metadata(document, part, "source_file", source_file,
                         "request.source_file");
            add_metadata(document, part, "source_object_id",
                         id_text(part_input.source_object_id));
            add_metadata(document, part, "source_volume_id",
                         id_text(part_input.source_volume_id));
            add_metadata(document, part, "source_offset_x", part_input.offset_x,
                         part_input.context + ".source_offset_x");
            add_metadata(document, part, "source_offset_y", part_input.offset_y,
                         part_input.context + ".source_offset_y");
            add_metadata(document, part, "source_offset_z", part_input.offset_z,
                         part_input.context + ".source_offset_z");
            add_metadata(document, part, "extruder",
                         id_text(part_input.material_index + 1));
            auto *mesh_stat = document.NewElement("mesh_stat");
            for (const char *key : {"edges_fixed", "degenerate_facets", "facets_removed",
                                    "facets_reversed", "backwards_edges"}) {
                mesh_stat->SetAttribute(key, "0");
            }
            part->InsertEndChild(mesh_stat);
    }

    auto *plate_element = document.NewElement("plate");
    config->InsertEndChild(plate_element);
    add_metadata(document, plate_element, "plater_id", plater_id,
                 "request.plate.plater_id");
    add_metadata(document, plate_element, "plater_name", plater_name,
                 "request.plate.plater_name");
    add_metadata(document, plate_element, "locked", locked,
                 "request.plate.locked");
    add_metadata(document, plate_element, "bed_type", bed_type,
                 bed_type_field);
    if (component_plan.contains("filament_map_mode")) {
        add_metadata(document, plate_element, "filament_map_mode",
                     required_text(component_plan, "filament_map_mode",
                                   "target metadata components"),
                     "target metadata components.filament_map_mode");
    }
    if (component_plan.value("filament_maps", std::string()) == "constant_one") {
        const auto count = required_id(request, "active_material_count", "request");
        std::string maps;
        for (std::uint64_t index = 0; index < count; ++index) {
            if (index != 0) maps += ' ';
            maps += '1';
        }
        add_metadata(document, plate_element, "filament_maps", maps);
    } else if (component_plan.value("filament_maps", std::string()) == "sequential") {
        const auto count = required_id(request, "active_material_count", "request");
        std::string maps;
        for (std::uint64_t index = 0; index < count; ++index) {
            if (index != 0) maps += ' ';
            maps += std::to_string(index + 1);
        }
        add_metadata(document, plate_element, "filament_maps", maps);
    }
    if (component_plan.value("filament_volume_maps", std::string()) == "constant_zero") {
        const auto count = required_id(request, "active_material_count", "request");
        std::string volume_maps;
        for (std::uint64_t index = 0; index < count; ++index) {
            if (index != 0) volume_maps += ' ';
            volume_maps += '0';
        }
        add_metadata(document, plate_element, "filament_volume_maps", volume_maps);
    }
    for (const auto &[field, path] : resources.items()) {
        add_metadata(document, plate_element, field.c_str(), path.get<std::string>(),
                     "target metadata resources." + field);
    }

    auto *instance = document.NewElement("model_instance");
    plate_element->InsertEndChild(instance);
    add_metadata(document, instance, "object_id", id_text(assembly_id));
    add_metadata(document, instance, "instance_id",
                 required_identifier(request, "instance_id", "request"),
                 "request.instance_id");
    add_metadata(document, instance, "identify_id",
                 required_identifier(request, "identify_id", "request"),
                 "request.identify_id");
    return serialize_xml(document);
}

tinyxml2::XMLElement *metadata_by_key(tinyxml2::XMLElement *parent,
                                      const char *key) {
    for (auto *metadata = parent->FirstChildElement("metadata"); metadata != nullptr;
         metadata = metadata->NextSiblingElement("metadata")) {
        const char *candidate = metadata->Attribute("key");
        if (candidate != nullptr && std::string_view(candidate) == key) return metadata;
    }
    return nullptr;
}

void set_metadata(tinyxml2::XMLDocument &document, tinyxml2::XMLElement *parent,
                  const char *key, const std::string &value) {
    auto *metadata = metadata_by_key(parent, key);
    if (metadata == nullptr) {
        add_metadata(document, parent, key, value);
    } else {
        metadata->SetAttribute("value", value.c_str());
    }
}

bool remap_extruder(tinyxml2::XMLElement *element, const ModelInput &model,
                    std::size_t slot_count,
                    bool allow_excluded_source_slot = false) {
    auto *extruder = metadata_by_key(element, "extruder");
    if (extruder == nullptr) return false;
    const char *value = extruder->Attribute("value");
    if (value == nullptr) invalid(model.context + " has an empty extruder mapping");
    const std::string_view text(value);
    std::uint64_t source_index = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), source_index);
    if (error != std::errc{} || end != text.data() + text.size() || source_index == 0 ||
        source_index > model.source_slot_output_indexes.size()) {
        invalid(model.context + " has an unmapped source extruder " + std::string(text));
    }
    const auto output_index = model.source_slot_output_indexes[source_index - 1];
    if (output_index >= slot_count) {
        if (allow_excluded_source_slot && output_index == slot_count) return false;
        invalid(model.context + " maps an extruder outside the final project slots");
    }
    extruder->SetAttribute("value", id_text(output_index + 1).c_str());
    return true;
}

std::string build_merged_model_settings(const std::vector<ModelInput> &models,
                                       const std::string &bed_type,
                                       const json &resources,
                                       std::size_t slot_count,
                                       bool emit_lumina_merged_slots_json) {
    tinyxml2::XMLDocument document;
    document.InsertEndChild(
        document.NewDeclaration("xml version=\"1.0\" encoding=\"UTF-8\""));
    auto *config = document.NewElement("config");
    document.InsertEndChild(config);
    tinyxml2::XMLElement *output_plate = nullptr;

    for (const auto &model : models) {
        tinyxml2::XMLDocument source_document;
        if (source_document.Parse(model.source_model_settings_xml.data(),
                                  model.source_model_settings_xml.size()) != tinyxml2::XML_SUCCESS) {
            invalid(model.context + " has invalid Metadata/model_settings.config");
        }
        auto *source_config = source_document.FirstChildElement("config");
        const auto source_id = id_text(model.source_assembly_id);
        auto *source_object = source_config == nullptr
                                  ? nullptr
                                  : source_config->FirstChildElement("object");
        while (source_object != nullptr &&
               (source_object->Attribute("id") == nullptr ||
                std::string_view(source_object->Attribute("id")) != source_id)) {
            source_object = source_object->NextSiblingElement("object");
        }
        if (source_object == nullptr) {
            invalid(model.context + " source object was not found in model_settings.config");
        }

        auto *object = source_object->DeepClone(&document)->ToElement();
        object->SetAttribute("id", id_text(model.assembly_id).c_str());
        auto *object_extruder = metadata_by_key(object, "extruder");
        const bool object_default_mapped =
            remap_extruder(object, model, slot_count, true);
        const bool object_default_was_excluded =
            object_extruder != nullptr && !object_default_mapped;
        if (object_default_was_excluded) {
            if (model.default_slot_output_index >= slot_count) {
                invalid(model.context + " has no output slot for its default extruder");
            }
            object_extruder->SetAttribute(
                "value", id_text(model.default_slot_output_index + 1).c_str());
        } else if (object_extruder == nullptr) {
            if (model.default_slot_output_index >= slot_count) {
                invalid(model.context + " has no output slot for its default extruder");
            }
            add_metadata(document, object, "extruder",
                         id_text(model.default_slot_output_index + 1));
        }

        auto *part = object->FirstChildElement("part");
        while (part != nullptr) {
            auto *next = part->NextSiblingElement("part");
            object->DeleteChild(part);
            part = next;
        }
        for (const auto &part_input : model.parts) {
            auto *source_part = source_object->FirstChildElement("part");
            const auto part_source_id = id_text(part_input.source_part_id);
            while (source_part != nullptr &&
                   (source_part->Attribute("id") == nullptr ||
                    std::string_view(source_part->Attribute("id")) != part_source_id)) {
                source_part = source_part->NextSiblingElement("part");
            }
            if (source_part == nullptr) {
                invalid(part_input.context + " was not found in source model_settings.config");
            }
            auto *part_copy = source_part->DeepClone(&document)->ToElement();
            part_copy->SetAttribute("id", id_text(part_input.part_id).c_str());
            const bool part_has_extruder =
                metadata_by_key(part_copy, "extruder") != nullptr;
            remap_extruder(part_copy, model, slot_count);
            if (object_default_was_excluded && !part_has_extruder) {
                invalid(part_input.context +
                        " relies on an excluded source object default extruder");
            }
            object->InsertEndChild(part_copy);
        }
        if (model.parts.size() == 1) {
            const auto *part_extruder = metadata_by_key(
                object->FirstChildElement("part"), "extruder");
            if (part_extruder != nullptr) {
                set_metadata(document, object, "extruder", part_extruder->Attribute("value"));
            }
        }

        bool has_face_count = false;
        for (auto *metadata = object->FirstChildElement("metadata"); metadata != nullptr;
             metadata = metadata->NextSiblingElement("metadata")) {
            has_face_count = has_face_count || metadata->Attribute("face_count") != nullptr;
        }
        if (!has_face_count) {
            auto *metadata = document.NewElement("metadata");
            metadata->SetAttribute("face_count", id_text(model.face_count).c_str());
            object->InsertEndChild(metadata);
        }
        if (emit_lumina_merged_slots_json) {
            set_metadata(document, object, "lumina_merged_slots_json",
                         model.source_slots.dump(-1, ' ', false));
        } else if (auto *metadata = metadata_by_key(
                       object, "lumina_merged_slots_json")) {
            object->DeleteChild(metadata);
        }
        config->InsertEndChild(object);

        if (output_plate == nullptr) {
            auto *source_plate = source_config->FirstChildElement("plate");
            if (source_plate == nullptr) {
                invalid(model.context + " source model_settings.config has no plate");
            }
            output_plate = source_plate->DeepClone(&document)->ToElement();
            auto *instance = output_plate->FirstChildElement("model_instance");
            while (instance != nullptr) {
                auto *next = instance->NextSiblingElement("model_instance");
                output_plate->DeleteChild(instance);
                instance = next;
            }
            set_metadata(document, output_plate, "bed_type", bed_type);
            for (const char *field : {"thumbnail_file", "thumbnail_no_light_file",
                                      "top_file", "pick_file"}) {
                if (resources.contains(field)) {
                    set_metadata(document, output_plate, field,
                                 required_text(resources, field, "target metadata resources"));
                } else if (auto *metadata = metadata_by_key(output_plate, field)) {
                    output_plate->DeleteChild(metadata);
                }
            }
        }
    }

    config->InsertEndChild(output_plate);
    for (const auto &model : models) {
        auto *instance = document.NewElement("model_instance");
        output_plate->InsertEndChild(instance);
        add_metadata(document, instance, "object_id", id_text(model.assembly_id));
        add_metadata(document, instance, "instance_id", model.instance_id);
        add_metadata(document, instance, "identify_id", model.identify_id);
    }
    auto *assemble = document.NewElement("assemble");
    config->InsertEndChild(assemble);
    for (const auto &model : models) {
        auto *item = document.NewElement("assemble_item");
        item->SetAttribute("object_id", id_text(model.assembly_id).c_str());
        item->SetAttribute("instance_id", model.instance_id.c_str());
        item->SetAttribute("transform", model.assemble_transform.c_str());
        item->SetAttribute("offset", model.assemble_offset.c_str());
        assemble->InsertEndChild(item);
    }
    return serialize_xml(document);
}

std::string format_layer_range_number(double value, std::string_view field) {
    if (!std::isfinite(value)) {
        invalid(std::string(field) + " must be finite");
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(17) << std::defaultfloat << value;
    return output.str();
}

double final_project_layer_height(const json &project) {
    const json &value = required_member(project, "layer_height", "final project settings");
    double layer_height = 0.0;
    if (value.is_number()) {
        layer_height = value.get<double>();
    } else if (value.is_string()) {
        std::istringstream input(value.get<std::string>());
        input.imbue(std::locale::classic());
        if (!(input >> layer_height)) {
            invalid("final project settings.layer_height must be a positive number");
        }
        std::string trailing;
        if (input >> trailing) {
            invalid("final project settings.layer_height must be a positive number");
        }
    } else {
        invalid("final project settings.layer_height must be a positive number");
    }
    if (!std::isfinite(layer_height) || layer_height <= 0.0) {
        invalid("final project settings.layer_height must be a positive number");
    }
    return layer_height;
}

std::string build_layer_config_ranges(const json &input, const json &project,
                                      const json &layer_plan) {
    if (!input.is_object()) {
        invalid("request.component_inputs.layer_config_ranges must be an object");
    }
    struct ModelRanges {
        std::uint64_t model_index;
        const json *ranges;
        std::string context;
    };
    std::vector<ModelRanges> model_ranges;
    if (input.contains("objects")) {
        if (input.contains("model_index") || input.contains("ranges")) {
            invalid("request.component_inputs.layer_config_ranges must use either objects or one model");
        }
        const auto &objects = required_array(
            input, "objects", "request.component_inputs.layer_config_ranges", true);
        for (std::size_t index = 0; index < objects.size(); ++index) {
            const auto &object = objects.at(index);
            const std::string context = "request.component_inputs.layer_config_ranges.objects[" +
                                        std::to_string(index) + "]";
            if (!object.is_object()) invalid(context + " must be an object");
            const auto model_index = required_id(object, "model_index", context);
            if (model_index != index + 1) {
                invalid(context + ".model_index must match final build order");
            }
            const auto &ranges = required_array(object, "ranges", context, true);
            model_ranges.push_back({model_index, &ranges, context});
        }
    } else {
        const auto model_index = required_id(
            input, "model_index", "request.component_inputs.layer_config_ranges");
        const auto &ranges = required_array(
            input, "ranges", "request.component_inputs.layer_config_ranges", true);
        model_ranges.push_back({model_index, &ranges,
                                "request.component_inputs.layer_config_ranges"});
    }

    const json &option_fields = required_object(
        layer_plan, "option_fields", "target layer config ranges");
    const std::string default_extruder_key = required_text(
        option_fields, "default_extruder_key", "target layer range option fields");
    const std::string default_extruder_value = required_text(
        option_fields, "default_extruder_value", "target layer range option fields");
    const std::string layer_height_key = required_text(
        option_fields, "layer_height_key", "target layer range option fields");
    const std::string infill_density_key = required_text(
        option_fields, "infill_density_key", "target layer range option fields");
    const std::string infill_density_suffix = required_text(
        option_fields, "infill_density_suffix", "target layer range option fields", true);
    if (default_extruder_key == layer_height_key ||
        default_extruder_key == infill_density_key ||
        layer_height_key == infill_density_key) {
        invalid("target layer range option fields must use distinct keys");
    }
    validate_xml10_text(default_extruder_key, "target default extruder option key");
    validate_xml10_text(default_extruder_value, "target default extruder option value");
    validate_xml10_text(layer_height_key, "target layer height option key");
    validate_xml10_text(infill_density_key, "target infill density option key");
    validate_xml10_text(infill_density_suffix, "target infill density suffix");

    tinyxml2::XMLDocument document;
    document.InsertEndChild(
        document.NewDeclaration("xml version=\"1.0\" encoding=\"UTF-8\""));
    auto *objects = document.NewElement("objects");
    document.InsertEndChild(objects);
    bool has_serialized_ranges = false;
    for (const auto &model : model_ranges) {
        if (model.ranges->empty()) continue;
        auto *object = document.NewElement("object");
        object->SetAttribute("id", id_text(model.model_index).c_str());
        objects->InsertEndChild(object);
        for (std::size_t index = 0; index < model.ranges->size(); ++index) {
            const auto &range = model.ranges->at(index);
            const std::string context = model.context + ".ranges[" +
                                        std::to_string(index) + "]";
            if (!range.is_object()) {
                invalid(context + " must be an object");
            }
            const json &minimum = required_member(range, "min_z", context);
            const json &maximum = required_member(range, "max_z", context);
            if (!minimum.is_number() || !maximum.is_number()) {
                invalid(context + " min_z and max_z must be numbers");
            }
            const double min_z = minimum.get<double>();
            const double max_z = maximum.get<double>();
            if (!std::isfinite(min_z) || !std::isfinite(max_z) || max_z <= min_z) {
                invalid(context + " must have finite min_z < max_z");
            }
            auto *range_element = document.NewElement("range");
            const auto min_text = format_layer_range_number(min_z, context + ".min_z");
            const auto max_text = format_layer_range_number(max_z, context + ".max_z");
            range_element->SetAttribute("min_z", min_text.c_str());
            range_element->SetAttribute("max_z", max_text.c_str());
            object->InsertEndChild(range_element);

            if (range.contains("options")) {
                invalid(context + " must use numeric business fields, not target options");
            }
            bool use_default_extruder = false;
            if (range.contains("use_default_extruder")) {
                if (!range.at("use_default_extruder").is_boolean()) {
                    invalid(context + ".use_default_extruder must be a boolean");
                }
                use_default_extruder = range.at("use_default_extruder").get<bool>();
            }
            if (use_default_extruder) {
                auto *option_element = document.NewElement("option");
                option_element->SetAttribute("opt_key", default_extruder_key.c_str());
                option_element->SetText(default_extruder_value.c_str());
                range_element->InsertEndChild(option_element);
            }

            double layer_height = 0.0;
            if (range.contains("layer_height_mm")) {
                const json &height_value = range.at("layer_height_mm");
                if (!height_value.is_number()) {
                    invalid(context + ".layer_height_mm must be a positive number");
                }
                layer_height = height_value.get<double>();
            } else {
                layer_height = final_project_layer_height(project);
            }
            if (!std::isfinite(layer_height) || layer_height <= 0.0) {
                invalid(context + ".layer_height_mm must be a positive number");
            }
            const auto layer_height_text =
                format_layer_range_number(layer_height, context + ".layer_height_mm");
            auto *layer_height_element = document.NewElement("option");
            layer_height_element->SetAttribute("opt_key", layer_height_key.c_str());
            layer_height_element->SetText(layer_height_text.c_str());
            range_element->InsertEndChild(layer_height_element);

            if (range.contains("infill_density_percent")) {
                const json &density_value = range.at("infill_density_percent");
                if (!density_value.is_number()) {
                    invalid(context + ".infill_density_percent must be a number from 0 to 100");
                }
                const double density = density_value.get<double>();
                if (!std::isfinite(density) || density < 0.0 || density > 100.0) {
                    invalid(context + ".infill_density_percent must be a number from 0 to 100");
                }
                const auto density_text = format_layer_range_number(
                    density, context + ".infill_density_percent") + infill_density_suffix;
                auto *density_element = document.NewElement("option");
                density_element->SetAttribute("opt_key", infill_density_key.c_str());
                density_element->SetText(density_text.c_str());
                range_element->InsertEndChild(density_element);
            }
        }
        has_serialized_ranges = true;
    }
    if (!has_serialized_ranges) return {};
    return serialize_xml(document);
}

std::string build_merged_layer_config_ranges(const std::vector<ModelInput> &models,
                                            const json &layer_plan,
                                            std::size_t slot_count) {
    const json &option_fields = required_object(
        layer_plan, "option_fields", "target layer config ranges");
    const std::string extruder_key = required_text(
        option_fields, "default_extruder_key", "target layer range option fields");
    const std::string default_extruder_value = required_text(
        option_fields, "default_extruder_value", "target layer range option fields");
    tinyxml2::XMLDocument document;
    document.InsertEndChild(
        document.NewDeclaration("xml version=\"1.0\" encoding=\"UTF-8\""));
    auto *objects = document.NewElement("objects");
    document.InsertEndChild(objects);
    bool has_ranges = false;
    for (const auto &model : models) {
        if (model.source_layer_config_ranges_xml.empty()) continue;
        tinyxml2::XMLDocument source_document;
        if (source_document.Parse(model.source_layer_config_ranges_xml.data(),
                                  model.source_layer_config_ranges_xml.size()) !=
            tinyxml2::XML_SUCCESS) {
            invalid(model.context + " has invalid Metadata/layer_config_ranges.xml");
        }
        const auto *source_objects = source_document.FirstChildElement("objects");
        if (source_objects == nullptr) continue;
        auto *output_object = document.NewElement("object");
        output_object->SetAttribute("id", id_text(model.model_index).c_str());
        for (auto *source_object = source_objects->FirstChildElement("object");
             source_object != nullptr;
             source_object = source_object->NextSiblingElement("object")) {
            const char *source_index = source_object->Attribute("id");
            if (source_index == nullptr || source_index != id_text(model.source_model_index)) {
                continue;
            }
            for (auto *source_range = source_object->FirstChildElement("range");
                 source_range != nullptr;
                 source_range = source_range->NextSiblingElement("range")) {
                auto *range = source_range->DeepClone(&document)->ToElement();
                for (auto *option = range->FirstChildElement("option"); option != nullptr;
                     option = option->NextSiblingElement("option")) {
                    const char *key = option->Attribute("opt_key");
                    if (key == nullptr || std::string_view(key) != extruder_key) continue;
                    const char *value = option->GetText();
                    if (value == nullptr) {
                        invalid(model.context + " has an empty layer-range extruder");
                    }
                    const std::string_view text(value);
                    if (text == default_extruder_value) continue;
                    std::uint64_t source_extruder = 0;
                    const auto [end, error] = std::from_chars(
                        text.data(), text.data() + text.size(), source_extruder);
                    if (error != std::errc{} || end != text.data() + text.size() ||
                        source_extruder == 0 ||
                        source_extruder > model.source_slot_output_indexes.size()) {
                        invalid(model.context + " has an unmapped layer-range extruder " +
                                std::string(text));
                    }
                    const auto output_index = model.source_slot_output_indexes[source_extruder - 1];
                    if (output_index >= slot_count) {
                        invalid(model.context + " layer range references an excluded source slot");
                    }
                    option->SetText(id_text(output_index + 1).c_str());
                }
                output_object->InsertEndChild(range);
                has_ranges = true;
            }
        }
        if (output_object->FirstChildElement("range") != nullptr) {
            objects->InsertEndChild(output_object);
        } else {
            document.DeleteNode(output_object);
        }
    }
    if (!has_ranges) return {};
    return serialize_xml(document);
}

std::string build_cut_information(const std::vector<ModelInput> &models,
                                  bool multi_model,
                                  const json &component_plan) {
    const auto cut_id = required_text(component_plan, "cut_id",
                                      "target cut information");
    const auto checksum = required_text(component_plan, "check_sum",
                                         "target cut information");
    const auto connectors = required_text(component_plan, "connectors_cnt",
                                          "target cut information");
    tinyxml2::XMLDocument document;
    document.InsertEndChild(
        document.NewDeclaration("xml version=\"1.0\" encoding=\"utf-8\""));
    auto *objects = document.NewElement("objects");
    document.InsertEndChild(objects);
    for (const auto &model : models) {
        auto *object = document.NewElement("object");
        const auto id = multi_model ? model.model_index
            : component_plan.contains("single_object_id")
                ? required_id(component_plan, "single_object_id", "target cut information")
                : model.assembly_id;
        object->SetAttribute("id", id_text(id).c_str());
        objects->InsertEndChild(object);
        auto *cut = document.NewElement("cut_id");
        cut->SetAttribute("id", cut_id.c_str());
        cut->SetAttribute("check_sum", checksum.c_str());
        cut->SetAttribute("connectors_cnt", connectors.c_str());
        object->InsertEndChild(cut);
    }
    return serialize_xml(document);
}

std::string build_custom_gcode_per_layer(const json &component_plan) {
    tinyxml2::XMLDocument document;
    document.InsertEndChild(
        document.NewDeclaration("xml version=\"1.0\" encoding=\"utf-8\""));
    auto *root = document.NewElement("custom_gcodes_per_layer");
    auto *plate = document.NewElement("plate");
    auto *plate_info = document.NewElement("plate_info");
    plate_info->SetAttribute("id", required_text(
        component_plan, "plate_id", "target custom gcode per layer").c_str());
    plate->InsertEndChild(plate_info);
    auto *mode = document.NewElement("mode");
    mode->SetAttribute("value", required_text(
        component_plan, "mode", "target custom gcode per layer").c_str());
    plate->InsertEndChild(mode);
    root->InsertEndChild(plate);
    document.InsertEndChild(root);
    return serialize_xml(document);
}

std::string build_slice_info(const json &dialect, const json &request,
                             const json &component_plan) {
    const json &headers = required_array(dialect, "slice_headers", "package dialect");
    tinyxml2::XMLDocument document;
    document.InsertEndChild(
        document.NewDeclaration("xml version=\"1.0\" encoding=\"UTF-8\""));
    auto *config = document.NewElement("config");
    auto *header = document.NewElement("header");
    config->InsertEndChild(header);
    document.InsertEndChild(config);
    std::set<std::string> names;
    for (std::size_t index = 0; index < headers.size(); ++index) {
        const auto &item = headers.at(index);
        if (!item.is_object()) {
            invalid("package dialect.slice_headers entries must be objects");
        }
        const auto name = required_text(item, "name", "slice header");
        const auto value = required_text(item, "value", "slice header", true);
        const std::string context =
            "target.package_dialect.slice_headers[" + std::to_string(index) + "]";
        validate_xml10_text(name, context + ".name");
        validate_xml10_text(value, context + ".value");
        if (!names.insert(name).second) {
            invalid("package dialect.slice_headers contains duplicate names");
        }
        auto *header_item = document.NewElement("header_item");
        header_item->SetAttribute("key", name.c_str());
        header_item->SetAttribute("value", value.c_str());
        header->InsertEndChild(header_item);
    }
    if (component_plan.value("slice_uuid", false)) {
        std::string uuid_value;
        if (request.contains("source_slice_info_xml")) {
            const auto source_xml = required_text(
                request, "source_slice_info_xml", "metadata request");
            tinyxml2::XMLDocument source_document;
            if (source_document.Parse(source_xml.data(), source_xml.size()) !=
                tinyxml2::XML_SUCCESS) {
                invalid("metadata request.source_slice_info_xml is invalid XML");
            }
            auto *config = source_document.FirstChildElement("config");
            auto *source_header = config == nullptr
                                      ? nullptr
                                      : config->FirstChildElement("header");
            auto *source_uuid = source_header == nullptr
                                    ? nullptr
                                    : source_header->FirstChildElement("uuid");
            const char *value = source_uuid == nullptr
                                    ? nullptr
                                    : source_uuid->Attribute("value");
            if (value == nullptr || *value == '\0') {
                invalid("metadata request.source_slice_info_xml has no uuid");
            }
            uuid_value = value;
        } else {
            uuid_value = required_text(request, "slice_uuid", "metadata request");
        }
        auto *uuid = document.NewElement("uuid");
        uuid->SetAttribute("value", uuid_value.c_str());
        header->InsertEndChild(uuid);
    }
    return serialize_xml(document);
}

std::string build_plate_summary(const json &input, const json &plan) {
    const json &bbox = required_array(input, "bbox_all", "plate summary");
    const json &input_colors = required_array(input, "filament_colors", "plate summary");
    const json colors = plan.value("filament_entries", std::string()) == "empty"
                            ? json::array() : input_colors;
    json filament_ids = json::array();
    for (std::size_t index = 0; index < colors.size(); ++index) {
        filament_ids.push_back(index);
    }
    const auto make_object = [&](const json &bounds, std::uint64_t index,
                                 const std::string &name) {
        const double width = bounds.at(2).get<double>() - bounds.at(0).get<double>();
        const double height = bounds.at(3).get<double>() - bounds.at(1).get<double>();
        return json{{"area", std::max(0.0, width) * std::max(0.0, height)},
                    {"bbox", bounds}, {"id", index},
                    {"layer_height", required_member(input, "layer_height", "plate summary")},
                    {"name", name}};
    };
    json objects = json::array();
    if (input.contains("objects")) {
        for (const auto &object : required_array(input, "objects", "plate summary")) {
            objects.push_back(make_object(
                required_array(object, "bbox", "plate summary object"),
                required_id(object, "model_index", "plate summary object"),
                required_text(object, "name", "plate summary object")));
        }
    } else {
        objects.push_back(make_object(bbox, 1, required_text(input, "name", "plate summary")));
    }
    const json payload = {
        {"bbox_all", bbox},
        {"bbox_objects", std::move(objects)},
        {"bed_type", plan.value("bed_type_source", std::string()) == "input"
                         ? required_text(input, "bed_type", "plate summary")
                         : required_text(plan, "bed_type", "target plate summary")},
        {"filament_colors", colors},
        {"filament_ids", std::move(filament_ids)},
        {"first_extruder", 0},
        {"first_layer_time", 0.0},
        {"is_seq_print", false},
        {"nozzle_diameter", required_member(input, "nozzle_diameter", "plate summary")},
        {"version", 2},
    };
    return payload.dump(2, ' ', false);
}

json validated_target_root(const json &dialect) {
    const json &namespaces = required_array(dialect, "namespaces", "package dialect");
    const json &metadata = required_array(dialect, "root_metadata", "package dialect");
    const json &external_names = required_array(
        dialect, "root_external_metadata_names", "package dialect", true);
    const std::string merge_policy =
        required_text(dialect, "root_metadata_merge_policy", "package dialect");
    std::set<std::string> prefixes;
    for (const auto &item : namespaces) {
        if (!item.is_object()) {
            invalid("package dialect.namespaces entries must be objects");
        }
        const auto prefix = required_text(item, "prefix", "namespace");
        required_text(item, "uri", "namespace");
        if (!prefixes.insert(prefix).second) {
            invalid("package dialect.namespaces contains duplicate prefixes");
        }
    }
    std::set<std::string> metadata_names;
    for (const auto &item : metadata) {
        if (!item.is_object()) {
            invalid("package dialect.root_metadata entries must be objects");
        }
        const auto name = required_text(item, "name", "root metadata");
        required_text(item, "value", "root metadata", true);
        if (!metadata_names.insert(name).second) {
            invalid("package dialect.root_metadata contains duplicate names");
        }
    }
    json external_metadata = json::array();
    std::set<std::string> external_name_set;
    for (const auto &external_name_value : external_names) {
        if (!external_name_value.is_string() || external_name_value.empty()) {
            invalid("package dialect.root_external_metadata_names entries must be non-empty text");
        }
        const std::string external_name = external_name_value.get<std::string>();
        if (!external_name_set.insert(external_name).second) {
            invalid("package dialect.root_external_metadata_names contains duplicate names");
        }
        const auto found = std::find_if(
            metadata.begin(), metadata.end(), [&](const json &item) {
                return item.at("name").get<std::string>() == external_name;
            });
        if (found == metadata.end()) {
            invalid("package dialect.root_external_metadata_names references unknown root metadata: " +
                    external_name);
        }
        external_metadata.push_back(*found);
    }
    return {{"namespaces", namespaces},
            {"metadata", metadata},
            {"external_metadata", std::move(external_metadata)},
            {"merge_policy", merge_policy}};
}

std::pair<std::string, std::string> resolve_metadata_bed(const json &project, const json &plate) {
    const std::string project_bed = optional_text(project, "curr_bed_type", "project settings");
    const std::string requested_bed = optional_text(plate, "bed_type", "request.plate");
    if (project_bed.empty() && requested_bed.empty()) {
        invalid("project settings or request.plate must provide bed_type");
    }
    if (!project_bed.empty() && !requested_bed.empty() && project_bed != requested_bed) {
        invalid("project and model metadata bed_type values conflict");
    }
    const std::string bed_type = project_bed.empty() ? requested_bed : project_bed;
    const std::string bed_type_field = project_bed.empty()
                                           ? "request.plate.bed_type"
                                           : "project.curr_bed_type";
    return {bed_type, bed_type_field};
}

void validate_merged_source_identity(const std::vector<ModelInput> &models,
                                     const json &dialect, const json &root) {
    const json &source_identity = dialect.contains("merged_source_identity")
        ? required_object(dialect, "merged_source_identity", "package dialect")
        : required_object(dialect, "source_identity", "package dialect");
    const std::string identity_name = required_text(
        source_identity, "name", "package dialect.source_identity");
    const std::string identity_value = required_text(
        source_identity, "value", "package dialect.source_identity");
    const auto current_identity = std::find_if(
        root.at("metadata").begin(), root.at("metadata").end(),
        [&](const json &item) { return item.at("name") == identity_name; });
    for (const auto &model : models) {
        const auto found = model.source_root_metadata.find(identity_name);
        if (found == model.source_root_metadata.end() ||
            (found->get<std::string>() != identity_value &&
             (current_identity == root.at("metadata").end() ||
              *found != current_identity->at("value")))) {
            invalid(model.context + " source identity does not match target " +
                    identity_name + "=" + identity_value);
        }
    }
}

}  // namespace

std::string serialize_layer_config_ranges(std::string_view data_json,
                                          double fine_layer_height_mm) {
    const json input = detail::parse_json_object<MetadataComponentsError>(data_json, "layer range data");
    const json project = {{"layer_height", fine_layer_height_mm}};
    const json plan = {{"option_fields", {
        {"default_extruder_key", "extruder"}, {"default_extruder_value", "0"},
        {"layer_height_key", "layer_height"}, {"infill_density_key", "sparse_infill_density"},
        {"infill_density_suffix", "%"}}}};
    return build_layer_config_ranges(input, project, plan);
}

std::string compose_model_metadata(std::string_view project_json,
                                   std::string_view request_json,
                                   std::string_view target_json) {
    const json project = detail::parse_json_object<MetadataComponentsError>(project_json, "project settings");
    const json request = detail::parse_json_object<MetadataComponentsError>(request_json, "metadata request");
    const json target = detail::parse_json_object<MetadataComponentsError>(target_json, "target data");
    const json &slot_values = required_array(project, "filament_settings_id",
                                             "project settings");
    const std::size_t slot_count = slot_values.size();
    const json &dialect = required_object(target, "package_dialect", "target data");
    const json &contract = required_object(target, "target_contract", "target data");
    const auto slicer_id = required_text(contract, "slicer_id", "target contract");
    const auto version = required_text(contract, "application_version", "target contract");
    if (!detail::supports_target(slicer_id, version)) {
        invalid("unsupported metadata target slicer/version: " + slicer_id + "/" + version);
    }
    if (required_text(request, "slicer_id", "metadata request") != slicer_id ||
        required_text(request, "application_version", "metadata request") != version) {
        invalid("request slicer/version does not match the target data");
    }
    const json &settings_parts =
        required_object(dialect, "settings_parts", "package dialect");
    const std::string model_path = required_text(
        settings_parts, "model_settings", "settings parts");
    const std::string slice_path = required_text(
        settings_parts, "slice_info", "settings parts");
    const std::string project_path = required_text(
        settings_parts, "project_settings", "settings parts");
    if (model_path == slice_path || model_path == project_path || slice_path == project_path) {
        invalid("package dialect settings-part paths must be distinct");
    }
    validate_reference(model_path, "target settings parts.model_settings");
    validate_reference(slice_path, "target settings parts.slice_info");
    validate_reference(project_path, "target settings parts.project_settings");
    const json &component_plan = required_object(
        dialect, "metadata_components", "package dialect");
    bool emit_lumina_merged_slots_json = false;
    if (request.contains("output_options")) {
        const json &output_options = required_object(
            request, "output_options", "metadata request");
        if (output_options.contains("emit_lumina_merged_slots_json")) {
            const json &selection = output_options.at("emit_lumina_merged_slots_json");
            if (!selection.is_boolean()) {
                invalid(
                    "request.output_options.emit_lumina_merged_slots_json must be boolean");
            }
            emit_lumina_merged_slots_json = selection.get<bool>();
        }
    }
    const auto [multi_model, models] = parse_model_inputs(
        request, slot_count, emit_lumina_merged_slots_json);
    if (emit_lumina_merged_slots_json && !multi_model) {
        invalid(
            "request.output_options.emit_lumina_merged_slots_json requires merged objects");
    }
    const auto object_count = models.size();
    const std::vector<ResourcePartPlan> resource_plan =
        parse_resource_part_plan(component_plan, request);
    const json model_resources = build_model_resource_paths(resource_plan);
    const json content_types = validate_content_types(component_plan);
    const json &plate = required_object(request, "plate", "request");
    const auto [bed_type, bed_type_field] = resolve_metadata_bed(project, plate);
    const json root = validated_target_root(dialect);
    if (multi_model) validate_merged_source_identity(models, dialect, root);
    const std::string model_xml = multi_model
                                      ? build_merged_model_settings(
                                            models, bed_type, model_resources, slot_count,
                                            emit_lumina_merged_slots_json)
                                      : build_model_settings(
                                            request, models.front().parts, bed_type,
                                            bed_type_field, model_resources, component_plan,
                                            slicer_id == "QIDIStudio");
    const std::string slice_xml = build_slice_info(dialect, request, component_plan);

    const std::string settings_media_type = required_text(
        component_plan, "settings_media_type", "target metadata components");
    MetadataParts package_parts;
    package_parts.add_text("model_settings", model_path, settings_media_type, model_xml);
    package_parts.add_text("project_settings", project_path, settings_media_type,
                           std::string(project_json));
    package_parts.add_text("slice_info", slice_path, settings_media_type, slice_xml);

    const json &component_inputs = required_object(
        request, "component_inputs", "metadata request");
    if (component_is_included(component_plan, "plate_summary", object_count) &&
        component_inputs.contains("plate_summary")) {
        const json &summary_plan = required_object(
            component_plan, "plate_summary", "target metadata components");
        package_parts.add_text(
            "plate_summary",
            required_text(summary_plan, "path", "target plate summary"),
            required_text(summary_plan, "media_type", "target plate summary"),
            build_plate_summary(
                required_object(component_inputs, "plate_summary", "request.component_inputs"),
                summary_plan));
    }
    const json &wipe_plan = required_object(
        component_plan, "wipe_tower_placement", "target metadata components");
    if (component_inputs.contains("wipe_tower_placement")) {
        const json &wipe_input = required_object(
            component_inputs, "wipe_tower_placement", "request.component_inputs");
        package_parts.add_text(
            "wipe_tower_placement",
            required_text(wipe_plan, "path", "target wipe tower placement"),
            required_text(wipe_plan, "media_type", "target wipe tower placement"),
            wipe_input.dump(2, ' ', false));
    }

    if (component_is_included(component_plan, "filament_sequence", object_count)) {
        const json &sequence_plan = required_object(
            component_plan, "filament_sequence", "target metadata components");
        const auto plate_key = required_text(
            sequence_plan, "plate_key", "target filament sequence");
        const auto sequence_key = required_text(
            sequence_plan, "sequence_key", "target filament sequence");
        json plate_sequence = {{sequence_key, json::array()}};
        if ((multi_model || slicer_id == "QIDIStudio") &&
            sequence_plan.contains("merge_sequence_keys")) {
            for (const auto &key : sequence_plan.at("merge_sequence_keys")) {
                plate_sequence[key.get<std::string>()] = json::array();
            }
        }
        const json sequence = {{plate_key, std::move(plate_sequence)}};
        package_parts.add_text(
            "filament_sequence",
            required_text(sequence_plan, "path", "target filament sequence"),
            required_text(sequence_plan, "media_type", "target filament sequence"),
            sequence.dump());
    }

    if (component_is_included(component_plan, "cut_information", object_count)) {
        const json &cut_plan = required_object(
            component_plan, "cut_information", "target metadata components");
        package_parts.add_text(
            "cut_information",
            required_text(cut_plan, "path", "target cut information"),
            required_text(cut_plan, "media_type", "target cut information"),
            build_cut_information(models, multi_model, cut_plan));
    }

    if (component_plan.contains("custom_gcode_per_layer")) {
        const json &custom_plan = required_object(
            component_plan, "custom_gcode_per_layer", "target metadata components");
        package_parts.add_text(
            "custom_gcode_per_layer",
            required_text(custom_plan, "path", "target custom gcode per layer"),
            required_text(custom_plan, "media_type", "target custom gcode per layer"),
            build_custom_gcode_per_layer(custom_plan));
    }

    if (multi_model) {
        const json &layer_plan = required_object(
            component_plan, "layer_config_ranges", "target metadata components");
        const std::string content = build_merged_layer_config_ranges(models, layer_plan, slot_count);
        if (!content.empty()) {
            package_parts.add_text(
                "layer_config_ranges",
                required_text(layer_plan, "path", "target layer config ranges"),
                required_text(layer_plan, "media_type", "target layer config ranges"),
                content);
        }
    } else if (component_inputs.contains("layer_config_ranges") &&
               !component_inputs.at("layer_config_ranges").is_null()) {
        const json &layer_plan = required_object(
            component_plan, "layer_config_ranges", "target metadata components");
        const std::string content = build_layer_config_ranges(
            component_inputs.at("layer_config_ranges"), project, layer_plan);
        if (!content.empty()) {
            package_parts.add_text(
                "layer_config_ranges",
                required_text(layer_plan, "path", "target layer config ranges"),
                required_text(layer_plan, "media_type", "target layer config ranges"),
                content);
        }
    }

    for (const auto &resource : resource_plan) {
        package_parts.add_resource(resource);
    }
    const auto relationships = build_metadata_relationships(
        component_plan, object_count, resource_plan, package_parts);
    json output = {{"parts", package_parts.take_parts()},
                   {"relationships", relationships},
                   {"content_types", content_types},
                   {"root_model", root},
                   {"settings_parts", {{"model_settings", model_path},
                                        {"project_settings", project_path},
                                        {"slice_info", slice_path}}},
                   {"project_bed_type", bed_type},
                   {"model_settings_bed_type", bed_type}};
    if (multi_model) output["build_items"] = build_metadata_build_items(models, component_plan);
    return output.dump();
}

}  // namespace fatcat
