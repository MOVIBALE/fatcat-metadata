"""Native process and material selection must respect the actual printer."""

import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import fatcat_metadata as fatcat


def request(machine="creator-5-pro", nozzle="0.4"):
    return {
        "project_source": "fatcat_native", "slicer_id": "FlashStudio",
        "application_version": "1.7.19", "machine_uid": "flashforge:" + machine,
        "nozzle_uid": "nozzle:" + nozzle + "mm", "build_plate_uid": "plate:textured-pei",
        "source_materials": [{"name": "PLA", "material_type": "PLA", "colour": colour}
                             for colour in ("#FFFFFF", "#FF0000", "#FFFF00", "#0000FF")],
        "process_settings": {"layer_height": "0.08", "wall_loops": "1"},
    }


class NativePresetCompatibilityTests(unittest.TestCase):
    def test_anycubic_inherited_process_is_checked_by_the_same_rule(self):
        selected = {**request(), "slicer_id": "AnycubicSlicerNext", "application_version": "2.0.0.6",
                    "machine_uid": "anycubic:kobra-4", "nozzle_uid": "nozzle:0.8mm"}
        selected.pop("build_plate_uid")
        options = fatcat.list_project_options(selected)
        chosen = options["default_print_profile_name"]
        self.assertNotEqual(chosen, "0.40mm Standard @Anycubic Kobra X 0.8 nozzle")
        self.assertTrue(next(p for p in options["print_profiles"] if p["name"] == chosen)["available"])

    def test_multitool_process_comes_from_compatible_native_chain(self):
        for machine, nozzle, process in (
            ("creator-5-pro", "0.4", "0.20mm Standard @FF C5"),
            ("creator-5", "0.4", "0.20mm Standard @FF C5"),
            ("creator-5", "0.25", "0.12mm Standard @FF C5 0.25 nozzle"),
        ):
            with self.subTest(machine=machine, nozzle=nozzle):
                selected = request(machine, nozzle)
                options = fatcat.list_project_options(selected)
                self.assertEqual(options["default_print_profile_name"], process)
                project = fatcat.compose_project_settings(selected)["project_settings"]
                self.assertEqual(project["default_print_profile"], process)
                for key, value in {"ooze_prevention": "1", "standby_temperature_delta": "-100",
                                   "enable_tower_interface_features": "1", "wipe_tower_wall_type": "rib",
                                   "layer_height": "0.08", "wall_loops": "1"}.items():
                    self.assertEqual(project[key], value, key)

    def test_explicit_incompatible_process_is_not_silently_accepted_or_replaced(self):
        selected = request()
        wrong = "0.20mm Standard @Flashforge AD5M Pro 0.4 Nozzle"
        options = fatcat.list_project_options(selected)
        offered = next(p for p in options["print_profiles"] if p["name"] == wrong)
        self.assertFalse(offered["available"])
        selected["native_print_profile_name"] = wrong
        with self.assertRaisesRegex(ValueError, "not compatible"):
            fatcat.compose_project_settings(selected)

    def test_guider_defaults_also_reject_inherited_adventurer_process(self):
        for machine, nozzle, process in (
            ("guider4", "0.4", "0.20mm Standard @FF G4"),
            ("guider4", "0.25", "0.12mm Standard @FF G4 0.25 nozzle"),
            ("guider4-pro", "0.4", "0.20mm Standard @FF G4P"),
            ("guider-3-ultra", "0.4", "0.20mm Standard @Flashforge G3U 0.4 Nozzle"),
        ):
            with self.subTest(machine=machine, nozzle=nozzle):
                selected = request(machine, nozzle)
                selected.pop("build_plate_uid")
                options = fatcat.list_project_options(selected)
                self.assertEqual(options["default_print_profile_name"], process)


class NativeConditionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        installed = Path(fatcat.__file__).parent
        shutil.copytree(installed / "fatcat_metadata_data", root / "fatcat_metadata_data")
        patched = patch.object(fatcat, "__file__", str(root / Path(fatcat.__file__).name))
        patched.start()
        self.addCleanup(patched.stop)
        self.sources = root / "fatcat_metadata_data/native_project_sources"
        index = json.loads((self.sources / "source-index.json").read_text(encoding="utf-8"))
        self.source = next(row for row in index["sources"] if row["slicer_id"] == "FlashStudio"
                           and row["machine_uid"] == "flashforge:creator-5"
                           and row["nozzle_uid"] == "nozzle:0.25mm")
        self.selected = request("creator-5", "0.25")

    def change_profile(self, role, changes):
        option = next(p for p in self.source[role + "_profile_options"]
                      if "path" in p and "unavailable_reason" not in p)
        path = self.sources / option["path"]
        profile = json.loads(path.read_text(encoding="utf-8-sig"))
        profile.update(changes)
        path.write_text(json.dumps(profile), encoding="utf-8")
        return option["name"]

    def test_condition_applies_to_process_and_material_without_explicit_list(self):
        expression = 'not ((printer_model == "Flashforge Creator 5" or printer_model == "Flashforge Creator 5 Pro") and printer_variant == "0.25")'
        for role, output_key in (("print", "print_profiles"), ("filament", "filament_profiles")):
            with self.subTest(role=role):
                name = self.change_profile(role, {"compatible_printers": [],
                    "compatible_printers_condition": expression})
                options = fatcat.list_project_options(self.selected)
                choice = next(p for p in options[output_key] if p["name"] == name)
                self.assertFalse(choice["available"])

    def test_explicit_printer_list_takes_precedence_over_condition(self):
        name = self.change_profile("print", {
            "compatible_printers": [self.source["source_machine_profile_name"]],
            "compatible_printers_condition": "false",
        })
        choice = next(p for p in fatcat.list_project_options(self.selected)["print_profiles"]
                      if p["name"] == name)
        self.assertTrue(choice["available"])

    def test_invalid_condition_does_not_become_unrestricted(self):
        name = self.change_profile("print", {"compatible_printers": [],
            "compatible_printers_condition": 'missing_machine_field == "true"'})
        selected = self.selected | {"native_print_profile_name": name}
        with self.assertRaisesRegex(ValueError, "compatible_printers_condition"):
            fatcat.compose_project_settings(selected)

    def test_native_numeric_regex_and_extra_options(self):
        for condition, expected in (
            ('nozzle_diameter[0] == 0.25 and num_extruders == 4', True),
            ('nozzle_diameter[0] >= 0.4 or num_extruders != 4', False),
            ('printer_preset =~ /Flashforge Creator 5.*0.25 nozzle/', True),
            ('printer_preset !~ /Flashforge.*/', False),
        ):
            with self.subTest(condition=condition):
                name = self.change_profile("print", {"compatible_printers": [],
                    "compatible_printers_condition": condition})
                choice = next(p for p in fatcat.list_project_options(self.selected)["print_profiles"]
                              if p["name"] == name)
                self.assertEqual(choice["available"], expected)

    def test_native_boolean_values_compare_as_booleans(self):
        for role, key in (("print", "print_profiles"), ("filament", "filament_profiles")):
            for condition, expected in (
                ("single_extruder_multi_material == false", True),
                ("single_extruder_multi_material != false", False),
                ("use_relative_e_distances == true", True),
                ("use_relative_e_distances != true", False),
                ('single_extruder_multi_material != "false"', False),
                ('single_extruder_multi_material == "false"', True),
                ('use_relative_e_distances == "true"', True),
                ('"1" == true', False),
                ('printer_variant == 0.25', True),
                ('printer_variant == 0.2500000001', True),
                ('nozzle_diameter[0] == "0.250"', False),
                ('nozzle_diameter[0] == 0.2500000001', True),
            ):
                with self.subTest(role=role, condition=condition):
                    name = self.change_profile(role, {"compatible_printers": [],
                        "compatible_printers_condition": condition})
                    choice = next(p for p in fatcat.list_project_options(self.selected)[key]
                                  if p["name"] == name)
                    self.assertEqual(choice["available"], expected)

    def test_invalid_logical_operands_and_unresolved_enum_arrays_are_rejected(self):
        for role, key in (("print", "print_profiles"), ("filament", "filament_profiles")):
            for condition in ('true or "not a boolean"', 'true or nozzle_diameter[0]',
                              'false and "not a boolean"',
                              'nozzle_type[0] == "hardened_steel"'):
                with self.subTest(role=role, condition=condition):
                    name = self.change_profile(role, {"compatible_printers": [],
                        "compatible_printers_condition": condition})
                    choice = next(p for p in fatcat.list_project_options(self.selected)[key]
                                  if p["name"] == name)
                    self.assertFalse(choice["available"])
                    self.assertIn("compatible_printers_condition", choice["unavailable_reason"])

    def test_no_compatible_process_rejects_instead_of_using_inherited_settings(self):
        for option in self.source["print_profile_options"]:
            if "path" not in option:
                continue
            path = self.sources / option["path"]
            profile = json.loads(path.read_text(encoding="utf-8-sig"))
            profile["compatible_printers"] = ["Unrelated machine"]
            path.write_text(json.dumps(profile), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "no.*native default process"):
            fatcat.compose_project_settings(self.selected)

    def test_fallback_matches_numeric_layer_height_with_native_tolerance(self):
        preferred = self.source["default_print_profile_name"]
        candidates = sorted(p["name"] for p in self.source["print_profile_options"]
                            if "path" in p and p["name"] != preferred)
        earlier, later = candidates[0], candidates[-1]
        self.assertNotEqual(earlier, later)
        for option in self.source["print_profile_options"]:
            if "path" not in option:
                continue
            path = self.sources / option["path"]
            profile = json.loads(path.read_text(encoding="utf-8-sig"))
            profile.update({"alias": option["name"], "layer_height": "0.2000",
                            "compatible_printers": ["Unrelated machine"]})
            if option["name"] in (earlier, later):
                profile["compatible_printers"] = [self.source["source_machine_profile_name"]]
                profile["layer_height"] = "0.2006" if option["name"] == earlier else "0.2004"
            path.write_text(json.dumps(profile), encoding="utf-8")
        self.assertEqual(fatcat.list_project_options(self.selected)["default_print_profile_name"], later)


