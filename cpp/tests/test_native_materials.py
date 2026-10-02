"""Exercise exact native material bindings in both installed targets."""

import json
import unittest
from pathlib import Path

import fatcat_metadata as fatcat

from test_target_selection import TARGETS, material_profile, project, project_request


class NativeMaterialTests(unittest.TestCase):
    def test_native_selection_uses_the_exact_packaged_profile(self):
        data_root = Path(fatcat.__file__).resolve().parent / "fatcat_metadata_data"
        for slicer, version in TARGETS:
            with self.subTest(target=slicer):
                source = project()
                request = project_request(slicer, version)
                request.update(
                    material_mode="target_native_preset",
                    material_uid="material:pla",
                )
                result = json.loads(
                    fatcat.compose_project_settings(
                        json.dumps(source), json.dumps(request)
                    )
                )
                final = json.loads(result["project_settings_json"])
                target_filename = (
                    "orca-slicer-2.4.2.json"
                    if slicer == "OrcaSlicer"
                    else "bambu-studio-02.08.02.61.json"
                )
                target = json.loads(
                    (data_root / "translations" / "targets" / target_filename)
                    .read_text(encoding="utf-8")
                )
                binding = next(
                    item for item in target["material_bindings"]
                    if item["machine_uid"] == request["machine_uid"]
                    and item["nozzle_uid"] == request["nozzle_uid"]
                    and item["material_uid"] == "material:pla"
                )
                profile = material_profile(target, binding)
                self.assertEqual(
                    final["filament_settings_id"],
                    [binding["native_profile_id"]] * len(source["filament_type"]),
                )
                for key, parameter in profile["target_parameters"].items():
                    self.assertEqual(
                        final[key], parameter["values"] * len(source["filament_type"]),
                        key,
                    )
                self.assertEqual(
                    final["outer_wall_speed"], source["outer_wall_speed"]
                )
                self.assertEqual(
                    final["unknown_nested"], source["unknown_nested"]
                )


if __name__ == "__main__":
    unittest.main(verbosity=2)
