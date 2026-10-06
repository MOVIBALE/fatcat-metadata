#include "fatcat/source_material_slots.h"
#include "fatcat/source_project_settings.h"
#include "source_identity.h"
#include "prusa_project.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <iterator>
#include <cmath>
#include <regex>
#include <set>
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

[[noreturn]] void invalid(const std::string &message) {
    throw SourceMaterialSlotsError(message);
}

json parse_json(std::string_view text, const char *label) {
    try {
        return json::parse(text.begin(), text.end());
    } catch (const json::exception &error) {
        invalid(std::string(label) + " is invalid JSON: " + error.what());
    }
}

const json &required_object_member(const json &object, const char *key,
                                   const char *context) {
    if (!object.is_object() || !object.contains(key) ||
        !object.at(key).is_object()) {
        invalid(std::string(context) + "." + key + " must be an object");
    }
    return object.at(key);
}

std::string required_text_member(const json &object, const char *key,
                                 const char *context) {
    if (!object.is_object() || !object.contains(key) ||
        !object.at(key).is_string() ||
        object.at(key).get_ref<const std::string &>().empty()) {
        invalid(std::string(context) + "." + key + " must be non-empty text");
    }
    return object.at(key).get<std::string>();
}

json optional_array(const json &object, const char *key) {
    if (!object.contains(key) || object.at(key).is_null()) {
        return json::array();
    }
    if (!object.at(key).is_array()) {
        invalid(std::string("project_settings.") + key + " must be an array");
    }
    return object.at(key);
}

std::string normalized_color(const json &value) {
    if (!value.is_string()) {
        return "#CCCCCC";
    }
    std::string color = value.get<std::string>();
    color.erase(color.begin(),
                std::find_if(color.begin(), color.end(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }));
    color.erase(std::find_if(color.rbegin(), color.rend(),
                             [](unsigned char ch) { return !std::isspace(ch); })
                    .base(),
                color.end());
    if (color.empty()) {
        return "#CCCCCC";
    }
    if (color.front() != '#') {
        color.insert(color.begin(), '#');
    }
    std::transform(color.begin(), color.end(), color.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::toupper(ch));
                   });
    if (color.size() != 7) {
        return "#CCCCCC";
    }
    return color;
}

std::optional<std::string> material_identity(const json &values,
                                             std::size_t index) {
    if (index >= values.size() || values.at(index).is_null()) {
        return std::nullopt;
    }
    const auto identity = values.at(index).is_string()
                              ? values.at(index).get<std::string>()
                              : values.at(index).dump();
    if (identity.empty()) {
        return std::nullopt;
    }
    return identity;
}

std::optional<std::size_t> parse_extruder_index(const std::string &text) {
    auto begin = text.begin();
    auto end = text.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    if (begin != end && *begin == '+') {
        ++begin;
    }
    if (begin == end) {
        return std::nullopt;
    }
    const auto first = text.data() + std::distance(text.begin(), begin);
    const auto last = text.data() + std::distance(text.begin(), end);
    long long extruder = 0;
    const auto result = std::from_chars(first, last, extruder);
    if (result.ec != std::errc{} || result.ptr != last || extruder < 1) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(extruder - 1);
}

template <typename Visitor>
void visit_parts(const tinyxml2::XMLElement *parent, Visitor visitor) {
    for (const auto *element = parent->FirstChildElement(); element != nullptr;
         element = element->NextSiblingElement()) {
        if (std::string_view(element->Name()) == "part") {
            visitor(element);
        }
        visit_parts(element, visitor);
    }
}

const tinyxml2::XMLElement *read_model_settings(
    tinyxml2::XMLDocument &document, std::string_view xml) {
    if (document.Parse(xml.data(), xml.size()) != tinyxml2::XML_SUCCESS ||
        document.RootElement() == nullptr) {
        invalid("Metadata/model_settings.config is malformed");
    }
    return document.RootElement();
}