class AllNativeMachinesCompatibilityTests(unittest.TestCase):
    def test_every_machine_default_and_selected_material_is_compatible(self):
        from functools import lru_cache
        root = Path(fatcat.__file__).parent / "fatcat_metadata_data/native_project_sources"
        index = json.loads((root / "source-index.json").read_text(encoding="utf-8"))

        @lru_cache(None)
        def profile(path):
            document = json.loads((root / path).read_text(encoding="utf-8-sig"))
            result = {}
            for kind in ("inherits", "include"):
                names = document.get(kind, [])
                if isinstance(names, str):
                    names = [names]
                for name in names:
                    if name:
                        result.update(profile(index["references"][path][kind + ":" + name]))
            result.update(document)
            return result

        for source in index["sources"]:
            selection = {key: source[key] for key in ("slicer_id", "application_version", "machine_uid", "nozzle_uid")}
            with self.subTest(**selection):
                options = fatcat.list_project_options(selection)
                default = options["default_print_profile_name"]
                if not default:
                    self.assertIn("compatibility_project", source)
                    continue
                chosen = next(p for p in options["print_profiles"] if p["name"] == default)
                self.assertTrue(chosen["available"], chosen)
                path = next(p["path"] for p in source["print_profile_options"] if p["name"] == default)
                printers = profile(path).get("compatible_printers", [])
                if printers:
                    self.assertIn(source["source_machine_profile_name"], printers)
                exported_source = fatcat.native_project_source(selection)
                self.assertEqual(exported_source["default_print_profile_name"], default)
                # Exercise automatic material fallback, including invalid
                # inherited defaults, through the actual composition boundary.
                plan = fatcat.resolve_project_materials(selection | {
                    "project_source": "fatcat_native", "source_materials": [
                        {"name": "PLA", "material_type": "PLA", "colour": "#123456"}],
                })
                if not plan["ready"]:
                    self.assertTrue(plan["issues"])
                    continue
                for material in plan["materials"]:
                    name = material["selected_profile_name"]
                    choice = next(p for p in options["filament_profiles"] if p["name"] == name)
                    self.assertTrue(choice["available"], choice)
                    path = next(p["path"] for p in source["filament_profile_options"] if p["name"] == name)
                    printers = profile(path).get("compatible_printers", [])
                    if printers:
                        self.assertIn(source["source_machine_profile_name"], printers)

    def test_prusa_defaults_are_from_native_queried_configuration(self):
        root = Path(fatcat.__file__).parent / "fatcat_metadata_data/native_project_sources"
        catalog = json.loads((root / "prusa-3/catalog.json").read_text(encoding="utf-8"))
        for source in catalog["sources"]:
            selection = {"slicer_id": "PrusaSlicer", "application_version": "3.0.0-alpha13",
                         "machine_uid": source["machine_uid"], "nozzle_uid": source["nozzle_uid"]}
            with self.subTest(**selection):
                options = fatcat.list_project_options(selection)
                self.assertIn(source["default_print_profile_name"],
                              [p["name"] for p in options["print_profiles"] if p["available"]])
                for name in source["default_filament_profile_names"]:
                    self.assertIn(name, [p["name"] for p in options["filament_profiles"] if p["available"]])


if __name__ == "__main__":
    unittest.main()
