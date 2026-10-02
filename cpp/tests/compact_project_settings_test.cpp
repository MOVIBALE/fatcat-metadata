#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fatcat/project_settings.h"

namespace {
using json = nlohmann::json;

json read_json(const std::string &path) {
    std::ifstream stream(std::string(FATCAT_TEST_REPOSITORY_ROOT) + "/" + path);
    if (!stream) {
        throw std::runtime_error("cannot read fixture " + path);
    }
    return json::parse(stream);
}

json device(const std::string &slicer, const std::string &machine,
            const std::string &nozzle = "nozzle:0.4mm") {
    const std::string version = slicer == "BambuStudio" ? "02.08.02.61" : "2.4.2";
    const std::string target_name = slicer == "BambuStudio"
        ? "bambu-studio-02.08.02.61.json" : "orca-slicer-2.4.2.json";
    const auto target = read_json(
        "compatibility/current-src/translations/targets/" + target_name);
    const auto binding = std::find_if(
        target.at("machine_bindings").begin(), target.at("machine_bindings").end(),
        [&](const json &candidate) {
            return candidate.at("machine_uid") == machine &&
                   candidate.at("nozzle_uid") == nozzle;
        });
    if (binding == target.at("machine_bindings").end()) {
        throw std::runtime_error("missing public target machine binding");
    }

    const std::size_t count = nozzle == "nozzle:0.2mm" &&
                                      machine == "bambu-lab:p1s"
                                  ? 9
                                  : 8;
    const std::size_t group_width = machine == "bambu-lab:p1s" ? 2 : 1;
    json project = {
        {"printer_settings_id", binding->at("source_profile_name")},
        {"printer_model", binding->at("printer_model")},
        {"printer_variant", binding->at("printer_variant")},
        {"nozzle_diameter", binding->at("nozzle_diameter")},
        {"printable_area", binding->at("printable_area")},
        {"printable_height", binding->at("printable_height")},
        {"print_settings_id", "Synthetic Standard Process"},
        {"version", version},
        {"default_filament_profile", json::array({"Synthetic default", "keep-extra"})},
        {"filament_settings_id", json::array()},
        {"filament_type", json::array()},
        {"filament_vendor", json::array()},
        {"filament_colour", json::array()},
        {"filament_multi_colour", json::array()},
        {"filament_flow_ratio", json::array()},
        {"filament_max_volumetric_speed", json::array()},
        {"nozzle_temperature", json::array()},
        {"nozzle_temperature_initial_layer", json::array()},
        {"flush_volumes_vector", json::array()},
        {"flush_volumes_matrix", json::array()},
        {"filament_self_index", json::array()},
        {"filament_extruder_variant", json::array()},
        {"different_settings_to_system", json::array({"synthetic-process"})},
    };
    for (std::size_t slot = 0; slot < count; ++slot) {
        const bool petg = nozzle == "nozzle:0.2mm" &&
                          machine == "bambu-lab:p1s" && slot == 0;
        const std::string profile = machine == "bambu-lab:a1" &&
                                            nozzle == "nozzle:0.2mm" && slot == 0
                                        ? "Bambu PLA Basic @BBL A1"
                                        : "Synthetic " + std::string(petg ? "PETG" : "PLA") +
                                              " slot " + std::to_string(slot);
        project["filament_settings_id"].push_back(profile);
        project["filament_type"].push_back(petg ? "PETG" : "PLA");
        project["filament_vendor"].push_back("Synthetic Vendor");
        project["filament_colour"].push_back("#AABBCC");
        project["filament_multi_colour"].push_back("#AABBCC");
        for (std::size_t variant = 0; variant < group_width; ++variant) {
            const std::string flow = petg ? "0.95" : "0.98";
            project["filament_flow_ratio"].push_back(flow);
            project["filament_max_volumetric_speed"].push_back(petg ? "1" : "2");
            project["nozzle_temperature"].push_back(petg ? "250" : "220");
            project["nozzle_temperature_initial_layer"].push_back(petg ? "255" : "225");
            project["filament_self_index"].push_back(std::to_string(slot + 1));
            project["filament_extruder_variant"].push_back("0");
            project["flush_volumes_vector"].push_back("flush-" + std::to_string(slot * group_width + variant));
        }
        project["different_settings_to_system"].push_back("");
    }
    project["different_settings_to_system"].push_back("synthetic-machine-tail");
    const std::size_t matrix_cell_width = machine == "bambu-lab:p1s" ? 2 : 1;
    for (std::size_t pair = 0; pair < count * count; ++pair) {
        for (std::size_t variant = 0; variant < matrix_cell_width; ++variant) {
            project["flush_volumes_matrix"].push_back(
                "matrix-" + std::to_string(pair * matrix_cell_width + variant));
        }
    }
    return {
        {"slicer_id", slicer},
        {"application_version", version},
        {"machine_uid", machine},
        {"nozzle_uid", nozzle},
        {"base_project", std::move(project)},
    };
}

json sequence(std::size_t count, const std::string &prefix) {
    json values = json::array();
    for (std::size_t i = 0; i < count; ++i) {
        values.push_back(prefix + std::to_string(i));
    }
    return values;
}

json repeated(std::size_t count, const json &value) {
    json values = json::array();
    for (std::size_t i = 0; i < count; ++i) {
        values.push_back(value);
    }
    return values;
}

json selection(const json &item, std::size_t count, std::size_t default_source = 1) {
    return {
        {"slicer_id", item.at("slicer_id")},
        {"application_version", item.at("application_version")},
        {"machine_uid", item.at("machine_uid")},
        {"nozzle_uid", item.at("nozzle_uid")},
        {"build_plate_uid", "plate:textured-pei"},
        {"material_mode", "preserve_template"},
        {"filament_slot_mode", "compact"},
        {"default_filament_source_slot", default_source},
        {"filament_colour", repeated(count, "#AABBCC")},
        {"filament_multi_colour", repeated(count, "#001122")},
    };
}

json compose(const json &source, const json &request) {
    const std::string filename = request.at("slicer_id") == "BambuStudio"
        ? "bambu-studio-02.08.02.61.json" : "orca-slicer-2.4.2.json";
    const auto output = fatcat::compose_project_settings(
        source.dump(), request.dump(),
        read_json("compatibility/current-src/translations/canonical.json").dump(),
        read_json("compatibility/current-src/translations/targets/" + filename).dump());
    return json::parse(json::parse(output).at("project_settings_json").get<std::string>());
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
               std::string("wrong rejection: ") + error.what());
        return;
    }
    throw std::runtime_error("missing rejection for " + needle);
}