std::vector<std::optional<std::string>> part_names(
    const tinyxml2::XMLElement *model_settings, std::size_t slot_count) {
    std::vector<std::optional<std::string>> names(slot_count);
    visit_parts(model_settings, [&](const tinyxml2::XMLElement *part) {
        std::optional<std::string> name;
        std::optional<std::string> extruder;
        for (const auto *metadata = part->FirstChildElement("metadata");
             metadata != nullptr;
             metadata = metadata->NextSiblingElement("metadata")) {
            const char *key = metadata->Attribute("key");
            const char *value = metadata->Attribute("value");
            if (key == nullptr) {
                continue;
            }
            if (std::string_view(key) == "name" && value != nullptr) {
                name = value;
            } else if (std::string_view(key) == "extruder" && value != nullptr) {
                extruder = value;
            }
        }
        if (!extruder.has_value() || !name.has_value()) {
            return;
        }
        const auto index = parse_extruder_index(*extruder);
        if (!index.has_value()) {
            return;
        }
        if (*index >= names.size()) {
            names.resize(*index + 1);
        }
        if (!names[*index].has_value()) {
            names[*index] = *name;
        }
    });
    return names;
}

std::string scalar_text(const json &value) {
    if (value.is_null()) {
        return {};
    }
    return value.is_string() ? value.get<std::string>() : value.dump();
}

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::optional<double> numeric_value(const json &value) {
    const auto &candidate = value.is_array() && !value.empty() ? value.front() : value;
    if (candidate.is_number()) {
        return candidate.get<double>();
    }
    if (candidate.is_boolean()) {
        return candidate.get<bool>() ? 1.0 : 0.0;
    }
    if (!candidate.is_string()) {
        return std::nullopt;
    }
    const auto text = trim(candidate.get<std::string>());
    try {
        std::size_t consumed = 0;
        const auto result = std::stod(text, &consumed);
        if (consumed == text.size()) {
            return result;
        }
    } catch (const std::invalid_argument &) {
    } catch (const std::out_of_range &) {
    }
    return std::nullopt;
}

double number(const json &value, double fallback = 0.0) {
    return numeric_value(value).value_or(fallback);
}

std::vector<double> numbers(const json &value) {
    std::vector<double> result;
    const auto values = value.is_array() ? value : json::array({value});
    for (const auto &item : values) {
        result.push_back(number(item));
    }
    return result;
}

double largest(const json &value, double fallback = 0.0) {
    const auto values = numbers(value);
    return values.empty() ? fallback : *std::max_element(values.begin(), values.end());
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

using Point = std::pair<double, double>;
using Polygon = std::vector<Point>;

Polygon polygon_item(const json &value) {
    const auto text = trim(scalar_text(value));
    static const std::regex pair_pattern(
        R"(([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)\s*[xX]\s*([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?))");
    Polygon result;
    for (auto match = std::sregex_iterator(text.begin(), text.end(), pair_pattern);
         match != std::sregex_iterator(); ++match) {
        result.emplace_back(std::stod((*match)[1].str()), std::stod((*match)[2].str()));
    }
    if (!result.empty()) {
        return result;
    }
    static const std::regex separator(R"([xX,;\s]+)");
    const std::vector<std::string> parts(
        std::sregex_token_iterator(text.begin(), text.end(), separator, -1),
        std::sregex_token_iterator{});
    if (parts.size() >= 2) {
        const auto x = numeric_value(parts[0]);
        const auto y = numeric_value(parts[1]);
        if (x.has_value() && y.has_value()) {
            result.emplace_back(*x, *y);
        }
    }
    return result;
}

Polygon polygon(const json &value) {
    Polygon result;
    if (value.is_array()) {
        for (const auto &item : value) {
            const auto points = polygon_item(item);
            result.insert(result.end(), points.begin(), points.end());
        }
    }
    return result;
}

double signed_area_twice(const Polygon &points) {
    double area = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto &next = points[(i + 1) % points.size()];
        area += points[i].first * next.second - next.first * points[i].second;
    }
    return area;
}

bool repeated_sentinel(const Polygon &points) {
    return std::set<Point>(points.begin(), points.end()).size() < points.size() &&
           std::abs(signed_area_twice(points)) <= 1e-9;
}

