#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <windows.h>

#include <nlohmann/json.hpp>
#include "fatcat/native_project_source.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

// Fixture I/O deliberately bypasses the ordinary Win32 limit. The production
// calls below receive ordinary paths, including a root itself longer than 260.
fs::path fixture_path(fs::path path) {
    path = fs::absolute(path).make_preferred();
    return fs::path(L"\\\\?\\" + path.native());
}

int main() {
    fs::path temp = fs::temp_directory_path() /
        ("fatcat-path-test-" + std::to_string(GetCurrentProcessId()));
    try {
        const fs::path source = fs::u8path(FATCAT_TEST_REPOSITORY_ROOT) /
            "compatibility/current-src";
        auto root = temp / fs::u8path(u8"中文用户 有空格");
        while (root.native().size() < 280) root /= "long-installation-directory";
        fs::create_directories(fixture_path(root));
        for (const auto &entry : fs::recursive_directory_iterator(source)) {
            auto relative = entry.path().lexically_relative(source);
            // The source tree uses a hyphen; the installed SDK uses an underscore.
            if (*relative.begin() == "native-project-sources") {
                relative = fs::path("native_project_sources") /
                    relative.lexically_relative("native-project-sources");
            }
            const auto output = fixture_path(root / relative);
            if (entry.is_directory()) fs::create_directories(output);
            else if (entry.is_regular_file()) fs::copy_file(entry.path(), output);
        }
        const auto sentinel = root / "native_project_sources/source-index.json";
        if (GetFileAttributesW(sentinel.c_str()) != INVALID_FILE_ATTRIBUTES) {
            throw std::runtime_error("test process must have longPathAware disabled");
        }
        for (const auto &data_root : {root, fixture_path(root)}) {
            const auto target = json::parse(fatcat::metadata_target_data("BambuStudio", "", data_root));
            if (target.at("target_contract").at("slicer_id") != "BambuStudio")
                throw std::runtime_error("wrong target");
            const auto catalog = json::parse(fatcat::native_project_source_catalog(
                "BambuStudio", "02.08.02.61", data_root / "native_project_sources"));
            if (catalog.at("sources").empty()) throw std::runtime_error("empty catalog");
            for (const auto *machine : {"bambu-lab:p2s", "bambu-lab:x2d"}) {
                const json request = {
                    {"project_source", "fatcat_native"}, {"slicer_id", "BambuStudio"},
                    {"application_version", "02.08.02.61"}, {"machine_uid", machine},
                    {"nozzle_uid", "nozzle:0.4mm"}, {"build_plate_uid", "plate:textured-pei"},
                    {"source_materials", json::array({{{"name", "PLA"}, {"colour", "#FF0000"},
                                                     {"material_type", "PLA"}}})}};
                const auto composed = json::parse(fatcat::compose_builtin_project_settings(request.dump(), data_root));
                const auto project = json::parse(composed.at("project_settings_json").get<std::string>());
                if (project.at("machine_start_gcode").get<std::string>().size() < 100)
                    throw std::runtime_error("native G-code was not loaded");
            }
        }
        fs::remove_all(fixture_path(temp));
        std::cout << "Long native paths: plain + extended roots, P2S + X2D passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all(fixture_path(temp), ignored);
        return 1;
    }
}