json source_for(const json &item) {
    json source = item.at("base_project");
    source["curr_bed_type"] = "Textured PEI Plate";
    return source;
}

void default_arrays_keep_current_full_count_but_repeat_default_when_reduced() {
    const json item = device("BambuStudio", "bambu-lab:a1-mini");
    json source = source_for(item);
    source["filament_settings_id"] = sequence(8, "profile-");
    source["nozzle_temperature"] = sequence(8, "temp-");
    source["filament_flow_ratio"] = sequence(8, "flow-");
    source["filament_custom_unknown"] = sequence(8, "unknown-");
    source["unknown_double"] = sequence(16, "unknown-");
    source["unknown_square"] = sequence(64, "unknown-");
    source["retraction_distances_when_cut"] = sequence(8, "tool-");
    for (const std::size_t count : {1, 2, 6, 8}) {
        const auto request = selection(item, count);
        const auto out = compose(source, request);
        expect(out.at("filament_settings_id") == (count == 8 ? source.at("filament_settings_id") : repeated(count, "profile-1")),
               "compact ordinary identities must use the explicit default, except already-M arrays");
        expect(out.at("nozzle_temperature") == (count == 8 ? source.at("nozzle_temperature") : repeated(count, "temp-1")),
               "ordinary temperatures must not take the first M entries");
        expect(out.at("filament_flow_ratio") == (count == 8 ? source.at("filament_flow_ratio") : repeated(count, "flow-1")),
               "single-variant flow follows ordinary default semantics");
        expect(out.at("filament_colour") == request.at("filament_colour") &&
                   out.at("filament_multi_colour") == request.at("filament_multi_colour"),
               "compact main and multi colours must remain independent requested values");
        for (const auto *key : {"filament_custom_unknown", "unknown_double", "unknown_square",
                                "retraction_distances_when_cut", "printer_settings_id", "print_settings_id"}) {
            expect(out.at(key) == source.at(key), std::string("compact inferred ownership for ") + key);
        }
        expect(out.at("different_settings_to_system").size() == count + 2,
               "compact difference count must follow M, not the source N");
        expect(out.at("default_filament_profile") == source.at("default_filament_profile"),
               "default source must not implicitly change the machine default profile");
    }
}

void hardware_groups_and_temperatures_have_different_source_selection() {
    for (const auto *slicer : {"BambuStudio", "OrcaSlicer"}) {
        for (const auto *machine : {"bambu-lab:a1-mini", "bambu-lab:a1", "bambu-lab:p1s"}) {
            const json item = device(slicer, machine);
            json source = source_for(item);
            source["filament_flow_ratio"] = sequence(16, "flow-");
            source["nozzle_temperature"] = sequence(16, "temp-");
            source["nozzle_temperature_initial_layer"] = sequence(16, "initial-");
            source["flush_volumes_vector"] = sequence(16, "flush-");
            const bool p1s = std::string(machine) == "bambu-lab:p1s";
            const bool first_flow = std::string(slicer) == "OrcaSlicer" && !p1s;
            for (const std::size_t count : {1, 2, 6, 8}) {
                const auto out = compose(source, selection(item, count));
                json flow = json::array(), temperature = json::array(), flush = json::array();
                for (std::size_t i = 0; i < count; ++i) {
                    flow.push_back("flow-" + std::to_string(2 * i));
                    flush.push_back("flush-" + std::to_string(2 * i));
                    temperature.push_back("temp-2");
                    if (!first_flow) {
                        flow.push_back("flow-" + std::to_string(2 * i + 1));
                        flush.push_back("flush-" + std::to_string(2 * i + 1));
                    }
                    if (p1s) {
                        temperature.push_back("temp-3");
                    }
                }
                expect(out.at("filament_flow_ratio") == flow, "known flow groups lost their slot or variant");
                expect(out.at("flush_volumes_vector") == flush, "known flush groups lost their slot or variant");
                expect(out.at("nozzle_temperature") == temperature,
                       "temperatures must repeat the default-source values with the declared hardware variants");
            }
        }
    }
}

