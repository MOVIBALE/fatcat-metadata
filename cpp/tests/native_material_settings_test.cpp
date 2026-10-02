#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fatcat/project_settings.h"

namespace {
using json = nlohmann::json;

json read_json(const std::string &path) {
    std::ifstream stream(std::string(FATCAT_TEST_REPOSITORY_ROOT) + "/" + path);
    if (!stream) throw std::runtime_error("cannot read " + path);
    return json::parse(stream);
}

json repeated(std::size_t count, const json &value) {
    json result = json::array();
    while (count--) result.push_back(value);
    return result;
}

std::string target_filename(const std::string &slicer) {
    return slicer == "BambuStudio" ? "bambu-studio-02.08.02.61.json"
                                     : "orca-slicer-2.4.2.json";
}

json synthetic_project(const json &machine, std::size_t count,
                       bool machine_preset = false) {
    const auto width = machine.at("machine_uid") == "bambu-lab:p1s" ? 2U : 1U;
    json project = {
        {"printer_settings_id", machine.at("source_profile_name")},
        {"printer_model", machine.at("printer_model")},
        {"printer_variant", machine.at("printer_variant")},
        {"nozzle_diameter", machine.at("nozzle_diameter")},
        {"printable_area", machine.at("printable_area")},
        {"printable_height", machine.at("printable_height")},
        {"print_settings_id", "Synthetic Standard Process"},
        {"version", machine.value("source_version", "synthetic-project-v1")},
        {"default_filament_profile", json::array({"Synthetic Default PLA"})},
        {"filament_type", json::array()},
        {"filament_vendor", json::array()},
        {"filament_colour", json::array()},
        {"filament_multi_colour", json::array()},
        {"nozzle_temperature", json::array()},
        {"filament_flow_ratio", json::array()},
        {"filament_max_volumetric_speed", json::array()},
        {"filament_self_index", json::array()},
        {"filament_extruder_variant", json::array()},
        {"different_settings_to_system", json::array({"synthetic-process"})},
    };
    if (!machine_preset) project["filament_settings_id"] = json::array();
    for (std::size_t slot = 0; slot < count; ++slot) {
        const bool petg = slot == 0;
        if (!machine_preset) {
            project["filament_settings_id"].push_back(
                "Synthetic " + std::string(petg ? "PETG" : "PLA") + " slot " +
                std::to_string(slot));
        }
        project["filament_type"].push_back(petg ? "PETG" : "PLA");
        project["filament_vendor"].push_back("Synthetic Vendor");
        project["filament_colour"].push_back("#AABBCC");
        project["filament_multi_colour"].push_back("#AABBCC");
        for (std::size_t variant = 0; variant < width; ++variant) {
            project["nozzle_temperature"].push_back(petg ? "250" : "220");
            project["filament_flow_ratio"].push_back(petg ? "0.95" : "0.98");
            project["filament_max_volumetric_speed"].push_back(petg ? "1" : "2");
            project["filament_self_index"].push_back(std::to_string(slot + 1));
            project["filament_extruder_variant"].push_back("0");
        }
        project["different_settings_to_system"].push_back("");
    }
    project["different_settings_to_system"].push_back("synthetic-machine-tail");
    if (machine_preset) {
        project["type"] = "machine";
        for (auto it = machine.at("native_project_defaults").begin();
             it != machine.at("native_project_defaults").end(); ++it) {
            project.erase(it.key());
        }
    }
    return project;
}

struct Fixture {
    json project, request, canonical, target;

