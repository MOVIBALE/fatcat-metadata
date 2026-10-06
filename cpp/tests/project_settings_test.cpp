#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "fatcat/project_settings.h"

namespace {

using json = nlohmann::json;

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

json canonical_data() {
    return json::parse(read_file(
        "compatibility/current-src/translations/canonical.json"));
}

json target_data() {
    return json::parse(read_file(
        "compatibility/current-src/translations/targets/"
        "bambu-studio-02.08.02.61.json"));
}

json material_profile(const json &target, const json &binding) {
    const auto key = binding.at("material_profile_key");
    for (const auto &profile : target.at("material_profiles")) {
        if (profile.at("material_profile_key") != key) continue;
        const auto parameter_set_key = profile.at("target_parameter_set_key");
        for (const auto &parameter_set : target.at("target_parameter_sets")) {
            if (parameter_set.at("target_parameter_set_key") != parameter_set_key) continue;
            json resolved = profile;
            resolved["managed_parameter_keys"] = parameter_set.at("managed_parameter_keys");
            resolved["target_parameters"] = parameter_set.at("target_parameters");
            return resolved;
        }
        throw std::runtime_error("missing shared native material parameter set");
    }
    throw std::runtime_error("missing shared native material profile");
}

json base_project() {
    return {
        {"printer_settings_id", "Bambu Lab A1 mini 0.4 nozzle"},
        {"printer_model", "Bambu Lab A1 mini"},
        {"printer_variant", "0.4"},
        {"nozzle_diameter", json::array({"0.4"})},
        {"filament_settings_id",
         json::array({"Template PLA A", "Template PETG B", "Tail C",
                      "Tail D"})},
        {"filament_type", json::array({"PLA", "PETG", "PLA", "PLA"})},
        {"filament_vendor",
         json::array({"Template Vendor", "Template Vendor", "Tail Vendor",
                      "Tail Vendor"})},
        {"filament_colour",
         json::array({"#111111", "#222222", "#333333", "#444444"})},
        {"filament_multi_colour",
         json::array({"#111111", "#222222", "#333333", "#444444"})},
        {"filament_compatible_printers",
         json::array({"Bambu Lab A1 mini 0.4 nozzle",
                      "Bambu Lab A1 mini 0.4 nozzle",
                      "Bambu Lab A1 mini 0.4 nozzle",
                      "Bambu Lab A1 mini 0.4 nozzle"})},
        {"nozzle_temperature", json::array({"213", "213", "213", "213"})},
        {"nozzle_temperature_initial_layer",
         json::array({"217", "217", "217", "217"})},
        {"filament_flow_ratio", json::array({"0.93", "0.93", "0.93", "0.93"})},
        {"outer_wall_speed", "47"},
        {"outer_wall_acceleration", "1234"},
        {"layer_height", "0.08"},
        {"initial_layer_print_height", "0.20"},
        {"initial_layer_height", "0.08"},
        {"line_width", "0.42"},
        {"initial_layer_line_width", "0.50"},
        {"wall_loops", "2"},
        {"top_shell_layers", "4"},
        {"bottom_shell_layers", "3"},
        {"bottom_surface_pattern", "monotonic"},
        {"elefant_foot_compensation", "0.10"},
        {"sparse_infill_density", "100%"},
        {"sparse_infill_pattern", "grid"},
        {"print_speed", "80"},
        {"travel_speed", "180"},
        {"enable_support", "0"},
        {"single_extruder_multi_material", "1"},
        {"brim_type", "auto_brim"},
        {"brim_width", "5"},
        {"enable_prime_tower", "1"},
        {"prime_tower_width", "20"},
        {"prime_tower_brim_width", "12"},
        {"prime_tower_rib_wall", "1"},
        {"prime_tower_rib_width", "15"},
        {"prime_tower_extra_rib_length", "4"},
        {"prime_tower_infill_gap", "0.5"},
        {"wipe_tower_rotation_angle", "10"},
        {"wipe_tower_x", json::array({"10"})},
        {"wipe_tower_y", json::array({"20"})},
        {"bed_exclude_area", json::array({"0x0", "1x0", "1x1"})},
        {"extruder_printable_area", json::array({"0x0", "180x0", "180x180"})},
        {"filament_prime_volume", json::array({"30", "30", "30", "30"})},
        {"filament_change_length", json::array({"5", "5", "5", "5"})},
        {"filament_diameter", json::array({"1.75", "1.75", "1.75", "1.75"})},
        {"different_settings_to_system",
         json::array({"existing_process", "", "", "", "", "tail_marker"})},
        {"unknown_group", json::array({"keep-a", "keep-b", "keep-c", "keep-d"})},
        {"unknown_nested", { {"enabled", true}, {"label", "keep-me"} }},
    };
}

json request(const std::string &mode) {
    json result = {
        {"slicer_id", "BambuStudio"},
        {"application_version", "02.08.02.61"},
        {"machine_uid", "bambu-lab:a1-mini"},
        {"nozzle_uid", "nozzle:0.4mm"},
        {"build_plate_uid", "plate:textured-pei"},
        {"material_mode", mode},
    };
    if (mode == "target_native_preset") result["material_uid"] = "material:pla";
    return result;
}

std::string compose(const json &project, const json &request_data,
                    const json &canonical = canonical_data(),
                    const json &target = target_data()) {
    return fatcat::compose_project_settings(
        project.dump(), request_data.dump(), canonical.dump(), target.dump());
}

std::string compose_raw_request(const json &project,
                                const std::string &request_json,
                                const json &canonical = canonical_data(),
                                const json &target = target_data()) {
    return fatcat::compose_project_settings(
        project.dump(), request_json, canonical.dump(), target.dump());
}

json parsed_result(const std::string &output) {
    json result = json::parse(output);
    if (!result.contains("project_settings_json") ||
        !result.contains("effective_settings")) {
        throw std::runtime_error("result does not contain the two required outputs");
    }
    result["project"] = json::parse(result.at("project_settings_json").get<std::string>());
    return result;
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
    } catch (const fatcat::ProjectSettingsError &error) {
        expect(std::string(error.what()).find(needle) != std::string::npos,
               "error did not mention '" + needle + "': " + error.what());
        return;
    }
    throw std::runtime_error("expected ProjectSettingsError containing " + needle);
}