void two_nozzle_matrix_planes_project_top_left_rows_and_columns() {
    const json item = device("BambuStudio", "bambu-lab:p1s");
    json source = source_for(item);
    source["filament_dev_ams_drying_temperature"] = sequence(32, "dry-");
    source["filament_dev_ams_drying_time"] = sequence(32, "time-");
    source["filament_dev_ams_drying_ams_limitations"] = sequence(16, "limit-");
    source["flush_volumes_matrix"] = sequence(128, "matrix-");
    const auto out = compose(source, selection(item, 2));
    expect(out.at("filament_dev_ams_drying_temperature") == sequence(8, "dry-") &&
               out.at("filament_dev_ams_drying_time") == sequence(8, "time-") &&
               out.at("filament_dev_ams_drying_ams_limitations") == sequence(4, "limit-"),
           "AMS drying K4/K2 groups must retain complete first-M groups");
    expect(out.at("flush_volumes_matrix") == json::array({
               "matrix-0", "matrix-1", "matrix-8", "matrix-9",
               "matrix-64", "matrix-65", "matrix-72", "matrix-73"}),
           "each nozzle plane must project its top-left row-major matrix independently");
}

void two_nozzle_matrix_planes_project_through_merge_entrypoint() {
    const json item = device("BambuStudio", "bambu-lab:p1s");
    json source = source_for(item);
    source["flush_volumes_matrix"] = sequence(128, "matrix-");
    const json request = {
        {"slicer_id", item.at("slicer_id")},
        {"application_version", item.at("application_version")},
        {"machine_uid", item.at("machine_uid")},
        {"nozzle_uid", item.at("nozzle_uid")},
        {"build_plate_uid", "plate:textured-pei"},
        {"material_mode", "preserve_template"},
        {"merge_sources", json::array({{
             {"source_id", "synthetic-source"},
             {"slots", json::array({
                  {{"source_slot_id", 10}, {"slot_name", "First"},
                   {"material_id", "material:first"}, {"preview_color", "#AA0000"}},
                  {{"source_slot_id", 20}, {"slot_name", "Second"},
                   {"material_id", "material:second"}, {"preview_color", "#0000AA"}},
              })},
         }})},
    };
    const auto out = compose(source, request);
    expect(out.at("flush_volumes_matrix") == json::array({
               "matrix-0", "matrix-1", "matrix-8", "matrix-9",
               "matrix-64", "matrix-65", "matrix-72", "matrix-73"}),
           "merged source composition must preserve row-major nozzle planes");
}

void material_sources_copy_whole_groups_and_preserve_unselected_tail() {
    for (const auto *slicer : {"BambuStudio", "OrcaSlicer"}) {
        const json item = device(slicer, "bambu-lab:p1s");
        json source = source_for(item);
        source["filament_settings_id"] = sequence(8, "profile-");
        source["filament_flow_ratio"] = sequence(16, "flow-");
        source["nozzle_temperature"] = sequence(16, "temp-");
        source["flush_volumes_vector"] = sequence(16, "flush-");
        source["default_filament_profile"] = json::array({"existing-default", "keep-extra"});
        json request = selection(item, 2);
        request["filament_source_slots"] = json::array({7, nullptr});
        auto out = compose(source, request);
        expect(out.at("filament_settings_id") == json::array({"profile-7", "profile-1"}) &&
                   out.at("filament_flow_ratio") == json::array({"flow-14", "flow-15", "flow-2", "flow-3"}) &&
                   out.at("nozzle_temperature") == json::array({"temp-14", "temp-15", "temp-2", "temp-3"}),
               "material choice must use the whole selected source group");
        expect(out.at("flush_volumes_vector") == sequence(4, "flush-"),
               "material choice must not remap a flushing vector");
        expect(out.at("default_filament_profile") == json::array({"profile-7", "keep-extra"}),
               "the first explicit material source must supply its actual profile identity");
        request["filament_slot_mode"] = "preserve";
        request.erase("default_filament_source_slot");
        out = compose(source, request);
        json expected_ids = source.at("filament_settings_id");
        expected_ids[0] = "profile-7";
        json expected_flow = source.at("filament_flow_ratio");
        expected_flow[0] = "flow-14";
        expected_flow[1] = "flow-15";
        expect(out.at("filament_settings_id") == expected_ids && out.at("filament_flow_ratio") == expected_flow,
               "preserve material choice must retain every unselected source slot and variant");
        expect(out.at("different_settings_to_system").size() == 10, "preserve source choice dropped a tail");
    }
}

void mixed_nine_slot_source_preserves_or_explicitly_rebuilds_materials() {
    const json item = device("BambuStudio", "bambu-lab:p1s", "nozzle:0.2mm");
    const json source = source_for(item);
    expect(source.at("filament_type").at(0) == "PETG", "mixed fixture lost its actual PETG source");
    for (const std::size_t count : {1, 2, 6, 8, 9}) {
        json request = selection(item, count);
        const auto out = compose(source, request);
        expect(out.at("filament_type") == (count == 9 ? source.at("filament_type") : repeated(count, "PLA")),
               "compact must not guess the default from source slot zero");
        expect(out.at("filament_settings_id") == (count == 9 ? source.at("filament_settings_id") : repeated(count, source.at("filament_settings_id").at(1))),
               "compact must preserve exact material preset identities");
        request["material_mode"] = "target_native_preset";
        request["material_uid"] = "material:pla";
        expect(compose(source, request).at("filament_type") == repeated(count, "PLA"),
               "explicit native PLA selection must rebuild even a mixed PETG source");
    }
    json request = selection(item, 2);
    request["filament_source_slots"] = json::array({nullptr, 0});
    const auto out = compose(source, request);
    expect(out.at("filament_type") == json::array({"PLA", "PETG"}), "actual PETG source was not adopted");
    expect(out.at("default_filament_profile").at(0) == source.at("filament_settings_id").at(0),
           "default profile must use the actual PETG source, without profile-name fabrication");
}

