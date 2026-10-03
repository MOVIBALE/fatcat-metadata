"""Check the recommended dictionary API against the published JSON API."""

import json
from pathlib import Path
import unittest

import fatcat_metadata


EXAMPLES = Path(__file__).resolve().parents[1] / "examples/python_neroued_consumer"


def fixture(name: str) -> dict:
    return json.loads((EXAMPLES / name).read_text(encoding="utf-8"))


class DictionaryApiTests(unittest.TestCase):
    def assert_project_equivalent(self, modern: dict, legacy: dict) -> None:
        expected = dict(legacy)
        expected["project_settings"] = json.loads(expected.pop("project_settings_json"))
        self.assertEqual(modern, expected)
        self.assertNotIn("project_settings_json", modern)

    def test_builtin_and_none_use_the_same_composer(self) -> None:
        for name in ("builtin_project_request.json", "orca_u1_project_request.json"):
            with self.subTest(source=name):
                request = fixture(name)
                modern = fatcat_metadata.compose_project_settings(request)
                legacy = json.loads(fatcat_metadata.compose_project_settings(json.dumps(request)))
                self.assert_project_equivalent(modern, legacy)
                self.assertEqual(modern, fatcat_metadata.compose_project_settings(None, request))
                self.assertEqual(modern, fatcat_metadata.compose_project_settings(request=request))

    def test_explicit_source_and_unknown_fields_survive(self) -> None:
        project, request = fixture("project.json"), fixture("project_request.json")
        project["caller_owned"] = {"name": "自定义材料", "values": [1, None, True]}
        original = json.dumps(project, ensure_ascii=False)
        modern = fatcat_metadata.compose_project_settings(project=project, request=request)
        legacy = json.loads(fatcat_metadata.compose_project_settings(original, json.dumps(request)))
        self.assert_project_equivalent(modern, legacy)
        self.assertEqual(project["caller_owned"], modern["project_settings"]["caller_owned"])
        self.assertEqual(original, json.dumps(project, ensure_ascii=False))

    def test_final_metadata_matches_string_api(self) -> None:
        request = fixture("builtin_project_request.json")
        result = fatcat_metadata.compose_project_settings(request)
        project = result["project_settings"]
        metadata_request = json.loads(
            (EXAMPLES.parent / "out_of_tree_consumer/request.json").read_text(encoding="utf-8")
        )
        metadata_request["slicer_id"] = request["slicer_id"]
        metadata_request["application_version"] = request["application_version"]
        metadata_request["plate"]["bed_type"] = result["metadata_defaults"]["plate_value"]
        modern = fatcat_metadata.compose_model_metadata(project, metadata_request)
        legacy = json.loads(fatcat_metadata.compose_model_metadata(
            json.dumps(project, ensure_ascii=False, sort_keys=True, separators=(",", ":")),
            json.dumps(metadata_request),
        ))
        self.assertEqual(modern, legacy)

    def test_incompatible_request_has_same_error(self) -> None:
        request = fixture("builtin_project_request.json")
        request["application_version"] = "unsupported"
        with self.assertRaises(ValueError) as legacy:
            fatcat_metadata.compose_project_settings(json.dumps(request))
        with self.assertRaises(ValueError) as modern:
            fatcat_metadata.compose_project_settings(request)
        self.assertEqual(str(modern.exception), str(legacy.exception))

    def test_installed_stub_describes_all_public_functions(self) -> None:
        stub = Path(fatcat_metadata.__file__).parent / "fatcat_metadata-stubs/__init__.pyi"
        content = stub.read_text(encoding="utf-8")
        for name in dir(fatcat_metadata):
            if not name.startswith("_") and callable(getattr(fatcat_metadata, name)):
                self.assertIn(f"def {name}(", content)


if __name__ == "__main__":
    unittest.main()