void test_preserve_template_process_and_colors() {
    json project = base_project();
    json request_data = request("preserve_template");
    request_data["layer_height"] = "0.12";
    request_data["filament_colour"] = json::array({"#AAAAAA", "#BBBBBB"});
    request_data["filament_multi_colour"] = json::array({"#AAAAAA", "#BBBBBB"});
    json result = parsed_result(compose(project, request_data));
    const json &out = result.at("project");
    expect(out.at("layer_height") == "0.12", "requested layer height was not applied");
    expect(out.at("outer_wall_speed") == "47" &&
               out.at("outer_wall_acceleration") == "1234",
           "template process values were overwritten");
    expect(out.at("nozzle_temperature") == project.at("nozzle_temperature") &&
               out.at("nozzle_temperature_initial_layer") ==
                   project.at("nozzle_temperature_initial_layer") &&
               out.at("filament_flow_ratio") == project.at("filament_flow_ratio"),
           "template temperature or flow arrays were overwritten");
    expect(out.at("filament_colour") ==
               json::array({"#AAAAAA", "#BBBBBB", "#333333", "#444444"}),
           "only the requested colour slots should change");
    expect(out.at("filament_multi_colour") ==
               json::array({"#AAAAAA", "#BBBBBB", "#333333", "#444444"}),
           "multi-colour tail was not retained");
    expect(out.at("filament_settings_id") == project.at("filament_settings_id") &&
               out.at("unknown_group") == project.at("unknown_group") &&
               out.at("unknown_nested") == project.at("unknown_nested"),
           "template identity or unknown fields were lost");
    expect(result.at("effective_settings").at("layer_height") == "0.12",
           "effective summary did not come from final project settings");
}

void test_preserve_template_does_not_require_native_material() {
    json target = target_data();
    target.erase("material_bindings");
    json canonical = canonical_data();
    canonical.erase("materials");
    json selection = request("preserve_template");
    selection.erase("material_uid");
    json source = base_project();
    source["nozzle_temperature"] = json::array({"211", "244", "217", "231"});
    source["filament_flow_ratio"] = json::array({"0.91", "0.96", "0.93", "0.98"});
    source["filament_retraction_length"] = json::array({"0.6", "1.2", "0.8", "1.4"});
    selection["filament_colour"] = json::array({"#ABCDEF"});
    const json out = parsed_result(compose(source, selection, canonical, target)).at("project");
    for (const auto *key : {"filament_settings_id", "filament_type", "filament_vendor",
                           "nozzle_temperature", "filament_flow_ratio", "filament_retraction_length"}) {
        expect(out.at(key) == source.at(key), std::string("missing native preset changed ") + key);
    }
    expect(out.at("filament_colour").at(3) == source.at("filament_colour").at(3),
           "mixed-material tail colour was lost");
    // User-template preservation is separate from native material selection.
    selection["material_uid"] = "material:pla";
    expect_error([&] { compose(source, selection, canonical, target); }, "preserve_template");
}