void difference_rows_follow_each_field_and_keep_the_actual_machine_tail() {
    const json item = device("BambuStudio", "bambu-lab:p1s");
    json source = source_for(item);
    source["filament_flow_ratio"] = sequence(16, "flow-");
    source["nozzle_temperature"] = sequence(16, "temp-");
    source["filament_density"] = sequence(8, "density-");
    source["different_settings_to_system"] = json::array({
        "process-existing", "nozzle_temperature;filament_flow_ratio;unknown0",
        "filament_density;unknown1", "removed2", "removed3", "removed4",
        "removed5", "removed6", "removed7", "machine-existing"});
    json request = selection(item, 2);
    request["layer_height"] = "0.16";
    request["prime_tower_width"] = "35";
    const auto out = compose(source, request);
    const auto &diff = out.at("different_settings_to_system");
    expect(diff.size() == 4 && diff.at(3) == "machine-existing", "removed material row masqueraded as machine tail");
    expect(diff.at(1) == "filament_density;filament_flow_ratio;filament_multi_colour;unknown0;unknown1" &&
               diff.at(2) == "filament_density;filament_multi_colour;unknown1",
           "material registration must follow actual field sources and identity differences: " +
               diff.at(1).dump() + ", " + diff.at(2).dump());
    const auto process = diff.at(0).get<std::string>();
    for (const auto *key : {"process-existing", "layer_height", "prime_tower_width", "filament_colour", "filament_flow_ratio", "flush_volumes_matrix"}) {
        expect(process.find(key) != std::string::npos, std::string("missing changed field registration: ") + key);
    }
    expect(process.find("machine-existing") == std::string::npos, "machine settings were frozen into the process entry");
}

void cut_retraction_is_an_explicit_policy_and_preserves_tool_count() {
    const json item = device("OrcaSlicer", "bambu-lab:p1s");
    json source = source_for(item);
    source["filament_long_retractions_when_cut"] = sequence(16, "cut-");
    source["filament_retraction_distances_when_cut"] = sequence(16, "distance-");
    source["long_retractions_when_cut"] = json::array({"1", "1"});
    source["retraction_distances_when_cut"] = json::array({"18", "19"});
    json request = selection(item, 2);
    const auto untouched = compose(source, request);
    expect(untouched.at("filament_long_retractions_when_cut") == sequence(4, "cut-") &&
               untouched.at("long_retractions_when_cut") == source.at("long_retractions_when_cut"),
           "cut policy must not be forced for other callers");
    request["disable_cut_retraction"] = true;
    const auto out = compose(source, request);
    expect(out.at("filament_long_retractions_when_cut") == json::array({"nil", "nil"}) &&
               out.at("filament_retraction_distances_when_cut") == json::array({"nil", "nil"}),
           "explicit cut policy must write M nil entries");
    expect(out.at("long_retractions_when_cut") == json::array({"0", "0"}) &&
               out.at("retraction_distances_when_cut") == source.at("retraction_distances_when_cut"),
           "cut policy must preserve tool count and other machine retraction settings");
    source.erase("filament_long_retractions_when_cut");
    expect(!compose(source, request).contains("filament_long_retractions_when_cut"),
           "cut policy must not invent missing override fields");
}

void expanded_material_archive_palette_uses_default_groups_without_inventing_matrix() {
    const json item = device("BambuStudio", "bambu-lab:p1s");
    json source = source_for(item);
    source["filament_settings_id"] = sequence(8, "profile-");
    source["filament_flow_ratio"] = sequence(16, "flow-");
    source["nozzle_temperature"] = sequence(16, "temp-");
    source["filament_dev_ams_drying_temperature"] = sequence(32, "dry-");
    source["flush_volumes_matrix"] = sequence(64, "matrix-");
    source["different_settings_to_system"] = json::array({
        "process-existing", "unused-zero", "filament_flow_ratio;source-default", "unused-two",
        "unused-three", "unused-four", "unused-five", "unused-six", "unused-seven", "machine-existing"});
    json request = selection(item, 10);
    request["filament_source_slots"] = repeated(10, nullptr);
    request["filament_source_slots"][9] = 0;
    const auto out = compose(source, request);
    json expected_flow = json::array(), expected_temp = json::array(), expected_dry = json::array();
    for (std::size_t slot = 0; slot < 10; ++slot) {
        const std::size_t from = slot == 9 ? 0 : 1;
        expected_flow.push_back("flow-" + std::to_string(2 * from));
        expected_flow.push_back("flow-" + std::to_string(2 * from + 1));
        expected_temp.push_back("temp-" + std::to_string(2 * from));
        expected_temp.push_back("temp-" + std::to_string(2 * from + 1));
        for (std::size_t variant = 0; variant < 4; ++variant) {
            expected_dry.push_back("dry-" + std::to_string(4 * from + variant));
        }
    }
    expect(out.at("filament_flow_ratio") == expected_flow && out.at("nozzle_temperature") == expected_temp &&
               out.at("filament_dev_ams_drying_temperature") == expected_dry,
           "M>N must repeat the actual default source groups and honor explicit source choices");
    expect(out.at("filament_settings_id").size() == 10 && out.at("filament_settings_id").at(8) == "profile-1" &&
               out.at("filament_settings_id").at(9) == "profile-0", "expanded identities have wrong sources");
    expect(out.at("flush_volumes_matrix") == source.at("flush_volumes_matrix"),
           "M>N must keep the legacy source matrix rather than invent values");
    const auto &diff = out.at("different_settings_to_system");
    expect(diff.size() == 12 && diff.at(11) == "default_filament_profile;machine-existing",
           "expanded layout must retain the actual machine entry and register its requested default-profile change");
    expect(diff.at(9).get<std::string>().find("filament_flow_ratio") != std::string::npos &&
               diff.at(9).get<std::string>().find("source-default") != std::string::npos,
           "new material slots must carry their actual default-source registrations");
}

