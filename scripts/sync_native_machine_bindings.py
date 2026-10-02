#!/usr/bin/env python3
"""Rebuild canonical identities and exact target machine/nozzle bindings."""

from __future__ import annotations

from collections import defaultdict
from decimal import Decimal, InvalidOperation
import json
from pathlib import Path
import re
import sys
from typing import Any

from sync_native_project_sources import ProfileCatalog


ROOT = Path(__file__).resolve().parents[1]
SOURCE_MAP = ROOT / "compatibility/current-src/native-source-map.json"
SOURCE_INDEX = ROOT / "compatibility/current-src/native-project-sources/source-index.json"
SOURCES_DIR = SOURCE_INDEX.parent
TRANSLATIONS = ROOT / "compatibility/current-src/translations"
CANONICAL = TRANSLATIONS / "canonical.json"
TARGET_DIR = TRANSLATIONS / "targets"
TARGET_FILES = {
    "AnycubicSlicerNext": "anycubic-slicer-next-2.0.0.2.json",
    "BambuStudio": "bambu-studio-02.08.02.61.json",
    "ElegooSlicer": "elegoo-slicer-1.5.3.5.json",
    "FlashStudio": "flash-studio-1.7.15.json",
    "OrcaSlicer": "orca-slicer-2.4.2.json",
    "QIDIStudio": "qidi-studio-02.07.02.60.json",
    "SnapmakerOrca": "snapmaker-orca-2.3.6.json",
}
MANUFACTURERS = {
    "anycubic": "Anycubic",
    "bambu-lab": "Bambu Lab",
    "elegoo": "Elegoo",
    "flashforge": "Flashforge",
    "qidi": "QIDI",
    "snapmaker": "Snapmaker",
}