void test_native_material_lookup_matches_nozzle_independent_of_order() {
    json target = target_data();
    json material = target.at("material_bindings").at(0);
    material["nozzle_uid"] = "nozzle:0.9mm";
    material["native_profile_id"] = "Synthetic 0.9 PLA";
    json small_profile = material_profile(target, material);
    small_profile["material_profile_key"] = "material:pla::Synthetic 0.9 PLA";
    small_profile["native_profile_id"] = "Synthetic 0.9 PLA";
    small_profile["target_parameters"]["filament_flow_ratio"]["values"] = json::array({"0.87"});
    const std::string parameter_set_key = "parameters::material:pla::Synthetic 0.9 PLA";
    json small_parameter_set = {
        {"target_parameter_set_key", parameter_set_key},
        {"managed_parameter_keys", small_profile.at("managed_parameter_keys")},
        {"target_parameters", small_profile.at("target_parameters")},
    };
    small_profile.erase("managed_parameter_keys");
    small_profile.erase("target_parameters");
    small_profile["target_parameter_set_key"] = parameter_set_key;
    material["material_profile_key"] = small_profile.at("material_profile_key");
    target["material_profiles"].push_back(small_profile);
    target["target_parameter_sets"].push_back(small_parameter_set);
    const json original = target.at("material_bindings").at(0);
    json canonical = canonical_data();
    for (auto &machine : canonical["machines"]) {
        if (machine.at("machine_uid") == "bambu-lab:a1-mini") {
            machine["supported_nozzle_uids"].push_back("nozzle:0.9mm");
        }
    }
    canonical["nozzles"].push_back({{"nozzle_uid", "nozzle:0.9mm"}});
    json small_machine = target.at("machine_bindings").at(0);
    small_machine["nozzle_uid"] = "nozzle:0.9mm";
    small_machine["nozzle_diameter"] = json::array({"0.9"});
    small_machine["printer_variant"] = "0.9";
    small_machine["source_profile_name"] = "Synthetic A1 mini 0.9 nozzle";
    target["machine_bindings"].push_back(small_machine);
    for (bool reverse : {false, true}) {
        target["material_bindings"] = reverse ? json::array({material, original})
                                              : json::array({original, material});
        for (const auto *nozzle : {"0.9", "0.4"}) {
            json source = base_project();
            source["filament_type"] = json::array({"PLA", "PLA", "PLA", "PLA"});
            source["nozzle_diameter"] = json::array({nozzle});
            source["printer_settings_id"] = std::string("Bambu Lab A1 mini ") + nozzle + " nozzle";
            source["filament_compatible_printers"] = json::array();
            for (int slot = 0; slot < 4; ++slot) {
                source["filament_compatible_printers"].push_back(source.at("printer_settings_id"));
            }
            json selection = request("target_native_preset");
            selection["nozzle_uid"] = std::string("nozzle:") + nozzle + "mm";
            const json out = parsed_result(compose(source, selection, canonical, target)).at("project");
            const json &expected = std::string(nozzle) == "0.9" ? material : original;
            const json &expected_profile = material_profile(target, expected);
            expect(out.at("filament_settings_id").at(3) == expected.at("native_profile_id"),
                   "native material lookup depended on binding order");
            expect(out.at("filament_flow_ratio").at(3) ==
                       expected_profile.at("target_parameters").at("filament_flow_ratio").at("values").at(0),
                   "native material used another nozzle's tuning");
        }
    }
    target["material_bindings"] = json::array({material});
    json source = base_project();
    source["filament_type"] = json::array({"PLA", "PLA", "PLA", "PLA"});
    expect_error([&] { compose(source, request("target_native_preset"), canonical, target); },
                 "material_bindings");
    compose(source, request("preserve_template"), canonical, target);
}

void test_preserve_template_extended_process_overrides() {
    json project = base_project();
    json request_data = request("preserve_template");
    request_data["line_width"] = "0.48";
    request_data["initial_layer_line_width"] = "0.62";
    request_data["wall_loops"] = "3";
    request_data["top_shell_layers"] = "5";
    request_data["bottom_shell_layers"] = "4";
    request_data["bottom_surface_pattern"] = "zig-zag";
    request_data["elefant_foot_compensation"] = "0.14";
    request_data["outer_wall_speed"] = "95";
    request_data["travel_speed"] = "210";
    request_data["enable_support"] = "1";
    request_data["single_extruder_multi_material"] = "0";

    const json out = parsed_result(compose(project, request_data)).at("project");
    for (const auto &key : {"line_width", "initial_layer_line_width", "wall_loops",
                            "top_shell_layers", "bottom_shell_layers",
                            "bottom_surface_pattern", "elefant_foot_compensation",
                            "enable_support",
                            "single_extruder_multi_material"}) {
        expect(out.at(key) == request_data.at(key),
               std::string("extended process field was not applied: ") + key);
    }
    expect(out.at("nozzle_temperature") == project.at("nozzle_temperature") &&
               out.at("filament_flow_ratio") == project.at("filament_flow_ratio"),
           "extended process overrides changed material tuning");
    expect(out.at("outer_wall_speed") == json::array({"95"}) &&
               out.at("travel_speed") == json::array({"210"}),
           "speed overrides did not use native arrays");
}

void test_native_material_is_data_driven_and_distinct() {
    json project = base_project();
    project["filament_type"] = json::array({"PLA", "PLA", "PLA", "PLA"});
    json request_data = request("target_native_preset");
    request_data["filament_colour"] = json::array({"#AAAAAA", "#BBBBBB"});
    json result = parsed_result(compose(project, request_data));
    const json &out = result.at("project");
    const json target_fixture = target_data();
    const json &material = target_fixture.at("material_bindings").at(0);
    const json &machine = target_fixture.at("machine_bindings").at(0);
    expect(out.at("filament_settings_id") ==
               json::array({material.at("native_profile_id").get<std::string>(),
                            material.at("native_profile_id").get<std::string>(),
                            material.at("native_profile_id").get<std::string>(),
                            material.at("native_profile_id").get<std::string>()}),
           "native profile was not applied to every slot");
    expect(out.at("filament_ids") ==
               json::array({material.at("native_filament_id").get<std::string>(),
                            material.at("native_filament_id").get<std::string>(),
                            material.at("native_filament_id").get<std::string>(),
                            material.at("native_filament_id").get<std::string>()}),
           "native filament id was not applied to every slot");
    expect(out.at("filament_compatible_printers") ==
               json::array({machine.at("source_profile_name").get<std::string>(),
                            machine.at("source_profile_name").get<std::string>(),
                            machine.at("source_profile_name").get<std::string>(),
                            machine.at("source_profile_name").get<std::string>()}),
           "native machine provenance was not applied");
    const auto &profile = material_profile(target_fixture, material);
    const auto &flow = profile.at("target_parameters").at("filament_flow_ratio")
                           .at("values").at(0);
    expect(out.at("filament_flow_ratio") == json::array({flow, flow, flow, flow}),
           "native flow did not come from target data");
    const auto &temp = profile.at("target_parameters").at("nozzle_temperature")
                           .at("values").at(0);
    expect(out.at("nozzle_temperature") == json::array({temp, temp, temp, temp}),
           "native temperature did not come from target data");
    expect(out.at("unknown_nested") == project.at("unknown_nested"),
           "unrelated nested field was lost in native mode");
    expect(out.at("filament_colour") ==
               json::array({"#AAAAAA", "#BBBBBB", "#333333", "#444444"}),
           "native mode did not apply explicit colours without truncation");
    expect(result.at("effective_settings").at("material_mode") ==
               "target_native_preset",
           "effective summary did not identify native mode");
}

