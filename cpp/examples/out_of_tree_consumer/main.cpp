#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "fatcat/metadata_components.h"

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

int main() {
    try {
        const std::string root = FATCAT_FIXTURE_ROOT;
        const json target = read_json(
            root + "/compatibility/current-src/translations/targets/"
                  "bambu-studio-02.08.02.61.json");
        const json project = read_json(
            root + "/cpp/examples/out_of_tree_consumer/project.json");
        const json request = read_json(
            root + "/cpp/examples/out_of_tree_consumer/request.json");
        const json result = json::parse(fatcat::compose_model_metadata(
            project.dump(), request.dump(), target.dump()));

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
                saw_layer_ranges =
                    content.find("opt_key=\"layer_height\">0.080000000000000002") !=
                        std::string::npos &&
                    content.find("opt_key=\"sparse_infill_density\">100%") !=
                        std::string::npos;
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
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Fat Cat native C++ consumer failed: " << error.what() << '\n';
        return 1;
    }
}