void preserve_material_selection_retains_unselected_difference_entries_exactly() {
    const json item = device("OrcaSlicer", "bambu-lab:p1s");
    json source = source_for(item);
    source["different_settings_to_system"] = json::array({
        "existing_process", "filament_density", "source-one", "filament_colour;untouched2",
        "filament_multi_colour;untouched3", "flush_volumes_vector;untouched4",
        "filament_colour;nozzle_temperature;untouched5", "untouched6", "untouched7", "machine-tail"});
    json request = selection(item, 2);
    request["filament_slot_mode"] = "preserve";
    request.erase("default_filament_source_slot");
    request["filament_source_slots"] = json::array({1, nullptr});
    const auto out = compose(source, request);
    for (std::size_t index = 3; index < 9; ++index) {
        expect(out.at("different_settings_to_system").at(index) == source.at("different_settings_to_system").at(index),
               "preserve material selection changed difference entry " + std::to_string(index) + ": " +
                   out.at("different_settings_to_system").at(index).dump() + " != " +
                   source.at("different_settings_to_system").at(index).dump());
    }
    expect(out.at("different_settings_to_system").at(9) ==
               "default_filament_profile;machine-tail",
           "the machine tail must be retained when the selected default profile identity changes");
}

void expanded_m16_palette_does_not_reinterpret_two_source_variants_as_slots() {
    for (const auto *slicer : {"BambuStudio", "OrcaSlicer"}) {
        const json item = device(slicer, "bambu-lab:p1s");
        json source = source_for(item);
        source["nozzle_temperature"] = sequence(16, "temp-");
        source["nozzle_temperature_initial_layer"] = sequence(16, "initial-");
        source["filament_flow_ratio"] = sequence(16, "flow-");
        json request = selection(item, 16);
        request["filament_source_slots"] = repeated(16, nullptr);
        request["filament_source_slots"][15] = 7;
        const auto out = compose(source, request);
        for (const auto &[key, prefix] : std::vector<std::pair<std::string, std::string>>{
                 {"nozzle_temperature", "temp-"}, {"nozzle_temperature_initial_layer", "initial-"},
                 {"filament_flow_ratio", "flow-"}}) {
            json expected = json::array();
            for (std::size_t slot = 0; slot < 16; ++slot) {
                const auto from = slot == 15 ? 7 : 1;
                expected.push_back(prefix + std::to_string(from * 2));
                expected.push_back(prefix + std::to_string(from * 2 + 1));
            }
            expect(out.at(key) == expected,
                   "M=2N must repeat actual source groups instead of treating two variants as sixteen source slots: " + key);
        }
        expect(out.at("filament_settings_id").size() == 16 &&
                   out.at("different_settings_to_system").size() == 18,
               "M16 expansion must produce sixteen materials and the corresponding difference layout");
        expect(out.at("flush_volumes_matrix") == source.at("flush_volumes_matrix"),
               "M16 expansion must retain the original matrix");
    }
}

void expanded_m32_palette_does_not_reinterpret_four_source_variants_as_slots() {
    const json item = device("BambuStudio", "bambu-lab:a1-mini");
    json source = source_for(item);
    source["filament_dev_ams_drying_temperature"] = sequence(32, "dry-");
    source["filament_dev_ams_drying_time"] = sequence(32, "time-");
    const auto out = compose(source, selection(item, 32, 2));
    for (const auto &[key, prefix] : std::vector<std::pair<std::string, std::string>>{
             {"filament_dev_ams_drying_temperature", "dry-"}, {"filament_dev_ams_drying_time", "time-"}}) {
        json expected = json::array();
        for (std::size_t slot = 0; slot < 32; ++slot) {
            for (std::size_t variant = 0; variant < 4; ++variant) {
                expected.push_back(prefix + std::to_string(8 + variant));
            }
        }
        expect(out.at(key) == expected,
               "M=4N must repeat the selected source's four drying values for every output slot: " + key);
    }
    expect(out.at("filament_settings_id").size() == 32 &&
               out.at("different_settings_to_system").size() == 34,
           "M32 expansion must preserve material and difference counts");
    expect(out.at("flush_volumes_matrix") == source.at("flush_volumes_matrix"),
           "M32 expansion must retain the original matrix");
}