void test_layer_aliases_brim_and_wipe_summary() {
    for (const auto &[brim_type, brim_width] :
         {std::pair<std::string, std::string>{"none", "0"},
          {"auto_brim", "5"},
          {"auto_brim", "20"}}) {
        json project = base_project();
        json request_data = request("preserve_template");
        request_data["initial_layer_print_height"] = "0.20";
        request_data["initial_layer_height"] = "0.20";
        request_data["brim_type"] = brim_type;
        request_data["brim_width"] = brim_width;
        request_data["sparse_infill_density"] = "80%";
        request_data["sparse_infill_pattern"] = "grid";
        json result = parsed_result(compose(project, request_data));
        const json &out = result.at("project");
        const json &summary = result.at("effective_settings");
        expect(out.at("initial_layer_print_height") == "0.20" &&
                   out.at("initial_layer_height") == "0.08",
               "first-layer alias did not resolve to the native FFF field");
        expect(summary.at("initial_layer_print_height") == "0.20" &&
                   summary.at("initial_layer_height") == "0.08",
               "first-layer summary differs from the resolved native field");
        const auto native_brim = brim_type == "none" ? "no_brim" : brim_type;
        expect(out.at("brim_type") == native_brim && out.at("brim_width") == brim_width &&
                   summary.at("brim_type") == native_brim &&
                   summary.at("brim_width") == brim_width,
               "brim output and summary disagree");
        expect(out.at("sparse_infill_pattern") == "grid" &&
                   out.at("enable_prime_tower") == "1" &&
                   out.at("prime_tower_width") == "20" &&
                   out.at("wipe_tower_x") == json::array({"10"}),
               "unrelated infill or wipe fields were changed");
    }
}

void test_errors_and_input_immutability() {
    json project = base_project();
    const json original = project;
    json colours = request("preserve_template");
    colours["filament_colour"] =
        json::array({"#1", "#2", "#3", "#4", "#5"});
    expect_error([&] { compose(project, colours); }, "slot");
    expect(project == original, "compose mutated the caller project object");

    json wrong_machine = request("preserve_template");
    wrong_machine["machine_uid"] = "bambu-lab:wrong";
    expect_error([&] { compose(project, wrong_machine); }, "machine");
    json wrong_nozzle = request("preserve_template");
    wrong_nozzle["nozzle_uid"] = "nozzle:0.2mm";
    expect_error([&] { compose(project, wrong_nozzle); }, "nozzle");

    json invalid_type = request("preserve_template");
    invalid_type["layer_height"] = true;
    expect_error([&] { compose(project, invalid_type); }, "boolean");

    json missing = target_data();
    missing.erase("material_bindings");
    expect_error([&] { compose(project, request("target_native_preset"),
                                canonical_data(), missing); },
                 "material");
}

void test_difference_stability_and_alternate_mapping() {
    json project = base_project();
    json request_data = request("preserve_template");
    request_data["layer_height"] = "0.12";
    request_data["filament_colour"] = json::array({"#AAAAAA"});
    json first = parsed_result(compose(project, request_data));
    json second = parsed_result(compose(first.at("project"), request_data));
    expect(first.at("project") == second.at("project"),
           "repeating an identical request was not stable");
    const auto &differences = first.at("project").at("different_settings_to_system");
    expect(differences.at(5) == "tail_marker" && differences.at(0).get<std::string>().find("layer_height") != std::string::npos,
           "difference list update did not preserve the trailing entry");

    json alternate_target = target_data();
    alternate_target["package_dialect"]["filament_snapshot"]["difference_list_key"] =
        "custom_difference";
    project["custom_difference"] = project.at("different_settings_to_system");
    project.erase("different_settings_to_system");
    json alternate = parsed_result(compose(project, request_data, canonical_data(),
                                           alternate_target));
    expect(alternate.at("project").contains("custom_difference") &&
               !alternate.at("project").contains("different_settings_to_system"),
           "alternate data-driven difference mapping was ignored");
}