def load(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected JSON object: {path}")
    return value


def write(path: Path, value: dict[str, Any]) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def source_key(row: dict[str, Any]) -> tuple[str, str, str]:
    return row["slicer_id"], row["machine_uid"], row["nozzle_uid"]


def process_names(source: dict[str, Any]) -> set[str]:
    return {
        option["name"] for option in source.get("print_profile_options", [])
        if isinstance(option, dict) and isinstance(option.get("name"), str)
        and isinstance(option.get("path"), str)
    }


def diameter(nozzle_uid: str) -> str:
    match = re.fullmatch(r"nozzle:(\d+(?:\.\d+)?)mm", nozzle_uid)
    if match is None:
        raise ValueError(f"unsupported nozzle identity: {nozzle_uid}")
    return format(Decimal(match.group(1)).normalize(), "f")


def extent(machine: dict[str, Any]) -> tuple[str, str, str]:
    area = machine.get("printable_area")
    height = machine.get("printable_height")
    if not isinstance(area, list) or len(area) < 3 or not isinstance(height, str):
        raise ValueError("native machine profile has no usable printable area or height")
    points = []
    for point in area:
        if not isinstance(point, str) or "x" not in point:
            raise ValueError(f"invalid native printable-area point: {point!r}")
        try:
            x, y = point.strip().split("x", 1)
            points.append((Decimal(x.strip()), Decimal(y.strip())))
        except (InvalidOperation, ValueError) as error:
            raise ValueError(f"invalid native printable-area point: {point!r}") from error
    try:
        z = Decimal(height)
    except InvalidOperation as error:
        raise ValueError(f"invalid native printable height: {height!r}") from error
    fmt = lambda value: format(value.normalize(), "f")
    return (
        fmt(max(x for x, _ in points) - min(x for x, _ in points)),
        fmt(max(y for _, y in points) - min(y for _, y in points)),
        fmt(z),
    )


def build_binding(
    row: dict[str, Any], source: dict[str, Any], machine: dict[str, Any], target: dict[str, Any],
) -> dict[str, Any]:
    native_nozzles = machine.get("nozzle_diameter")
    selected_diameter = diameter(row["nozzle_uid"])
    if not isinstance(native_nozzles, list) or not native_nozzles or any(
        str(value).strip() != selected_diameter for value in native_nozzles
    ):
        raise ValueError(f"physical nozzle array conflicts with {source_key(row)}")
    model = machine.get("printer_model")
    area = machine.get("printable_area")
    height = machine.get("printable_height")
    if not isinstance(model, str) or not isinstance(area, list) or not isinstance(height, str):
        raise ValueError(f"native hardware profile is incomplete: {source_key(row)}")
    binding = {
        "machine_uid": row["machine_uid"],
        "nozzle_uid": row["nozzle_uid"],
        "source_profile_name": row["source_machine_profile_name"],
        "printer_model": model,
        "nozzle_diameter": native_nozzles,
        "printable_area": area,
        "printable_height": height,
    }
    if isinstance(machine.get("printer_variant"), str):
        binding["printer_variant"] = machine["printer_variant"]
    plates = row.get("supported_build_plate_uids")
    if isinstance(plates, list):
        target_plates = {
            item.get("build_plate_uid") for item in target.get("build_plate_bindings", [])
            if isinstance(item, dict)
        }
        binding["supported_build_plate_uids"] = sorted(set(plates) & target_plates)
    default = source.get("default_print_profile_name")
    if isinstance(default, str) and default in process_names(source):
        binding["default_print_profile"] = default
    return binding


def sync() -> int:
    source_map = load(SOURCE_MAP)
    map_rows = source_map.get("machine_profiles")
    source_index = load(SOURCE_INDEX)
    source_rows = source_index.get("sources")
    if source_map.get("schema_version") != 1 or not isinstance(map_rows, list):
        raise ValueError("native source map has an unsupported schema")
    if source_index.get("schema_version") != 1 or not isinstance(source_rows, list):
        raise ValueError("native source index has an unsupported schema")
    targets = {slicer: load(TARGET_DIR / name) for slicer, name in TARGET_FILES.items()}
    sources = {source_key(row): row for row in source_rows}
    if len(sources) != len(source_rows):
        raise ValueError("native source index contains duplicate machine/nozzle identities")
    catalog = ProfileCatalog(SOURCES_DIR)

    machine_rows: dict[str, list[dict[str, Any]]] = defaultdict(list)
    unavailable: list[dict[str, Any]] = []
    added_bindings = 0
    for row in map_rows:
        key = source_key(row)
        source = sources.get(key)
        if source is None:
            raise ValueError(f"native source index is missing {key}")
        target = targets[key[0]]
        version = target["target_contract"]["application_version"]
        if source.get("application_version") != version:
            raise ValueError(f"source version does not match pinned target for {key}: {version}")
        availability = source.get("availability", {})
        if availability.get("machine") != "available":
            unavailable.append({**dict(zip(("slicer_id", "machine_uid", "nozzle_uid"), key)),
                                "application_version": version,
                                "source_machine_profile_name": row["source_machine_profile_name"],
                                "reason": availability.get("reason", "exact machine source unavailable")})
            continue
        machine_path = source.get("machine_profile_path")
        machine = catalog.resolve("machine", machine_path, {})
        binding = build_binding(row, source, machine, target)
        bindings = target.setdefault("machine_bindings", [])
        matched = [item for item in bindings if isinstance(item, dict)
                   and item.get("machine_uid") == key[1] and item.get("nozzle_uid") == key[2]]
        if len(matched) > 1:
            raise ValueError(f"duplicate target machine binding: {key}")
        if matched:
            if matched[0].get("source_profile_name") != row["source_machine_profile_name"]:
                raise ValueError(f"target source name conflicts with exact source map: {key}")
            matched[0].update(binding)
            if "default_print_profile" not in binding:
                matched[0].pop("default_print_profile", None)
        else:
            bindings.append(binding)
            added_bindings += 1
        machine_rows[key[1]].append(row)
        missing_roles = [
            f"{role}: " + "; ".join(item.get("reason", "unavailable")
                                     for item in availability.get("missing", [])
                                     if isinstance(item, dict) and item.get("role") == role)
            for role in ("process", "material") if availability.get(role) != "available"
        ]
        if row.get("supported_build_plate_uids") == []:
            missing_roles.append("build plate: source map has no machine-specific plate selection")
        elif isinstance(row.get("supported_build_plate_uids"), list) and not binding.get("supported_build_plate_uids"):
            missing_roles.append("build plate: target has no binding for source-supported plates")
        if missing_roles:
            unavailable.append({"slicer_id": key[0], "application_version": version,
                                "machine_uid": key[1], "nozzle_uid": key[2],
                                "source_machine_profile_name": row["source_machine_profile_name"],
                                "reason": "; ".join(missing_roles)})

    canonical = load(CANONICAL)
    canonical_rows = {item["machine_uid"]: item for item in canonical["machines"]}
    for machine_uid, rows in machine_rows.items():
        source = sources[source_key(rows[0])]
        machine = catalog.resolve("machine", source["machine_profile_path"], {})
        volume = extent(machine)
        physical_count = len(machine["nozzle_diameter"])
        if any(len(catalog.resolve("machine", sources[source_key(row)]["machine_profile_path"], {})
                   .get("nozzle_diameter", [])) != physical_count for row in rows):
            raise ValueError(f"physical tool count differs by nozzle profile: {machine_uid}")
        nozzle_uids = sorted({row["nozzle_uid"] for row in rows}, key=lambda uid: Decimal(diameter(uid)))
        plate_sets = [set(row["supported_build_plate_uids"]) for row in rows
                      if isinstance(row.get("supported_build_plate_uids"), list)]
        if plate_sets:
            supported_plates = sorted(set.union(*plate_sets))
        else:
            supported_plates = sorted({
                plate["build_plate_uid"] for row in rows
                for plate in targets[row["slicer_id"]].get("build_plate_bindings", [])
                if isinstance(plate, dict) and isinstance(plate.get("build_plate_uid"), str)
            })
        if machine_uid in canonical_rows:
            current = canonical_rows[machine_uid]
            current["build_volume_mm"] = {"x": volume[0], "y": volume[1], "z": volume[2]}
            current["physical_tool_count"] = physical_count
            current["supported_nozzle_uids"] = sorted(
                set(current.get("supported_nozzle_uids", [])) | set(nozzle_uids),
                key=lambda uid: Decimal(diameter(uid)),
            )
            if plate_sets:
                current["supported_build_plate_uids"] = supported_plates
        else:
            display_name = rows[0]["machine_display_name"]
            manufacturer = MANUFACTURERS[machine_uid.split(":", 1)[0]]
            prefix = manufacturer + " "
            canonical_rows[machine_uid] = {
                "build_volume_mm": {"x": volume[0], "y": volume[1], "z": volume[2]},
                "display_name": display_name,
                "machine_uid": machine_uid,
                "manufacturer": manufacturer,
                "model": display_name[len(prefix):] if display_name.startswith(prefix) else display_name,
                "physical_tool_count": physical_count,
                "supported_build_plate_uids": supported_plates,
                "supported_nozzle_uids": nozzle_uids,
            }
    canonical["machines"] = list(canonical_rows.values())

    nozzles = list(canonical.get("nozzles", []))
    known_nozzles = {item["nozzle_uid"] for item in nozzles}
    for uid in sorted({row["nozzle_uid"] for row in map_rows}, key=lambda item: Decimal(diameter(item))):
        if uid not in known_nozzles:
            nozzles.append({"diameter_mm": diameter(uid), "nozzle_uid": uid})
    canonical["nozzles"] = nozzles

    write(CANONICAL, canonical)
    for slicer, target in targets.items():
        write(TARGET_DIR / TARGET_FILES[slicer], target)
    write(SOURCE_INDEX, source_index)
    write(SOURCES_DIR / "unavailable-sources.json", {"schema_version": 1, "sources": unavailable})
    print(f"Reconciled {len(map_rows)} exact identities; added {added_bindings} target bindings.")
    print(f"Canonical coverage: {len(canonical['machines'])} machines, {len(canonical['nozzles'])} nozzle diameters.")
    if unavailable:
        print(f"Combinations needing the existing Lumina path: {len(unavailable)}")
        for row in unavailable:
            print(f"- {row['slicer_id']} {row['application_version']} {row['machine_uid']} "
                  f"{row['nozzle_uid']}: {row['reason']}")
    else:
        print("All exact machine, process, and material source roles are available.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(sync())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"native machine binding sync failed: {error}", file=sys.stderr)
        raise SystemExit(2)
