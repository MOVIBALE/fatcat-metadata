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
_prusa_spec = importlib.util.spec_from_file_location('prusa_source_refresh', ROOT / 'scripts/sync_prusa_sources.py')
prusa_refresh = importlib.util.module_from_spec(_prusa_spec)
_prusa_spec.loader.exec_module(prusa_refresh)


class RetainedSourceRefreshTests(unittest.TestCase):
    def test_prusa_catalog_provenance_matches_the_reviewed_release(self):
        catalog = json.loads((ROOT / 'compatibility/current-src/native-project-sources/prusa-3/catalog.json').read_text(encoding='utf-8'))
        provenance = prusa_refresh.source_provenance()
        for key, value in provenance.items():
            self.assertEqual(catalog['source'][key], value)
        with patch.object(prusa_refresh, 'VERSION', 'unreviewed-version'):
            with self.assertRaisesRegex(ValueError, 'provenance differs'):
                prusa_refresh.source_provenance()

    def test_native_profile_bom_and_duplicate_fields_match_slicer_parser(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "process").mkdir()
            raw = '\ufeff{"name":"Native", "layer_height":"0.12", "layer_height":"0.20"}'
            path = root / "process/Native.json"
            path.write_text(raw, encoding="utf-8")
            catalog = refresh.ProfileCatalog(root)
            self.assertEqual(catalog.find_named("process", "Native", ""), "process/Native.json")
            self.assertEqual(catalog.resolve("process", "process/Native.json", {})["layer_height"], "0.20")
            self.assertEqual(path.read_text(encoding="utf-8"), raw)

    def test_sdk_manifest_still_rejects_duplicate_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text('{"schema_version":1,"schema_version":2}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate JSON field"):
                refresh._load_json(path)

    def test_partial_refresh_preserves_other_targets_and_format_catalogs(self):
        previous = {
            "schema_version": 1,
            "sources": [
                {"slicer_id": "BambuStudio", "machine_uid": "old", "nozzle_uid": "0.4"},
                {"slicer_id": "OrcaSlicer", "machine_uid": "u1", "nozzle_uid": "0.4",
                 "compatibility_project": {"path": "compatibility-projects/u1.json"}},
            ],
            "profile_files": ["profiles/BambuStudio/old.json", "profiles/OrcaSlicer/u1.json"],
            "references": {"profiles/BambuStudio/old.json": {"inherits": "old-parent"},
                           "profiles/OrcaSlicer/u1.json": {"inherits": "u1-parent"}},
            "compatibility_project_files": ["compatibility-projects/u1.json"],
            "format_catalogs": {"PrusaSlicer": {"catalog": "prusa-3/catalog.json"}},
        }
        staged = {"schema_version": 1,
                  "sources": [{"slicer_id": "BambuStudio", "machine_uid": "h3c", "nozzle_uid": "0.4"}],
                  "profile_files": ["profiles/BambuStudio/h3c.json"],
                  "references": {}, "compatibility_project_files": []}
        original = copy.deepcopy(previous)
        result = refresh._merge_source_indexes(previous, staged, {"BambuStudio"})
        self.assertEqual(previous, original)
        self.assertEqual(result["sources"], staged["sources"] + previous["sources"][1:])
        self.assertEqual(result["profile_files"],
                         ["profiles/BambuStudio/h3c.json", "profiles/OrcaSlicer/u1.json"])
        self.assertEqual(result["references"],
                         {"profiles/OrcaSlicer/u1.json": {"inherits": "u1-parent"}})
        self.assertEqual(result["format_catalogs"], previous["format_catalogs"])
        self.assertEqual(result["compatibility_project_files"], previous["compatibility_project_files"])

    def test_partial_refresh_requires_exact_requested_roots(self):
        with self.assertRaisesRegex(ValueError, "no app root"):
            refresh.sync({}, slicers={"BambuStudio"}, dry_run=True)
        with self.assertRaisesRegex(ValueError, "unknown slicer"):
            refresh.sync({}, slicers={"Typo"}, dry_run=True)

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

    def test_refresh_discovers_native_compatible_alternative_without_rewriting_default(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for category in ("machine", "filament"):
                (root / category).mkdir()
            profiles = {
                "machine/Printer.json": {"type": "machine", "name": "Printer", "default_filament_profile": ["PLA"]},
                "filament/PLA.json": {"type": "filament", "name": "PLA", "compatible_printers": ["Other"]},
                "filament/PLA @Printer.json": {"type": "filament", "name": "PLA @Printer", "instantiation": "true", "compatible_printers": ["Printer"]},
            }
            for name, profile in profiles.items():
                (root / name).write_text(json.dumps(profile), encoding="utf-8")
            entry = refresh._source_entry(refresh.ProfileCatalog(root),
                {"source_machine_profile_name": "Printer", "machine_uid": "test:printer", "nozzle_uid": "nozzle:0.4mm"},
                {"target_contract": {"application_version": "1.0"}, "print_profile_bindings": [], "material_bindings": []}, set(), {})
            self.assertEqual(entry["default_filament_profile_names"], ["PLA"])
            self.assertEqual(entry["availability"]["material"], "available")
            options = {option["name"]: option for option in entry["filament_profile_options"]}
            self.assertIn("unavailable_reason", options["PLA"])
            self.assertEqual(options["PLA @Printer"]["path"], "filament/PLA @Printer.json")
            self.assertNotIn("unavailable_reason", options["PLA @Printer"])

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