void test_target_data_is_not_brand_hardcoded() {
    json target = target_data();
    target["machine_bindings"][0]["printer_model"] = "Synthetic Model";
    target["machine_bindings"][0]["printer_variant"] = "synthetic";
    target["machine_bindings"][0]["source_profile_name"] = "Synthetic Profile";
    json project = base_project();
    project["filament_type"] = json::array({"PLA", "PLA", "PLA", "PLA"});
    project["printer_settings_id"] = "Synthetic Profile";
    project["printer_model"] = "Synthetic Model";
    project["printer_variant"] = "synthetic";
    project["filament_compatible_printers"] =
        json::array({"Synthetic Profile", "Synthetic Profile", "Synthetic Profile",
                     "Synthetic Profile"});
    json result = parsed_result(compose(project, request("target_native_preset"),
                                        canonical_data(), target));
    expect(result.at("project").at("printer_model") == "Synthetic Model" &&
               result.at("project").at("filament_compatible_printers").at(0) ==
                   "Synthetic Profile",
           "native target mapping relied on Bambu brand literals");
}

void test_f1_validates_values_and_reuses_tower_rules() {
    for (const auto &[key, value] :
         {std::pair<std::string, std::string>{"prime_tower_width", "-1"},
          {"prime_tower_width", "0"},
          {"prime_tower_width", "nan"},
          {"layer_height", "-0.12"},
          {"layer_height", "nan"},
          {"layer_height", ""},
          {"sparse_infill_density", "150%"},
          {"enable_prime_tower", "garbage"}}) {
        json request_data = request("preserve_template");
        request_data[key] = value;
        expect_error([&] { compose(base_project(), request_data); }, key);
    }

    json non_numeric_coordinates = request("preserve_template");
    non_numeric_coordinates["wipe_tower_x"] = json::array({"NaN"});
    non_numeric_coordinates["wipe_tower_y"] = json::array({"20"});
    expect_error([&] { compose(base_project(), non_numeric_coordinates); }, "wipe_tower_x");

    json unequal_coordinates = request("preserve_template");
    unequal_coordinates["wipe_tower_x"] = json::array({"12"});
    unequal_coordinates["wipe_tower_y"] = json::array({"20", "30"});
    expect_error([&] { compose(base_project(), unequal_coordinates); }, "coordinate");

    json project = base_project();
    project["wipe_tower_x"] = json::array({"10", "20.000"});
    project["wipe_tower_y"] = json::array({"30", "40.000"});
    json update_one = request("preserve_template");
    update_one["wipe_tower_x"] = json::array({"12.5", "20.000"});
    update_one["wipe_tower_y"] = json::array({"-2", "40.000"});
    json result = parsed_result(compose(project, update_one));
    expect(result.at("project").at("wipe_tower_x") ==
               json::array({"12.5", "20.000"}) &&
               result.at("project").at("wipe_tower_y") ==
                   json::array({"-2", "40.000"}),
           "tower update did not preserve the untouched tail string");
    expect(result.at("effective_settings").at("wipe_tower_x") ==
               result.at("project").at("wipe_tower_x"),
           "tower summary disagrees with the final project");

    json initialize = base_project();
    initialize.erase("wipe_tower_x");
    initialize.erase("wipe_tower_y");
    json initialize_request = request("preserve_template");
    initialize_request["wipe_tower_positions"] =
        json::array({{{"plate_index", 0}, {"x_mm", 15.0}, {"y_mm", 25.0}}});
    json initialized = parsed_result(compose(initialize, initialize_request));
    expect(initialized.at("project").at("wipe_tower_x") == json::array({"15"}) &&
               initialized.at("project").at("wipe_tower_y") == json::array({"25"}),
           "tower plate zero was not initialized through the A2 path");

    for (const auto &positions :
         {json::array({{{"plate_index", 2}, {"x_mm", 1.0}, {"y_mm", 2.0}}}),
          json::array({{{"plate_index", 0}, {"x_mm", 1.0}, {"y_mm", 2.0}},
                       {{"plate_index", 0}, {"x_mm", 3.0}, {"y_mm", 4.0}}})}) {
        json invalid_position_request = request("preserve_template");
        invalid_position_request["wipe_tower_positions"] = positions;
        expect_error([&] { compose(base_project(), invalid_position_request); },
                     "plate_index");
    }
}

void test_f2_preserve_identity_and_optional_source_fields() {
    json project = base_project();
    project["printer_settings_id"] = "My tuned machine preset";
    project["print_settings_id"] = "My tuned process";
    project["filament_vendor"] = json::array({"", "", "", ""});
    project["filament_compatible_printers"] = json::array({"", "", "", ""});
    json result = parsed_result(compose(project, request("preserve_template")));
    expect(result.at("project").at("printer_settings_id") == "My tuned machine preset" &&
               result.at("project").at("print_settings_id") == "My tuned process" &&
               result.at("project").at("filament_vendor") == project.at("filament_vendor"),
           "preserve mode overwrote custom template identity or optional fields");

    json absent_descriptions = project;
    absent_descriptions.erase("filament_vendor");
    absent_descriptions.erase("filament_compatible_printers");
    parsed_result(compose(absent_descriptions, request("preserve_template")));

    json bed_conflict = project;
    bed_conflict["curr_bed_type"] = "Cool Plate";
    expect_error([&] { compose(bed_conflict, request("preserve_template")); },
                 "build plate");

    json wrong_model = project;
    wrong_model["printer_model"] = "Wrong Model";
    expect_error([&] { compose(wrong_model, request("preserve_template")); }, "machine");
    json wrong_nozzle = project;
    wrong_nozzle["nozzle_diameter"] = json::array({"0.2"});
    expect_error([&] { compose(wrong_nozzle, request("preserve_template")); }, "nozzle");

    json native = project;
    native["filament_type"] = json::array({"PLA", "PLA", "PLA", "PLA"});
    json native_result = parsed_result(compose(native, request("target_native_preset")));
    expect(native_result.at("project").at("filament_vendor").at(0) == "Bambu Lab" &&
               native_result.at("project").at("filament_compatible_printers").at(0) ==
                   "Bambu Lab A1 mini 0.4 nozzle",
           "native mode did not apply its separate exact source policy");

    json missing_ids = project;
    missing_ids.erase("filament_settings_id");
    expect_error([&] { compose(missing_ids, request("preserve_template")); }, "filament_settings_id");
    json missing_preserve_type = project;
    missing_preserve_type.erase("filament_type");
    expect_error([&] { compose(missing_preserve_type, request("preserve_template")); },
                 "filament_type");
    json missing_native_type = native;
    missing_native_type.erase("filament_type");
    expect_error([&] { compose(missing_native_type, request("target_native_preset")); },
                 "filament_type");
}

