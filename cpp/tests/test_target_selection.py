"""Exercise both packaged targets through the installed C++ extension."""

import json
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import fatcat_metadata as fatcat
TARGETS = (
    ("BambuStudio", "02.08.02.61"),
    ("OrcaSlicer", "2.4.2"),
)


def material_profile(target, binding):
    profile_key = binding["material_profile_key"]
    profile = next(
        item for item in target["material_profiles"]
        if item["material_profile_key"] == profile_key
    )
    parameter_set = next(
        item for item in target["target_parameter_sets"]
        if item["target_parameter_set_key"]
        == profile["target_parameter_set_key"]
    )
    return {
        **profile,
        "managed_parameter_keys": parameter_set["managed_parameter_keys"],
        "target_parameters": parameter_set["target_parameters"],
    }


def project():
    return {
        "printer_settings_id": "Tuned A1 mini",
        "printer_model": "Bambu Lab A1 mini",
        "printer_variant": "0.4",
        "nozzle_diameter": ["0.4"],
        "curr_bed_type": "Textured PEI Plate",
        "filament_settings_id": ["Custom PLA", "Custom PLA 2", "Tail PLA", "Tail PETG"],
        "filament_type": ["PLA", "PLA", "PLA", "PETG"],
        "filament_vendor": ["Custom", "Custom", "Tail Vendor", "Tail Vendor"],
        "filament_colour": ["#123456", "#234567", "#345678", "#456789"],
        "nozzle_temperature": ["217", "213", "219", "242"],
        "filament_flow_ratio": ["0.93", "0.94", "0.95", "0.97"],
        "filament_retraction_length": ["0.6", "0.7", "0.8", "1.2"],
        "different_settings_to_system": ["", "", "", "", "", ""],
        "outer_wall_speed": "47",
        "normal_acceleration": "1234",
        "unknown_nested": {"preserve": True},
    }


def project_request(slicer, version):
    return {
        "slicer_id": slicer,
        "application_version": version,
        "machine_uid": "bambu-lab:a1-mini",
        "nozzle_uid": "nozzle:0.4mm",
        "build_plate_uid": "plate:textured-pei",
        "material_mode": "preserve_template",
        "layer_height": "0.12",
        "filament_colour": ["#ABCDEF", "#FEDCBA"],
    }


