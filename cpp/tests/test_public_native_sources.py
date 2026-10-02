"""Exercise real native printer, process, plate, and material sources."""

import json
import unittest
from pathlib import Path

import fatcat_metadata as fatcat


TARGETS = (
    ("BambuStudio", "02.08.02.61", "bambu-lab:a1", "plate:textured-pei"),
    ("OrcaSlicer", "2.4.2", "bambu-lab:p1s", "plate:textured-pei"),
    ("QIDIStudio", "02.07.02.60", "qidi:q2", "plate:cool"),
    ("ElegooSlicer", "1.5.3.5", "elegoo:centauri", "plate:cool"),
    ("AnycubicSlicerNext", "2.0.0.2", "anycubic:kobra-2-pro", "plate:cool"),
    ("FlashStudio", "1.7.15", "flashforge:ad5x", "plate:textured-pei"),
    ("SnapmakerOrca", "2.3.6", "snapmaker:u1", "plate:textured-pei"),
)
NOZZLE_UID = "nozzle:0.4mm"
TARGET_FILES = {
    "BambuStudio": "bambu-studio-02.08.02.61.json",
    "OrcaSlicer": "orca-slicer-2.4.2.json",
    "QIDIStudio": "qidi-studio-02.07.02.60.json",
    "ElegooSlicer": "elegoo-slicer-1.5.3.5.json",
    "AnycubicSlicerNext": "anycubic-slicer-next-2.0.0.2.json",
    "FlashStudio": "flash-studio-1.7.15.json",
    "SnapmakerOrca": "snapmaker-orca-2.3.6.json",
}


class PublicNativeSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.package_root = Path(fatcat.__file__).resolve().parent
        cls.data_root = cls.package_root / "fatcat_metadata_data"
        cls.source_root = cls.data_root / "native_project_sources"
        cls.source_index = json.loads(
            (cls.source_root / "source-index.json").read_text(encoding="utf-8")
        )

    def _target(self, slicer):
        path = self.data_root / "translations" / "targets" / TARGET_FILES[slicer]
        self.assertTrue(path.is_file(), path)
        return json.loads(path.read_text(encoding="utf-8"))

    def _source(self, slicer, version, machine_uid):
        rows = [
            row for row in self.source_index["sources"]
            if row.get("slicer_id") == slicer
            and row.get("application_version") == version
            and row.get("machine_uid") == machine_uid
            and row.get("nozzle_uid") == NOZZLE_UID
        ]
        self.assertEqual(len(rows), 1, (slicer, machine_uid, NOZZLE_UID))
        return rows[0]

    def test_seven_targets_compose_from_exact_packaged_sources(self):
        self.assertEqual(self.source_index["schema_version"], 1)
        for slicer, version, machine_uid, expected_plate_uid in TARGETS:
            with self.subTest(slicer=slicer):
                target = self._target(slicer)
                self.assertEqual(target["target_contract"]["slicer_id"], slicer)
                self.assertEqual(
                    target["target_contract"]["application_version"], version
                )
                source = self._source(slicer, version, machine_uid)
                self.assertEqual(
                    source["availability"]["machine"], "available"
                )
                self.assertEqual(source["availability"]["process"], "available")
                self.assertEqual(source["availability"]["material"], "available")

                machine_path = self.source_root / source["machine_profile_path"]
                self.assertTrue(machine_path.is_file(), machine_path)
                machine = json.loads(machine_path.read_text(encoding="utf-8"))
                machine_binding = next(
                    binding for binding in target["machine_bindings"]
                    if binding.get("machine_uid") == machine_uid
                    and binding.get("nozzle_uid") == NOZZLE_UID
                )
                self.assertEqual(
                    machine_binding["source_profile_name"],
                    source["source_machine_profile_name"],
                )
                self.assertEqual(machine_binding["printer_model"], machine["printer_model"])
                self.assertEqual(machine_binding["nozzle_diameter"], machine["nozzle_diameter"])

                catalog = json.loads(fatcat.native_project_source_catalog(slicer))
                self.assertEqual(catalog["application_version"], version)
                catalog_row = next(
                    row for row in catalog["sources"]
                    if row["machine_uid"] == machine_uid
                    and row["nozzle_uid"] == NOZZLE_UID
                )
                self.assertEqual(
                    catalog_row["hardware_settings"]["printer_model"],
                    machine_binding["printer_model"],
                )
                self.assertEqual(
                    catalog_row["hardware_settings"]["nozzle_diameter"],
                    machine_binding["nozzle_diameter"],
                )

                process_options = source["print_profile_options"]
                process_name = source["default_print_profile_name"]
                process = next(item for item in process_options if item["name"] == process_name)
                self.assertTrue((self.source_root / process["path"]).is_file())
                plate_binding = next(
                    item for item in target["build_plate_bindings"]
                    if item["build_plate_uid"] == expected_plate_uid
                )
                if "supported_build_plate_uids" in machine_binding:
                    self.assertIn(
                        expected_plate_uid,
                        machine_binding["supported_build_plate_uids"],
                    )

                available_materials = [
                    option for option in source["filament_profile_options"]
                    if isinstance(option.get("path"), str)
                    and (self.source_root / option["path"]).is_file()
                ]
                self.assertTrue(available_materials)
                selected_materials = available_materials[:2]
                profile_names = [option["name"] for option in selected_materials]
                request = {
                    "project_source": "fatcat_native",
                    "slicer_id": slicer,
                    "application_version": version,
                    "machine_uid": machine_uid,
                    "nozzle_uid": NOZZLE_UID,
                    "build_plate_uid": expected_plate_uid,
                    "source_materials": [
                        {"name": name, "colour": f"#{index + 1:06X}"}
                        for index, name in enumerate(profile_names)
                    ],
                    "native_filament_profile_names": profile_names,
                }
                result = json.loads(fatcat.compose_project_settings(json.dumps(request)))
                composed = json.loads(result["project_settings_json"])
                self.assertEqual(
                    composed["printer_settings_id"], source["source_machine_profile_name"]
                )
                self.assertEqual(composed["print_settings_id"], process_name)
                self.assertEqual(composed["filament_settings_id"], profile_names)
                self.assertEqual(composed["printer_model"], machine_binding["printer_model"])
                self.assertEqual(composed["nozzle_diameter"], machine_binding["nozzle_diameter"])
                self.assertEqual(composed["curr_bed_type"], plate_binding["project_value"])
                self.assertEqual(
                    composed["filament_colour"],
                    [material["colour"] for material in request["source_materials"]],
                )

    def test_orca_u1_without_an_exact_process_does_not_invent_a_default(self):
        slicer, version, machine_uid, _ = next(
            row for row in TARGETS if row[0] == "SnapmakerOrca"
        )
        orca = self._target("OrcaSlicer")
        source = self._source("OrcaSlicer", "2.4.2", "snapmaker:u1")
        self.assertEqual(source["availability"]["machine"], "available")
        self.assertEqual(source["availability"]["material"], "available")
        self.assertEqual(source["availability"]["process"], "unavailable")
        self.assertFalse(source.get("default_print_profile_name"))
        self.assertTrue(
            any(
                binding.get("machine_uid") == machine_uid
                and binding.get("nozzle_uid") == NOZZLE_UID
                for binding in orca["machine_bindings"]
            )
        )
        request = {
            "project_source": "fatcat_native",
            "slicer_id": "OrcaSlicer",
            "application_version": "2.4.2",
            "machine_uid": "snapmaker:u1",
            "nozzle_uid": NOZZLE_UID,
            "build_plate_uid": "plate:textured-pei",
            "source_materials": [{"name": "Snapmaker PLA", "colour": "#123456"}],
        }
        with self.assertRaisesRegex(ValueError, "no unique native default process"):
            fatcat.compose_project_settings(json.dumps(request))


if __name__ == "__main__":
    unittest.main(verbosity=2)