void test_f3_colors_use_real_slots_and_keep_independent_values() {
    json one_slot = base_project();
    one_slot["filament_settings_id"] = json::array({"Only Slot"});
    one_slot["filament_type"] = json::array({"PLA"});
    one_slot["filament_vendor"] = json::array({""});
    one_slot["filament_compatible_printers"] = json::array({""});
    json too_many = request("preserve_template");
    too_many["filament_colour"] = json::array({"#1", "#2"});
    expect_error([&] { compose(one_slot, too_many); }, "slot");

    json project = base_project();
    json main_only = request("preserve_template");
    main_only["filament_colour"] = json::array({"#AAAAAA", "#BBBBBB"});
    json result = parsed_result(compose(project, main_only));
    expect(result.at("project").at("filament_colour") ==
               json::array({"#AAAAAA", "#BBBBBB", "#333333", "#444444"}) &&
               result.at("project").at("filament_multi_colour") ==
                   project.at("filament_multi_colour"),
           "main and multi-colour inputs were incorrectly forced together");
}

void test_f4_effective_summary_contains_final_layout_fields() {
    json result = parsed_result(compose(base_project(), request("preserve_template")));
    const json &project = result.at("project");
    const json &summary = result.at("effective_settings");
    for (const auto &key : {"brim_width", "prime_tower_brim_width",
                            "prime_tower_rib_wall", "prime_tower_rib_width",
                            "prime_tower_extra_rib_length", "prime_tower_infill_gap",
                            "bed_exclude_area", "extruder_printable_area",
                            "filament_prime_volume", "filament_change_length",
                            "filament_diameter"}) {
        expect(summary.contains(key) && summary.at(key) == project.at(key),
               std::string("effective summary omitted final layout field ") + key);
    }
    expect(summary.at("initial_layer_print_height") == "0.20" &&
               summary.at("initial_layer_height") == "0.08",
           "effective summary changed the independent first-layer fields");
}

void test_f5_native_differences_preserve_unmanaged_registered_values() {
    json project = base_project();
    project["filament_type"] = json::array({"PLA", "PLA", "PLA", "PLA"});
    project["my_vendor_knob"] = json::array({"keep-a", "keep-b", "keep-c", "keep-d"});
    project["different_settings_to_system"] =
        json::array({"existing_process", "nozzle_temperature;my_vendor_knob", "", "", "", "tail_marker"});
    json result = parsed_result(compose(project, request("target_native_preset")));
    expect(result.at("project").at("my_vendor_knob") == project.at("my_vendor_knob") &&
               result.at("project").at("different_settings_to_system").at(1) ==
                   "my_vendor_knob" &&
               result.at("project").at("different_settings_to_system").at(5) ==
                   "default_filament_profile;tail_marker",
           "native mode dropped an unmanaged value or its slot registration");

    json unclassified = project;
    unclassified["different_settings_to_system"] =
        json::array({"existing_process", "missing_knob", "", "", "", "tail_marker"});
    expect_error([&] { compose(unclassified, request("target_native_preset")); },
                 "missing_knob");

    json nonportable = project;
    nonportable["filament_custom_gcode"] =
        json::array({"keep-a", "keep-b", "keep-c", "keep-d"});
    nonportable["different_settings_to_system"] =
        json::array({"existing_process", "filament_custom_gcode", "", "", "", "tail_marker"});
    expect_error([&] { compose(nonportable, request("target_native_preset")); },
                 "non-portable");
}