    Fixture(const std::string &slicer, std::size_t count = 2) {
        const std::string version = slicer == "BambuStudio" ? "02.08.02.61" : "2.4.2";
        target = read_json("compatibility/current-src/translations/targets/" +
                           target_filename(slicer));
        const auto machine = std::find_if(
            target.at("machine_bindings").begin(), target.at("machine_bindings").end(),
            [](const json &candidate) {
                return candidate.at("machine_uid") == "bambu-lab:p1s" &&
                       candidate.at("nozzle_uid") == "nozzle:0.2mm";
            });
        if (machine == target.at("machine_bindings").end()) {
            throw std::runtime_error("missing public P1S target binding");
        }
        project = synthetic_project(*machine, 9);
        project["curr_bed_type"] = "Textured PEI Plate";
        request = {{"slicer_id", slicer}, {"application_version", version},
                   {"machine_uid", "bambu-lab:p1s"}, {"nozzle_uid", "nozzle:0.2mm"},
                   {"build_plate_uid", "plate:textured-pei"}, {"material_mode", "target_native_preset"},
                   {"filament_slot_mode", "compact"}, {"default_filament_source_slot", 0},
                   {"filament_colour", repeated(count, "#AABBCC")},
                   {"material_uids", repeated(count, "material:pla")}};
        canonical = read_json("compatibility/current-src/translations/canonical.json");
    }

    json compose() const {
        const auto result = json::parse(fatcat::compose_project_settings(
            project.dump(), request.dump(), canonical.dump(), target.dump()));
        return json::parse(result.at("project_settings_json").get<std::string>());
    }

    json &binding(const std::string &uid) {
        for (auto &item : target["material_bindings"]) {
            if (item.at("machine_uid") == request.at("machine_uid") &&
                item.at("nozzle_uid") == request.at("nozzle_uid") && item.at("material_uid") == uid) return item;
        }
        throw std::runtime_error("missing test binding");
    }

