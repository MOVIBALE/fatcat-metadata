"""H3C's tool changer and material variants use separate native axes."""

import json
import math
import unittest

import fatcat_metadata as fatcat


class H3CNativeSourceTests(unittest.TestCase):
    def test_layout_uses_nozzle_change_volumes_only_with_hotend_tool_changes(self):
        for machine, uses_hotend in (("h3c", True), ("p1s", False)):
            with self.subTest(machine=machine):
                request = self.request()
                request["machine_uid"] = f"bambu-lab:{machine}"
                request["source_materials"] = request["source_materials"][:2]
                project = fatcat.compose_project_settings(request)["project_settings"]
                self.assertEqual(project.get("handle_hotend_as_extruder", "0"),
                                 "1" if uses_hotend else "0")
                project.update(filament_prime_volume=["30", "48"],
                               filament_prime_volume_nc=["60", "75"],
                               filament_change_length=["2", "4"],
                               filament_change_length_nc=["5", "10"])
                layout = json.loads(fatcat.read_project_layout(json.dumps(project), json.dumps(request)))
                self.assertEqual(layout["purge_volume"], 75 if uses_hotend else 48)
                area = math.pi * 1.75**2 / 4
                self.assertAlmostEqual(layout["filament_change_volume"], 4 * area)
                self.assertAlmostEqual(layout["nozzle_change_volume"], 10 * area if uses_hotend else 0)

    def mixed_sources(self):
        projects, palette = [], []
        for kind in ("PLA Basic", "TPU for AMS"):
            request = self.request()
            request["source_materials"] = [dict(request["source_materials"][0], material_type=kind)]
            request["native_filament_profile_names"] = [f"Bambu {kind} @BBL H3C"]
            palette.extend(request["source_materials"])
            projects.append(fatcat.compose_project_settings(request)["project_settings"])
        return projects, palette

    def merge(self, projects, palette, mode="preserve_source"):
        request = self.request()
        request.pop("project_source")
        request.update(hardware_mode=mode, source_materials=palette,
                       material_mode="preserve_template",
                       preserve_source_material_settings=True, merge_sources=[])
        for index, project in enumerate(projects):
            request["merge_sources"].append({
                "source_id": str(index),
                "slots": [{"source_slot_id": index, "source_slot_index": 0,
                           "slot_name": palette[index]["name"],
                           "preview_color": palette[index]["colour"],
                           "material_id": project["filament_settings_id"][0]}]})
            if index:
                request["merge_sources"][-1]["project_settings"] = project
        return fatcat.compose_project_settings(projects[0], request)["project_settings"]

    def test_native_and_custom_groups_merge_in_either_order(self):
        projects, palette = self.mixed_sources()
        projects[1]["filament_settings_id"] = ["My edited TPU"]
        for mode in ("preserve_source", "target_binding"):
            for reverse in (False, True):
                with self.subTest(mode=mode, reverse=reverse):
                    sources = projects[::-1] if reverse else projects
                    colours = palette[::-1] if reverse else palette
                    merged = self.merge(sources, colours, mode)
                    for key in ("filament_dev_ams_drying_ams_limitations", "nozzle_temperature"):
                        self.assertEqual(merged[key], sources[0][key] + sources[1][key])

    def test_nonprefix_material_field_reorders_with_custom_presets(self):
        projects, palette = self.mixed_sources()
        project = self.merge(projects, palette)
        project["filament_settings_id"] = ["My PLA", "My TPU"]
        project["fins_extrude_safe_temp"] = ["177", "188"]
        # A user-saved project normalizes this list to one value per slot.
        project["filament_dev_ams_drying_ams_limitations"] = ["1", "2"]
        request = self.request()
        request.pop("project_source")
        request.update(hardware_mode="preserve_source", source_materials=palette[::-1],
                       preserve_source_material_settings=True, filament_source_slots=[1, 0])
        reordered = fatcat.compose_project_settings(project, request)["project_settings"]
        self.assertEqual(reordered["fins_extrude_safe_temp"], ["188", "177"])

    def test_unknown_uneven_groups_are_rejected_instead_of_left_unmapped(self):
        projects, palette = self.mixed_sources()
        project = self.merge(projects, palette)
        project["filament_settings_id"] = ["My PLA", "My TPU"]
        request = self.request()
        request.pop("project_source")
        request.update(hardware_mode="preserve_source", source_materials=palette[::-1],
                       preserve_source_material_settings=True, filament_source_slots=[1, 0])
        with self.assertRaisesRegex(ValueError, "filament_dev_ams_drying_ams_limitations.*shape"):
            fatcat.compose_project_settings(project, request)

    def test_native_groups_support_compaction_duplicates_and_default_slots(self):
        projects, palette = self.mixed_sources()
        project = self.merge(projects, palette)
        for slots in ([1], [1, 1, 0], [None, 1]):
            with self.subTest(slots=slots):
                request = self.request()
                request.pop("project_source")
                indices = [0 if index is None else index for index in slots]
                request.update(hardware_mode="preserve_source", filament_slot_mode="compact",
                               source_materials=[palette[index] for index in indices],
                               preserve_source_material_settings=True, filament_source_slots=slots)
                result = fatcat.compose_project_settings(project, request)["project_settings"]
                for key in ("filament_dev_ams_drying_ams_limitations", "nozzle_temperature"):
                    expected = [value for index in indices for value in projects[index][key]]
                    self.assertEqual(result[key], expected)
                self.assertFalse({"native_group_sizes", "native_profile_sizes"} & result.keys())

    def test_known_presets_with_edited_uniform_vectors_keep_edited_values(self):
        projects, palette = self.mixed_sources()
        project = self.merge(projects, palette)
        project["filament_dev_ams_drying_ams_limitations"] = ["edited PLA", "edited TPU"]
        request = self.request()
        request.pop("project_source")
        request.update(hardware_mode="preserve_source", source_materials=palette[::-1],
                       preserve_source_material_settings=True, filament_source_slots=[1, 0])
        result = fatcat.compose_project_settings(project, request)["project_settings"]
        self.assertEqual(result["filament_dev_ams_drying_ams_limitations"], ["edited TPU", "edited PLA"])

    def test_absent_palette_slot_uses_native_defaults_without_replacing_source(self):
        projects, palette = self.mixed_sources()
        for material, project in zip(palette, projects):
            material["native_filament_profile_name"] = project["filament_settings_id"][0]
        projects[0]["nozzle_temperature"] = ["221", "222"]
        for mode in ("preserve_source", "target_binding"):
            with self.subTest(mode=mode):
                result = self.merge(projects[:1], palette, mode)
                self.assertEqual(result["nozzle_temperature"], ["221", "222", "230", "230"])
                self.assertEqual(result["filament_dev_ams_drying_ams_limitations"],
                                 projects[0]["filament_dev_ams_drying_ams_limitations"] +
                                 projects[1]["filament_dev_ams_drying_ams_limitations"])

    def test_target_binding_projects_whole_groups_for_explicit_slot_selection(self):
        projects, palette = self.mixed_sources()
        project = self.merge(projects, palette)
        for slots in ([1, 0], [1], [1, 1, 0]):
            with self.subTest(slots=slots):
                request = self.request()
                request.pop("project_source")
                request.update(hardware_mode="target_binding", material_mode="preserve_template",
                               filament_slot_mode="compact", default_filament_source_slot=0,
                               filament_colour=[palette[index]["colour"] for index in slots],
                               filament_source_slots=slots)
                result = fatcat.compose_project_settings(project, request)["project_settings"]
                for key in ("filament_dev_ams_drying_ams_limitations", "nozzle_temperature"):
                    self.assertEqual(result[key], [value for index in slots for value in projects[index][key]])

    def test_same_logical_material_conflict_is_still_rejected(self):
        projects, palette = self.mixed_sources()
        source = dict(projects[0])
        source["nozzle_temperature"] = ["239", "240"]
        request = self.request()
        request.pop("project_source")
        request.update(hardware_mode="preserve_source", material_mode="preserve_template",
                       source_materials=palette[:1], merge_sources=[])
        for index, project in enumerate((projects[0], source)):
            item = {"source_id": str(index), "slots": [{
                "source_slot_id": 0, "source_slot_index": 0, "slot_name": palette[0]["name"],
                "preview_color": palette[0]["colour"], "material_id": project["filament_settings_id"][0]}]}
            if index:
                item["project_settings"] = project
            request["merge_sources"].append(item)
        with self.assertRaisesRegex(ValueError, "conflict for nozzle_temperature"):
            fatcat.compose_project_settings(projects[0], request)

    def request(self, diameter="0.4"):
        return {
            "project_source": "fatcat_native",
            "slicer_id": "BambuStudio",
            "application_version": "02.09.01.70",
            "machine_uid": "bambu-lab:h3c",
            "nozzle_uid": f"nozzle:{diameter}mm",
            "build_plate_uid": "plate:textured-pei",
            "source_materials": [
                {"name": f"Colour {slot + 1}", "material_type": "PLA Basic",
                 "colour": f"#{slot + 1:02X}2233"}
                for slot in range(9)
            ],
            "process_settings": {"wall_loops": "3", "outer_wall_speed": "47"},
        }

    def test_all_four_nozzles_keep_native_tool_changer_configuration(self):
        for diameter in ("0.2", "0.4", "0.6", "0.8"):
            with self.subTest(diameter=diameter):
                project = fatcat.compose_project_settings(
                    self.request(diameter))["project_settings"]
                self.assertEqual(project["printer_model"], "Bambu Lab H3C")
                self.assertEqual(project["printer_variant"], diameter)
                self.assertEqual(project["nozzle_diameter"], [diameter])
                self.assertEqual(project["extruder_max_nozzle_count"], ["9"])
                expected = (["Standard"] if diameter == "0.2" else
                            ["Standard"] * 7 + ["TPU285", "Standard"])
                self.assertEqual(project["default_nozzle_volume_type"], expected)
                self.assertEqual(project["wall_loops"], "3")
                process_variants = 2 if diameter in ("0.2", "0.8") else 3
                self.assertEqual(project["outer_wall_speed"],
                                 ["47"] * process_variants)

    def test_nine_materials_keep_parameter_groups_and_material_identity(self):
        request = self.request()
        project = fatcat.compose_project_settings(request)["project_settings"]
        self.assertEqual(project["filament_colour"],
                         [row["colour"] for row in request["source_materials"]])
        self.assertEqual(project["filament_ids"], ["GFA00"] * 9)
        self.assertEqual(len(project["nozzle_temperature"]), 18)
        self.assertEqual(len(project["filament_self_index"]), 18)
        self.assertEqual(len(project["filament_preheat_temperature_delta"]), 18)
        self.assertEqual(len(project["filament_dev_drying_softening_temperature"]), 36)
        self.assertEqual(len(project["filament_dev_ams_drying_temperature"]), 72)
        # Hardware remains one extruder, with nine changeable nozzles; it
        # must not grow when another material is added to the project.
        self.assertEqual(len(project["extruder_colour"]), 1)

    def test_mixed_materials_preserve_each_native_parameter_group(self):
        request = self.request()
        request["source_materials"] = request["source_materials"][:2]
        request["source_materials"][1]["material_type"] = "TPU for AMS"
        request["native_filament_profile_names"] = [
            "Bambu PLA Basic @BBL H3C", "Bambu TPU for AMS @BBL H3C"]
        project = fatcat.compose_project_settings(request)["project_settings"]
        self.assertEqual(project["filament_type"], ["PLA", "TPU-AMS"])
        self.assertEqual(project["nozzle_temperature"], ["220", "220", "230", "230"])
        self.assertEqual(project["filament_dev_ams_drying_ams_limitations"],
                         ["1", "0", "2", "3", "1", "2", "3"])
        self.assertEqual(project["filament_self_index"], ["1", "1", "2", "2"])
        # Reverse the two material slots without changing their own values.
        request.pop("project_source")
        request.pop("native_filament_profile_names")
        request.update(hardware_mode="preserve_source", preserve_source_material_settings=True,
                       filament_source_slots=[1, 0])
        reordered = fatcat.compose_project_settings(project, request)["project_settings"]
        self.assertEqual(reordered["filament_dev_ams_drying_ams_limitations"],
                         ["1", "2", "3", "1", "0", "2", "3"])
        self.assertEqual(reordered["nozzle_temperature"], ["230", "230", "220", "220"])

    def test_merge_preserves_unequal_native_material_groups(self):
        projects = []
        palette = []
        for kind in ("PLA Basic", "TPU for AMS"):
            request = self.request()
            request["source_materials"] = [dict(request["source_materials"][0], material_type=kind)]
            request["native_filament_profile_names"] = [f"Bambu {kind} @BBL H3C"]
            palette.extend(request["source_materials"])
            projects.append(fatcat.compose_project_settings(request)["project_settings"])
        request.pop("project_source")
        request.pop("native_filament_profile_names")
        request.update(hardware_mode="preserve_source", source_materials=palette,
                       material_mode="preserve_template",
                       preserve_source_material_settings=True, merge_sources=[])
        for index, project in enumerate(projects):
            source = {"source_id": str(index), "slots": [{
                "source_slot_id": index, "source_slot_index": 0,
                "slot_name": palette[index]["name"], "preview_color": palette[index]["colour"],
                "material_id": project["filament_settings_id"][0]}]}
            if index:
                source["project_settings"] = project
            request["merge_sources"].append(source)
        for hardware_mode in ("preserve_source", "target_binding"):
            with self.subTest(hardware_mode=hardware_mode):
                request["hardware_mode"] = hardware_mode
                merged = fatcat.compose_project_settings(projects[0], request)["project_settings"]
                self.assertEqual(merged["filament_dev_ams_drying_ams_limitations"],
                                 ["1", "0", "2", "3", "1", "2", "3"])
                self.assertEqual(merged["nozzle_temperature"], ["220", "220", "230", "230"])
                self.assertEqual(merged["filament_self_index"], ["1", "1", "2", "2"])
                self.assertEqual(merged["filament_idle_travel_cooling_enabled"], ["0", "1"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
