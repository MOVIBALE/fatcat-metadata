#!/usr/bin/env python3
"""Refresh the SDK's supported process subset from native FFF definitions.

Supply a reviewed upstream PrintConfig.cpp, its revision URL and, when available,
the exact application's --export-settings output. This is a maintainer import
tool, not a general C++ parser. It refuses definitions it cannot resolve.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re


ROOT = Path(__file__).resolve().parents[1]
TARGETS = ROOT / "compatibility/current-src/translations"
FIELDS = """
layer_height initial_layer_print_height line_width initial_layer_line_width
inner_wall_line_width outer_wall_line_width top_surface_line_width
sparse_infill_line_width internal_solid_infill_line_width support_line_width
wall_loops top_shell_layers bottom_shell_layers top_surface_pattern
bottom_surface_pattern elefant_foot_compensation sparse_infill_density
sparse_infill_pattern travel_speed outer_wall_speed inner_wall_speed
sparse_infill_speed internal_solid_infill_speed top_surface_speed
initial_layer_speed initial_layer_infill_speed support_speed support_interface_speed
enable_support single_extruder_multi_material precise_outer_wall brim_type
brim_width wall_generator skirt_loops skirt_distance skirt_height draft_shield
""".split()
TYPES = {"coFloat": "float", "coFloats": "float", "coFloatOrPercent": "float_or_percent",
         "coPercent": "percent", "coInt": "int", "coBool": "bool", "coEnum": "enum"}


def extract_fields(source: str) -> dict:
    # Keep quoted text intact while removing commented-out native choices.
    source = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/',
                    lambda m: m.group() if m.group().startswith('"') else "", source,
                    flags=re.DOTALL)
    source = source[source.index("void PrintConfigDef::init_common_params()"):
                    source.index("void PrintConfigDef::init_sla_params()")]
    matches = list(re.finditer(r'def\s*=\s*this->add\("([^"]+)",\s*(co\w+)\)', source))
    fields, references = {}, {}
    for index, match in enumerate(matches):
        key, native_type = match.groups()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(source)
        block = source[match.end():end]
        values = re.findall(r'def->enum_values\.(?:push_back|emplace_back)\("([^"]+)"\)', block)
        inherited = re.search(r'def->enum_values\s*=\s*(\w+)->enum_values', block)
        if inherited and native_type == "coEnum":
            values = references[inherited[1]]["values"]
        field = {"type": TYPES.get(native_type, native_type)}
        if native_type == "coEnum":
            field["values"] = values
        if native_type == "coFloats":
            field["array"] = True
        for native, name in [("min", "minimum"), ("max", "maximum")]:
            bound = re.search(rf'def->{native}\s*=\s*(-?\d+(?:\.\d+)?)\s*;', block)
            if bound:
                field[name] = float(bound[1])
        if native_type == "coInt":
            field.setdefault("maximum", 2147483647)
        alias = re.search(r'auto\s+(\w+)\s*=\s*$', source[max(0, match.start() - 100):match.start()])
        if alias:
            references[alias[1]] = field
        if key in FIELDS:
            if native_type not in TYPES or (native_type == "coEnum" and not values):
                raise ValueError(f"unresolved definition for {key}: {field}")
            fields[key] = field
    missing = set(FIELDS) - fields.keys()
    if missing:
        raise ValueError(f"missing native FFF definitions: {sorted(missing)}")
    for key in ("layer_height", "initial_layer_print_height"):
        fields[key]["exclusive_minimum"] = 0
    return fields


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--slicer", required=True)
    parser.add_argument("--print-config", type=Path, required=True)
    parser.add_argument("--source-url", required=True)
    parser.add_argument("--definition-version", required=True)
    parser.add_argument("--native-defaults", type=Path)
    parser.add_argument("--application-root", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    entries = json.loads((TARGETS / "supported-targets.json").read_text(encoding="utf-8"))["targets"]
    entry = next(row for row in entries if row["slicer_id"] == args.slicer)
    target_path = TARGETS / "targets" / entry["filename"]
    target = json.loads(target_path.read_text(encoding="utf-8"))
    source_bytes = args.print_config.read_bytes()
    fields = extract_fields(source_bytes.decode("utf-8"))
    provenance = {"definition_url": args.source_url,
                  "definition_sha256": hashlib.sha256(source_bytes).hexdigest(),
                  "definition_version": args.definition_version,
                  "exact_application_definitions": args.definition_version == entry["application_version"]}
    if args.native_defaults:
        if args.application_root is None:
            raise ValueError("native defaults require --application-root for exact version verification")
        info = plistlib.loads((args.application_root / "Contents/Info.plist").read_bytes())
        if info.get("CFBundleShortVersionString") != entry["application_version"]:
            raise ValueError("application version does not match the pinned target")
        raw = args.native_defaults.read_bytes()
        defaults = json.loads(raw)
        if set(fields) - defaults.keys():
            raise ValueError("application export is missing declared process fields")
        for key, field in fields.items():
            if isinstance(defaults[key], list):
                field["array"] = True
            else:
                field.pop("array", None)
            if field["type"] == "enum" and defaults[key] not in field["values"]:
                raise ValueError(f"native enum default differs at {key}")
        provenance["native_defaults_sha256"] = hashlib.sha256(raw).hexdigest()
        provenance["native_defaults_application_version"] = entry["application_version"]
        provenance["exported_schema_version"] = defaults.get("version")
    if not provenance["exact_application_definitions"] and not args.native_defaults:
        raise ValueError("older definitions require an exact application defaults export")
    value_aliases = {"brim_type": {"none": "no_brim"}}
    for key in ("sparse_infill_pattern", "top_surface_pattern", "bottom_surface_pattern"):
        values = fields[key]["values"]
        if "zig-zag" in values and "rectilinear" not in values:
            value_aliases[key] = {"rectilinear": "zig-zag"}
        elif "rectilinear" in values and "zig-zag" not in values:
            value_aliases[key] = {"zig-zag": "rectilinear"}
    target["process_settings_contract"] = {
        "scope": "SDK-supported explicit FFF overrides; not the application's entire schema",
        "source": provenance, "fields": fields,
        "aliases": {"initial_layer_height": "initial_layer_print_height",
                    "first_layer_height": "initial_layer_print_height",
                    "elephant_foot_compensation": "elefant_foot_compensation"},
        "value_aliases": value_aliases,
        "unsupported_fields": {"print_speed": "No single native overall speed. Use outer_wall_speed, inner_wall_speed, sparse_infill_speed or another specific speed field."}}
    if not args.dry_run:
        target_path.write_text(json.dumps(target, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"{args.slicer} {entry['application_version']}: {len(fields)} process fields")


if __name__ == "__main__":
    main()
