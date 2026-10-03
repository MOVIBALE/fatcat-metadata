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

    def _orca_builtin_request(self, material):
        return {
            "project_source": "fatcat_native",
            "slicer_id": "OrcaSlicer",
            "application_version": "2.4.2",
            "machine_uid": "bambu-lab:p1s",
            "nozzle_uid": NOZZLE_UID,
            "build_plate_uid": "plate:textured-pei",
            "source_materials": [material],
            "process_settings": {"wall_loops": "1"},
        }

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
                    and 'unavailable_reason' not in option
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

    def test_orca_u1_uses_recorded_process_and_materials_with_native_hardware(self):
        slicer, version, machine_uid, _ = next(
            row for row in TARGETS if row[0] == "SnapmakerOrca"
        )
        orca = self._target("OrcaSlicer")
        source = self._source("OrcaSlicer", "2.4.2", "snapmaker:u1")
        self.assertEqual(source["availability"]["machine"], "available")
        self.assertEqual(source["availability"]["material"], "unavailable")
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
        result = json.loads(fatcat.compose_project_settings(json.dumps(request)))
        project = json.loads(result['project_settings_json'])
        self.assertEqual(result['process_source']['source_application_version'], '2.2.4')
        self.assertEqual(result['material_source']['source_application_version'], '2.2.4')
        self.assertEqual(result['material_source']['filament_settings_id'], ['Snapmaker PLA Basic @U1'])
        self.assertEqual(project['version'], '2.2.4')
        self.assertEqual(project['printer_model'], 'Snapmaker U1')
        self.assertEqual(project['filament_colour'], ['#123456'])
        self.assertEqual(project['print_settings_id'], source['compatibility_project']['source_profile_name'])
        retained = json.loads((self.source_root / source['compatibility_project']['path']).read_text(encoding='utf-8'))['project_settings']
        for key in ('filament_settings_id', 'filament_ids', 'filament_type',
                    'textured_plate_temp', 'textured_plate_temp_initial_layer',
                    'hot_plate_temp', 'hot_plate_temp_initial_layer',
                    'nozzle_temperature', 'nozzle_temperature_initial_layer', 'filament_flow_ratio'):
            self.assertEqual(project[key], retained[key][:1], key)
        request['native_print_profile_name'] = 'Invented native default'
        with self.assertRaisesRegex(ValueError, 'recorded compatibility'):
            fatcat.compose_project_settings(json.dumps(request))

    def test_incompatible_native_material_is_rejected_even_when_explicitly_selected(self):
        for nozzle in ('nozzle:0.4mm', 'nozzle:0.6mm'):
            request = self._orca_builtin_request({'name': 'PLA', 'colour': '#123456'})
            request.update(machine_uid='snapmaker:u1', nozzle_uid=nozzle,
                           native_filament_profile_names=['Snapmaker PLA'])
            with self.subTest(nozzle=nozzle), self.assertRaisesRegex(ValueError, 'not compatible'):
                fatcat.compose_project_settings(json.dumps(request))

    def test_missing_native_material_does_not_reuse_a_different_nozzle_source(self):
        request = self._orca_builtin_request({'name': 'PLA', 'colour': '#123456'})
        request.update(machine_uid='snapmaker:u1', nozzle_uid='nozzle:0.6mm')
        with self.assertRaisesRegex(ValueError, 'no available native filament profile'):
            fatcat.compose_project_settings(json.dumps(request))

    def test_retained_materials_do_not_claim_an_unsupported_requested_type(self):
        for material_type in ('PETG', 'PLA-CF'):
            request = self._orca_builtin_request({'name': material_type, 'material_type': material_type,
                                                 'colour': '#123456'})
            request['machine_uid'] = 'snapmaker:u1'
            with self.subTest(material_type=material_type), self.assertRaisesRegex(ValueError, 'retained material'):
                fatcat.compose_project_settings(json.dumps(request))

    def test_native_petg_name_selects_real_profile_and_specialty_types_do_not_fall_back(self):
        basic_request = self._orca_builtin_request({
            "name": "Bambu PETG Basic @BBL X1C",
            "colour": "#345678",
        })
        basic_result = json.loads(fatcat.compose_project_settings(json.dumps(basic_request)))
        basic_project = json.loads(basic_result["project_settings_json"])
        self.assertEqual(
            basic_project["filament_settings_id"], ["Bambu PETG Basic @BBL X1C"]
        )

        for name, material_type in (
            ("PETG HF", "PETG HF"),
            ("PETG-CF", "PETG-CF"),
            ("PETG Translucent", "PETG Translucent"),
        ):
            with self.subTest(material_type=material_type):
                request = self._orca_builtin_request({"name": name, "colour": "#345678"})
                with self.assertRaisesRegex(
                    ValueError,
                    rf"no available native filament profile matches source material slot 0 type '{material_type.upper()}'",
                ):
                    fatcat.compose_project_settings(json.dumps(request))

        explicit_request = self._orca_builtin_request({
            "name": "PETG HF",
            "colour": "#345678",
            "material_type": "petg basic",
        })
        explicit_result = json.loads(
            fatcat.compose_project_settings(json.dumps(explicit_request))
        )
        explicit_project = json.loads(explicit_result["project_settings_json"])
        self.assertEqual(
            explicit_project["filament_settings_id"], ["Bambu PETG Basic @BBL X1C"]
        )

    def test_untyped_native_material_name_uses_native_default_profile(self):
        slicer = "AnycubicSlicerNext"
        version = "2.0.0.2"
        machine_uid = "anycubic:kobra-2"
        source = self._source(slicer, version, machine_uid)
        defaults = source["default_filament_profile_names"]
        materials = [
            {
                "name": defaults[0],
                "colour": "#010203",
                "material_type": "PLA",
            },
            {"name": "Red", "colour": "#ABCDEF"},
        ]
        request = {
            "project_source": "fatcat_native",
            "slicer_id": slicer,
            "application_version": version,
            "machine_uid": machine_uid,
            "nozzle_uid": NOZZLE_UID,
            "build_plate_uid": "plate:cool",
            "source_materials": materials,
        }
        result = json.loads(fatcat.compose_project_settings(json.dumps(request)))
        project = json.loads(result["project_settings_json"])

        self.assertEqual(
            project["filament_settings_id"], defaults
        )

    def test_orca_builtin_process_keeps_one_wall_fix_and_template_override(self):
        request = self._orca_builtin_request({
            "name": "Bambu PLA Basic @BBL X1C",
            "colour": "#345678",
            "material_type": "PLA Basic",
        })
        builtin_result = json.loads(fatcat.compose_project_settings(json.dumps(request)))
        builtin_project = json.loads(builtin_result["project_settings_json"])
        self.assertEqual(builtin_project["wall_loops"], "1")
        self.assertEqual(builtin_project["precise_outer_wall"], "0")
        self.assertIn(
            "precise_outer_wall",
            builtin_project["different_settings_to_system"][0].split(";"),
        )

        explicit_request = self._orca_builtin_request({
            "name": "Bambu PLA Basic @BBL X1C",
            "colour": "#345678",
            "material_type": "PLA Basic",
        })
        explicit_request["process_settings"]["precise_outer_wall"] = "1"
        explicit_result = json.loads(
            fatcat.compose_project_settings(json.dumps(explicit_request))
        )
        explicit_project = json.loads(explicit_result["project_settings_json"])
        self.assertEqual(explicit_project["precise_outer_wall"], "1")
        self.assertIn(
            "precise_outer_wall",
            explicit_project["different_settings_to_system"][0].split(";"),
        )

        builtin_project["precise_outer_wall"] = "1"
        template_request = {
            "slicer_id": "OrcaSlicer",
            "application_version": "2.4.2",
            "hardware_mode": "preserve_source",
            "material_mode": "preserve_template",
            "preserve_source_material_settings": True,
            "source_materials": request["source_materials"],
            "process_settings": {"wall_loops": "1"},
        }
        template_result = json.loads(fatcat.compose_project_settings(
            json.dumps(builtin_project), json.dumps(template_request)
        ))
        template_project = json.loads(template_result["project_settings_json"])
        self.assertEqual(template_project["precise_outer_wall"], "1")


if __name__ == "__main__":
    unittest.main(verbosity=2)