void unchanged_compact_colours_keep_their_original_material_registrations() {
    const json item = device("BambuStudio", "bambu-lab:a1-mini");
    json source = source_for(item);
    source["different_settings_to_system"] = json::array({
        "existing_process", "filament_colour;filament_multi_colour;unknown0", "unknown1",
        "unknown2", "unknown3", "unknown4", "unknown5", "unknown6", "unknown7", "actual_machine_tail"});
    json request = selection(item, 8);
    request["filament_colour"] = source.at("filament_colour");
    request["filament_multi_colour"] = source.at("filament_multi_colour");
    for (const bool request_multi : {true, false}) {
        if (!request_multi) {
            request.erase("filament_multi_colour");
        }
        const auto out = compose(source, request);
        expect(out.at("filament_colour") == source.at("filament_colour") &&
                   out.at("filament_multi_colour") == source.at("filament_multi_colour"),
               "an unchanged compact colour request must retain its values");
        for (std::size_t entry = 1; entry < 10; ++entry) {
            expect(out.at("different_settings_to_system").at(entry) == source.at("different_settings_to_system").at(entry),
                   "unchanged colours must retain source material overrides and the actual tail");
        }
    }
}

void native_material_compaction_still_obeys_the_explicit_final_cut_policy() {
    for (const auto *slicer : {"BambuStudio", "OrcaSlicer"}) {
        const json item = device(slicer, "bambu-lab:p1s");
        json source = source_for(item);
        expect(source.at("filament_type") == repeated(8, "PLA"), "native compact fixture must contain actual PLA sources");
        source["long_retractions_when_cut"] = json::array({"1", "1"});
        json request = selection(item, 2);
        request["material_mode"] = "target_native_preset";
        request["material_uid"] = "material:pla";
        request["disable_cut_retraction"] = true;
        const auto out = compose(source, request);
        expect(out.at("filament_long_retractions_when_cut") == json::array({"nil", "nil"}) &&
                   out.at("filament_retraction_distances_when_cut") == json::array({"nil", "nil"}),
               "native material application must not undo the caller's explicit compact cut policy");
        expect(out.at("long_retractions_when_cut") == json::array({"0", "0"}),
               "native cut policy must preserve the machine tool count while disabling cut retraction");
        const auto &diff = out.at("different_settings_to_system");
        expect(diff.size() == 4, "native compact difference entries must follow the final slot count");
        for (std::size_t slot = 1; slot <= 2; ++slot) {
            const auto entry = diff.at(slot).get<std::string>();
            expect(entry.find("filament_long_retractions_when_cut") != std::string::npos &&
                       entry.find("filament_retraction_distances_when_cut") != std::string::npos,
                   "native difference cleanup must retain the final explicit cut overrides");
        }
        expect(diff.at(3).get<std::string>().find("long_retractions_when_cut") != std::string::npos,
               "native cut policy must register the actual machine override");
    }
}

void routing_difference_tokens_follow_their_own_sources_not_material_overrides() {
    const json item = device("OrcaSlicer", "bambu-lab:p1s");
    json source = source_for(item);
    source["different_settings_to_system"] = json::array({
        "process-existing", "flush_volumes_vector;long_retractions_when_ec;unknown0",
        "filament_density;unknown1", "unused2", "unused3", "unused4", "unused5", "unused6",
        "nozzle_temperature;filament_flow_ratio;unknown7", "actual_machine_tail"});
    source["long_retractions_when_ec"] = sequence(16, "retraction-");
    json request = selection(item, 2);
    request["filament_source_slots"] = json::array({7, nullptr});
    request["filament_colour"] = repeated(2, "#123456");
    const auto out = compose(source, request);
    expect(out.at("different_settings_to_system").at(1) ==
               "filament_colour;filament_flow_ratio;filament_multi_colour;flush_volumes_vector;long_retractions_when_ec;nozzle_temperature;unknown0;unknown7",
           "routing fields must retain their actual source registrations even though material source overrides do not apply to them: " +
               out.at("different_settings_to_system").at(1).dump());
    expect(out.at("flush_volumes_vector") == json::array({source.at("flush_volumes_vector").at(0), source.at("flush_volumes_vector").at(1),
                                                           source.at("flush_volumes_vector").at(2), source.at("flush_volumes_vector").at(3)}),
           "routing source tracking must not change its actual values");
    expect(out.at("long_retractions_when_ec") == sequence(4, "retraction-"),
           "routing arrays must keep their slot sources when a material source is selected");
}

void explicit_default_material_identity_follows_the_final_material_mode() {
    for (const auto *slicer : {"BambuStudio", "OrcaSlicer"}) {
        const json item = device(slicer, "bambu-lab:a1", "nozzle:0.2mm");
        const json source = source_for(item);
        expect(source.at("filament_settings_id").at(0) == "Bambu PLA Basic @BBL A1",
               "the regression requires the actual source preset without a nozzle suffix");
        json request = selection(item, 2, 0);
        request["filament_source_slots"] = json::array({0, nullptr});
        const auto preserved = compose(source, request);
        expect(preserved.at("default_filament_profile").at(0) == "Bambu PLA Basic @BBL A1",
               "preserve mode must use the actual explicitly selected source identity");
        request["material_mode"] = "target_native_preset";
        request["material_uid"] = "material:pla";
        const auto native = compose(source, request);
        const auto expected = "Bambu PLA Basic @BBL A1 0.2 nozzle";
        expect(native.at("filament_settings_id") == json::array({expected, expected}),
               "native material identity must use the exact nozzle-specific preset");
        expect(native.at("default_filament_profile").at(0) == expected,
               "an explicit default material selection must follow the final native identity, not the prior source name");
    }
}