bool axis_aligned_rectangle(const Polygon &points) {
    std::set<double> xs;
    std::set<double> ys;
    for (const auto &[x, y] : points) {
        xs.insert(x);
        ys.insert(y);
    }
    return points.size() == 4 && std::set<Point>(points.begin(), points.end()).size() == 4 &&
           xs.size() == 2 && ys.size() == 2 && std::abs(signed_area_twice(points)) > 1e-9;
}

std::vector<Polygon> split_exclusions(const Polygon &points, bool require_sentinels) {
    if (points.size() < 3) {
        return {};
    }
    if (points.size() <= 7) {
        return {points};
    }
    const auto remainder = points.size() % 4;
    if (remainder == 0 && !require_sentinels) {
        std::vector<Polygon> result;
        for (std::size_t i = 0; i < points.size(); i += 4) {
            result.emplace_back(points.begin() + i, points.begin() + i + 4);
        }
        return result;
    }
    const auto prefix_size = points.size() - (remainder == 0 ? 4 : 4 + remainder);
    std::vector<Polygon> result;
    int sentinels = 0;
    bool recognized = true;
    for (std::size_t i = 0; i < prefix_size; i += 4) {
        Polygon group(points.begin() + i, points.begin() + i + 4);
        const auto sentinel = repeated_sentinel(group);
        sentinels += sentinel ? 1 : 0;
        recognized = recognized && (sentinel || axis_aligned_rectangle(group));
        result.push_back(std::move(group));
    }
    Polygon tail(points.begin() + prefix_size, points.end());
    if (sentinels >= 2 && recognized && std::abs(signed_area_twice(tail)) > 1e-9) {
        result.push_back(std::move(tail));
        return result;
    }
    return {points};
}

std::vector<Polygon> bed_exclusions(const json &value) {
    if (!value.is_array()) {
        return {};
    }
    const auto packed = std::any_of(value.begin(), value.end(), [](const json &item) {
        return scalar_text(item).find(',') != std::string::npos;
    });
    if (!packed) {
        return split_exclusions(polygon(value), false);
    }
    std::vector<Polygon> result;
    for (const auto &item : value) {
        const auto groups = split_exclusions(polygon_item(item), true);
        result.insert(result.end(), groups.begin(), groups.end());
    }
    return result;
}

json layer_limit(const json &value, bool minimum) {
    auto values = numbers(value);
    values.erase(std::remove_if(values.begin(), values.end(), [](double item) {
        return !std::isfinite(item) || item <= 0.0;
    }), values.end());
    if (values.empty()) {
        return nullptr;
    }
    return minimum ? *std::max_element(values.begin(), values.end())
                   : *std::min_element(values.begin(), values.end());
}

std::string_view local_name(std::string_view name) {
    const auto separator = name.find_last_of(':');
    return separator == std::string_view::npos ? name : name.substr(separator + 1);
}

json attributes(const tinyxml2::XMLElement *element) {
    json result = json::object();
    for (const auto *attribute = element->FirstAttribute(); attribute != nullptr;
         attribute = attribute->Next()) {
        result[attribute->Name()] = attribute->Value();
    }
    return result;
}

json metadata_values(const tinyxml2::XMLElement *parent) {
    json result = json::object();
    for (const auto *element = parent->FirstChildElement(); element != nullptr;
         element = element->NextSiblingElement()) {
        if (local_name(element->Name()) != "metadata") {
            continue;
        }
        const char *key = element->Attribute("key");
        if (key == nullptr) {
            key = element->Attribute("name");
        }
        const char *value = element->Attribute("value");
        if (value == nullptr) {
            value = element->GetText();
        }
        if (key != nullptr) {
            result[key] = value == nullptr ? "" : value;
        }
    }
    return result;
}

json metadata_record(const tinyxml2::XMLElement *element) {
    return {{"attributes", attributes(element)}, {"metadata", metadata_values(element)}};
}

