#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "fatcat/metadata_components.h"
#include "fatcat/native_project_source.h"

namespace {

using json = nlohmann::json;

json read_json(const std::string &path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open fixture: " + path);
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return json::parse(contents.str());
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const std::filesystem::path data_root = argc > 1 ? argv[1] : FATCAT_PUBLIC_DATA_ROOT;
        json request = read_json(FATCAT_EXAMPLE_REQUEST);
        json selection = {{"project_source", "fatcat_native"},
            {"slicer_id", "BambuStudio"}, {"application_version", "02.08.02.61"},
            {"machine_uid", "bambu-lab:a1"}, {"nozzle_uid", "nozzle:0.4mm"},
            {"build_plate_uid", "plate:textured-pei"},
            {"source_materials", json::array({{{"name", "PLA"}, {"material_type", "PLA"}, {"colour", "#123456"}}})}};
        const json composed = json::parse(fatcat::compose_builtin_project_settings(selection.dump(), data_root));
        const json project = json::parse(composed.at("project_settings_json").get<std::string>());
        const json result = json::parse(fatcat::compose_model_metadata_from_data(
            project.dump(), request.dump(), data_root));

        bool saw_project = false;
        bool saw_layer_ranges = false;
        bool saw_binary_resource = false;
        for (const auto &part : result.at("parts")) {
            saw_project = saw_project ||
                (part.at("role") == "project_settings" &&
                 part.at("path") == "Metadata/project_settings.config" &&
                 part.at("content") == project.dump());
            if (part.at("role") == "layer_config_ranges" &&
                part.at("path") == "Metadata/layer_config_ranges.xml") {
                const auto &content = part.at("content").get_ref<const std::string &>();
                const std::string marker = "opt_key=\"layer_height\">";
                const auto position = content.find(marker);
                const auto &height = project.at("layer_height");
                const double expected = height.is_string() ? std::stod(height.get<std::string>()) : height.get<double>();
                saw_layer_ranges = position != std::string::npos &&
                    std::abs(std::stod(content.substr(position + marker.size())) - expected) < 1e-9 &&
                    content.find("opt_key=\"sparse_infill_density\">100%") != std::string::npos;
            }
            saw_binary_resource = saw_binary_resource ||
                (part.at("role") == "thumbnail_package" &&
                 part.at("resource_role") == "thumbnail_package" &&
                 !part.contains("content"));
        }
        const bool saw_external_metadata =
            result.at("root_model").at("external_metadata") ==
            json::array({{{"name", "BambuStudio:3mfVersion"}, {"value", "1"}}});
        if (!saw_project || !saw_layer_ranges || !saw_binary_resource ||
            !saw_external_metadata ||
            result.at("relationships").size() != 10 ||
            result.at("content_types").size() != 4) {
            throw std::runtime_error("consumer observed an incomplete package description");
        }
        std::cout << "Fat Cat native C++ consumer passed: "
                  << result.at("parts").size() << " parts, "
                  << result.at("relationships").size() << " relationships\n";
        selection["slicer_id"] = "OrcaSlicer";
        selection["application_version"] = "2.4.2";
        selection["machine_uid"] = "snapmaker:u1";
        const auto compatible = json::parse(fatcat::compose_builtin_project_settings(selection.dump(), data_root));
        if (compatible.at("process_source").at("source_application_version") != "2.2.4") {
            throw std::runtime_error("consumer lost the historical process provenance");
        }
        if (compatible.at("material_source").at("source_application_version") != "2.2.4") {
            throw std::runtime_error("consumer lost the historical material provenance");
        }
        request["slicer_id"] = selection.at("slicer_id");
        request["application_version"] = selection.at("application_version");
        const auto compatible_metadata = json::parse(fatcat::compose_model_metadata_from_data(
            compatible.at("project_settings_json").get<std::string>(), request.dump(), data_root));
        if (compatible_metadata.at("parts").empty()) throw std::runtime_error("U1 metadata is empty");
        std::cout << "Fat Cat compatible C++ consumer passed: Orca 2.4.2 hardware, 2.2.4 process/materials\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Fat Cat native C++ consumer failed: " << error.what() << '\n';
        return 1;
    }
}
