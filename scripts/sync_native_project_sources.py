#!/usr/bin/env python3
"""Import exact-version native machine and preset inheritance sources."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import plistlib
import shutil
import sys
import tempfile
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
SOURCE_MAP_PATH = ROOT / "compatibility/current-src/native-source-map.json"
TARGET_DIR = ROOT / "compatibility/current-src/translations/targets"
OUTPUT_DIR = ROOT / "compatibility/current-src/native-project-sources"
PROFILE_CATEGORIES = ("machine", "process", "filament")
TARGET_FILES = {
    "BambuStudio": "bambu-studio-02.08.02.61.json",
    "OrcaSlicer": "orca-slicer-2.4.2.json",
    "QIDIStudio": "qidi-studio-02.07.02.60.json",
    "ElegooSlicer": "elegoo-slicer-1.5.3.5.json",
    "AnycubicSlicerNext": "anycubic-slicer-next-2.0.0.2.json",
    "FlashStudio": "flash-studio-1.7.15.json",
    "SnapmakerOrca": "snapmaker-orca-2.3.6.json",
}


def _reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def _load_json(path: Path) -> dict[str, Any]:
    value = json.loads(
        path.read_text(encoding="utf-8"),
        object_pairs_hook=_reject_duplicate_pairs,
        parse_constant=lambda token: (_ for _ in ()).throw(
            ValueError(f"non-finite JSON number: {token}")
        ),
    )
    if not isinstance(value, dict):
        raise ValueError(f"JSON document must be an object: {path}")
    return value


def _render_json(value: dict[str, Any]) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2) + "\n"


def _safe_relative(value: str) -> tuple[str, ...]:
    parts = value.split("/")
    if (
        not value
        or value.startswith("/")
        or any(part in {"", ".", ".."} for part in parts)
    ):
        raise ValueError(f"unsafe relative source path: {value}")
    return tuple(parts)


def _installed_version(application: Path) -> str:
    info_path = application / "Contents/Info.plist"
    if not info_path.is_file():
        raise ValueError(f"slicer application Info.plist is missing: {info_path}")
    with info_path.open("rb") as info_file:
        info = plistlib.load(info_file)
    version = info.get("CFBundleShortVersionString")
    if not isinstance(version, str) or not version:
        raise ValueError(f"slicer application has no exact version: {application}")
    return version


def _profile_category(relative_path: str) -> str | None:
    parts = PurePosixPath(relative_path).parts
    for category in PROFILE_CATEGORIES:
        if category in parts:
            return category
    return None


class ProfileCatalog:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.documents: dict[str, dict[str, Any]] = {}
        self.categories: dict[str, dict[str, list[str]]] = {
            category: {"name": {}, "stem": {}} for category in PROFILE_CATEGORIES
        }
        for path in sorted(root.rglob("*.json")):
            relative = path.relative_to(root).as_posix()
            category = _profile_category(relative)
            if category is None:
                continue
            try:
                document = _load_json(path)
            except (OSError, ValueError, json.JSONDecodeError):
                continue
            self.documents[relative] = document
            stem = path.stem
            self.categories[category]["stem"].setdefault(stem, []).append(relative)
            name = document.get("name")
            if isinstance(name, str) and name:
                self.categories[category]["name"].setdefault(name, []).append(relative)

    def find_named(self, category: str, name: str, preferred_scope: str) -> str | None:
        matches = self.categories[category]["name"].get(name, [])
        if not matches:
            matches = self.categories[category]["stem"].get(name, [])
        if not matches:
            return None

        exact_filename = [
            path for path in matches
            if PurePosixPath(path).name == f"{name}.json"
        ]
        if len(exact_filename) == 1:
            return exact_filename[0]
        if exact_filename:
            matches = exact_filename

        same_scope = [path for path in matches if self._scope(path, category) == preferred_scope]
        if len(same_scope) == 1:
            return same_scope[0]
        if same_scope:
            matches = same_scope

        if len(matches) == 1:
            return matches[0]
        signatures = {json.dumps(self.documents[path], sort_keys=True) for path in matches}
        if len(signatures) == 1:
            return sorted(matches)[0]
        return None

    @staticmethod
    def _scope(path: str, category: str) -> str:
        parts = PurePosixPath(path).parts
        try:
            category_index = parts.index(category)
        except ValueError:
            return ""
        return "/".join(parts[:category_index])

    def resolve_reference(self, category: str, path: str, name: str) -> str | None:
        if not isinstance(name, str) or not name:
            return None
        # Slicer include and inheritance references are local preset names.
        if len(_safe_relative(name)) != 1:
            return None
        sibling = (PurePosixPath(path).parent / f"{name}.json").as_posix()
        if sibling in self.documents:
            return sibling
        scope = self._scope(path, category)
        matches = self.categories[category]["stem"].get(name, [])
        if not matches:
            matches = self.categories[category]["name"].get(name, [])
        same_scope = [candidate for candidate in matches if self._scope(candidate, category) == scope]
        if len(same_scope) == 1:
            return same_scope[0]
        if same_scope:
            matches = same_scope
        if len(matches) == 1:
            return matches[0]
        if matches and len({json.dumps(self.documents[item], sort_keys=True) for item in matches}) == 1:
            return sorted(matches)[0]
        return None

    def resolve(self, category: str, path: str, links: dict[str, dict[str, str]],
                ancestry: tuple[str, ...] = ()) -> dict[str, Any]:
        if path in ancestry or len(ancestry) >= 48:
            raise ValueError(f"cyclic or excessively deep native profile chain: {path}")
        document = self.documents.get(path)
        if document is None:
            raise ValueError(f"native profile source file is missing: {path}")

        merged: dict[str, Any] = {}
        inherited = document.get("inherits")
        if inherited is not None and inherited != "":
            if not isinstance(inherited, str):
                raise ValueError(f"native profile inherits must be text: {path}")
            parent = self.resolve_reference(category, path, inherited)
            if parent is None:
                raise ValueError(f"unresolved exact inherits reference {inherited!r} in {path}")
            links.setdefault(path, {})[f"inherits:{inherited}"] = parent
            merged.update(self.resolve(category, parent, links, (*ancestry, path)))

        includes = document.get("include", [])
        if isinstance(includes, str):
            includes = [includes]
        if not isinstance(includes, list) or not all(
            isinstance(value, str) and value for value in includes
        ):
            raise ValueError(f"native profile include must contain preset names: {path}")
        for name in includes:
            included_path = self.resolve_reference(category, path, name)
            if included_path is None:
                raise ValueError(f"unresolved exact include reference {name!r} in {path}")
            links.setdefault(path, {})[f"include:{name}"] = included_path
            merged.update(self.resolve(category, included_path, links, (*ancestry, path)))

        merged.update(document)
        return merged


def _select_machine(catalog: ProfileCatalog, name: str) -> str | None:
    matches = catalog.categories["machine"]["name"].get(name, [])
    if len(matches) == 1:
        return matches[0]
    exact = [path for path in matches if PurePosixPath(path).name == f"{name}.json"]
    return exact[0] if len(exact) == 1 else None


def _names(value: Any) -> list[str]:
    if isinstance(value, str) and value:
        return [value]
    if isinstance(value, list):
        return [item for item in value if isinstance(item, str) and item]
    return []


def _target_profile_options(target: dict[str, Any], row: dict[str, Any]) -> tuple[list[str], list[str]]:
    machine_bindings = [
        item for item in target.get("machine_bindings", [])
        if isinstance(item, dict)
        and item.get("machine_uid") == row["machine_uid"]
        and item.get("nozzle_uid") == row["nozzle_uid"]
        and item.get("source_profile_name") == row["source_machine_profile_name"]
    ]
    process_names = [
        item["default_print_profile"]
        for item in machine_bindings
        if isinstance(item.get("default_print_profile"), str) and item["default_print_profile"]
    ]
    material_names = [
        item["native_profile_id"]
        for item in target.get("material_bindings", [])
        if isinstance(item, dict)
        and item.get("machine_uid") == row["machine_uid"]
        and item.get("nozzle_uid") == row["nozzle_uid"]
        and isinstance(item.get("native_profile_id"), str)
        and item["native_profile_id"]
    ]
    return process_names, material_names


def _profile_option(catalog: ProfileCatalog, category: str, name: str,
                    scope: str, source_paths: set[str], links: dict[str, dict[str, str]],
                    machine_name: str | None = None) -> dict[str, Any]:
    path = catalog.find_named(category, name, scope)
    if path is None:
        return {"name": name, "unavailable_reason": "no unique exact native preset file"}
    try:
        profile = catalog.resolve(category, path, links)
        printers = profile.get('compatible_printers', [])
        if machine_name is not None and printers and machine_name not in printers:
            source_paths.add(path)
            return {'name': name, 'path': path, 'unavailable_reason':
                    f"native filament profile is not compatible with '{machine_name}'"}
    except ValueError as error:
        return {"name": name, "unavailable_reason": str(error)}
    source_paths.add(path)
    return {"name": name, "path": path}


def _load_target(slicer_id: str) -> dict[str, Any]:
    return _load_json(TARGET_DIR / TARGET_FILES[slicer_id])


def _source_entry(catalog: ProfileCatalog, row: dict[str, Any], target: dict[str, Any],
                  source_paths: set[str], links: dict[str, dict[str, str]]) -> dict[str, Any]:
    entry = dict(row)
    entry["application_version"] = target["target_contract"]["application_version"]
    machine_path = _select_machine(catalog, row["source_machine_profile_name"])
    if machine_path is None:
        entry["availability"] = {
            "machine": "unavailable",
            "reason": "no unique exact machine preset in the pinned native source",
        }
        return entry

    try:
        machine = catalog.resolve("machine", machine_path, links)
    except ValueError as error:
        entry["availability"] = {"machine": "unavailable", "reason": str(error)}
        return entry
    source_paths.add(machine_path)
    entry["machine_profile_path"] = machine_path
    machine_process_names = _names(machine.get("default_print_profile"))
    machine_material_names = _names(machine.get("default_filament_profile"))
    target_processes, target_materials = _target_profile_options(target, row)
    process_names = list(dict.fromkeys((*machine_process_names, *target_processes)))
    material_names = list(dict.fromkeys((*machine_material_names, *target_materials)))
    # Some machines declare a default whose explicit compatibility list targets
    # other printers. Include genuine compatible alternatives from that vendor's
    # native files instead of treating the incomplete default list as the catalog.
    scope = ProfileCatalog._scope(machine_path, "machine")
    incompatible_default = False
    for name in machine_material_names:
        path = catalog.find_named('filament', name, scope)
        if path is not None:
            profile = catalog.resolve('filament', path, {})
            printers = profile.get('compatible_printers', [])
            incompatible_default |= bool(printers and row['source_machine_profile_name'] not in printers)
    if incompatible_default:
        for path, document in catalog.documents.items():
            if _profile_category(path) != 'filament' or ProfileCatalog._scope(path, 'filament') != scope:
                continue
            try:
                profile = catalog.resolve('filament', path, {})
            except ValueError:
                continue
            if str(profile.get('instantiation', '')).lower() != 'true':
                continue
            if row['source_machine_profile_name'] in profile.get('compatible_printers', []):
                name = document.get('name')
                if isinstance(name, str) and name not in material_names:
                    material_names.append(name)

    process_options = [
        _profile_option(catalog, "process", name, ProfileCatalog._scope(machine_path, "machine"),
                        source_paths, links)
        for name in dict.fromkeys(process_names)
    ]
    material_options = [
        _profile_option(catalog, "filament", name, ProfileCatalog._scope(machine_path, "machine"),
                        source_paths, links, row['source_machine_profile_name'])
        for name in dict.fromkeys(material_names)
    ]
    entry["print_profile_options"] = process_options
    entry["filament_profile_options"] = material_options
    available_processes = [option["name"] for option in process_options if "path" in option]
    available_materials = [option["name"] for option in material_options
                           if "path" in option and 'unavailable_reason' not in option]
    default_process = (
        machine_process_names[0]
        if len(machine_process_names) == 1
        else target_processes[0]
        if not machine_process_names and len(target_processes) == 1
        else None
    )
    default_material = (
        machine_material_names[0]
        if len(machine_material_names) == 1
        else None
    )
    if machine_material_names:
        # Preserve the native order: slicers use these names as preferred
        # defaults by material slot, with the first name as the fallback.
        entry["default_filament_profile_names"] = machine_material_names
    if default_process in available_processes:
        entry["default_print_profile_name"] = default_process
    if default_material in available_materials:
        entry["default_filament_profile_name"] = default_material
    missing = []
    if not available_processes:
        reason = (
            "native machine preset does not declare one and the exact target source has no matching preset"
            if not process_options else process_options[0].get("unavailable_reason", "native process preset is unavailable")
        )
        missing.append({"role": "process", "reason": reason})
    if not available_materials:
        reason = (
            "native machine preset does not declare one and the exact target source has no matching preset"
            if not material_options else material_options[0].get("unavailable_reason", "native material preset is unavailable")
        )
        missing.append({"role": "material", "reason": reason})
    entry["availability"] = {
        "machine": "available",
        "process": "available" if available_processes else "unavailable",
        "material": "available" if available_materials else "unavailable",
        "print_profile_selection_required": "default_print_profile_name" not in entry,
        "filament_profile_selection_required": (
            not machine_material_names
            or any(name not in available_materials for name in machine_material_names)
        ),
        "missing": missing,
    }
    return entry


def _application_roots(values: list[str]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for value in values:
        slicer, separator, path = value.partition("=")
        if not separator or not slicer or not path:
            raise ValueError("--application-root must be SLICER_ID=APP_BUNDLE_PATH")
        if slicer in result:
            raise ValueError(f"duplicate application root for {slicer}")
        result[slicer] = Path(path).expanduser().resolve()
    return result


def _copy_profile(app_profiles: Path, output_root: Path, slicer: str,
                  relative_path: str) -> str:
    parts = _safe_relative(relative_path)
    source = app_profiles.joinpath(*parts)
    resolved_root = app_profiles.resolve(strict=True)
    resolved_source = source.resolve(strict=True)
    if not resolved_source.is_file() or not resolved_source.is_relative_to(resolved_root):
        raise ValueError(f"native profile source must be a regular file below its profile root: {source}")
    relative = PurePosixPath("profiles", slicer, *parts).as_posix()
    _safe_relative(relative)
    destination = output_root.joinpath(*PurePosixPath(relative).parts)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(resolved_source, destination)
    return relative


def _format_availability(entry: dict[str, Any]) -> str | None:
    availability = entry.get("availability", {})
    if availability.get("machine") != "available":
        return availability.get("reason", "machine preset unavailable")
    unavailable = [
        f"{item.get('role')}: {item.get('reason')}"
        for item in availability.get("missing", [])
    ]
    return "; ".join(unavailable) if unavailable else None


def _report_exact_unavailable(rows: list[dict[str, Any]], slicer: str,
                               reason: str) -> None:
    for row in rows:
        if row.get("slicer_id") != slicer:
            continue
        print(
            f"unavailable exact source: {slicer} {row['machine_uid']} "
            f"{row['nozzle_uid']} ({row['source_machine_profile_name']}): {reason}",
            file=sys.stderr,
        )


def _write_tree(stage: Path, rows: list[dict[str, Any]], roots: dict[str, Path],
                targets: dict[str, dict[str, Any]]) -> list[dict[str, Any]]:
    profiles_output = stage / "profiles"
    profiles_output.mkdir(parents=True)
    references: dict[str, dict[str, str]] = {}
    copied_entries: list[dict[str, Any]] = []
    reports: list[dict[str, Any]] = []

    for slicer_id, application_root in roots.items():
        expected = targets[slicer_id]["target_contract"]["application_version"]
        actual = _installed_version(application_root)
        if actual != expected:
            raise ValueError(
                f"{slicer_id}: installed application version {actual} does not match pinned target {expected}"
            )
        app_profiles = application_root / "Contents/Resources/profiles"
        if not app_profiles.is_dir():
            raise ValueError(f"native profiles root is missing: {app_profiles}")

        catalog = ProfileCatalog(app_profiles)
        target = targets[slicer_id]
        source_paths: set[str] = set()
        links: dict[str, dict[str, str]] = {}
        slicer_rows = [row for row in rows if row["slicer_id"] == slicer_id]
        slice_entries = [
            _source_entry(catalog, row, target, source_paths, links)
            for row in slicer_rows
        ]
        for row, entry in zip(slicer_rows, slice_entries):
            if "compatibility_project" in row:
                record = row["compatibility_project"]
                relative = Path(*_safe_relative(record["path"]))
                original = OUTPUT_DIR / relative
                payload = _load_json(original)
                for key in ("machine_uid", "nozzle_uid"):
                    if payload["source"][key] != row[key]:
                        raise ValueError("compatibility source hardware identity mismatch")
                for key in ("source_slicer_id", "source_application_version", "source_profile_name"):
                    if payload["source"][key] != record[key]:
                        raise ValueError("compatibility source process provenance mismatch")
                if hashlib.sha256(original.read_bytes()).hexdigest() != record["source_sha256"]:
                    raise ValueError("compatibility source content hash mismatch")
                license_path = ROOT.joinpath(*_safe_relative(record["license_file"]))
                if not license_path.is_file() or not license_path.resolve().is_relative_to(ROOT.resolve()):
                    raise ValueError("compatibility source license is missing or outside the repository")
                entry["compatibility_project"] = record
                destination = stage / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(original, destination)

        # Include every exact parent/include file in the closure. Keeping only
        # selected roots would leave dangling source-index edges at runtime.
        source_paths.update(links)
        for edges in links.values():
            source_paths.update(edges.values())

        for path in sorted(source_paths):
            _copy_profile(app_profiles, stage, slicer_id, path)
        for from_path, edges in links.items():
            output_from = PurePosixPath("profiles", slicer_id, from_path).as_posix()
            references[output_from] = {
                relation: PurePosixPath("profiles", slicer_id, to_path).as_posix()
                for relation, to_path in sorted(edges.items())
            }

        for entry in slice_entries:
            if "machine_profile_path" in entry:
                entry["machine_profile_path"] = PurePosixPath(
                    "profiles", slicer_id, entry["machine_profile_path"]
                ).as_posix()
            for key in ("print_profile_options", "filament_profile_options"):
                for option in entry.get(key, []):
                    if "path" in option:
                        option["path"] = PurePosixPath(
                            "profiles", slicer_id, option["path"]
                        ).as_posix()
            copied_entries.append(entry)
            reason = _format_availability(entry)
            if reason:
                reports.append({
                    "slicer_id": slicer_id,
                    "application_version": expected,
                    "machine_uid": entry["machine_uid"],
                    "nozzle_uid": entry["nozzle_uid"],
                    "source_machine_profile_name": entry["source_machine_profile_name"],
                    "reason": reason,
                })

    copied_files = sorted(
        path.relative_to(stage).as_posix()
        for path in profiles_output.rglob("*.json")
    )
    index = {
        "schema_version": 1,
        "sources": sorted(
            copied_entries,
            key=lambda entry: (
                entry["slicer_id"], entry["machine_uid"], entry["nozzle_uid"]
            ),
        ),
        "profile_files": copied_files,
        "compatibility_project_files": sorted({
            entry["compatibility_project"]["path"] for entry in copied_entries
            if "compatibility_project" in entry
        }),
        "references": references,
    }
    (stage / "source-index.json").write_text(_render_json(index), encoding="utf-8")
    return reports


def sync(application_roots: dict[str, Path], *, dry_run: bool = False) -> int:
    source_map = _load_json(SOURCE_MAP_PATH)
    rows = source_map.get("machine_profiles")
    if source_map.get("schema_version") != 1 or not isinstance(rows, list):
        raise ValueError("native source map has an unsupported schema")
    required = sorted({row.get("slicer_id") for row in rows if isinstance(row, dict)})
    missing_roots = sorted(set(required) - application_roots.keys())
    unexpected_roots = sorted(application_roots.keys() - set(required))
    if missing_roots or unexpected_roots:
        details = []
        for slicer in missing_roots:
            count = sum(row.get("slicer_id") == slicer for row in rows if isinstance(row, dict))
            details.append(f"{slicer}: no app root for {count} exact machine/nozzle combinations")
            _report_exact_unavailable(rows, slicer, "no exact-version application root supplied")
        if unexpected_roots:
            details.append(f"unexpected app roots: {', '.join(unexpected_roots)}")
        raise ValueError("; ".join(details))

    targets = {slicer: _load_target(slicer) for slicer in required}
    for slicer in required:
        expected = targets[slicer]["target_contract"]["application_version"]
        actual = _installed_version(application_roots[slicer])
        if actual != expected:
            _report_exact_unavailable(
                rows,
                slicer,
                f"installed application version {actual} does not match pinned {expected}",
            )
            raise ValueError(
                f"{slicer}: application version {actual} != pinned {expected}; "
                "the generated native source index was left unchanged"
            )

    if not dry_run:
        OUTPUT_DIR.parent.mkdir(parents=True, exist_ok=True)
    stage_parent = None if dry_run else OUTPUT_DIR.parent
    with tempfile.TemporaryDirectory(prefix=".native-project-sources-", dir=stage_parent) as temp_dir:
        stage = Path(temp_dir) / "package"
        stage.mkdir()
        reports = _write_tree(stage, rows, application_roots, targets)
        staged_index = _load_json(stage / "source-index.json")
        profile_count = len(staged_index["profile_files"])
        if dry_run:
            print(f"Would import {len(rows)} identities and {profile_count} native profile files.")
            print(f"Unavailable exact combinations: {len(reports)}; repository files unchanged.")
            for item in reports:
                print(f"- {item['slicer_id']} {item['machine_uid']} {item['nozzle_uid']}: {item['reason']}")
            return 0
        old_files: set[str] = set()
        old_index = OUTPUT_DIR / "source-index.json"
        if old_index.is_file():
            try:
                old_files = set(_load_json(old_index).get("profile_files", []))
            except (OSError, ValueError, json.JSONDecodeError):
                old_files = set()

        # Reconcile only files named by our previous generated index.
        new_files = set(staged_index["profile_files"])
        for path in sorted(old_files):
            if path not in new_files:
                previous = OUTPUT_DIR / path
                if previous.is_file() and previous.resolve().is_relative_to(OUTPUT_DIR.resolve()):
                    previous.unlink()
        for path in (stage / "profiles").rglob("*.json"):
            relative = path.relative_to(stage).as_posix()
            destination = OUTPUT_DIR / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, destination)
        for path in (stage / "compatibility-projects").rglob("*.json"):
            destination = OUTPUT_DIR / path.relative_to(stage)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, destination)
        shutil.copyfile(stage / "source-index.json", OUTPUT_DIR / "source-index.json")

    print(f"Imported {len(rows)} exact machine/nozzle identities.")
    print(f"Packaged {profile_count} shared native profile files.")
    if reports:
        print(f"Unavailable exact combinations: {len(reports)}")
        for item in reports:
            print(
                f"- {item['slicer_id']} {item['application_version']} "
                f"{item['machine_uid']} {item['nozzle_uid']}: {item['reason']}"
            )
    else:
        print("All exact source combinations have machine, process, and material data.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true",
                        help="validate and preview in a temporary directory without changing repository files")
    parser.add_argument(
        "--application-root",
        action="append",
        default=[],
        metavar="SLICER_ID=APP_BUNDLE_PATH",
        help="installed slicer .app root; repeat once for each pinned target",
    )
    args = parser.parse_args()
    try:
        return sync(_application_roots(args.application_root), dry_run=args.dry_run)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"native source sync failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
