"""Exact machine lookups must not resolve unrelated machines."""

import json
from pathlib import Path
import shutil
import os
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import fatcat_metadata as fatcat


class NativeSourceLookupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        installed = Path(fatcat.__file__).parent
        shutil.copytree(installed / "fatcat_metadata_data", self.root / "fatcat_metadata_data")
        self.patch = patch.object(fatcat, "__file__", str(self.root / Path(fatcat.__file__).name))
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.sources = self.root / "fatcat_metadata_data/native_project_sources"

    def request(self, machine="p2s", **overrides):
        return dict(slicer_id="BambuStudio", application_version="02.08.02.61",
                    machine_uid="bambu-lab:" + machine, nozzle_uid="nozzle:0.4mm", **overrides)

    def test_unrelated_broken_profiles_do_not_block_exact_lookup(self):
        self.assertTrue(callable(getattr(fatcat, "native_project_source", None)),
                        "the SDK must expose an exact machine-source lookup")
        for name in ["Bambu Lab H2D Pro 0.4 nozzle template wrapping_detection_gcode.json",
                     "Bambu Lab A1 mini 0.4 nozzle template machine_start_gcode.json"]:
            (self.sources / "profiles/BambuStudio/BBL/machine" / name).unlink()
        for machine in ("p2s", "x2d"):
            with self.subTest(machine=machine):
                source = fatcat.native_project_source(self.request(machine))
                self.assertEqual(source["machine_uid"], "bambu-lab:" + machine)
                self.assertEqual(source["hardware_settings"]["nozzle_diameter"], ["0.4"] * (2 if machine == "x2d" else 1))
                by_name = self.request(machine)
                del by_name["machine_uid"]
                by_name["source_machine_profile_name"] = source["source_machine_profile_name"]
                self.assertEqual(fatcat.native_project_source(by_name), source)

    def test_selected_dependency_is_still_required(self):
        self.assertTrue(callable(getattr(fatcat, "native_project_source", None)))
        name = "Bambu Lab P2S 0.4 nozzle template machine_start_gcode.json"
        (self.sources / "profiles/BambuStudio/BBL/machine" / name).unlink()
        with self.assertRaisesRegex(ValueError, "P2S.*machine_start_gcode"):
            fatcat.native_project_source(self.request())

    def test_unrelated_target_corruption_does_not_block_material_selection(self):
        targets = self.root / "fatcat_metadata_data/translations/targets"
        path = next(targets.glob("prusa-*.json"))
        path.write_text("{invalid", encoding="utf-8")
        request = self.request(
            project_source="fatcat_native",
            build_plate_uid="plate:textured-pei",
            source_materials=[{"name": "PLA", "material_type": "PLA", "colour": "#123456"}],
        )
        self.assertTrue(fatcat.resolve_project_materials(request)["ready"])

    def test_source_identity_ignores_titles_and_unrelated_target_files(self):
        targets = self.root / "fatcat_metadata_data/translations/targets"
        for path in targets.glob("*.json"):
            if not path.name.startswith("bambu-studio-"):
                path.write_text("{invalid", encoding="utf-8")
        model = '<model><metadata name="Application">BambuStudio-02.08.02.61</metadata>{}</model>'
        for title in ("", '<metadata name="Title">Snapmaker adapter</metadata>',
                      '<metadata name="Description">OrcaSlicer-0.0.0</metadata>'):
            with self.subTest(title=title):
                result = json.loads(fatcat.detect_template_source(model.format(title), None))
                self.assertEqual(result, {"source_slicer": "BambuStudio", "source_version": "02.08.02.61"})

    def test_unavailable_abs_does_not_block_pla_but_remains_unselectable(self):
        index = json.loads((self.sources / "source-index.json").read_text(encoding="utf-8"))
        source = next(row for row in index["sources"]
                      if row["slicer_id"] == "BambuStudio"
                      and row["machine_uid"] == "bambu-lab:p2s"
                      and row["nozzle_uid"] == "nozzle:0.4mm")
        material = next(row for row in source["filament_profile_options"]
                        if "ABS" in row["name"] and "path" in row)
        path = self.sources / material["path"]
        original = json.loads(path.read_text(encoding="utf-8"))
        request = self.request(
            project_source="fatcat_native",
            build_plate_uid="plate:textured-pei",
            source_materials=[{"name": "PLA", "material_type": "PLA", "colour": "#123456"}],
        )
        for fault in (None, {"filament_type": ["ABS", "PLA"]},
                      {"compatible_printers": "invalid"}):
            with self.subTest(fault=fault):
                if fault is None:
                    path.unlink()
                else:
                    path.write_text(json.dumps(original | fault), encoding="utf-8")
                request["source_materials"][0].pop("native_filament_profile_name", None)
                self.assertTrue(fatcat.resolve_project_materials(request)["ready"])
                request["source_materials"][0]["native_filament_profile_name"] = material["name"]
                rejected = fatcat.resolve_project_materials(request)
                self.assertFalse(rejected["ready"])
                self.assertIn("native_material_unavailable", json.dumps(rejected["issues"]))

    def test_prusa_exact_lookup_retains_native_capacity_policy(self):
        catalog = fatcat.list_machines("PrusaSlicer", "3.0.0-alpha12")
        row = catalog["sources"][0]
        selected = fatcat.native_project_source({
            "slicer_id": "PrusaSlicer", "application_version": "3.0.0-alpha12",
            "machine_uid": row["machine_uid"], "nozzle_uid": row["nozzle_uid"],
        })
        self.assertEqual(selected["machine_uid"], row["machine_uid"])
        self.assertEqual(selected["filament_slot_policy"], "preserve_native_capacity")

    def test_unknown_ambiguous_or_incomplete_identity_is_rejected(self):
        self.assertTrue(callable(getattr(fatcat, "native_project_source", None)))
        for change in ({"machine_uid": "unknown"}, {"nozzle_uid": ""},
                       {"application_version": "unknown"},
                       {"source_machine_profile_name": "Bambu Lab X2D 0.4 nozzle"}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                fatcat.native_project_source(self.request() | change)
        index = self.sources / "source-index.json"
        data = json.loads(index.read_text())
        data["sources"].append(next(row for row in data["sources"]
                                    if row["machine_uid"] == "bambu-lab:p2s"
                                    and row["nozzle_uid"] == "nozzle:0.4mm"
                                    and row["slicer_id"] == "BambuStudio"))
        index.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "unique"):
            fatcat.native_project_source(self.request())

    def test_exact_lookup_keeps_path_traversal_protection(self):
        self.assertTrue(callable(getattr(fatcat, "native_project_source", None)))
        index = self.sources / "source-index.json"
        data = json.loads(index.read_text())
        row = next(row for row in data["sources"]
                   if row["machine_uid"] == "bambu-lab:p2s"
                   and row["nozzle_uid"] == "nozzle:0.4mm"
                   and row["slicer_id"] == "BambuStudio")
        row["machine_profile_path"] = "../outside.json"
        index.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "traversal"):
            fatcat.native_project_source(self.request())

    def test_exact_lookup_rejects_directory_link_escaping_data_root(self):
        outside = self.root / "outside"
        outside.mkdir()
        original = self.sources / "profiles/BambuStudio/BBL/machine/Bambu Lab P2S 0.4 nozzle.json"
        shutil.copy2(original, outside / "machine.json")
        link = self.sources / "escape"
        if os.name == "nt":
            subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(outside)],
                           check=True, capture_output=True)
        else:
            link.symlink_to(outside, target_is_directory=True)
        index = self.sources / "source-index.json"
        data = json.loads(index.read_text())
        row = next(row for row in data["sources"]
                   if row["machine_uid"] == "bambu-lab:p2s"
                   and row["nozzle_uid"] == "nozzle:0.4mm"
                   and row["slicer_id"] == "BambuStudio")
        row["machine_profile_path"] = "escape/machine.json"
        index.write_text(json.dumps(data))
        try:
            # Windows may reject the junction as an untrusted mount point
            # before canonicalization can return its out-of-root destination.
            with self.assertRaisesRegex(ValueError, "escapes|native source file access failed"):
                fatcat.native_project_source(self.request())
        finally:
            if os.name == "nt":
                link.rmdir()
            else:
                link.unlink()


if __name__ == "__main__":
    unittest.main()
