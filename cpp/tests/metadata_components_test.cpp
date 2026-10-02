#include <cstdlib>
#include <fstream>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <tinyxml2.h>

#include "fatcat/metadata_components.h"

namespace {

using json = nlohmann::json;

class comma_decimal_punct final : public std::numpunct<char> {
protected:
    char do_decimal_point() const override { return ','; }
};

class global_locale_guard final {
public:
    global_locale_guard() : previous_(std::locale()) {}
    ~global_locale_guard() { std::locale::global(previous_); }

private:
    std::locale previous_;
};

#ifndef FATCAT_TEST_REPOSITORY_ROOT
#error "FATCAT_TEST_REPOSITORY_ROOT must point at the source checkout for data-driven tests"
#endif

std::string read_file(const std::string &relative_path) {
    std::ifstream stream(std::string(FATCAT_TEST_REPOSITORY_ROOT) + "/" +
                         relative_path);
    if (!stream) {
        throw std::runtime_error("cannot read fixture: " + relative_path);
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

json target_data() {
    return json::parse(read_file(
        "compatibility/current-src/translations/targets/"
        "bambu-studio-02.08.02.61.json"));
}

json project_data() {
    return {
        {"curr_bed_type", "Textured PEI Plate"},
        {"layer_height", "0.08"},
        {"filament_settings_id",
         json::array({"Slot A", "Slot B", "Slot C", "Slot D"})},
        {"filament_type", json::array({"PLA", "PETG", "PLA", "PLA"})},
        {"filament_colour",
         json::array({"#111111", "#222222", "#333333", "#444444"})},
        {"nozzle_temperature", json::array({"217", "218", "219", "220"})},
        {"filament_flow_ratio", json::array({"0.93", "0.94", "0.95", "0.96"})},
    };
}

json metadata_request() {
    return {
        {"slicer_id", "BambuStudio"},
        {"application_version", "02.08.02.61"},
        {"assembly_id", 7},
        {"instance_id", "9"},
        {"identify_id", "1"},
        {"source_file", "模型 & <source> \"A\".stl"},
        {"parts",
         json::array({
             {{"part_id", 11},
              {"name", "第一 <部件> & \"A\"\nline"},
              {"material_index", 0},
              {"source_object_id", 101},
              {"source_volume_id", 3},
              {"matrix", "1 0 0 1 0 1 0 2 0 0 1 3 0 0 0 1"},
              {"source_offset_x", "0.25"},
              {"source_offset_y", "-2"},
              {"source_offset_z", "3.5"}},
             {{"part_id", 42},
              {"name", "第二部件"},
              {"material_index", 2},
              {"source_object_id", 202},
              {"source_volume_id", 8},
              {"matrix", "1 0 0 4 0 1 0 5 0 0 1 6 0 0 0 1"},
              {"source_offset_x", "7"},
              {"source_offset_y", "8"},
              {"source_offset_z", "9"}},
         })},
        {"plate",
         {{"plater_id", "1"},
          {"plater_name", "盘 & A"},
          {"locked", false},
          {"bed_type", "Textured PEI Plate"},
          {"filament_map_mode", "Auto For Flush"}}},
        {"component_inputs",
         {{"wipe_tower_placement",
           {{"schema_version", 1},
            {"enabled", true},
            {"available", true},
            {"adapted", false},
            {"requires_manual_adjustment", false},
            {"warning_code", nullptr},
            {"reason", "within_printable_area"},
            {"model_translation_mm", json::array({0.0, 0.0})},
            {"tower", {{"x_mm", 210.0}, {"y_mm", 210.0}, {"width_mm", 30.0}}}}},
          {"layer_config_ranges",
           {{"model_index", 1},
            {"ranges",
             json::array({{{"min_z", 0.4},
                           {"max_z", 1.2},
                           {"layer_height_mm", 0.08},
                           {"infill_density_percent", 100}}})}}}}},
    };
}

json compose(const json &project, const json &request_data,
             const json &target = target_data()) {
    return json::parse(fatcat::compose_model_metadata(
        project.dump(), request_data.dump(), target.dump()));
}

json compose_raw(const std::string &project_json, const std::string &request_json,
                 const std::string &target_json = target_data().dump()) {
    return json::parse(fatcat::compose_model_metadata(
        project_json, request_json, target_json));
}

void expect(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
void expect_error(Function &&function, const std::string &needle) {
    try {
        function();
    } catch (const fatcat::MetadataComponentsError &error) {
        expect(std::string(error.what()).find(needle) != std::string::npos,
               "error did not mention '" + needle + "': " + error.what());
        return;
    }
    throw std::runtime_error("expected MetadataComponentsError containing " + needle);
}

const json &part_for_path(const json &result, const std::string &path) {
    for (const auto &part : result.at("parts")) {
        if (part.at("path") == path) {
            return part;
        }
    }
    throw std::runtime_error("missing metadata part: " + path);
}

const json &part_for_role(const json &result, const std::string &role) {
    for (const auto &part : result.at("parts")) {
        if (part.value("role", std::string{}) == role) {
            return part;
        }
    }
    throw std::runtime_error("missing metadata role: " + role);
}

bool has_part_role(const json &result, const std::string &role) {
    for (const auto &part : result.at("parts")) {
        if (part.value("role", std::string{}) == role) {
            return true;
        }
    }
    return false;
}

const char *layer_range_option(const tinyxml2::XMLElement *range,
                               const char *key) {
    for (const auto *option = range == nullptr ? nullptr
                                               : range->FirstChildElement("option");
         option != nullptr; option = option->NextSiblingElement("option")) {
        const char *option_key = option->Attribute("opt_key");
        if (option_key != nullptr && std::string(option_key) == key) {
            return option->GetText();
        }
    }
    return nullptr;
}

std::string layer_ranges_xml(const json &result) {
    return part_for_path(result, "Metadata/layer_config_ranges.xml")
        .at("content").get<std::string>();
}

json full_component_request() {
    json request = metadata_request();
    request["component_inputs"] = {
        {"wipe_tower_placement",
         {{"schema_version", 1},
          {"enabled", true},
          {"available", true},
          {"adapted", false},
          {"requires_manual_adjustment", false},
          {"warning_code", nullptr},
          {"reason", "within_printable_area"},
          {"model_translation_mm", json::array({0.0, 0.0})},
          {"tower", {{"x_mm", 210.0}, {"y_mm", 210.0}, {"width_mm", 30.0}}}}},
        {"layer_config_ranges",
         {{"model_index", 1},
          {"ranges",
           json::array({{{"min_z", 0.4},
                         {"max_z", 1.2},
                         {"layer_height_mm", 0.08},
                         {"infill_density_percent", 100}}})}}},
    };
    return request;
}

const tinyxml2::XMLElement *metadata(const tinyxml2::XMLElement *parent,
                                     const char *key) {
    for (const auto *item = parent->FirstChildElement("metadata"); item != nullptr;
         item = item->NextSiblingElement("metadata")) {
        if (std::string(item->Attribute("key") ? item->Attribute("key") : "") ==
            key) {
            return item;
        }
    }
    return nullptr;
}

void test_model_component_preserves_ids_slots_and_text() {
    const json project = project_data();
    const json request = metadata_request();
    const json result = compose(project, request);
    expect(result.at("project_bed_type") == "Textured PEI Plate",
           "metadata result did not report the final project bed type");
    expect(result.at("model_settings_bed_type") == "Textured PEI Plate",
           "model settings bed type did not match the project");

    tinyxml2::XMLDocument document;
    const auto &model_part = part_for_path(
        result, "Metadata/model_settings.config");
    expect(document.Parse(model_part.at("content").get<std::string>().c_str()) ==
               tinyxml2::XML_SUCCESS,
           "model_settings.config is not parseable XML");
    const auto *config = document.FirstChildElement("config");
    const auto *object = config == nullptr ? nullptr : config->FirstChildElement("object");
    expect(object != nullptr && std::string(object->Attribute("id")) == "7",
           "assembly id was changed");
    const auto *parts = object->FirstChildElement("part");
    expect(parts != nullptr && std::string(parts->Attribute("id")) == "11",
           "first non-contiguous part id was changed");
    const auto *first_name = metadata(parts, "name");
    const auto *first_extruder = metadata(parts, "extruder");
    expect(first_name != nullptr && std::string(first_name->Attribute("value")) ==
               "第一 <部件> & \"A\"\nline",
           "XML escaping did not round-trip the first part name");
    expect(first_extruder != nullptr &&
               std::string(first_extruder->Attribute("value")) == "1",
           "material slot zero was not converted to extruder one");
    parts = parts->NextSiblingElement("part");
    expect(parts != nullptr && std::string(parts->Attribute("id")) == "42",
           "second non-contiguous part id was changed");
    const auto *second_extruder = metadata(parts, "extruder");
    expect(second_extruder != nullptr &&
               std::string(second_extruder->Attribute("value")) == "3",
           "material slot two was not converted to extruder three");

    const auto *plate = object->NextSiblingElement("plate");
    const auto *bed = plate == nullptr ? nullptr : metadata(plate, "bed_type");
    expect(bed != nullptr && std::string(bed->Attribute("value")) ==
               "Textured PEI Plate",
           "model settings did not carry the project bed type");
    const auto *instance = plate == nullptr ? nullptr : plate->FirstChildElement("model_instance");
    const auto *instance_id = instance == nullptr ? nullptr : metadata(instance, "instance_id");
    const auto *identify_id = instance == nullptr ? nullptr : metadata(instance, "identify_id");
    expect(instance_id != nullptr && std::string(instance_id->Attribute("value")) == "9" &&
               identify_id != nullptr && std::string(identify_id->Attribute("value")) == "1",
           "model instance identifiers were changed");

    tinyxml2::XMLDocument slice_document;
    const auto &slice_part = part_for_path(result, "Metadata/slice_info.config");
    expect(slice_document.Parse(slice_part.at("content").get<std::string>().c_str()) ==
               tinyxml2::XML_SUCCESS,
           "slice_info.config is not parseable XML");
    const auto *header = slice_document.FirstChildElement("config")
                             ->FirstChildElement("header");
    expect(header != nullptr && header->ChildElementCount("header_item") == 2,
           "slice header did not use the target header list");
    expect(result.at("root_model").at("merge_policy") == "replace_by_name" &&
               result.at("root_model").at("namespaces").size() == 1 &&
               result.at("root_model").at("metadata").size() == 2 &&
               result.at("root_model").at("external_metadata") ==
                   json::array({{{"name", "BambuStudio:3mfVersion"},
                                 {"value", "1"}}}),
           "root model description did not expose the target contract");
}

void test_target_data_drives_slice_and_root_descriptions() {
    json target = target_data();
    target["package_dialect"]["slice_headers"][1]["value"] = "02.08.TEST";
    target["package_dialect"]["root_metadata"][0]["value"] = "BambuStudio-Test";
    const json result = compose(project_data(), metadata_request(), target);
    const auto &slice_part = part_for_path(result, "Metadata/slice_info.config");
    tinyxml2::XMLDocument slice_document;
    expect(slice_document.Parse(slice_part.at("content").get<std::string>().c_str()) ==
               tinyxml2::XML_SUCCESS,
           "mutated target slice part is not parseable XML");
    const auto *header_item = slice_document.FirstChildElement("config")
                                  ->FirstChildElement("header")
                                  ->FirstChildElement("header_item")
                                  ->NextSiblingElement("header_item");
    expect(header_item != nullptr &&
               std::string(header_item->Attribute("value")) == "02.08.TEST",
           "slice header was hardcoded instead of target-driven");
    expect(result.at("root_model").at("metadata").at(0).at("value") ==
               "BambuStudio-Test",
           "root metadata was hardcoded instead of target-driven");

    target["package_dialect"]["root_external_metadata_names"] =
        json::array({"Application"});
    const json explicit_external = compose(project_data(), metadata_request(), target);
    expect(explicit_external.at("root_model").at("external_metadata") ==
               json::array({{{"name", "Application"},
                             {"value", "BambuStudio-Test"}}}),
           "external-model metadata did not follow the explicit target reference");
}

void test_numeric_layer_ranges_use_final_project_and_preserve_explicit_values() {
    json request = metadata_request();
    request["component_inputs"]["layer_config_ranges"]["ranges"] =
        json::array({{{"min_z", 0.4},
                      {"max_z", 1.2},
                      {"layer_height_mm", 0.16},
                      {"use_default_extruder", true},
                      {"infill_density_percent", 100}}});
    const json result = compose(project_data(), request);
    tinyxml2::XMLDocument document;
    const std::string xml = layer_ranges_xml(result);
    expect(document.Parse(xml.c_str()) == tinyxml2::XML_SUCCESS,
           "numeric layer ranges did not produce parseable XML");
    const auto *object = document.FirstChildElement("objects")
                             ->FirstChildElement("object");
    const auto *range = object == nullptr ? nullptr : object->FirstChildElement("range");
    expect(object != nullptr && std::string(object->Attribute("id")) == "1",
           "layer-range model index was replaced by the assembly resource id");
    expect(layer_range_option(range, "extruder") != nullptr &&
               std::string(layer_range_option(range, "extruder")) == "0",
           "default extruder intent was not mapped by Fat Cat");
    expect(layer_range_option(range, "layer_height") != nullptr &&
               std::stod(layer_range_option(range, "layer_height")) == 0.16,
           "explicit coarse layer height was overwritten by project layer height");
    expect(layer_range_option(range, "sparse_infill_density") != nullptr &&
               std::string(layer_range_option(range, "sparse_infill_density")) == "100%",
           "numeric infill density was not formatted by Fat Cat");
}

void test_density_only_layer_range_uses_final_project_height_or_fails() {
    json request = metadata_request();
    request["component_inputs"]["layer_config_ranges"]["ranges"] =
        json::array({{{"min_z", 0.4},
                      {"max_z", 1.2},
                      {"infill_density_percent", 100}}});
    const json result = compose(project_data(), request);
    tinyxml2::XMLDocument document;
    const std::string xml = layer_ranges_xml(result);
    expect(document.Parse(xml.c_str()) == tinyxml2::XML_SUCCESS,
           "density-only business range did not produce parseable XML");
    const auto *range = document.FirstChildElement("objects")
                            ->FirstChildElement("object")
                            ->FirstChildElement("range");
    expect(layer_range_option(range, "layer_height") != nullptr &&
               std::stod(layer_range_option(range, "layer_height")) == 0.08,
           "density-only range did not use the final project layer height");

    json missing_height = project_data();
    missing_height.erase("layer_height");
    expect_error([&] { compose(missing_height, request); }, "layer_height");
}

void test_layer_range_numbers_ignore_comma_global_locale() {
    global_locale_guard restore_locale;
    std::locale::global(std::locale(std::locale(), new comma_decimal_punct));
    expect(std::use_facet<std::numpunct<char>>(std::locale()).decimal_point() == ',',
           "test did not install a comma-decimal C++ locale");

    json request = metadata_request();
    request["component_inputs"]["layer_config_ranges"]["ranges"] =
        json::array({{{"min_z", 0.4},
                      {"max_z", 1.2},
                      {"infill_density_percent", 100}}});
    const std::string xml = layer_ranges_xml(compose(project_data(), request));
    expect(xml.find("min_z=\"0.40000000000000002\"") != std::string::npos &&
               xml.find("max_z=\"1.2\"") != std::string::npos &&
               xml.find(">0.080000000000000002</option>") != std::string::npos,
           "layer-range numeric text changed under comma-decimal host locale");
    expect(std::use_facet<std::numpunct<char>>(std::locale()).decimal_point() == ',',
           "Fat Cat changed the host's global locale");
}

void test_invalid_relationships_and_bed_conflicts_fail() {
    json duplicate_part = metadata_request();
    duplicate_part["parts"][1]["part_id"] = duplicate_part["parts"][0]["part_id"];
    expect_error([&] { compose(project_data(), duplicate_part); }, "part_id");

    json assembly_collision = metadata_request();
    assembly_collision["assembly_id"] = 11;
    expect_error([&] { compose(project_data(), assembly_collision); }, "assembly_id");

    json material_out_of_range = metadata_request();
    material_out_of_range["parts"][1]["material_index"] = 4;
    expect_error([&] { compose(project_data(), material_out_of_range); }, "material_index");

    json missing_descriptor = metadata_request();
    missing_descriptor["parts"][0].erase("matrix");
    expect_error([&] { compose(project_data(), missing_descriptor); }, "matrix");

    json missing_resource = metadata_request();
    (void)missing_resource;
    json target_without_model_resource = target_data();
    for (auto &resource : target_without_model_resource["package_dialect"]
                              ["metadata_components"]["resources"]) {
        if (resource.value("role", std::string{}) == "top_preview") {
            resource.erase("model_field");
        }
    }
    expect_error([&] {
        compose(project_data(), metadata_request(), target_without_model_resource);
    }, "top_file");

    json target_with_unknown_external_metadata = target_data();
    target_with_unknown_external_metadata["package_dialect"]
                                        ["root_external_metadata_names"] =
        json::array({"Unknown:3mfVersion"});
    expect_error([&] {
        compose(project_data(), metadata_request(), target_with_unknown_external_metadata);
    }, "references unknown root metadata");

    json bed_conflict = metadata_request();
    bed_conflict["plate"]["bed_type"] = "Cool Plate";
    expect_error([&] { compose(project_data(), bed_conflict); }, "bed_type");
}

void test_numeric_descriptors_are_finite_and_exact() {
    const std::string valid_matrix =
        "1.0 -2e-1 0.0 1 0 1 0 2 0 0 1 3 0 0 0 1";
    json valid = metadata_request();
    valid["parts"][0]["matrix"] = valid_matrix;
    valid["parts"][0]["source_offset_x"] = "-1e-3";
    valid["parts"][0]["source_offset_y"] = "+2.5";
    valid["parts"][0]["source_offset_z"] = "0";
    const json result = compose(project_data(), valid);
    tinyxml2::XMLDocument document;
    const auto &model = part_for_path(result, "Metadata/model_settings.config");
    expect(document.Parse(model.at("content").get<std::string>().c_str()) ==
               tinyxml2::XML_SUCCESS,
           "valid numeric descriptors did not produce XML");
    const auto *part = document.FirstChildElement("config")
                           ->FirstChildElement("object")
                           ->FirstChildElement("part");
    expect(part != nullptr && std::string(metadata(part, "matrix")->Attribute("value")) ==
               valid_matrix,
           "matrix descriptor formatting was rewritten");
    expect(std::string(metadata(part, "source_offset_x")->Attribute("value")) ==
               "-1e-3",
           "offset descriptor formatting was rewritten");

    const std::vector<std::pair<std::string, std::string>> invalid_matrices = {
        {"1 0 0", "matrix"},
        {"1 0 0 1 0 1 0 2 0 0 1 3 0 0 0 1 0", "matrix"},
        {"1 0 0 1 0 1 0 2 0 0 1 3 0 0 NaN 1", "matrix"},
        {"1 0 0 1 0 1 0 2 0 0 1 3 0 0 0 Infinity", "matrix"},
        {"1 0 0 1 0 1 0 2 0 0 1 3 0 0 0 1mm", "matrix"},
    };
    for (const auto &[matrix, field] : invalid_matrices) {
        json invalid_request = metadata_request();
        invalid_request["parts"][0]["matrix"] = matrix;
        expect_error([&] { compose(project_data(), invalid_request); }, field);
    }
    json invalid_offset = metadata_request();
    invalid_offset["parts"][0]["source_offset_y"] = "1e400";
    expect_error([&] { compose(project_data(), invalid_offset); }, "source_offset_y");
}

void test_xml10_text_is_rejected_before_serialization() {
    json invalid_name = metadata_request();
    invalid_name["parts"][0]["name"] = std::string("bad\x01name");
    expect_error([&] { compose(project_data(), invalid_name); },
                 "request.parts[0].name");

    json invalid_plater = metadata_request();
    invalid_plater["plate"]["plater_name"] = std::string("bad\x1Fname");
    expect_error([&] { compose(project_data(), invalid_plater); },
                 "request.plate.plater_name");

    json invalid_project_bed = project_data();
    invalid_project_bed["curr_bed_type"] = std::string("bad\x01") + "bed";
    json request_without_bed = metadata_request();
    request_without_bed["plate"]["bed_type"] = "";
    expect_error(
        [&] { compose(invalid_project_bed, request_without_bed); },
        "project.curr_bed_type");

    json invalid_source = metadata_request();
    invalid_source["source_file"] = std::string("bad\x0Csource.stl");
    expect_error([&] { compose(project_data(), invalid_source); },
                 "request.source_file");

    json invalid_resource = metadata_request();
    (void)invalid_resource;
    json invalid_target_resource = target_data();
    invalid_target_resource["package_dialect"]["metadata_components"]
                          ["resources"][2]["path"] =
        std::string("Metadata/bad\x01.png");
    expect_error([&] {
        compose(project_data(), metadata_request(), invalid_target_resource);
    }, "target metadata resources.top_file");

    json invalid_noncharacter = metadata_request();
    invalid_noncharacter["parts"][0]["name"] = std::string("bad\xEF\xBF\xBEname");
    expect_error([&] { compose(project_data(), invalid_noncharacter); },
                 "request.parts[0].name");

    json utf8_marker = metadata_request();
    utf8_marker["parts"][0]["name"] = "UTF8_MARKER";
    std::string invalid_utf8_request = utf8_marker.dump();
    const std::string marker = "\"UTF8_MARKER\"";
    const auto marker_position = invalid_utf8_request.find(marker);
    expect(marker_position != std::string::npos,
           "could not locate the invalid UTF-8 test marker");
    const std::string invalid_utf8_value =
        std::string("\"bad\xC3\x28name\"");
    invalid_utf8_request.replace(marker_position, marker.size(), invalid_utf8_value);
    expect_error(
        [&] {
            compose_raw(project_data().dump(), invalid_utf8_request);
        },
        "metadata request");

    json invalid_header = target_data();
    invalid_header["package_dialect"]["slice_headers"][0]["value"] =
        std::string("bad\x0Bheader");
    expect_error([&] { compose(project_data(), metadata_request(), invalid_header); },
                 "target.package_dialect.slice_headers[0].value");

    json valid = metadata_request();
    valid["parts"][0]["name"] =
        std::string("补充平面 \xF0\x90\x80\x80\r\tline");
    valid["source_file"] = std::string("模型\xF0\x90\x80\x80\r\t.stl");
    const json result = compose(project_data(), valid);
    const auto &model = part_for_path(result, "Metadata/model_settings.config");
    tinyxml2::XMLDocument document;
    expect(document.Parse(model.at("content").get<std::string>().c_str()) ==
               tinyxml2::XML_SUCCESS,
           "valid XML 1.0 text did not parse");
    const auto *part = document.FirstChildElement("config")
                           ->FirstChildElement("object")
                           ->FirstChildElement("part");
    expect(std::string(metadata(part, "name")->Attribute("value")) ==
               valid["parts"][0]["name"].get<std::string>(),
           "valid XML 1.0 text did not round-trip");
}

void test_reference_validation_uses_path_segments() {
    json valid = metadata_request();
    valid["source_file"] = "模型..v2.stl";
    json target = target_data();
    target["package_dialect"]["metadata_components"]["resources"][2]["path"] =
        "Metadata/top..v2.png";
    const json result = compose(project_data(), valid, target);
    const auto &model = part_for_path(result, "Metadata/model_settings.config");
    expect(model.at("content").get<std::string>().find("模型..v2.stl") !=
               std::string::npos,
           "source filename with consecutive dots was rejected or lost");
    expect(model.at("content").get<std::string>().find("Metadata/top..v2.png") !=
               std::string::npos,
           "resource filename with consecutive dots was rejected or lost");

    json parent_source = metadata_request();
    parent_source["source_file"] = "../top.png";
    expect_error([&] { compose(project_data(), parent_source); }, "source_file");

    json parent_resource_target = target_data();
    parent_resource_target["package_dialect"]["metadata_components"]
                         ["resources"][2]["path"] = "Metadata/../top.png";
    expect_error([&] {
        compose(project_data(), metadata_request(), parent_resource_target);
    }, "resources[2].path");

    json absolute_resource_target = target_data();
    absolute_resource_target["package_dialect"]["metadata_components"]
                            ["resources"][2]["path"] = "/tmp/top.png";
    expect_error([&] {
        compose(project_data(), metadata_request(), absolute_resource_target);
    }, "resources[2].path");
}

void test_output_is_stable_and_does_not_reset_project_materials() {
    const json project = project_data();
    const json request = metadata_request();
    const json first = compose(project, request);
    const json second = compose(project, request);
    expect(first == second, "identical metadata inputs did not produce stable output");
    expect(first.at("project_bed_type") == project.at("curr_bed_type"),
           "metadata generation changed project ownership");
    const auto &model = part_for_path(first, "Metadata/model_settings.config");
    expect(model.at("content").get<std::string>().find("Bambu PLA Basic") ==
               std::string::npos,
           "a native material preset was applied unexpectedly");
}

void test_orca_uses_shared_model_parts_and_its_own_dialect() {
    const json orca = json::parse(read_file(
        "compatibility/current-src/translations/targets/orca-slicer-2.4.2.json"));
    const json project = project_data();
    json request = metadata_request();
    const json bambu_result = compose(project, request);
    expect_error([&] { compose(project, request, orca); }, "slicer/version");
    request["slicer_id"] = "OrcaSlicer";
    request["application_version"] = "2.4.2";
    const json orca_result = compose(project, request, orca);
    request.erase("application_version");
    expect_error([&] { compose(project, request, orca); }, "application_version");
    expect(part_for_path(orca_result, "Metadata/model_settings.config") ==
               part_for_path(bambu_result, "Metadata/model_settings.config"),
           "the same geometry and material slots must use the shared model composer");
    expect(orca_result.at("root_model").at("metadata") ==
               orca.at("package_dialect").at("root_metadata"),
           "Orca root metadata did not come from the selected target");
    expect(orca_result.at("root_model").at("external_metadata") ==
               json::array({{{"name", "BambuStudio:3mfVersion"},
                             {"value", "1"}}}),
           "Orca external-model metadata did not come from its explicit target reference");

    tinyxml2::XMLDocument document;
    const auto xml = part_for_path(orca_result, "Metadata/slice_info.config")
                         .at("content").get<std::string>();
    expect(document.Parse(xml.c_str()) == tinyxml2::XML_SUCCESS,
           "Orca slice info is not parseable XML");
    json headers = json::array();
    const auto *header = document.FirstChildElement("config")->FirstChildElement("header");
    for (const auto *item = header->FirstChildElement("header_item"); item != nullptr;
         item = item->NextSiblingElement("header_item")) {
        headers.push_back({{"name", item->Attribute("key")},
                           {"value", item->Attribute("value")}});
    }
    expect(headers == orca.at("package_dialect").at("slice_headers"),
           "Orca slice headers did not match its target data");
    expect(orca_result.at("project_bed_type") == project.at("curr_bed_type"),
           "Orca metadata did not preserve the selected build plate");
}

void test_bambu_composes_all_metadata_parts_and_relationships() {
    const json project = project_data();
    json request = full_component_request();
    request["plate"]["resources"] = {
        {"thumbnail_file", "Caller/must-not-own/thumbnail.png"},
        {"thumbnail_no_light_file", "Caller/must-not-own/no-light.png"},
        {"top_file", "Caller/must-not-own/top.png"},
        {"pick_file", "Caller/must-not-own/pick.png"},
    };
    const json result = compose(project, request);

    const auto &project_part = part_for_role(result, "project_settings");
    expect(project_part.at("path") == "Metadata/project_settings.config" &&
               project_part.at("media_type") == "text/xml" &&
               project_part.at("content") == project.dump(),
           "project settings bytes or target part description changed");
    expect(part_for_role(result, "wipe_tower_placement").at("path") ==
               "Metadata/lumina_wipe_tower_placement.json",
           "wipe tower package path was not target-driven");
    expect(json::parse(part_for_role(result, "filament_sequence").at("content").get<std::string>()) ==
               json{{"plate_1", {{"sequence", json::array()}}}},
           "Bambu empty filament-sequence semantics changed");

    tinyxml2::XMLDocument model_document;
    const auto model_xml = part_for_role(result, "model_settings").at("content").get<std::string>();
    expect(model_document.Parse(model_xml.c_str()) == tinyxml2::XML_SUCCESS,
           "model settings part is not valid XML");
    const auto *plate = model_document.FirstChildElement("config")
                            ->FirstChildElement("plate");
    expect(std::string(metadata(plate, "thumbnail_file")->Attribute("value")) ==
               "Metadata/plate_1.png",
           "model resource path still came from the request");

    tinyxml2::XMLDocument cut_document;
    const auto cut_xml = part_for_role(result, "cut_information").at("content").get<std::string>();
    expect(cut_document.Parse(cut_xml.c_str()) == tinyxml2::XML_SUCCESS,
           "cut-information part is not valid XML");
    const auto *cut_object = cut_document.FirstChildElement("objects")
                                 ->FirstChildElement("object");
    expect(cut_object != nullptr && std::string(cut_object->Attribute("id")) == "7",
           "cut information no longer references the production assembly id");

    tinyxml2::XMLDocument range_document;
    const auto range_xml = part_for_role(result, "layer_config_ranges").at("content").get<std::string>();
    expect(range_document.Parse(range_xml.c_str()) == tinyxml2::XML_SUCCESS,
           "layer-range part is not valid XML");
    const auto *range_object = range_document.FirstChildElement("objects")
                                   ->FirstChildElement("object");
    expect(range_object != nullptr && std::string(range_object->Attribute("id")) == "1",
           "layer ranges used a resource or assembly id instead of model index");
    const auto &layer_part = part_for_role(result, "layer_config_ranges");
    expect(layer_part.at("path") == "Metadata/layer_config_ranges.xml",
           "layer-range path was not target-driven");

    const auto &plate_resource = part_for_role(result, "plate_main");
    expect(plate_resource.at("resource_role") == "plate_main" &&
               plate_resource.at("path") == "Metadata/plate_1.png" &&
               plate_resource.at("media_type") == "image/png" &&
               !plate_resource.contains("content"),
           "binary resources were embedded or left without a target mapping");
    expect(result.at("content_types").size() == 4,
           "target content-type contract was not returned");
    expect(result.at("relationships").size() == 10,
           "Bambu model and package thumbnail relationships changed");
    expect(result.at("relationships").at(0).at("source") ==
               "3D/3dmodel.model" &&
               result.at("relationships").at(0).at("id") == "bambuRel0" &&
               result.at("relationships").at(0).at("target") ==
                   "/Metadata/model_settings.config",
           "Bambu model relationship description changed");
    expect(result.at("relationships").at(7).at("source") == "" &&
               result.at("relationships").at(7).at("id") == "cover-rel-1" &&
               result.at("relationships").at(7).at("target") ==
                   "/Auxiliaries/.thumbnails/thumbnail_3mf.png",
           "package cover relationship description changed");
}

void test_orca_omits_bambu_only_components_and_relationships() {
    json request = full_component_request();
    request["slicer_id"] = "OrcaSlicer";
    request["application_version"] = "2.4.2";
    const json orca = json::parse(read_file(
        "compatibility/current-src/translations/targets/orca-slicer-2.4.2.json"));
    const json result = compose(project_data(), request, orca);

    expect(!has_part_role(result, "filament_sequence"),
           "Orca received Bambu's empty filament-sequence part");
    expect(!has_part_role(result, "cut_information"),
           "Orca received Bambu's cut-information part");
    expect(result.at("relationships").size() == 4,
           "Orca must relate only the optional layer range and three cover resources");
    expect(result.at("relationships").at(0).at("target") ==
               "/Metadata/layer_config_ranges.xml",
           "Orca layer-range relationship changed");

    request["component_inputs"].erase("layer_config_ranges");
    const json without_ranges = compose(project_data(), request, orca);
    expect(!has_part_role(without_ranges, "layer_config_ranges"),
           "empty layer ranges generated an XML part");
    expect(without_ranges.at("relationships").size() == 3,
           "Orca emitted a dangling layer-range relationship without a part");

    request["component_inputs"]["layer_config_ranges"] =
        {{"model_index", 1}, {"ranges", json::array()}};
    const json empty_ranges = compose(project_data(), request, orca);
    expect(!has_part_role(empty_ranges, "layer_config_ranges") &&
               empty_ranges.at("relationships").size() == 3,
           "empty business ranges emitted a part or dangling relationship");
}

}  // namespace

int main() {
    try {
        test_model_component_preserves_ids_slots_and_text();
        test_target_data_drives_slice_and_root_descriptions();
        test_numeric_layer_ranges_use_final_project_and_preserve_explicit_values();
        test_density_only_layer_range_uses_final_project_height_or_fails();
        test_layer_range_numbers_ignore_comma_global_locale();
        test_invalid_relationships_and_bed_conflicts_fail();
        test_numeric_descriptors_are_finite_and_exact();
        test_xml10_text_is_rejected_before_serialization();
        test_reference_validation_uses_path_segments();
        test_output_is_stable_and_does_not_reset_project_materials();
        test_orca_uses_shared_model_parts_and_its_own_dialect();
        test_bambu_composes_all_metadata_parts_and_relationships();
        test_orca_omits_bambu_only_components_and_relationships();
    } catch (const std::exception &error) {
        std::cerr << "metadata_components_test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "metadata_components_test passed: 13 behavior groups\n";
    return EXIT_SUCCESS;
}
