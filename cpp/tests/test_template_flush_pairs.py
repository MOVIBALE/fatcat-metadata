"""Imported templates retain flush tuning when their last pair is truncated."""

import copy
import unittest

import fatcat_metadata as fatcat


class TemplateFlushPairTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.request = {
            "project_source": "fatcat_native",
            "slicer_id": "BambuStudio",
            "application_version": "02.09.01.70",
            "machine_uid": "bambu-lab:h2c",
            "nozzle_uid": "nozzle:0.4mm",
            "build_plate_uid": "plate:textured-pei",
            "source_materials": [
                {"name": f"Colour {slot}", "material_type": "PLA Basic",
                 "colour": f"#{slot + 1:02X}2233"}
                for slot in range(19)
            ],
        }
        cls.project = fatcat.compose_project_settings(cls.request)["project_settings"]
        cls.project["filament_settings_id"] = ["My custom PLA"] * 19
        cls.project["flush_volumes_vector"] = [
            str(80 + value * 40) for value in range(38)
        ]
        cls.project["flush_volumes_matrix"] = [str(value) for value in range(19 * 19 * 2)]

    def compose(self, project, slots, mode="compact"):
        request = dict(self.request)
        request.pop("project_source")
        request.update(
            hardware_mode="preserve_source",
            material_mode="preserve_template",
            preserve_source_material_settings=True,
            filament_slot_mode=mode,
            filament_source_slots=slots,
            source_materials=[self.request["source_materials"][slot] for slot in slots],
        )
        return fatcat.compose_project_settings(project, request)["project_settings"]

    def test_truncated_tail_uses_source_pair_before_reordering(self):
        for missing in (1, 2, 3):
            with self.subTest(missing=missing):
                source = copy.deepcopy(self.project)
                source["flush_volumes_vector"] = source["flush_volumes_vector"][:-missing]
                unchanged_source = copy.deepcopy(source)
                slots = [18, 1, 0]
                result = self.compose(source, slots)
                repaired = source["flush_volumes_vector"] + [
                    self.project["flush_volumes_vector"][index % 2]
                    for index in range(38 - missing, 38)
                ]
                self.assertEqual(
                    result["flush_volumes_vector"],
                    [value for slot in slots for value in repaired[slot * 2:slot * 2 + 2]],
                )
                self.assertEqual(result["flush_volumes_matrix"], [
                    self.project["flush_volumes_matrix"][nozzle * 19 * 19 + row * 19 + col]
                    for nozzle in range(2) for row in slots for col in slots
                ])
                self.assertEqual(result["filament_settings_id"], ["My custom PLA"] * 3)
                self.assertEqual(source, unchanged_source)

    def test_preserve_mode_keeps_complete_pairs_and_unselected_tail(self):
        source = copy.deepcopy(self.project)
        source["flush_volumes_vector"].pop()
        result = self.compose(source, list(range(17)), mode="preserve")
        self.assertEqual(result["flush_volumes_vector"],
                         source["flush_volumes_vector"] + [source["flush_volumes_vector"][1]])
        self.assertEqual(result["flush_volumes_matrix"], source["flush_volumes_matrix"])
        self.assertEqual(result["nozzle_temperature"], source["nozzle_temperature"])

    def test_complete_vector_is_not_replaced(self):
        result = self.compose(self.project, list(range(19)), mode="preserve")
        self.assertEqual(result["flush_volumes_vector"], self.project["flush_volumes_vector"])
        self.assertEqual(result["flush_volumes_matrix"], self.project["flush_volumes_matrix"])

    def test_unresolved_shape_is_still_rejected(self):
        for count in (18, 39):
            with self.subTest(count=count):
                source = copy.deepcopy(self.project)
                source["flush_volumes_vector"] = ["140"] * count
                with self.assertRaisesRegex(ValueError, "unresolved slot shape"):
                    self.compose(source, list(range(19)))


if __name__ == "__main__":
    unittest.main(verbosity=2)
