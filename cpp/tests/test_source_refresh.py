"""Retained process data survives native profile refresh without relabeling."""
import copy
import importlib.util
import json
from pathlib import Path
import plistlib
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location('native_source_refresh', ROOT / 'scripts/sync_native_project_sources.py')
refresh = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(refresh)


class RetainedSourceRefreshTests(unittest.TestCase):
    def test_refresh_marks_inherited_incompatible_material_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'machine').mkdir()
            (root / 'filament').mkdir()
            (root / 'machine/Printer.json').write_text(json.dumps({
                'type': 'machine', 'name': 'Printer', 'default_filament_profile': ['PLA']}), encoding='utf-8')
            (root / 'filament/PLA.json').write_text(json.dumps({
                'type': 'filament', 'name': 'PLA', 'inherits': 'base'}), encoding='utf-8')
            (root / 'filament/base.json').write_text(json.dumps({
                'type': 'filament', 'name': 'base', 'compatible_printers': ['Different Printer']}), encoding='utf-8')
            catalog = refresh.ProfileCatalog(root)
            row = {'source_machine_profile_name': 'Printer', 'machine_uid': 'test:printer',
                   'nozzle_uid': 'nozzle:0.4mm'}
            target = {'target_contract': {'application_version': '1.0'},
                      'print_profile_bindings': [], 'material_bindings': []}
            entry = refresh._source_entry(catalog, row, target, set(), {})
            self.assertEqual(entry['availability']['material'], 'unavailable')
            self.assertIn('not compatible', entry['filament_profile_options'][0]['unavailable_reason'])

    def _refresh(self, change=None):
        source_map = json.loads(refresh.SOURCE_MAP_PATH.read_text(encoding='utf-8'))
        row = copy.deepcopy(next(row for row in source_map['machine_profiles'] if 'compatibility_project' in row))
        if change:
            change(row['compatibility_project'])
        index = json.loads((refresh.OUTPUT_DIR / 'source-index.json').read_text(encoding='utf-8'))
        entry = copy.deepcopy(next(entry for entry in index['sources'] if 'compatibility_project' in entry))
        target = json.loads((refresh.TARGET_DIR / refresh.TARGET_FILES['OrcaSlicer']).read_text(encoding='utf-8'))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = root / 'OrcaSlicer.app'
            (app / 'Contents/Resources/profiles').mkdir(parents=True)
            with (app / 'Contents/Info.plist').open('wb') as file:
                plistlib.dump({'CFBundleShortVersionString': '2.4.2'}, file)
            stage = root / 'stage'
            stage.mkdir()
            with patch.object(refresh, '_source_entry', return_value=entry):
                refresh._write_tree(stage, [row], {'OrcaSlicer': app}, {'OrcaSlicer': target})
            retained = row['compatibility_project']['path']
            self.assertEqual((stage / retained).read_bytes(), (refresh.OUTPUT_DIR / retained).read_bytes())
            result = json.loads((stage / 'source-index.json').read_text(encoding='utf-8'))
            self.assertEqual(result['compatibility_project_files'], [retained])
            self.assertEqual(result['sources'][0]['compatibility_project'], row['compatibility_project'])
            self.assertEqual(result['sources'][0]['availability']['process'], 'unavailable')

    def test_refresh_copies_retained_source_verbatim(self):
        self._refresh()

    def test_refresh_rejects_wrong_provenance(self):
        with self.assertRaisesRegex(ValueError, 'provenance mismatch'):
            self._refresh(lambda record: record.update(source_application_version='2.4.2'))

    def test_refresh_rejects_wrong_hash(self):
        with self.assertRaisesRegex(ValueError, 'content hash mismatch'):
            self._refresh(lambda record: record.update(source_sha256='0' * 64))

    def test_refresh_rejects_traversal(self):
        with self.assertRaisesRegex(ValueError, 'unsafe relative source path'):
            self._refresh(lambda record: record.update(path='../outside.json'))


if __name__ == '__main__':
    unittest.main()