json model_records(const tinyxml2::XMLElement *model_settings) {
    json objects = json::array();
    json plates = json::array();
    for (const auto *element = model_settings->FirstChildElement();
         element != nullptr; element = element->NextSiblingElement()) {
        const auto tag = local_name(element->Name());
        if (tag != "object" && tag != "plate") {
            continue;
        }
        auto record = metadata_record(element);
        if (tag == "object") {
            record["id"] = element->Attribute("id") == nullptr ? "" : element->Attribute("id");
            record["parts"] = json::array();
            for (const auto *part = element->FirstChildElement(); part != nullptr;
                 part = part->NextSiblingElement()) {
                if (local_name(part->Name()) != "part") {
                    continue;
                }
                auto part_record = metadata_record(part);
                part_record["id"] = part->Attribute("id") == nullptr ? "" : part->Attribute("id");
                part_record["mesh_stat"] = nullptr;
                for (const auto *child = part->FirstChildElement(); child != nullptr;
                     child = child->NextSiblingElement()) {
                    if (local_name(child->Name()) == "mesh_stat") {
                        part_record["mesh_stat"] = attributes(child);
                    }
                }
                record["parts"].push_back(std::move(part_record));
            }
            objects.push_back(std::move(record));
        } else {
            record["model_instances"] = json::array();
            for (const auto *instance = element->FirstChildElement(); instance != nullptr;
                 instance = instance->NextSiblingElement()) {
                if (local_name(instance->Name()) == "model_instance") {
                    record["model_instances"].push_back(metadata_record(instance));
                }
            }
            plates.push_back(std::move(record));
        }
    }
    return {{"objects", objects}, {"plates", plates}};
}

void collect_slice_headers(const tinyxml2::XMLElement *parent, json &headers) {
    for (const auto *element = parent->FirstChildElement(); element != nullptr;
         element = element->NextSiblingElement()) {
        if (local_name(element->Name()) == "header_item") {
            headers.push_back(attributes(element));
        }
        collect_slice_headers(element, headers);
    }
}

json slice_headers(const tinyxml2::XMLElement *slice_info) {
    json result = json::array();
    if (slice_info != nullptr) collect_slice_headers(slice_info, result);
    return result;
}

json settings_parts(const json &target) {
    const auto &dialect = required_object_member(target, "package_dialect", "target");
    const auto &parts = required_object_member(dialect, "settings_parts", "target.package_dialect");
    return json{{"model_settings",
                 required_text_member(parts, "model_settings",
                                      "target.package_dialect.settings_parts")},
                {"project_settings",
                 required_text_member(parts, "project_settings",
                                      "target.package_dialect.settings_parts")}};
}

json source_project(std::string_view project_json) {
    auto project = parse_json(project_json, "Metadata/project_settings.config");
    if (!project.is_object()) {
        invalid("Metadata/project_settings.config must be a JSON object");
    }
    return project;
}

json material_slots(const json &project, std::string_view model_settings_xml,
                    tinyxml2::XMLDocument &document) {
    const auto colors = optional_array(project, "filament_colour");
    const auto filament_ids = optional_array(project, "filament_ids");
    const auto settings_ids = optional_array(project, "filament_settings_id");
    const auto names = part_names(read_model_settings(document, model_settings_xml),
        std::max({colors.size(), filament_ids.size(), settings_ids.size(), std::size_t{1}}));
    const auto slot_count = names.size();
    json result = json::array();
    for (std::size_t index = 0; index < slot_count; ++index) {
        auto identity = material_identity(filament_ids, index);
        if (!identity.has_value()) {
            identity = material_identity(settings_ids, index);
        }
        if (!identity.has_value()) {
            const auto number = std::to_string(index);
            identity = std::string("GFA") + (number.size() < 2 ? "0" : "") + number;
        }
        const auto slot_name = names[index].has_value() && !names[index]->empty()
                                   ? *names[index]
                                   : "Slot " + std::to_string(index + 1);
        const auto color = index < colors.size() ? normalized_color(colors.at(index))
                                                 : "#CCCCCC";
        result.push_back({{"slot_id", index},
                          {"slot_name", slot_name},
                          {"preview_color", color},
                          {"material_id", *identity}});
    }
    return result;
}