def model_request(slicer, version):
    return {
        "slicer_id": slicer,
        "application_version": version,
        "assembly_id": 7,
        "instance_id": "0",
        "identify_id": "1",
        "source_file": "example.3mf",
        "parts": [{
            "part_id": 11, "name": "Artwork", "material_index": 0,
            "source_object_id": 0, "source_volume_id": 0,
            "matrix": "1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1",
            "source_offset_x": "0", "source_offset_y": "0", "source_offset_z": "0",
        }, {
            "part_id": 42, "name": "Tail slot", "material_index": 2,
            "source_object_id": 0, "source_volume_id": 1,
            "matrix": "1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1",
            "source_offset_x": "0", "source_offset_y": "0", "source_offset_z": "0",
        }],
        "plate": {
            "plater_id": "1", "plater_name": "", "locked": False,
            "bed_type": "Textured PEI Plate", "filament_map_mode": "Auto For Flush",
            "resources": {
                "thumbnail_file": "Metadata/plate_1.png",
                "thumbnail_no_light_file": "Metadata/plate_no_light_1.png",
                "top_file": "Metadata/top_1.png", "pick_file": "Metadata/pick_1.png",
            },
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


class TargetSelectionTests(unittest.TestCase):
    def test_native_material_uses_each_packaged_target_without_clearing_unmanaged_tuning(self):
        for slicer, version in TARGETS:
            with self.subTest(target=slicer):
                original = project()
                original["filament_type"] = ["PLA"] * 4
                request = project_request(slicer, version)
                request["material_mode"] = "target_native_preset"
                request["material_uid"] = "material:pla"
                result = json.loads(fatcat.compose_project_settings(json.dumps(original), json.dumps(request)))
                final = json.loads(result["project_settings_json"])
                filename = "orca-slicer-2.4.2.json" if slicer == "OrcaSlicer" else "bambu-studio-02.08.02.61.json"
                target_path = Path(fatcat.__file__).parent / "fatcat_metadata_data/translations/targets" / filename
                target = json.loads(target_path.read_text(encoding="utf-8"))
                material = target["material_bindings"][0]
                profile = material_profile(target, material)
                self.assertEqual(final["filament_settings_id"], [material["native_profile_id"]] * 4)
                for key in ("nozzle_temperature", "filament_flow_ratio"):
                    self.assertEqual(final[key], profile["target_parameters"][key]["values"] * 4)
                self.assertEqual(final["outer_wall_speed"], "47")
                self.assertEqual(final["normal_acceleration"], "1234")
                self.assertEqual(final["unknown_nested"], {"preserve": True})

    def test_both_targets_preserve_material_parameters_and_unused_slots(self):
        original = project()
        for slicer, version in TARGETS:
            with self.subTest(target=slicer):
                result = json.loads(fatcat.compose_project_settings(
                    json.dumps(original), json.dumps(project_request(slicer, version)),
                ))
                final = json.loads(result["project_settings_json"])
                self.assertEqual(final["filament_colour"], ["#ABCDEF", "#FEDCBA", "#345678", "#456789"])
                self.assertEqual(final["layer_height"], "0.12")
                for key in (
                    "filament_settings_id", "filament_type", "filament_vendor",
                    "nozzle_temperature", "filament_flow_ratio",
                    "filament_retraction_length", "unknown_nested", "outer_wall_speed",
                    "normal_acceleration",
                ):
                    self.assertEqual(final[key], original[key], key)
                self.assertEqual(result["wipe_tower_dialect"]["x_key"], "wipe_tower_x")
                patched = json.loads(fatcat.patch_wipe_tower(
                    result["project_settings_json"],
                    json.dumps({"positions": [{"plate_index": 0, "x_mm": 20, "y_mm": 30}]}),
                    json.dumps(result["wipe_tower_dialect"]),
                ))
                self.assertEqual(patched["wipe_tower_x"], ["20"])

    def test_model_metadata_uses_the_same_explicit_target(self):
        # Revisit Bambu in the same process to catch leaked target state.
        for slicer, version in (*TARGETS, TARGETS[0]):
            with self.subTest(target=slicer):
                result = json.loads(fatcat.compose_model_metadata(
                    json.dumps(project()), json.dumps(model_request(slicer, version)),
                ))
                root = {item["name"]: item["value"] for item in result["root_model"]["metadata"]}
                parts = {
                    item["path"]: item["content"]
                    for item in result["parts"]
                    if "content" in item
                }
                headers = {
                    item.attrib["key"]: item.attrib["value"]
                    for item in ET.fromstring(parts["Metadata/slice_info.config"])
                    .findall("header/header_item")
                }
                if slicer == "OrcaSlicer":
                    self.assertEqual(root["Application"], "BambuStudio-02.06.00.51")
                    self.assertEqual(root["OrcaSlicer"], version)
                    self.assertEqual(headers["X-BBL-Client-Version"], "02.06.00.51")
                    self.assertEqual(headers["OrcaSlicer-Version"], version)
                else:
                    self.assertEqual(root["Application"], "BambuStudio-" + version)
                    self.assertNotIn("OrcaSlicer", root)
                    self.assertNotIn("OrcaSlicer-Version", headers)
                self.assertEqual(root["BambuStudio:3mfVersion"], "1")
                part = ET.fromstring(parts["Metadata/model_settings.config"]).find("object/part")
                self.assertEqual(part.attrib["id"], "11")
                self.assertEqual(part.find("metadata[@key='extruder']").attrib["value"], "1")
                tail_part = ET.fromstring(parts["Metadata/model_settings.config"]).find("object/part[@id='42']")
                self.assertEqual(tail_part.find("metadata[@key='extruder']").attrib["value"], "3")

    def test_missing_unknown_and_wrong_version_targets_do_not_choose_bambu(self):
        for composer, request in (
            (fatcat.compose_project_settings, project_request("BambuStudio", "02.08.02.61")),
            (fatcat.compose_model_metadata, model_request("BambuStudio", "02.08.02.61")),
        ):
            for change in ({"slicer_id": "Unknown"}, {"application_version": "0.0"}):
                invalid = {**request, **change}
                with self.subTest(composer=composer.__name__, change=change):
                    with self.assertRaisesRegex(ValueError, "metadata target"):
                        composer(json.dumps(project()), json.dumps(invalid))
            for missing_key in ("slicer_id", "application_version"):
                invalid = {
                    key: value for key, value in request.items()
                    if key != missing_key
                }
                with self.subTest(composer=composer.__name__, missing=missing_key):
                    with self.assertRaises(ValueError):
                        composer(json.dumps(project()), json.dumps(invalid))


if __name__ == "__main__":
    unittest.main()