void real_mixed_source_groups_register_overrides_against_the_output_identity() {
    const json item = device("BambuStudio", "bambu-lab:p1s", "nozzle:0.2mm");
    const json source = source_for(item);
    expect(source.at("filament_flow_ratio").at(0) == "0.95" &&
               source.at("filament_flow_ratio").at(2) == "0.98" &&
               source.at("filament_max_volumetric_speed").at(0) == "1" &&
               source.at("filament_max_volumetric_speed").at(2) == "2",
           "the real mixed fixture must retain its distinct PETG and PLA parameters");
    expect(source.at("different_settings_to_system").at(1) == "" &&
               source.at("different_settings_to_system").at(2) == "",
           "the real source starts without material override tokens");
    json request = selection(item, 2, 1);
    request["filament_source_slots"] = json::array({nullptr, 0});
    request["disable_cut_retraction"] = true;
    const auto out = compose(source, request);
    expect(out.at("filament_type") == json::array({"PLA", "PETG"}) &&
               out.at("filament_flow_ratio") == json::array({"0.95", "0.95", "0.95", "0.95"}) &&
               out.at("filament_max_volumetric_speed") == json::array({"1", "1", "1", "1"}),
           "mixed compaction must retain its intended cross-source values");
    expect(out.at("filament_self_index") == json::array({"1", "1", "2", "2"}),
           "mixed material selection must not redirect the output slot's variant routing");
    const auto pla = out.at("different_settings_to_system").at(1).get<std::string>();
    const auto petg = out.at("different_settings_to_system").at(2).get<std::string>();
    expect(pla.find("filament_flow_ratio") != std::string::npos &&
               pla.find("filament_max_volumetric_speed") != std::string::npos,
           "the PLA identity needs explicit overrides for values adopted from the PETG source group");
    expect(petg.find("filament_flow_ratio") == std::string::npos &&
               petg.find("filament_max_volumetric_speed") == std::string::npos,
           "the PETG identity must not gain overrides for its own unchanged group values");
    expect(pla.find("nozzle_temperature") == std::string::npos,
           "default-selected PLA temperatures must not gain an unrelated override");
}

void material_override_comparison_checks_complete_groups_and_actual_values() {
    const json item = device("BambuStudio", "bambu-lab:p1s");
    json source = source_for(item);
    source["different_settings_to_system"] = repeated(10, "");
    source["filament_flow_ratio"] = repeated(16, "0.95");
    source["filament_flow_ratio"][1] = "0.96";
    source["filament_flow_ratio"][3] = "0.97";
    source["filament_dev_ams_drying_temperature"] = repeated(32, "40");
    source["filament_dev_ams_drying_temperature"][3] = "43";
    source["filament_dev_ams_drying_temperature"][7] = "44";
    source["filament_max_volumetric_speed"] = repeated(16, "5");
    source["filament_density"] = sequence(8, "density-");
    source["filament_custom_unknown"] = sequence(16, "unknown-");
    json request = selection(item, 2, 1);
    auto out = compose(source, request);
    const auto first = out.at("different_settings_to_system").at(1).get<std::string>();
    expect(first.find("filament_flow_ratio") != std::string::npos &&
               first.find("filament_dev_ams_drying_temperature") != std::string::npos,
           "a difference only in the last K2/K4 variant must register the complete material field");
    expect(first.find("filament_max_volumetric_speed") == std::string::npos &&
               first.find("filament_density") == std::string::npos &&
               first.find("filament_custom_unknown") == std::string::npos,
           "equal groups, default-source values and unknown fields must not gain invented registrations");
    expect(out.at("filament_density") == json::array({"density-1", "density-1"}) &&
               out.at("filament_custom_unknown") == source.at("filament_custom_unknown"),
           "registration must not alter ordinary default selection or unknown arrays");
    request["filament_source_slots"] = json::array({0, nullptr});
    out = compose(source, request);
    const auto explicitly_selected = out.at("different_settings_to_system").at(1).get<std::string>();
    expect(explicitly_selected.find("filament_flow_ratio") == std::string::npos &&
               explicitly_selected.find("filament_dev_ams_drying_temperature") == std::string::npos,
           "a complete group copied from the selected identity must not gain a new override");

    const json orca = device("OrcaSlicer", "bambu-lab:a1-mini");
    source = source_for(orca);
    source["different_settings_to_system"] = repeated(10, "");
    source["filament_flow_ratio"] = repeated(16, "0.95");
    source["filament_flow_ratio"][1] = "0.96";
    source["filament_flow_ratio"][3] = "0.97";
    out = compose(source, selection(orca, 2, 1));
    expect(out.at("filament_flow_ratio") == json::array({"0.95", "0.95"}) &&
               out.at("different_settings_to_system").at(1).get<std::string>().find("filament_flow_ratio") == std::string::npos,
           "discarded hardware variants must not generate an override for identical effective values");
}

