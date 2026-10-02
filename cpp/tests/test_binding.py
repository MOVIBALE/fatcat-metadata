"""Smoke tests for the pybind11 wheel.

Run this file from a clean virtual environment after installing the wheel:

    python -m unittest cpp/tests/test_binding.py
"""

import json
import importlib.metadata
import re
import sys
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import fatcat_metadata


class WipeTowerBindingTests(unittest.TestCase):
    dialect = {
        "enabled_key": "enable_prime_tower",
        "x_key": "wipe_tower_x",
        "y_key": "wipe_tower_y",
        "width_key": "prime_tower_width",
        "rotation_key": "wipe_tower_rotation_angle",
        "process_difference_key": "different_settings_to_system",
    }

    @classmethod
    def setUpClass(cls) -> None:
        print(
            "binding_file="
            + str(Path(fatcat_metadata.__file__).resolve())
            + " sys_prefix="
            + sys.prefix
            + " python="
            + sys.version.split()[0]
            + " distribution_version="
            + importlib.metadata.version("fatcat-metadata")
        )
        if not fatcat_metadata.__fatcat_cpp_extension__:
            raise AssertionError("the installed module is not the C++ extension")
        if importlib.metadata.version("fatcat-metadata") != "0.1.0":
            raise AssertionError("unexpected C++ wheel distribution version")
        if fatcat_metadata.__version__ != "0.1.0":
            raise AssertionError("unexpected C++ module version")
        if not fatcat_metadata.__fatcat_project_settings__:
            raise AssertionError("project-settings support is not enabled")
        module_root = Path(fatcat_metadata.__file__).resolve().parent
        if not (module_root / "fatcat_metadata_data/translations/canonical.json").is_file():
            raise AssertionError("canonical translation data was not packaged")
        if not (
            module_root
            / "fatcat_metadata_data/translations/targets/bambu-studio-02.08.02.61.json"
        ).is_file():
            raise AssertionError("Bambu target translation data was not packaged")
        if not (
            module_root
            / "fatcat_metadata_data/translations/targets/orca-slicer-2.4.2.json"
        ).is_file():
            raise AssertionError("Orca target translation data was not packaged")

    def test_patch_wipe_tower_round_trip(self) -> None:
        project = {
            "wipe_tower_x": ["10"],
            "wipe_tower_y": ["20"],
            "different_settings_to_system": ["layer_height"],
        }
        settings = {
            "enabled": True,
            "width_mm": 35,
            "positions": [{"plate_index": 0, "x_mm": 12.5, "y_mm": -2}],
        }
        result = json.loads(
            fatcat_metadata.patch_wipe_tower(
                json.dumps(project), json.dumps(settings), json.dumps(self.dialect)
            )
        )
        self.assertEqual(result["enable_prime_tower"], "1")
        self.assertEqual(result["prime_tower_width"], "35")
        self.assertEqual(result["wipe_tower_x"], ["12.5"])
        self.assertEqual(result["wipe_tower_y"], ["-2"])

    def test_invalid_project_is_value_error(self) -> None:
        with self.assertRaises(ValueError):
            fatcat_metadata.patch_wipe_tower(
                "[]", "{}", json.dumps(self.dialect)
            )

    def test_project_settings_compose_reads_packaged_data(self) -> None:
        project = {
            "printer_settings_id": "Bambu Lab A1 mini 0.4 nozzle",
            "printer_model": "Bambu Lab A1 mini",
            "printer_variant": "0.4",
            "nozzle_diameter": ["0.4"],
            "filament_settings_id": ["Template PLA"],
            "filament_type": ["PLA"],
            "filament_vendor": ["Template Vendor"],
            "filament_colour": ["#123456"],
            "filament_multi_colour": ["#123456"],
            "different_settings_to_system": ["", "", ""],
            "unknown_nested": {"preserve": True},
        }
        request = {
            "slicer_id": "BambuStudio",
            "application_version": "02.08.02.61",
            "machine_uid": "bambu-lab:a1-mini",
            "nozzle_uid": "nozzle:0.4mm",
            "build_plate_uid": "plate:textured-pei",
            "material_mode": "preserve_template",
            "layer_height": "0.12",
            "filament_colour": ["#abcdef"],
        }
        result = json.loads(
            fatcat_metadata.compose_project_settings(
                json.dumps(project), json.dumps(request)
            )
        )
        final_project = json.loads(result["project_settings_json"])
        self.assertEqual(final_project["layer_height"], "0.12")
        self.assertEqual(final_project["filament_colour"], ["#abcdef"])
        self.assertEqual(final_project["unknown_nested"], {"preserve": True})
        self.assertEqual(result["effective_settings"]["layer_height"], "0.12")

    def test_project_settings_tower_entry_reuses_a2_validation(self) -> None:
        project = {
            "printer_settings_id": "My tuned A1 mini",
            "printer_model": "Bambu Lab A1 mini",
            "printer_variant": "0.4",
            "nozzle_diameter": ["0.4"],
            "filament_settings_id": ["Template PLA"],
            "filament_type": ["PLA"],
            "filament_vendor": [""],
            "filament_compatible_printers": [""],
            "filament_colour": ["#123456"],
            "filament_multi_colour": ["#123456"],
            "wipe_tower_x": ["10"],
            "wipe_tower_y": ["20"],
            "different_settings_to_system": ["", "", ""],
        }
        request = {
            "slicer_id": "BambuStudio",
            "application_version": "02.08.02.61",
            "machine_uid": "bambu-lab:a1-mini",
            "nozzle_uid": "nozzle:0.4mm",
            "build_plate_uid": "plate:textured-pei",
            "material_mode": "preserve_template",
            "wipe_tower_positions": [
                {"plate_index": 0, "x_mm": 12.5, "y_mm": -2.0}
            ],
        }
        result = json.loads(
            fatcat_metadata.compose_project_settings(
                json.dumps(project), json.dumps(request)
            )
        )
        final_project = json.loads(result["project_settings_json"])
        self.assertEqual(final_project["wipe_tower_x"], ["12.5"])
        self.assertEqual(final_project["wipe_tower_y"], ["-2"])
        self.assertEqual(
            result["effective_settings"]["wipe_tower_x"],
            final_project["wipe_tower_x"],
        )

        invalid_request = dict(request)
        invalid_request["prime_tower_width"] = "0"
        with self.assertRaisesRegex(ValueError, "prime_tower_width"):
            fatcat_metadata.compose_project_settings(
                json.dumps(project), json.dumps(invalid_request)
            )

    def test_user_template_import_preserves_settings_and_optional_plate_sidecar(self) -> None:
        project = {
            "printer_model": "Bambu Lab A1 mini",
            "printer_settings_id": "My tuned A1 mini",
            "printable_area": ["0x0", "180x0", "180x180", "0x180"],
            "printable_height": "180",
            "nozzle_diameter": ["0.4"],
            "filament_settings_id": ["User PLA"],
            "filament_type": ["PLA"],
            "filament_colour": ["#123456"],
            "curr_bed_type": "Textured PEI Plate",
            "outer_wall_speed": "47",
        }
        source_model_xml = (
            '<model><metadata name="Application" value="BambuStudio-02.08.02.61"/>'
            '<metadata name="BambuStudio:3mfVersion" value="1"/></model>'
        )
        slice_info_xml = (
            '<config><header><header_item key="X-BBL-Client-Version" '
            'value="02.08.02.61"/></header></config>'
        )
        model_settings_with_plate = (
            '<config><plate><metadata key="bed_type" '
            'value="Textured PEI Plate"/></plate></config>'
        )
        model_settings_without_plate = "<config><plate/></config>"
        request = {
            "slicer_id": "BambuStudio",
            "application_version": "02.08.02.61",
        }
        parts = json.loads(fatcat_metadata.template_import_parts(json.dumps(request)))
        self.assertEqual(parts["project_settings"], "Metadata/project_settings.config")
        self.assertEqual(parts["model_settings"], "Metadata/model_settings.config")
        self.assertEqual(parts["plate_sidecar"], "Metadata/plate_1.json")

        imported_without_sidecar = json.loads(
            fatcat_metadata.import_template_metadata(
                json.dumps(project), source_model_xml, slice_info_xml,
                model_settings_with_plate, None, json.dumps(request), False,
            )
        )
        self.assertEqual(imported_without_sidecar["source_slicer"], "BambuStudio")
        self.assertEqual(imported_without_sidecar["source_version"], "02.08.02.61")
        self.assertTrue(imported_without_sidecar["matches_selected_target_identity"])
        self.assertEqual(imported_without_sidecar["settings"], project)
        self.assertEqual(imported_without_sidecar["settings"]["outer_wall_speed"], "47")
        self.assertEqual(imported_without_sidecar["plate_value"], "Textured PEI Plate")
        self.assertIsNone(imported_without_sidecar["sidecar_bed"])

        imported_with_sidecar = json.loads(
            fatcat_metadata.import_template_metadata(
                json.dumps(project), source_model_xml, slice_info_xml,
                model_settings_without_plate, json.dumps({"bed_type": "cool_plate"}),
                json.dumps(request), False,
            )
        )
        self.assertIsNone(imported_with_sidecar["plate_value"])
        self.assertEqual(imported_with_sidecar["sidecar_bed"], "cool_plate")
        self.assertEqual(imported_with_sidecar["settings"], project)
        effective_plate = json.loads(
            fatcat_metadata.resolve_template_build_plate(
                json.dumps(request), "plate:textured-pei", None, "cool_plate",
                project["curr_bed_type"],
            )
        )
        self.assertEqual(effective_plate["build_plate_uid"], "plate:cool")
        self.assertEqual(effective_plate["project_value"], "Cool Plate")

    def test_optional_placement_warning_sidecar(self) -> None:
        self.assertEqual(json.loads(fatcat_metadata.read_placement_warnings(None)), [])
        self.assertEqual(
            json.loads(fatcat_metadata.read_placement_warnings(json.dumps({
                "requires_manual_adjustment": False,
                "warning_code": "manual_adjustment",
            }))),
            [],
        )
        self.assertEqual(
            json.loads(fatcat_metadata.read_placement_warnings(json.dumps({
                "requires_manual_adjustment": True,
                "warning_code": "manual_adjustment",
            }))),
            ["manual_adjustment"],
        )

    def test_model_metadata_generates_complete_target_package_description(self) -> None:
        project = {
            "curr_bed_type": "Textured PEI Plate",
            "filament_settings_id": ["Slot A", "Slot B", "Slot C"],
            "filament_type": ["PLA", "PLA", "PLA"],
        }
        request = {
            "slicer_id": "BambuStudio",
            "application_version": "02.08.02.61",
            "assembly_id": 7,
            "instance_id": "9",
            "identify_id": "1",
            "source_file": "模型 & <source>.stl",
            "parts": [
                {
                    "part_id": 42,
                    "name": "部件 <&>\nA",
                    "material_index": 2,
                    "source_object_id": 101,
                    "source_volume_id": 3,
                    "matrix": "1 0 0 1 0 1 0 2 0 0 1 3 0 0 0 1",
                    "source_offset_x": "0",
                    "source_offset_y": "1",
                    "source_offset_z": "2",
                }
            ],
            "plate": {
                "plater_id": "1",
                "plater_name": "",
                "locked": False,
                "bed_type": "Textured PEI Plate",
                "filament_map_mode": "Auto For Flush",
            },
            "component_inputs": {
                "wipe_tower_placement": {
                    "schema_version": 1,
                    "enabled": False,
                    "available": False,
                    "adapted": False,
                    "requires_manual_adjustment": False,
                    "warning_code": None,
                    "reason": "disabled",
                    "model_translation_mm": [0.0, 0.0],
                    "tower": None,
                }
            },
        }
        result = json.loads(
            fatcat_metadata.compose_model_metadata(
                json.dumps(project, ensure_ascii=False),
                json.dumps(request, ensure_ascii=False),
            )
        )
        roles = {part["role"] for part in result["parts"]}
        self.assertEqual(
            roles,
            {
                "model_settings",
                "project_settings",
                "slice_info",
                "wipe_tower_placement",
                "filament_sequence",
                "cut_information",
                "plate_main",
                "plate_no_light",
                "top_preview",
                "pick_preview",
                "thumbnail_package",
                "thumbnail_middle",
                "thumbnail_small",
            },
        )
        self.assertEqual(len(result["relationships"]), 9)
        self.assertEqual(len(result["content_types"]), 4)
        model_xml = next(
            part["content"]
            for part in result["parts"]
            if part["path"] == "Metadata/model_settings.config"
        )
        root = ET.fromstring(model_xml)
        part = root.find("./object/part")
        self.assertIsNotNone(part)
        self.assertEqual(part.attrib["id"], "42")
        metadata = {
            item.attrib["key"]: item.attrib["value"]
            for item in part.findall("metadata")
        }
        self.assertEqual(metadata["name"], "部件 <&>\nA")
        self.assertEqual(metadata["extruder"], "3")
        slice_xml = next(
            part["content"]
            for part in result["parts"]
            if part["path"] == "Metadata/slice_info.config"
        )
        slice_root = ET.fromstring(slice_xml)
        self.assertEqual(
            [item.attrib for item in slice_root.findall("./header/header_item")],
            [
                {"key": "X-BBL-Client-Type", "value": "slicer"},
                {"key": "X-BBL-Client-Version", "value": "02.08.02.61"},
            ],
        )

    @staticmethod
    def _single_metadata_fixture() -> tuple[dict, dict]:
        return (
            {
                "curr_bed_type": "Textured PEI Plate",
                "filament_settings_id": ["Slot A", "Slot B", "Slot C"],
                "filament_type": ["PLA", "PLA", "PLA"],
            },
            {
                "slicer_id": "BambuStudio",
                "application_version": "02.08.02.61",
                "assembly_id": 7,
                "instance_id": "9",
                "identify_id": "1",
                "source_file": "模型.stl",
                "parts": [
                    {
                        "part_id": 42,
                        "name": "部件",
                        "material_index": 2,
                        "source_object_id": 101,
                        "source_volume_id": 3,
                        "matrix": "1 0 0 1 0 1 0 2 0 0 1 3 0 0 0 1",
                        "source_offset_x": "0",
                        "source_offset_y": "1",
                        "source_offset_z": "2",
                    }
                ],
                "plate": {
                    "plater_id": "1",
                    "plater_name": "",
                    "locked": False,
                    "bed_type": "Textured PEI Plate",
                    "filament_map_mode": "Auto For Flush",
                },
                "component_inputs": {
                    "wipe_tower_placement": {
                        "schema_version": 1,
                        "enabled": False,
                        "available": False,
                        "adapted": False,
                        "requires_manual_adjustment": False,
                        "warning_code": None,
                        "reason": "disabled",
                        "model_translation_mm": [0.0, 0.0],
                        "tower": None,
                    }
                },
            },
        )

    def test_model_metadata_rejects_invalid_numeric_descriptors(self) -> None:
        project, request = self._single_metadata_fixture()
        for matrix in (
            "1 0 0",
            "1 0 0 1 0 1 0 2 0 0 1 3 0 0 0 1 0",
            "1 0 0 1 0 1 0 2 0 0 1 3 0 0 NaN 1",
            "1 0 0 1 0 1 0 2 0 0 1 3 0 0 0 Infinity",
            "1 0 0 1 0 1 0 2 0 0 1 3 0 0 0 1mm",
        ):
            invalid_request = json.loads(json.dumps(request))
            invalid_request["parts"][0]["matrix"] = matrix
            with self.assertRaisesRegex(ValueError, "matrix"):
                fatcat_metadata.compose_model_metadata(
                    json.dumps(project, ensure_ascii=False),
                    json.dumps(invalid_request, ensure_ascii=False),
                )
        invalid_request = json.loads(json.dumps(request))
        invalid_request["parts"][0]["source_offset_x"] = "1e400"
        with self.assertRaisesRegex(ValueError, "source_offset_x"):
            fatcat_metadata.compose_model_metadata(
                json.dumps(project, ensure_ascii=False),
                json.dumps(invalid_request, ensure_ascii=False),
            )

    def test_model_metadata_rejects_invalid_xml10_text(self) -> None:
        project, request = self._single_metadata_fixture()
        for value, field, error_field in (
            ("bad\x01name", "name", "request.parts[0].name"),
            ("bad\x1fplate", "plater_name", "request.plate.plater_name"),
            ("bad\x0csource.stl", "source_file", "request.source_file"),
        ):
            invalid_request = json.loads(json.dumps(request))
            if field == "name":
                invalid_request["parts"][0]["name"] = value
            elif field == "plater_name":
                invalid_request["plate"]["plater_name"] = value
            else:
                invalid_request["source_file"] = value
            with self.assertRaisesRegex(ValueError, re.escape(error_field)):
                fatcat_metadata.compose_model_metadata(
                    json.dumps(project, ensure_ascii=False),
                    json.dumps(invalid_request, ensure_ascii=False),
                )
        invalid_project = json.loads(json.dumps(project))
        invalid_project["curr_bed_type"] = "bad\x01bed"
        invalid_request = json.loads(json.dumps(request))
        invalid_request["plate"]["bed_type"] = ""
        with self.assertRaisesRegex(ValueError, re.escape("project.curr_bed_type")):
            fatcat_metadata.compose_model_metadata(
                json.dumps(invalid_project, ensure_ascii=False),
                json.dumps(invalid_request, ensure_ascii=False),
            )

    def test_model_metadata_accepts_dot_filename_but_rejects_parent_path(self) -> None:
        project, request = self._single_metadata_fixture()
        valid_request = json.loads(json.dumps(request))
        valid_request["source_file"] = "模型..v2.stl"
        result = json.loads(
            fatcat_metadata.compose_model_metadata(
                json.dumps(project, ensure_ascii=False),
                json.dumps(valid_request, ensure_ascii=False),
            )
        )
        model_xml = next(
            part["content"]
            for part in result["parts"]
            if part["path"] == "Metadata/model_settings.config"
        )
        root = ET.fromstring(model_xml)
        values = {item.attrib["key"]: item.attrib["value"]
                  for item in root.findall("./object/part/metadata")}
        self.assertEqual(values["source_file"], "模型..v2.stl")
        self.assertEqual(
            root.find("./plate/metadata[@key='top_file']").attrib["value"],
            "Metadata/top_1.png",
        )
        for value in ("../top.png", "/tmp/model.stl"):
            invalid_request = json.loads(json.dumps(request))
            invalid_request["source_file"] = value
            with self.assertRaisesRegex(ValueError, "source_file"):
                fatcat_metadata.compose_model_metadata(
                    json.dumps(project, ensure_ascii=False),
                    json.dumps(invalid_request, ensure_ascii=False),
                )


if __name__ == "__main__":
    unittest.main()
