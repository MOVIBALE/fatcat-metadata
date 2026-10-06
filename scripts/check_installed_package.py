#!/usr/bin/env python3
"""Check that the installed wheel is imported outside its source checkout."""

from __future__ import annotations

import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import sys


def main() -> int:
    import fatcat_metadata
    import fatcat_metadata_neroued

    source_root = Path(
        os.environ.get("GITHUB_WORKSPACE", Path(__file__).resolve().parents[1])
    ).resolve()
    module_path = Path(fatcat_metadata.__file__).resolve()
    module_root = module_path.parent
    if not (module_root / "fatcat_metadata-stubs/__init__.pyi").is_file():
        raise SystemExit("Python interface type hints were not packaged")
    if module_path.is_relative_to(source_root):
        raise SystemExit(f"import resolved inside source checkout: {module_path}")
    if Path.cwd().resolve().is_relative_to(source_root):
        raise SystemExit("installed-package check must run outside the source checkout")
    if module_path.suffix == ".py":
        raise SystemExit(f"expected the compiled C++ extension, imported {module_path}")
    distribution = importlib.metadata.distribution("fatcat-metadata")
    if distribution.version != "0.1.0":
        raise SystemExit("unexpected fatcat-metadata distribution version")
    if not fatcat_metadata.__fatcat_cpp_extension__:
        raise SystemExit("fatcat_metadata is not the compiled C++ extension")
    if not callable(fatcat_metadata_neroued.apply_root_model_metadata):
        raise SystemExit("Neroued adapter did not import from the installed wheel")

    distribution_files = distribution.files or []
    metadata_licenses = {}
    for basename in ("LICENSE", "NOTICE.md", "COPYRIGHT.md"):
        matches = [
            item for item in distribution_files
            if str(item).endswith(f".dist-info/licenses/{basename}")
        ]
        if len(matches) != 1:
            raise SystemExit(
                f"wheel metadata does not contain exactly one {basename} license file"
            )
        path = Path(distribution.locate_file(matches[0]))
        if not path.is_file() or path.stat().st_size == 0:
            raise SystemExit(f"wheel metadata license file is missing or empty: {path}")
        metadata_licenses[basename] = path.read_text(encoding="utf-8")
    if "GNU AFFERO GENERAL PUBLIC LICENSE" not in metadata_licenses["LICENSE"]:
        raise SystemExit("wheel LICENSE is not the full AGPL text")
    if "Slicer profile snapshots" not in metadata_licenses["NOTICE.md"]:
        raise SystemExit("wheel NOTICE.md is missing the upstream profile notices")
    if "Fat Cat" not in metadata_licenses["COPYRIGHT.md"]:
        raise SystemExit("wheel COPYRIGHT.md is missing Fat Cat copyright information")

    data_root = module_root / "fatcat_metadata_data"
    relative_manifest = Path("translations/supported-targets.json")
    manifest = json.loads((data_root / relative_manifest).read_text(encoding="utf-8"))
    source_manifest = source_root / "compatibility/current-src" / relative_manifest
    if manifest != json.loads(source_manifest.read_text(encoding="utf-8")):
        raise SystemExit("installed support manifest differs from the source checkout")
    expected_targets = {
        target["slicer_id"]: target["application_version"]
        for target in manifest["targets"]
    }
    target_root = data_root / "translations" / "targets"
    target_paths = sorted(target_root.glob("*.json"))
    if {path.name for path in target_paths} != {target["filename"] for target in manifest["targets"]}:
        raise SystemExit("installed target filenames differ from the support manifest")
    actual_targets = {}
    targets_by_slicer = {}
    for target_path in target_paths:
        target = json.loads(target_path.read_text(encoding="utf-8"))
        contract = target["target_contract"]
        actual_targets[contract["slicer_id"]] = contract["application_version"]
        targets_by_slicer[contract["slicer_id"]] = target
    if actual_targets != expected_targets:
        raise SystemExit(f"installed targets differ from the support manifest: {actual_targets}")
    if not (data_root / "translations" / "canonical.json").is_file():
        raise SystemExit("canonical machine translation data is missing from the wheel")

    source_root_data = data_root / "native_project_sources"
    source_index_path = source_root_data / "source-index.json"
    source_index = json.loads(source_index_path.read_text(encoding="utf-8"))
    actual_slicers = {row["slicer_id"] for row in source_index["sources"]}
    for slicer, record in source_index.get("format_catalogs", {}).items():
        catalog = json.loads((source_root_data / record["catalog"]).read_text(encoding="utf-8"))
        schema_bytes = (source_root_data / record["schema"]).read_bytes()
        target = targets_by_slicer[slicer]
        if (catalog["slicer_id"] != slicer or catalog["application_version"] != expected_targets[slicer]
                or record["application_version"] != expected_targets[slicer]
                or record["project_format"] != target["project_format"] or not catalog["sources"]):
            raise SystemExit("installed format catalogue identity differs from target")
        if hashlib.sha256(schema_bytes).hexdigest() != target["process_settings_contract"]["source"]["schema_sha256"]:
            raise SystemExit("installed native schema differs from its override contract")
        actual_slicers.add(slicer)
    if actual_slicers != set(expected_targets):
        raise SystemExit(f"native source index does not cover all targets: {actual_slicers}")
    missing_profiles = [
        name for name in source_index["profile_files"]
        if not (source_root_data / name).is_file()
    ]
    if missing_profiles:
        raise SystemExit(f"native source index refers to missing profile files: {missing_profiles[:5]}")
    for row in source_index['sources']:
        record = row.get('compatibility_project')
        if record is None:
            continue
        path = source_root_data / record['path']
        if hashlib.sha256(path.read_bytes()).hexdigest() != record['source_sha256']:
            raise SystemExit('installed compatibility source content hash mismatch')
        payload = json.loads(path.read_text(encoding='utf-8'))
        for key in ('machine_uid', 'nozzle_uid'):
            if payload['source'][key] != row[key]:
                raise SystemExit('installed compatibility source hardware identity mismatch')
        for key in ('source_slicer_id', 'source_application_version', 'source_profile_name'):
            if payload['source'][key] != record[key]:
                raise SystemExit('installed compatibility source provenance mismatch')
        if not (data_root / record['license_file']).is_file():
            raise SystemExit('compatibility source license is missing')

    manifest_path = data_root / "licenses" / "third-party" / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for record in manifest["files"]:
        license_path = data_root / "licenses" / "third-party" / record["license_file"]
        contents = license_path.read_bytes()
        if len(contents) != record["license_bytes"]:
            raise SystemExit(f"packaged third-party license size mismatch: {license_path}")
        digest = hashlib.sha256(contents).hexdigest()
        if digest != record["license_sha256"]:
            raise SystemExit(f"packaged third-party license checksum mismatch: {license_path}")

    print(f"module_file={module_path}")
    print(f"working_directory={Path.cwd().resolve()}")
    print(f"target_count={len(actual_targets)} native_profile_count={len(source_index['profile_files'])}")
    print(f"compatibility_project_count={len(source_index.get('compatibility_project_files', []))}")
    print(f"third_party_license_count={len(manifest['files'])}")
    print("distribution_license_files=LICENSE,NOTICE.md,COPYRIGHT.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