json project_layout(json project, const json &target) {
    const auto slicer = required_text_member(
        required_object_member(target, "target_contract", "target"),
        "slicer_id", "target.target_contract");
    detail::apply_source_tower_defaults(project, target);
    json placement_geometry = json::object();
    const auto package = target.find("package_dialect");
    if (package != target.end() && package->is_object() &&
        package->contains("wipe_tower_placement")) {
        placement_geometry = package->at("wipe_tower_placement");
        if (!placement_geometry.is_object()) {
            invalid("target.package_dialect.wipe_tower_placement must be an object");
        }
    }
    const auto value = [&](const std::string &key) { return project.value(key, json(nullptr)); };
    const auto nozzles = value("nozzle_diameter");
    const auto rib_prefix = slicer == "BambuStudio" ? "prime_tower" : "wipe_tower";
    const auto wall_type_key = placement_geometry.value("wall_type_key", std::string());
    bool uses_rib = false;
    if (!wall_type_key.empty()) {
        uses_rib = lowercase(scalar_text(value(wall_type_key))) == lowercase(
            placement_geometry.value("wall_type_rib_value", std::string("rib")));
    } else {
        const auto rib_value = lowercase(scalar_text(value(std::string(rib_prefix) + "_rib_wall")));
        uses_rib = (slicer == "OrcaSlicer" || slicer == "ElegooSlicer" ||
                    slicer == "SnapmakerOrca")
                       ? lowercase(scalar_text(value("wipe_tower_wall_type"))) == "rib"
                       : rib_value == "1" || rib_value == "true";
    }
    const auto filament_prime = largest(value("filament_prime_volume"));
    double purge_volume = 45.0;
    if (slicer == "BambuStudio" && filament_prime > 0.0) {
        purge_volume = std::max(45.0, filament_prime);
    } else if (const auto prime = number(value("prime_volume"), -1.0); prime > 0.0) {
        purge_volume = prime;
    } else if (filament_prime > 0.0) {
        purge_volume = filament_prime;
    } else if (const auto flush = largest(value("flush_volumes_vector")); flush > 0.0) {
        purge_volume = flush;
    }
    const auto change_length = largest(value("filament_change_length"));
    auto filament_diameter = largest(value("filament_diameter"), 1.75);
    if (filament_diameter <= 0.0) {
        filament_diameter = 1.75;
    }
    const auto brim_type = lowercase(trim(scalar_text(value("brim_type"))));
    const auto no_model_brim = brim_type == "no_brim" || brim_type == "none" || brim_type == "0";
    const auto tower_brim = number(value("prime_tower_brim_width"));
    const auto spacing_key = placement_geometry.value(
        "depth_spacing_key", std::string("prime_tower_infill_gap"));
    auto infill = value(spacing_key);
    if (infill.is_array() && !infill.empty()) {
        infill = infill.front();
    }
    auto ratio_text = trim(scalar_text(infill));
    const auto percent = !ratio_text.empty() && ratio_text.back() == '%';
    if (percent) {
        ratio_text.pop_back();
    }
    auto infill_ratio = number(ratio_text, 1.0);
    if (percent || infill_ratio > 10.0) {
        infill_ratio /= 100.0;
    }
    std::vector<Polygon> extruder_areas;
    const auto extruder_area_values = value("extruder_printable_area");
    if (extruder_area_values.is_array()) {
        for (const auto &item : extruder_area_values) {
            auto points = polygon_item(item);
            if (points.size() >= 3) {
                extruder_areas.push_back(std::move(points));
            }
        }
    }
    const auto initial_height = numeric_value(value("initial_layer_print_height"));
    const auto rib_width_key = placement_geometry.value(
        "rib_width_key", std::string(rib_prefix) + "_rib_width");
    const auto extra_rib_length_key = placement_geometry.value(
        "extra_rib_length_key", std::string(rib_prefix) + "_extra_rib_length");
    const auto square_rib_body = placement_geometry.contains("rib_body_geometry")
        ? json(uses_rib && placement_geometry.at("rib_body_geometry") == "square_from_planned_depth")
        : json(nullptr);
    bool preview_x_guard_enabled = false;
    double preview_x_guard_margin = 0.0;
    if (placement_geometry.contains("native_preview_x_guard")) {
        const auto &guard = placement_geometry.at("native_preview_x_guard");
        if (!guard.is_object()) {
            invalid("target.package_dialect.wipe_tower_placement.native_preview_x_guard must be an object");
        }
        const auto brim_key = guard.value("brim_key", std::string());
        preview_x_guard_enabled = true;
        preview_x_guard_margin = guard.value("base_margin_mm", 0.0) +
            number(value(brim_key));
    }
    return json{
        {"printer_model", value("printer_model")},
        {"printer_settings_id", value("printer_settings_id")},
        {"bed_type", value("curr_bed_type")},
        {"printable_area", polygon(value("printable_area"))},
        {"bed_exclusions", bed_exclusions(value("bed_exclude_area"))},
        {"extruder_printable_areas", extruder_areas},
        {"nozzle_diameters", numbers(nozzles)},
        {"extruder_count", nozzles.is_array() && !nozzles.empty() ? nozzles.size() : 1},
        {"layer_height", std::max(0.01, number(value("layer_height"), 0.08))},
        {"initial_layer_height", initial_height.has_value() ? json(*initial_height) : json(nullptr)},
        {"min_layer_height", layer_limit(value("min_layer_height"), true)},
        {"max_layer_height", layer_limit(value("max_layer_height"), false)},
        {"model_brim", no_model_brim ? 0.0 : std::max(0.0, number(value("brim_width")))},
        {"tower_x", number(value("wipe_tower_x"), 15.0)},
        {"tower_y", number(value("wipe_tower_y"), 220.0)},
        {"tower_width", std::max(2.0, number(value("prime_tower_width"), 35.0))},
        {"tower_rotation", number(value("wipe_tower_rotation_angle"))},
        {"tower_brim_width", tower_brim},
        {"tower_brim_auto", tower_brim < 0.0},
        {"purge_volume", purge_volume},
        {"filament_change_volume", change_length * std::acos(-1.0) * filament_diameter * filament_diameter / 4.0},
        {"tower_infill_spacing_ratio", std::max(0.01, infill_ratio)},
        {"uses_rib_wall", uses_rib},
        {"tower_rib_width", std::max(0.0, number(value(rib_width_key)))},
        {"tower_extra_rib_length", number(value(extra_rib_length_key))},
        {"tower_square_rib_body", square_rib_body},
        {"tower_preview_x_guard_enabled", preview_x_guard_enabled},
        {"tower_preview_x_guard_margin", preview_x_guard_margin}
    };
}