    json &parameter_set(const std::string &uid) {
        const auto profile_key = binding(uid).at("material_profile_key");
        for (const auto &profile : target["material_profiles"]) {
            if (profile.at("material_profile_key") != profile_key) continue;
            const auto parameter_set_key = profile.at("target_parameter_set_key");
            for (auto &item : target["target_parameter_sets"]) {
                if (item.at("target_parameter_set_key") == parameter_set_key) return item;
            }
            throw std::runtime_error("missing test material parameter set");
        }
        throw std::runtime_error("missing test material profile");
    }
};

void expect(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void expect_error(const Fixture &fixture, const std::vector<std::string> &needles) {
    try { fixture.compose(); }
    catch (const fatcat::ProjectSettingsError &error) {
        for (const auto &needle : needles) {
            expect(std::string(error.what()).find(needle) != std::string::npos,
                   std::string("error missing ") + needle + ": " + error.what());
        }
        return;
    }
    throw std::runtime_error("expected a native material selection error");
}

void requested_materials_replace_mixed_sources() {
    for (const auto *slicer : {"BambuStudio", "OrcaSlicer"}) {
        for (const std::size_t count : {1, 2, 6, 8, 16, 32}) {
            Fixture f(slicer, count);
            f.project["print_settings_id"] = "My tuned process";
            f.project["printer_settings_id"] = "My tuned hardware";
            f.project["version"] = "source-version";
            f.project["unknown_native_value"] = json::array({"keep", "exact"});
            const auto source_slots = f.project.at("filament_settings_id").size();
            f.project["flush_volumes_matrix"] = json::array();
            for (std::size_t row = 0; row < source_slots; ++row) {
                for (std::size_t col = 0; col < source_slots; ++col) {
                    f.project["flush_volumes_matrix"].push_back(std::to_string(row * 100 + col));
                }
            }
            json types = json::array(), temperature = json::array(), flow = json::array(), speed = json::array(), routing = json::array();
            for (std::size_t slot = 0; slot < count; ++slot) {
                const bool petg = slot % 2 != 0;
                f.request["material_uids"][slot] = petg ? "material:petg" : "material:pla";
                types.push_back(petg ? "PETG" : "PLA");
                for (int variant = 0; variant < 2; ++variant) {
                    temperature.push_back(petg ? "255" : "220");
                    flow.push_back(petg ? "0.95" : "0.98");
                    speed.push_back(petg ? "1" : "2");
                    routing.push_back(std::to_string(slot + 1));
                }
            }
            const auto out = f.compose();
            json matrix = json::array();
            if (count > source_slots) matrix = f.project.at("flush_volumes_matrix");
            else for (std::size_t row = 0; row < count; ++row) {
                for (std::size_t col = 0; col < count; ++col) matrix.push_back(std::to_string(row * 100 + col));
            }
            expect(out.at("flush_volumes_matrix") == matrix, "native material selection changed compact matrix rules");
            expect(out.at("filament_type") == types, "native types must come from every requested slot");
            expect(out.at("nozzle_temperature") == temperature && out.at("filament_flow_ratio") == flow &&
                       out.at("filament_max_volumetric_speed") == speed, "native values borrowed source or adjacent material values");
            expect(out.at("filament_self_index") == routing, "native routing must use final output slot identity");
            expect(out.at("different_settings_to_system").size() == count + 2, "native compact difference count is not M+2");
            for (const auto *key : {"print_settings_id", "printer_settings_id", "version", "unknown_native_value", "printable_area", "printable_height"}) {
                expect(out.at(key) == f.project.at(key), std::string("native material changed nonmaterial field ") + key);
            }
            expect(out.at("default_filament_profile").at(0) == "Bambu PLA Basic @BBL X1C 0.2 nozzle", "default profile must use final native identity");
        }
    }
}

void scalar_and_array_modes_preserve_all_logical_slots() {
    Fixture f("BambuStudio");
    f.request.erase("filament_slot_mode");
    f.request.erase("default_filament_source_slot");
    f.request["material_uids"] = repeated(9, "material:petg");
    const auto array_output = f.compose();
    expect(array_output.at("filament_type") == repeated(9, "PETG"), "native preserve-slot mode dropped the N tail");
    expect(array_output.at("nozzle_temperature") == repeated(18, "255"), "scalar native PETG must replace all original materials");
    f.request.erase("material_uids");
    f.request["material_uid"] = "material:petg";
    expect(f.compose() == array_output, "scalar material selection must use the per-slot path");
}

void selector_conflicts_and_lengths_are_explicit() {
    Fixture f("BambuStudio");
    f.request["material_uid"] = "material:pla";
    expect_error(f, {"material_uid", "material_uids"});
    f.request.erase("material_uid");
    f.request["material_uids"] = json::array({"material:pla"});
    expect_error(f, {"material_uids", "2"});
    f.request["material_uids"] = json::array({"material:pla", ""});
    expect_error(f, {"material_uids"});
    f.request["material_mode"] = "preserve_template";
    f.request["material_uids"] = json::array({"material:pla", "material:petg"});
    expect_error(f, {"preserve_template", "material_uids"});
    f.request.erase("material_uids");
    f.request["material_uid"] = "material:pla";
    expect_error(f, {"preserve_template", "material_uid"});
}

void missing_exact_bindings_and_values_identify_the_slot() {
    Fixture f("OrcaSlicer");
    f.request["material_uids"] = json::array({"material:pla", "material:petg"});
    f.binding("material:petg")["nozzle_uid"] = "nozzle:0.8mm";
    expect_error(f, {"OrcaSlicer", "2.4.2", "bambu-lab:p1s", "nozzle:0.2mm", "material:petg", "slot 1"});
    f.request["material_uids"] = repeated(2, "material:pla");
    expect(f.compose().at("filament_type") == repeated(2, "PLA"), "missing PETG must not block an available PLA binding");
    Fixture missing("BambuStudio");
    missing.request["material_uids"] = json::array({"material:pla", "material:petg"});
    missing.parameter_set("material:pla")["target_parameters"].erase("first_x_layer_fan_speed");
    expect_error(missing, {"BambuStudio", "02.08.02.61", "bambu-lab:p1s", "nozzle:0.2mm", "material:pla", "slot 0", "first_x_layer_fan_speed"});
    Fixture absent("BambuStudio");
    absent.request["material_uids"] = json::array({"material:pla", "material:petg"});
    auto &petg = absent.parameter_set("material:petg");
    petg["target_parameters"].erase("first_x_layer_fan_speed");
    auto &managed = petg["managed_parameter_keys"];
    managed.erase(std::remove(managed.begin(), managed.end(), "first_x_layer_fan_speed"), managed.end());
    expect_error(absent, {"material:petg", "slot 1", "first_x_layer_fan_speed"});
    absent.request["material_uids"][1] = "material:petg-hf";
    expect_error(absent, {"material:petg-hf", "slot 1"});
}

void absent_native_parameters_clear_stale_values_without_inventing_defaults() {
    Fixture f("BambuStudio", 1);
    auto &profile = f.parameter_set("material:pla");
    profile["managed_parameter_keys"].push_back("filament_optional_native_value");
    f.project["filament_optional_native_value"] = repeated(4, "stale-source-value");

    const auto output = f.compose();

    expect(!output.contains("filament_optional_native_value"),
           "a native field with no evidenced value must clear the stale source value");
}

void duplicate_exact_bindings_are_rejected() {
    Fixture f("BambuStudio", 1);
    const auto expected_binding = f.binding("material:pla");
    f.target["material_bindings"].push_back(expected_binding);
    const auto duplicate_count = std::count_if(
        f.target.at("material_bindings").begin(), f.target.at("material_bindings").end(),
        [&](const json &binding) {
            return binding.at("machine_uid") == expected_binding.at("machine_uid") &&
                binding.at("nozzle_uid") == expected_binding.at("nozzle_uid") &&
                binding.at("material_uid") == expected_binding.at("material_uid");
        });
    expect(duplicate_count == 2,
           "duplicate binding test fixture did not preserve the selected identity");

    expect_error(f, {"multiple material_bindings", "material:pla"});
}

void binding_order_and_full_variant_groups_are_independent() {
    Fixture f("OrcaSlicer");
    f.request["material_uids"] = json::array({"material:petg", "material:pla"});
    f.parameter_set("material:pla")["target_parameters"]["filament_flow_ratio"]["values"] = json::array({"0.91", "0.92"});
    f.parameter_set("material:petg")["target_parameters"]["filament_flow_ratio"]["values"] = json::array({"0.96", "0.97"});
    for (const auto *uid : {"material:pla", "material:petg"}) {
        auto &profile = f.parameter_set(uid);
        profile["managed_parameter_keys"].push_back("filament_dev_ams_drying_temperature");
        profile["target_parameters"]["filament_dev_ams_drying_temperature"] = {
            {"cardinality", "per_slot_group"}, {"values", std::string(uid) == "material:pla"
                ? json::array({"41", "42", "43", "44"}) : json::array({"61", "62", "63", "64"})}};
    }
    const auto expected = f.compose();
    expect(expected.at("filament_flow_ratio") == json::array({"0.96", "0.97", "0.91", "0.92"}), "native copied only the first flow variant");
    expect(expected.at("filament_dev_ams_drying_temperature") == json::array({"61", "62", "63", "64", "41", "42", "43", "44"}), "native copied only part of a four-value material group");
    std::reverse(f.target["material_bindings"].begin(), f.target["material_bindings"].end());
    expect(f.compose() == expected, "native lookup depends on binding order");
}

void modes_do_not_leak_and_native_overrides_have_final_priority() {
    Fixture f("BambuStudio");
    f.request["material_uids"] = json::array({"material:pla", "material:petg"});
    f.request["filament_source_slots"] = json::array({0, 1});
    f.request["disable_cut_retraction"] = true;
    f.project["different_settings_to_system"] = repeated(11, "");
    f.project["different_settings_to_system"][0] = "process";
    f.project["different_settings_to_system"][1] = "nozzle_temperature;filament_flow_ratio;unknown_native_value";
    f.project["different_settings_to_system"][10] = "machine";
    f.project["unknown_native_value"] = json::array({"exact"});
    for (const auto *uid : {"material:pla", "material:petg"}) {
        auto &profile = f.parameter_set(uid);
        profile["managed_parameter_keys"].push_back("filament_self_index");
        profile["target_parameters"]["filament_self_index"] = {{"cardinality", "per_slot_group"}, {"values", json::array({"99", "99"})}};
    }
    const auto native = f.compose();
    expect(native.at("filament_self_index") == json::array({"1", "1", "2", "2"}), "native snapshot overwrote output routing");
    expect(native.at("filament_long_retractions_when_cut") == repeated(2, "nil") &&
               native.at("filament_retraction_distances_when_cut") == repeated(2, "nil"), "explicit cut policy lost final precedence");
    expect(native.at("filament_colour") == repeated(2, "#AABBCC"), "native material overwrote caller colours");
    const auto diff = native.at("different_settings_to_system").at(1).get<std::string>();
    expect(diff.find("filament_flow_ratio") == std::string::npos && diff.find("nozzle_temperature") == std::string::npos &&
               diff.find("unknown_native_value") != std::string::npos && diff.find("filament_colour") != std::string::npos &&
               diff.find("filament_long_retractions_when_cut") != std::string::npos, "native difference cleanup lost per-slot ownership/final overrides");
    Fixture preserve = f;
    preserve.request["material_mode"] = "preserve_template";
    preserve.request.erase("material_uids");
    preserve.target.erase("material_bindings");
    preserve.target.erase("material_profiles");
    preserve.target.erase("target_parameter_sets");
    preserve.canonical.erase("materials");
    const auto preserved = preserve.compose();
    expect(preserved.at("filament_type") == json::array({"PETG", "PLA"}), "preserve must retain explicit source materials without bindings");
    expect(f.compose() == native, "native/preserve/native calls shared state");
}

void registry_only_x1_defaults_apply_to_machine_shaped_source() {
    const auto canonical = read_json("compatibility/current-src/translations/canonical.json");
    for (const auto *slicer : {"BambuStudio", "OrcaSlicer"}) {
        const auto target = read_json("compatibility/current-src/translations/targets/" +
            target_filename(slicer));
        const std::string application_version =
            slicer == std::string("BambuStudio") ? "02.08.02.61" : "2.4.2";
        for (const auto &machine : target.at("machine_bindings")) {
            if (machine.at("machine_uid") != "bambu-lab:x1") continue;
            json source = synthetic_project(machine, 8, true);
            expect(!source.contains("filament_settings_id"), "X1 source unexpectedly gained a full project identity");
            const json request = {
                {"slicer_id", slicer}, {"application_version", application_version},
                {"machine_uid", machine.at("machine_uid")}, {"nozzle_uid", machine.at("nozzle_uid")},
                {"build_plate_uid", "plate:textured-pei"}, {"material_mode", "target_native_preset"},
                {"filament_slot_mode", "compact"}, {"default_filament_source_slot", 0},
                {"filament_colour", repeated(4, "#AABBCC")},
                {"material_uids", json::array({"material:pla", "material:petg", "material:petg", "material:pla"})},
            };
            const auto output = json::parse(fatcat::compose_project_settings(
                source.dump(), request.dump(), canonical.dump(), target.dump()));
            const auto final = json::parse(output.at("project_settings_json").get<std::string>());
            const json expected_tower = std::string(slicer) == "BambuStudio"
                ? json{{"prime_tower_brim_width", "3"}, {"prime_tower_infill_gap", "150%"},
                       {"prime_tower_rib_wall", "1"}, {"prime_tower_rib_width", "8"},
                       {"prime_tower_extra_rib_length", "0"}, {"prime_tower_fillet_wall", "1"}}
                : json{{"prime_tower_brim_width", "3"}, {"prime_tower_infill_gap", "150%"},
                       {"prime_volume", "45"},
                       {"wipe_tower_wall_type", "rib"}, {"wipe_tower_rib_width", "8"},
                       {"wipe_tower_extra_rib_length", "0"}, {"wipe_tower_fillet_wall", "1"},
                       {"wipe_tower_cone_angle", "30"}};
            for (const auto &[key, value] : expected_tower.items()) {
                expect(final.at(key) == value, "X1 native tower default is missing: " + key);
                expect(output.at("effective_settings").at(key) == value,
                       "X1 effective tower setting is missing: " + key);
            }
            if (std::string(slicer) == "BambuStudio") {
                expect(!final.contains("prime_volume"),
                       "Bambu must not inherit Orca's commented-out prime volume default");
            }
            expect(final.at("filament_type") == json::array({"PLA", "PETG", "PETG", "PLA"}),
                   "X1 must use requested exact native material types");
            expect(final.at("filament_settings_id").size() == 4, "X1 native identity count differs from final slots");
            expect(final.at("curr_bed_type") == "Textured PEI Plate", "X1 plate selection was not applied");
            expect(final.at("printer_model") == source.at("printer_model"), "X1 machine identity changed");
            expect(!final.contains("type") && !final.contains("instantiation") &&
                   !final.contains("inherits") && !final.contains("setting_id"),
                   "X1 native project retains machine-preset-only markers");
            expect(final.at("name") == "project_settings" && final.at("from") == "project",
                   "X1 native result is not a project preset");
            expect(final.at("version") == machine.at("source_version"),
                   "X1 native result lost the source software version");
            json partly_tuned_source = source;
            partly_tuned_source["prime_tower_brim_width"] = "9";
            partly_tuned_source["version"] = "tuned-version";
            const auto partly_tuned = json::parse(fatcat::compose_project_settings(
                partly_tuned_source.dump(), request.dump(), canonical.dump(), target.dump()));
            const auto partly_tuned_project = json::parse(partly_tuned.at("project_settings_json").get<std::string>());
            expect(partly_tuned_project.at("prime_tower_brim_width") == "9" &&
                   partly_tuned_project.at("version") == "tuned-version",
                   "X1 native defaults overwrote present source values");
            json complete = final;
            complete["prime_tower_brim_width"] = "9";
            complete["version"] = "user-version";
            const auto retained = json::parse(fatcat::compose_project_settings(
                complete.dump(), request.dump(), canonical.dump(), target.dump()));
            const auto retained_project = json::parse(retained.at("project_settings_json").get<std::string>());
            expect(retained_project.at("prime_tower_brim_width") == "9" &&
                   retained_project.at("version") == "user-version",
                   "complete user project was overwritten by registry-only defaults");
        }
    }
}
}  // namespace

int main() {
    int failed = 0;
    for (const auto &test : std::vector<std::pair<std::string, std::function<void()>>>{
             {"requested native materials", requested_materials_replace_mixed_sources},
             {"scalar and final slots", scalar_and_array_modes_preserve_all_logical_slots},
             {"selector errors", selector_conflicts_and_lengths_are_explicit},
             {"precise missing sources", missing_exact_bindings_and_values_identify_the_slot},
             {"absent native parameter cleanup", absent_native_parameters_clear_stale_values_without_inventing_defaults},
             {"duplicate exact binding rejection", duplicate_exact_bindings_are_rejected},
             {"binding order and variants", binding_order_and_full_variant_groups_are_independent},
             {"mode isolation and final overrides", modes_do_not_leak_and_native_overrides_have_final_priority},
             {"registry-only X1 machine source", registry_only_x1_defaults_apply_to_machine_shaped_source}}) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception &error) { ++failed; std::cerr << "FAIL " << test.first << ": " << error.what() << '\n'; }
    }
    return failed ? 1 : 0;
}