void test_adapter_boundaries_and_initialization() {
    json project = base_project();
    project["wipe_tower_x"] = json::array({"10", "+20.000"});
    project["wipe_tower_y"] = json::array({"30", ".5"});
    json array_request = request("preserve_template");
    array_request["wipe_tower_x"] = json::array({"12.5", "+20.000"});
    array_request["wipe_tower_y"] = json::array({"-2", ".5"});
    json array_result = parsed_result(compose(project, array_request));
    expect(array_result.at("project").at("wipe_tower_x") ==
               json::array({"12.5", "+20.000"}) &&
               array_result.at("project").at("wipe_tower_y") ==
                   json::array({"-2", ".5"}),
           "array coordinates must accept A2 numeric strings and preserve the untouched tail");

    json no_difference = base_project();
    no_difference.erase("different_settings_to_system");
    json tower_only = request("preserve_template");
    tower_only["prime_tower_width"] = "35";
    json tower_result = parsed_result(compose(no_difference, tower_only));
    expect(tower_result.at("project").at("prime_tower_width") == "35" &&
               tower_result.at("project").at("different_settings_to_system").size() == 6,
           "composer must initialize its own missing difference index before A2");

    json scalar_and_tower = request("preserve_template");
    scalar_and_tower["layer_height"] = "0.12";
    scalar_and_tower["prime_tower_width"] = "35";
    json one_call = parsed_result(compose(no_difference, scalar_and_tower));
    json first_call_request = request("preserve_template");
    first_call_request["layer_height"] = "0.12";
    json first_call = parsed_result(compose(no_difference, first_call_request));
    json second_call_request = request("preserve_template");
    second_call_request["prime_tower_width"] = "35";
    json two_calls = parsed_result(
        compose(first_call.at("project"), second_call_request));
    expect(one_call.at("project") == two_calls.at("project"),
           "one-call and two-call process/tower composition must agree");

    json native_project = no_difference;
    native_project["filament_type"] = json::array({"PLA", "PLA", "PLA", "PLA"});
    json native_tower = request("target_native_preset");
    native_tower["prime_tower_width"] = "35";
    json native_result = parsed_result(compose(native_project, native_tower));
    expect(native_result.at("project").at("prime_tower_width") == "35" &&
               native_result.at("project").at("different_settings_to_system").size() == 6,
           "native mode must use the same pre-A2 difference-index preparation");
}

void test_typed_positions_preserve_negative_zero_and_reject_plate_zero() {
    const std::string raw_request = R"json({
      "slicer_id":"BambuStudio","application_version":"02.08.02.61",
      "machine_uid":"bambu-lab:a1-mini","nozzle_uid":"nozzle:0.4mm",
      "build_plate_uid":"plate:textured-pei",
      "material_mode":"preserve_template",
      "wipe_tower_positions":[{"plate_index":0,"x_mm":-0,"y_mm":-0}]
    })json";
    json result = parsed_result(compose_raw_request(base_project(), raw_request));
    expect(result.at("project").at("wipe_tower_x").at(0) == "-0" &&
               result.at("project").at("wipe_tower_y").at(0) == "-0",
           "typed positions must preserve integer negative zero through the composer");

    const std::string negative_plate_request = R"json({
      "slicer_id":"BambuStudio","application_version":"02.08.02.61",
      "machine_uid":"bambu-lab:a1-mini","nozzle_uid":"nozzle:0.4mm",
      "build_plate_uid":"plate:textured-pei",
      "material_mode":"preserve_template",
      "wipe_tower_positions":[{"plate_index":-0,"x_mm":1,"y_mm":2}]
    })json";
    expect_error([&] { compose_raw_request(base_project(), negative_plate_request); },
                 "plate_index");
}

void test_missing_main_colour_is_not_filled_with_fake_tail() {
    json project = base_project();
    project.erase("filament_colour");
    json partial = request("preserve_template");
    partial["filament_colour"] = json::array({"#123456"});
    expect_error([&] { compose(project, partial); }, "filament_colour");

    json complete = request("preserve_template");
    complete["filament_colour"] =
        json::array({"#111111", "#222222", "#333333", "#444444"});
    json result = parsed_result(compose(project, complete));
    expect(result.at("project").at("filament_colour") ==
               complete.at("filament_colour"),
           "a complete requested main-colour vector should be accepted");
}

void test_two_targets_and_material_modes_share_composition() {
    for (const auto *file : {"bambu-studio-02.08.02.61.json", "orca-slicer-2.4.2.json"}) {
        const json target = json::parse(read_file(
            std::string("compatibility/current-src/translations/targets/") + file));
        for (const auto *mode : {"preserve_template", "target_native_preset"}) {
            json source = base_project();
            source["filament_type"] = json::array({"PLA", "PLA", "PLA", "PLA"});
            source["filament_retraction_length"] = json::array({"0.6", "0.7", "0.8", "0.9"});
            json selection = request(mode);
            selection["slicer_id"] = target.at("target_contract").at("slicer_id");
            selection["application_version"] = target.at("target_contract").at("application_version");
            selection["filament_colour"] = json::array({"#ABCDEF", "#FEDCBA"});
            const json result = parsed_result(compose(source, selection, canonical_data(), target));
            const json &out = result.at("project");
            expect(result.at("wipe_tower_dialect") == target.at("package_dialect").at("wipe_tower"),
                   "project result must return the selected tower dialect");
            const json expected_build_item_properties = target.at("package_dialect")
                .value("metadata_components", json::object())
                .value("build_item_properties", json::object());
            expect(result.at("build_item_properties") == expected_build_item_properties,
                   "build-item properties must come from the selected metadata target");
            expect(out.at("outer_wall_speed") == "47" && out.at("outer_wall_acceleration") == "1234",
                   "material mode changed unrelated process tuning");
            expect(out.at("filament_colour") == json::array({"#ABCDEF", "#FEDCBA", "#333333", "#444444"}),
                   "partial colour changes must preserve tail slots");
            if (std::string(mode) == "preserve_template") {
                for (const auto *key : {"filament_settings_id", "nozzle_temperature",
                                       "nozzle_temperature_initial_layer", "filament_flow_ratio",
                                       "filament_retraction_length"}) {
                    expect(out.at(key) == source.at(key), std::string("preserve changed ") + key);
                }
            } else {
                const auto &material = target.at("material_bindings").at(0);
                const auto &profile = material_profile(target, material);
                const auto &flow = profile.at("target_parameters").at("filament_flow_ratio").at("values").at(0);
                expect(out.at("filament_flow_ratio") == json::array({flow, flow, flow, flow}),
                       "native mode did not use its own target parameters");
                expect(out.at("filament_settings_id").at(3) == material.at("native_profile_id"),
                       "native material identity did not reach tail slot");
            }
        }
    }
}