json placement_warnings(std::optional<std::string_view> placement_json) {
    json warnings = json::array();
    if (!placement_json.has_value()) {
        return warnings;
    }
    const auto placement = json::parse(placement_json->begin(), placement_json->end(), nullptr, false);
    if (!placement.is_object() || !placement.contains("requires_manual_adjustment")) {
        return warnings;
    }
    const auto &requires_adjustment = placement.at("requires_manual_adjustment");
    const auto enabled = !requires_adjustment.is_null() &&
        (requires_adjustment.is_boolean() ? requires_adjustment.get<bool>()
         : requires_adjustment.is_number() ? requires_adjustment != 0
         : !requires_adjustment.empty());
    if (enabled && placement.contains("warning_code") && placement.at("warning_code").is_string()) {
        const auto code = trim(placement.at("warning_code").get<std::string>());
        if (!code.empty()) {
            warnings.push_back(code);
        }
    }
    return warnings;
}

}  // namespace

std::string source_material_settings_parts(std::string_view target_json) {
    return settings_parts(parse_json(target_json, "target data")).dump();
}

std::string extract_source_material_slots(std::string_view project_json,
                                         std::string_view model_settings_xml,
                                         std::string_view target_json) {
    (void)settings_parts(parse_json(target_json, "target data"));
    const auto project = source_project(project_json);
    if (detail::prusa::is_target(parse_json(target_json, "target data"))) {
        return detail::prusa::material_slots(detail::prusa::normalized_project(project)).dump();
    }
    tinyxml2::XMLDocument document;
    return material_slots(project, model_settings_xml, document).dump();
}

std::string read_project_layout(std::string_view project_json,
                                std::string_view target_json) {
    auto project = parse_json(project_json, "project settings");
    const auto target = parse_json(target_json, "target data");
    if (detail::prusa::is_target(target)) {
        const auto native = detail::prusa::normalized_project(project);
        return project_layout(detail::prusa::summary(native), target).dump();
    }
    return project_layout(std::move(project), target).dump();
}