void filament_variant_routing_uses_output_slots_not_material_sources() {
    for (const auto *slicer : {"BambuStudio", "OrcaSlicer"}) {
        for (const auto *machine : {"bambu-lab:a1-mini", "bambu-lab:a1", "bambu-lab:p1s"}) {
            const json item = device(slicer, machine);
            const json source = source_for(item);
            const std::size_t width = std::string(machine) == "bambu-lab:p1s" ? 2 : 1;
            for (const std::size_t count : {2, 8, 10, 16, 32}) {
                json request = selection(item, count, 1);
                request["filament_source_slots"] = repeated(count, nullptr);
                request["filament_source_slots"][0] = 7;
                request["filament_source_slots"][count - 1] = 0;
                const auto out = compose(source, request);
                json expected = json::array();
                for (std::size_t slot = 0; slot < count; ++slot) {
                    for (std::size_t variant = 0; variant < width; ++variant) {
                        expected.push_back(std::to_string(slot + 1));
                    }
                }
                expect(out.at("filament_self_index") == expected,
                       "compact routing must name every output slot at the effective hardware width, independent of material sources");
                expect(out.at("filament_self_index").size() == out.at("filament_extruder_variant").size(),
                       "routing and extruder variant arrays must describe the same effective groups");
                if (count == 2) {
                    request["material_mode"] = "target_native_preset";
                    request["material_uid"] = "material:pla";
                    expect(compose(source, request).at("filament_self_index") == expected,
                           "native material initialization must retain the final output routing");
                    request["material_mode"] = "preserve_template";
                    request.erase("material_uid");
                    request["filament_slot_mode"] = "preserve";
                    expected = json::array();
                    for (std::size_t slot = 0; slot < 8; ++slot) {
                        for (std::size_t variant = 0; variant < width; ++variant) {
                            expected.push_back(std::to_string(slot + 1));
                        }
                    }
                    expect(compose(source, request).at("filament_self_index") == expected,
                           "preserve material selection must retain every source slot's own routing and unselected tail");
                }
            }
        }
    }
}

void compact_source_requests_validate_meaningful_boundaries() {
    const json item = device("BambuStudio", "bambu-lab:a1-mini");
    const json source = source_for(item);
    json request = selection(item, 2);
    request.erase("default_filament_source_slot");
    expect_error([&] { compose(source, request); }, "default_filament_source_slot");
    request = selection(item, 2, 8);
    expect_error([&] { compose(source, request); }, "default_filament_source_slot");
    request = selection(item, 0);
    expect_error([&] { compose(source, request); }, "filament_colour");
    request = selection(item, 2);
    request["filament_source_slots"] = json::array({0});
    expect_error([&] { compose(source, request); }, "filament_source_slots");
    request["filament_source_slots"] = json::array({nullptr, 8});
    expect_error([&] { compose(source, request); }, "filament_source_slots");
    request["filament_source_slots"] = json::array({-1, nullptr});
    expect_error([&] { compose(source, request); }, "filament_source_slots");
    request = selection(item, 2);
    request["filament_slot_mode"] = "trim";
    expect_error([&] { compose(source, request); }, "filament_slot_mode");
    request = selection(item, 2);
    request["filament_multi_colour"] = json::array({"#ABCDEF"});
    expect_error([&] { compose(source, request); }, "filament_multi_colour");
    request = selection(item, 2);
    request["disable_cut_retraction"] = "1";
    expect_error([&] { compose(source, request); }, "disable_cut_retraction");
}
}  // namespace

int main() {
    const std::vector<std::pair<std::string, void (*)()>> tests = {
        {"ordinary defaults / M=1,2,6,8", default_arrays_keep_current_full_count_but_repeat_default_when_reduced},
        {"hardware groups and default temperatures", hardware_groups_and_temperatures_have_different_source_selection},
        {"K4 drying and two-nozzle matrix planes", two_nozzle_matrix_planes_project_top_left_rows_and_columns},
        {"merged two-nozzle matrix planes", two_nozzle_matrix_planes_project_through_merge_entrypoint},
        {"material source groups and preserve tail", material_sources_copy_whole_groups_and_preserve_unselected_tail},
        {"mixed N=9 preserve/native separation", mixed_nine_slot_source_preserves_or_explicitly_rebuilds_materials},
        {"per-field difference provenance and machine tail", difference_rows_follow_each_field_and_keep_the_actual_machine_tail},
        {"optional cut policy and tool count", cut_retraction_is_an_explicit_policy_and_preserves_tool_count},
        {"M10/N8 material archive expansion", expanded_material_archive_palette_uses_default_groups_without_inventing_matrix},
        {"preserve untouched difference entries", preserve_material_selection_retains_unselected_difference_entries_exactly},
        {"M16 expansion retains source K2 groups", expanded_m16_palette_does_not_reinterpret_two_source_variants_as_slots},
        {"M32 expansion retains source K4 groups", expanded_m32_palette_does_not_reinterpret_four_source_variants_as_slots},
        {"unchanged compact colour registrations", unchanged_compact_colours_keep_their_original_material_registrations},
        {"native compact final cut policy", native_material_compaction_still_obeys_the_explicit_final_cut_policy},
        {"routing differences keep their actual sources", routing_difference_tokens_follow_their_own_sources_not_material_overrides},
        {"explicit default follows final material identity", explicit_default_material_identity_follows_the_final_material_mode},
        {"real mixed-source material override registration", real_mixed_source_groups_register_overrides_against_the_output_identity},
        {"complete-group material value comparison", material_override_comparison_checks_complete_groups_and_actual_values},
        {"output slot variant routing", filament_variant_routing_uses_output_slots_not_material_sources},
        {"request source validation", compact_source_requests_validate_meaningful_boundaries},
    };
    int failed = 0;
    for (const auto &[name, run] : tests) {
        try {
            run();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