void test_orca_dual_variants_keep_every_template_slot() {
    const json target = json::parse(read_file(
        "compatibility/current-src/translations/targets/orca-slicer-2.4.2.json"));
    json source = base_project();
    source["filament_flow_ratio"] = json::array({"0.91", "1.1", "0.92", "1.2", "0.93", "1.3", "0.94", "1.4"});
    source["filament_retraction_length"] = json::array({"0.6", "1.6", "0.7", "1.7", "0.8", "1.8", "0.9", "1.9"});
    source["filament_custom_unknown"] = source.at("filament_flow_ratio");
    source["flush_volumes_matrix"] = json::array({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    json selection = request("preserve_template");
    selection["slicer_id"] = "OrcaSlicer";
    selection["application_version"] = "2.4.2";
    selection["filament_colour"] = json::array({"#ABCDEF", "#FEDCBA"});
    const json out = parsed_result(compose(source, selection, canonical_data(), target)).at("project");
    expect(out.at("filament_flow_ratio") == json::array({"0.91", "0.92", "0.93", "0.94"}),
           "Orca must choose the first variant in all four slots, not just active colours");
    expect(out.at("filament_retraction_length") == json::array({"0.6", "0.7", "0.8", "0.9"}),
           "Orca retraction variants lost slot identity");
    expect(out.at("filament_custom_unknown") == source.at("filament_custom_unknown") &&
               out.at("flush_volumes_matrix") == source.at("flush_volumes_matrix"),
           "unknown or square arrays must not be collapsed");
    selection["slicer_id"] = "BambuStudio";
    selection["application_version"] = "02.08.02.61";
    expect(parsed_result(compose(source, selection)).at("project").at("filament_flow_ratio") ==
               source.at("filament_flow_ratio"), "Bambu grouped values must remain unchanged");
}

void test_variant_shape_and_plate_support_are_machine_specific() {
    json target = json::parse(read_file(
        "compatibility/current-src/translations/targets/orca-slicer-2.4.2.json"));
    target["machine_bindings"][0]["first_variant_per_slot_keys"] = json::array({"filament_flow_ratio"});
    target["machine_bindings"][0]["supported_build_plate_uids"] = json::array({"plate:textured-pei"});
    json source = base_project();
    source["filament_flow_ratio"] = json::array({"0.91", "1.1", "0.92", "1.2", "0.93", "1.3", "0.94", "1.4"});
    source["filament_retraction_length"] = json::array({"0.6", "1.6", "0.7", "1.7", "0.8", "1.8", "0.9", "1.9"});
    source["unknown_2n"] = source.at("filament_flow_ratio");
    json selection = request("preserve_template");
    selection["slicer_id"] = "OrcaSlicer";
    selection["application_version"] = "2.4.2";
    const json out = parsed_result(compose(source, selection, canonical_data(), target)).at("project");
    expect(out.at("filament_flow_ratio") == json::array({"0.91", "0.92", "0.93", "0.94"}),
           "declared first-variant field did not preserve every slot");
    expect(out.at("filament_retraction_length") == source.at("filament_retraction_length") &&
               out.at("unknown_2n") == source.at("unknown_2n"),
           "array length or another machine's shape collapsed an undeclared field");
    target["machine_bindings"][0]["first_variant_per_slot_keys"] = json::array();
    expect(parsed_result(compose(source, selection, canonical_data(), target)).at("project") == source,
           "a machine with native dual variants must preserve grouped data");
    target["machine_bindings"][0]["supported_build_plate_uids"] = json::array();
    expect_error([&] { compose(source, selection, canonical_data(), target); }, "build plate");
}

}  // namespace

int main() {
    try {
        test_variant_shape_and_plate_support_are_machine_specific();
        test_preserve_template_does_not_require_native_material();
        test_native_material_lookup_matches_nozzle_independent_of_order();
        test_preserve_template_process_and_colors();
        test_preserve_template_extended_process_overrides();
        test_native_material_is_data_driven_and_distinct();
        test_layer_aliases_brim_and_wipe_summary();
        test_errors_and_input_immutability();
        test_difference_stability_and_alternate_mapping();
        test_target_data_is_not_brand_hardcoded();
        test_f1_validates_values_and_reuses_tower_rules();
        test_f2_preserve_identity_and_optional_source_fields();
        test_f3_colors_use_real_slots_and_keep_independent_values();
        test_f4_effective_summary_contains_final_layout_fields();
        test_f5_native_differences_preserve_unmanaged_registered_values();
        test_adapter_boundaries_and_initialization();
        test_typed_positions_preserve_negative_zero_and_reject_plate_zero();
        test_missing_main_colour_is_not_filled_with_fake_tail();
        test_two_targets_and_material_modes_share_composition();
        test_orca_dual_variants_keep_every_template_slot();
    } catch (const std::exception &error) {
        std::cerr << "project_settings_test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "project_settings_test passed: 20 behavior groups\n";
    return EXIT_SUCCESS;
}