std::string read_placement_warnings(std::optional<std::string_view> placement_json) {
    return placement_warnings(placement_json).dump();
}

std::string read_source_metadata(
    std::string_view project_json,
    std::string_view model_settings_xml,
    std::string_view target_json,
    std::optional<std::string_view> source_model_xml,
    std::optional<std::string_view> slice_info_xml,
    std::optional<std::string_view> placement_json) {
    const auto target = parse_json(target_json, "target data");
    (void)settings_parts(target);
    if (detail::prusa::is_target(target)) {
        const auto native = detail::prusa::normalized_project(source_project(project_json));
        tinyxml2::XMLDocument document;
        const auto *root = detail::read_source_xml(document, source_model_xml);
        const auto identity = detail::read_source_identity(root, nullptr, target);
        const auto slots = detail::prusa::material_slots(native);
        return json{{"project_settings", native}, {"slots", slots},
            {"project_filament_count", slots.size()},
            {"source_slicer", identity.at("source_slicer")},
            {"source_version", identity.at("source_version")},
            {"matches_selected_target_identity", identity.at("matches_selected_target_identity")},
            {"source_root_metadata", root == nullptr ? json::object() : metadata_values(root)},
            {"model_settings", {{"objects", native.at("objects")}, {"plates", json::array()}}},
            {"slice_headers", json::array()},
            {"layout", project_layout(detail::prusa::summary(native), target)},
            {"warnings", json::array()}}.dump();
    }
    const auto project = source_project(project_json);
    tinyxml2::XMLDocument settings_document, model_document, slice_document;
    const auto slots = material_slots(project, model_settings_xml, settings_document);
    const auto *source_model = detail::read_source_xml(model_document, source_model_xml);
    const auto *slice_info = detail::read_source_xml(slice_document, slice_info_xml);
    const auto identity = detail::read_source_identity(source_model, slice_info, target);
    return json{
        {"project_settings", project},
        {"slots", slots},
        {"project_filament_count", slots.size()},
        {"source_slicer", identity.at("source_slicer")},
        {"source_version", identity.at("source_version")},
        {"matches_selected_target_identity", identity.at("matches_selected_target_identity")},
        {"source_root_metadata", source_model == nullptr ? json::object() : metadata_values(source_model)},
        {"model_settings", model_records(settings_document.RootElement())},
        {"slice_headers", slice_headers(slice_info)},
        {"layout", project_layout(project, target)},
        {"warnings", placement_warnings(placement_json)}
    }.dump();
}

std::string read_model_object_metadata(std::string_view model_xml) {
    tinyxml2::XMLDocument document;
    if (document.Parse(model_xml.data(), model_xml.size()) != tinyxml2::XML_SUCCESS ||
        document.RootElement() == nullptr) {
        invalid("Source object model XML is malformed");
    }
    json result = json::array();
    for (const auto *resources = document.RootElement()->FirstChildElement();
         resources != nullptr; resources = resources->NextSiblingElement()) {
        if (local_name(resources->Name()) != "resources") {
            continue;
        }
        for (const auto *object = resources->FirstChildElement(); object != nullptr;
             object = object->NextSiblingElement()) {
            if (local_name(object->Name()) != "object") {
                continue;
            }
            json metadata = json::array();
            for (const auto *item = object->FirstChildElement(); item != nullptr;
                 item = item->NextSiblingElement()) {
                if (local_name(item->Name()) != "metadata") {
                    continue;
                }
                metadata.push_back({
                    {"name", item->Attribute("name") == nullptr ? "" : item->Attribute("name")},
                    {"value", item->GetText() == nullptr ? "" : item->GetText()},
                    {"type", item->Attribute("type") == nullptr ? "" : item->Attribute("type")}
                });
            }
            result.push_back({
                {"id", object->Attribute("id") == nullptr ? "" : object->Attribute("id")},
                {"metadata", metadata}
            });
        }
    }
    return result.dump();
}

}  // namespace fatcat
