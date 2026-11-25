"""Native material defaults must cover every tool used by the slicer."""

import copy
import unittest

import fatcat_metadata as fatcat


def request(machine="creator-5", nozzle="0.4"):
    return {
        "project_source": "fatcat_native", "slicer_id": "FlashStudio",
        "application_version": "1.7.19", "machine_uid": "flashforge:" + machine,
        "nozzle_uid": "nozzle:" + nozzle + "mm", "build_plate_uid": "plate:textured-pei",
        "source_materials": [{"name": f"PLA {i}", "material_type": "PLA", "colour": colour}
                             for i, colour in enumerate(("#FFFFFF", "#FF0000", "#FFFF00", "#0000FF"))],
        "process_settings": {"layer_height": "0.08"},
    }


def template_request(slots, hardware_mode="preserve_source"):
    result = request()
    result.pop("project_source")
    result.update(hardware_mode=hardware_mode, material_mode="preserve_template",
                  preserve_source_material_settings=True, filament_slot_mode="compact",
                  default_filament_source_slot=slots[0],
                  filament_source_slots=slots,
                  source_materials=[result["source_materials"][i] for i in slots])
    result["filament_colour"] = [material["colour"] for material in result["source_materials"]]
    return result


class NativeMaterialArrayTests(unittest.TestCase):
    def test_native_multitool_defaults_cover_every_material(self):
        for machine, nozzle in (("creator-5", "0.4"), ("creator-5-pro", "0.4"),
                                ("creator-5", "0.25"), ("ad5x", "0.4")):
            with self.subTest(machine=machine, nozzle=nozzle):
                project = fatcat.compose_project_settings(request(machine, nozzle))["project_settings"]
                self.assertEqual(project["filament_change_length"], ["0"] * 4)

    def test_imported_missing_empty_and_singleton_arrays_are_completed_before_selection(self):
        project = fatcat.compose_project_settings(request())["project_settings"]
        for source_value, expected in ((None, ["0", "0"]), ([], ["0", "0"]),
                                       (["7"], ["7", "7"]),
                                       (["1", "2", "3", "4"], ["4", "1"])):
            for mode in ("preserve_source", "target_binding"):
                with self.subTest(value=source_value, mode=mode):
                    source = copy.deepcopy(project)
                    if mode == "target_binding":
                        # Native re-save expands process/material/printer markers.
                        source["different_settings_to_system"] = [
                            source["different_settings_to_system"][0], "", "", "", "", "",
                        ]
                    source.pop("filament_change_length", None)
                    if source_value is not None:
                        source["filament_change_length"] = source_value
                    original = copy.deepcopy(source)
                    result = fatcat.compose_project_settings(source, template_request([3, 0], mode))["project_settings"]
                    self.assertEqual(result["filament_change_length"], expected)
                    self.assertEqual(result["nozzle_temperature"], [project["nozzle_temperature"][i] for i in (3, 0)])
                    self.assertEqual(source, original)

    def test_merge_keeps_custom_lengths_and_completes_missing_source(self):
        selected = request()
        projects = []
        for i in range(4):
            single = {**selected, "source_materials": [selected["source_materials"][i]]}
            project = fatcat.compose_project_settings(single)["project_settings"]
            project.pop("filament_change_length", None)
            if i % 2:
                project["filament_change_length"] = [str(i + 1)]
            projects.append(project)
        selected.pop("project_source")
        selected.update(hardware_mode="preserve_source", material_mode="preserve_template",
                        merge_sources=[{
                            "source_id": str(i),
                            **({"project_settings": project} if i else {}),
                            "slots": [{"source_slot_id": i, "source_slot_index": 0,
                                       "slot_name": f"PLA {i}", "preview_color": selected["source_materials"][i]["colour"],
                                       "material_id": project["filament_settings_id"][0]}],
                        } for i, project in enumerate(projects)])
        result = fatcat.compose_project_settings(projects[0], selected)["project_settings"]
        self.assertEqual(result["filament_change_length"], ["0", "2", "0", "4"])

    def test_partial_slot_arrays_are_not_guessed(self):
        project = fatcat.compose_project_settings(request())["project_settings"]
        project["filament_change_length"] = ["2", "5", "8"]
        with self.assertRaisesRegex(ValueError, "filament_change_length.*unresolved slot shape"):
            fatcat.compose_project_settings(project, template_request([3, 0]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
