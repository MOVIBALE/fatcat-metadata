"""Generator placement follows the selected project's printable rectangle."""

import copy
import importlib.util
import json
import io
from pathlib import Path
import unittest
import zipfile

import fatcat_metadata as fatcat


EXAMPLES = Path(__file__).resolve().parents[1] / "examples" / "python_neroued_consumer"
spec = importlib.util.spec_from_file_location("consumer_example", EXAMPLES / "write_example.py")
example = importlib.util.module_from_spec(spec)
spec.loader.exec_module(example)
verify_spec = importlib.util.spec_from_file_location("consumer_verify", EXAMPLES / "verify_3mf.py")
verify = importlib.util.module_from_spec(verify_spec)
verify_spec.loader.exec_module(verify)


def rectangle(x0=0, y0=0, x1=180, y1=180, height=180):
    return {"printable_area": [f"{x0}x{y0}", f"{x1}x{y0}", f"{x1}x{y1}", f"{x0}x{y1}"],
            "printable_height": str(height)}


class ConsumerLayoutTests(unittest.TestCase):
    def test_cube_material_uses_the_selected_request_palette(self):
        request = {'source_materials': [{'name': 'Selected PLA', 'colour': '#123456'}]}
        project = {'filament_settings_id': ['Actual native preset'], 'filament_colour': ['#123456']}
        self.assertEqual(example._cube_material(request, project), ('Selected PLA', '#123456'))
        self.assertEqual(example._cube_material({}, project), ('Actual native preset', '#123456'))

    def test_serialized_build_translation_is_checked_against_offset_bed(self):
        mesh = '<model xmlns="' + verify.CORE[1:-1] + '"><resources><basematerials id="1">'
        mesh += '<base name="Selected PLA" displaycolor="#123456"/></basematerials>'
        mesh += '<object id="2" pid="1" pindex="0"><mesh><vertices>'
        mesh += ''.join(f'<vertex x="{x}" y="{y}" z="{z}"/>' for x, y, z in example._CUBE_VERTICES)
        mesh += '</vertices></mesh></object></resources></model>'
        for translation, valid in [("0 0 0", False), ("125.5 126 0", True)]:
            with self.subTest(translation=translation):
                with zipfile.ZipFile(io.BytesIO(), "w") as archive:
                    archive.writestr('3D/3dmodel.model', f'<model xmlns="{verify.CORE[1:-1]}" '
                        f'xmlns:p="{verify.PRODUCTION[1:-1]}"><resources><object id="3"><components>'
                        '<component objectid="2" p:path="/3D/Objects/cube.model"/></components></object>'
                        f'</resources><build><item objectid="3" transform="1 0 0 0 1 0 0 0 1 {translation}"/></build></model>')
                    archive.writestr('3D/Objects/cube.model', mesh)
                    project = rectangle(.5, 1, 270.5, 271)
                    project['filament_colour'] = ['#123456']
                    archive.writestr('Metadata/project_settings.config', json.dumps(project))
                    archive.writestr('Metadata/model_settings.config', '<config><object><metadata key="extruder" value="1"/></object></config>')
                    request = {'source_materials': [{'name': 'Selected PLA', 'colour': '#123456'}]}
                    self.assertEqual(verify.verify_cube_material(archive, request), ('Selected PLA', '#123456'))
                    request['source_materials'][0]['name'] = 'Different PLA'
                    with self.assertRaisesRegex(RuntimeError, 'material name'):
                        verify.verify_cube_material(archive, request)
                    request['source_materials'][0].update(name='Selected PLA', colour='#654321')
                    with self.assertRaisesRegex(RuntimeError, 'material colour'):
                        verify.verify_cube_material(archive, request)
                    if valid:
                        self.assertEqual(verify.verify_cube_placement(archive), ((125.5, 126, 0), (145.5, 146, 20)))
                    else:
                        with self.assertRaisesRegex(RuntimeError, 'printable area boundary'):
                            verify.verify_cube_placement(archive)

    def test_selected_native_targets_center_cube_without_changing_settings(self):
        for slicer, version, machine in [("BambuStudio", "02.08.02.61", "bambu-lab:a1-mini"),
                                         ("OrcaSlicer", "2.4.2", "snapmaker:u1")]:
            with self.subTest(slicer=slicer):
                result = json.loads(fatcat.compose_project_settings(json.dumps({
                    "project_source": "fatcat_native", "slicer_id": slicer,
                    "application_version": version, "machine_uid": machine,
                    "nozzle_uid": "nozzle:0.4mm", "build_plate_uid": "plate:textured-pei",
                    "source_materials": [{"name": "PLA", "material_type": "PLA", "colour": "#123456"}],
                })))
                project = json.loads(result["project_settings_json"])
                before = copy.deepcopy(project)
                translation = example._cube_placement(project)
                points = [tuple(map(float, point.split("x"))) for point in project["printable_area"]]
                for axis in range(2):
                    lower = min(p[axis] for p in points)
                    upper = max(p[axis] for p in points)
                    self.assertGreater(translation[axis], lower)
                    self.assertLess(translation[axis] + 20, upper)
                    self.assertAlmostEqual(translation[axis] + 10, (lower + upper) / 2)
                self.assertEqual(translation[2], 0)
                self.assertEqual(project, before)

    def test_offset_rectangle_does_not_require_machine_identity(self):
        self.assertEqual(example._cube_placement(rectangle(10, 20, 90, 120)), (40, 60, 0))

    def test_custom_project_without_area_requires_explicit_center(self):
        with self.assertRaisesRegex(ValueError, "bed-center"):
            example._cube_placement({})
        self.assertEqual(example._cube_placement({}, (90, 90)), (80, 80, 0))

    def test_explicit_center_outside_native_bed_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "printable area"):
            example._cube_placement(rectangle(), (0, 0))

    def test_cube_too_large_for_printable_area_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "printable area"):
            example._cube_placement(rectangle(x1=19))

    def test_cube_exceeding_native_height_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "printable height"):
            example._cube_placement(rectangle(height=19.9))

    def test_nonfinite_center_and_nonrectangular_area_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "finite"):
            example._cube_placement(rectangle(), (float("nan"), 90))
        with self.assertRaisesRegex(ValueError, "rectangle"):
            example._cube_placement({"printable_area": ["0x0", "180x0", "0x180"]})


if __name__ == "__main__":
    unittest.main()
