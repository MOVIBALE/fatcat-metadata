#!/usr/bin/env python3
"""Collect PrusaSlicer 3 configurations through its native CLI, without meshes.

通过原生配置接口收集 PrusaSlicer 3 配置，不生成模型或工程包。
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import uuid


VERSION = "3.0.0-alpha12"
ROOT = Path(__file__).resolve().parents[1]
SCALAR_TYPES = {"Bool", "Int", "Float", "String", "Enum", "FloatOrPercentage", "Percentage", "OptInt"}


def refresh_contract(schema: dict) -> None:
    """Keep the installed override definitions in sync with native schema.

    按原生 schema 同步公开覆盖字段；名称等价映射单独人工审核。
    """
    path = ROOT / f"compatibility/current-src/translations/targets/prusa-slicer-{VERSION}.json"
    target = json.loads(path.read_text(encoding="utf-8"))
    if target["target_contract"] != {"slicer_id": "PrusaSlicer", "application_version": VERSION}:
        raise ValueError("Prusa target identity differs from native source version")
    for group, key in (("print", "process_settings_contract"), ("filament", "material_settings_contract")):
        target[key]["fields"] = {
            item["name"]: {k: v for k, v in item.items() if k not in {"name", "value", "location"}}
            for item in schema[group]["items"] if item["type"] in SCALAR_TYPES
        }
    target["process_settings_contract"]["source"]["schema_sha256"] = hashlib.sha256(
        json.dumps(schema, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n"
    ).hexdigest()
    # One line per generated definition keeps the reviewed aliases readable
    # without expanding a schema refresh into thousands of diff lines.
    text = json.dumps(target, ensure_ascii=False, indent=2)
    for key in ("process_settings_contract", "material_settings_contract"):
        fields = target[key]["fields"]
        block = json.dumps(fields, ensure_ascii=False, indent=2)
        indented = "\n".join("    " + line for line in block.splitlines())
        compact = "    {\n" + ",\n".join("      " + json.dumps(name) + ": " +
            json.dumps(definition, ensure_ascii=False, separators=(",", ":"))
            for name, definition in fields.items()) + "\n    }"
        text = text.replace(indented[4:], compact[4:], 1)
    path.write_text(text + "\n", encoding="utf-8")


def json_patch(before: object, after: object, path: str = "") -> list[dict]:
    """Encode exact differences, preserving nulls and deleting obsolete keys."""
    if before == after:
        return []
    if not isinstance(before, dict) or not isinstance(after, dict):
        return [{"op": "replace", "path": path, "value": after}]
    result = []
    for key in sorted(before.keys() - after.keys()):
        token = key.replace("~", "~0").replace("/", "~1")
        result.append({"op": "remove", "path": path + "/" + token})
    for key, value in sorted(after.items()):
        token = key.replace("~", "~0").replace("/", "~1")
        child = path + "/" + token
        if key not in before:
            result.append({"op": "add", "path": child, "value": value})
        else:
            result.extend(json_patch(before[key], value, child))
    return result


def machine_uid(hardware: dict) -> str:
    uid = "prusa:" + re.sub(r"[^a-z0-9]+", "-", hardware["model"].lower()).strip("-")
    count = hardware["tool_count"]
    if count > 1:
        uid += f"-{count}t"
    feeder = hardware["tools"]["0"].get("feeder")
    if feeder:
        uid += "-" + feeder["model"].lower().replace("_", "-")
    return uid


def collect(binary: Path, output: Path) -> None:
    """Export the app's offered default-nozzle FFF configurations.

    导出应用列出的默认喷嘴 FFF 配置；不宣称覆盖未列出的喷嘴组合。
    """
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fatcat-prusa-sources-") as temporary:
        temp = Path(temporary)
        serial = 0

        def query(arguments: list[str]) -> dict:
            nonlocal serial
            serial += 1
            file = temp / f"export-{serial}.json"
            completed = subprocess.run(
                [str(binary), "--datadir", str(temp / "settings"), *arguments, str(file)],
                capture_output=True, text=True, encoding="utf-8", timeout=90,
            )
            if completed.returncode != 0 or not file.is_file():
                raise RuntimeError(completed.stdout + completed.stderr)
            return json.loads(file.read_text(encoding="utf-8"))

        schema = query(["--export-config-schema"])
        models = query(["--query-printer-models", "--output"])
        snapshots = []
        sources = []
        for model in models["printer_models"]:
            if model["technology"] != "FFF":
                continue
            for variant in model["variants"]:
                selection = variant["name"]
                profiles = query(["--printer-profile", selection,
                                  "--query-print-tool-filament-profiles", "--output"])
                process = next((p for p in profiles["print_profiles"]
                                if p["name"].startswith("0.20mm")), None)
                if process is None:
                    raise RuntimeError(f"No native 0.20 mm process for {selection}")
                material = next((m for m in process["filament_profiles"]
                                 if m.startswith("Prusament PLA ")), None)
                if material is None:
                    raise RuntimeError(f"No native Prusament PLA for {selection}")
                config = query(["--printer-profile", selection,
                                "--print-profile", process["name"],
                                "--material-profile", material, "--save"])
                hardware = config["preset"]["hw_config"]
                # The app creates a random hardware instance UUID on every CLI
                # invocation. Retain stable preset IDs and a deterministic
                # instance UUID so repeated collection yields reviewable diffs.
                hardware["config_id"] = str(uuid.uuid5(uuid.NAMESPACE_URL, selection))
                nozzle = hardware["tools"]["0"]["features"]["nozzle_diameter"]
                uid = machine_uid(hardware)
                snapshots.append(config)
                sources.append({
                    "machine_uid": uid, "nozzle_uid": f"nozzle:{nozzle:g}mm",
                    "source_machine_profile_name": selection,
                    "display_name": re.sub(r" \d+(?:\.\d+)?(?: HF)?(?:,\s*\d+(?:\.\d+)?(?: HF)?)*$", "", selection),
                    "default_print_profile_name": process["name"],
                    "default_filament_profile_names": [m["name"] for m in config["preset"]["materials"]],
                    "availability": {"machine": "available", "process": "available", "material": "available"},
                    "hardware_settings": {
                        "printer_model": hardware["model"],
                        "nozzle_diameter": [f"{nozzle:g}"] * hardware["tool_count"],
                        "tool_count": hardware["tool_count"],
                        "material_slot_count": len(config["preset"]["materials"]),
                        "sheet": hardware["sheet"],
                        "bed": variant["printer_profiles"][0]["bed"],
                    },
                })
                print(f"Collected {selection}", flush=True)
        if len({(s["machine_uid"], s["nozzle_uid"]) for s in sources}) != len(sources):
            raise RuntimeError("Native hardware selections produced duplicate identities")
        # Native query order may vary; parent references and output order must not.
        pairs = sorted(zip(sources, snapshots, strict=True),
                       key=lambda pair: (pair[0]["machine_uid"], pair[0]["nozzle_uid"]))
        sources, snapshots = map(list, zip(*pairs, strict=True))
        catalog = {
            "schema_version": 1, "slicer_id": "PrusaSlicer", "application_version": VERSION,
            "source": {
                "release_tag": "version_" + VERSION,
                "source_commit": "30ef59195e0f3ee6f270b185bb5f9fb5f349f81f",
                "repository": "https://github.com/prusa3d/PrusaSlicer",
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "acquisition": "query-printer-models / query-print-tool-filament-profiles / save / export-config-schema",
                "scope": "Native CLI default-nozzle FFF selections, 0.20 mm, Prusament PLA; other tuning via explicit native config",
            },
            "base": snapshots[0], "sources": [],
        }
        for index, (source, config) in enumerate(zip(sources, snapshots, strict=True)):
            candidates = [(None, snapshots[0]), *enumerate(snapshots[:index])]
            parent, patch = min(((parent, json_patch(base, config)) for parent, base in candidates),
                key=lambda pair: len(json.dumps(pair[1], separators=(",", ":"))))
            catalog["sources"].append({**source, "config_parent": parent, "config_patch": patch})
        # Store exact native values once; differences refer only to earlier
        # configurations. This preserves arrays/nulls without one full copy per
        # machine and without maintaining a separate YAML condition evaluator.
        header = {k: v for k, v in catalog.items() if k not in {"base", "sources"}}
        text = (json.dumps(header, ensure_ascii=False, indent=2)[:-2] + ',\n  "base": '
            + json.dumps(catalog["base"], ensure_ascii=False, indent=2)
            + ',\n  "sources": [\n' + ',\n'.join('    ' + json.dumps(row,
                ensure_ascii=False, separators=(",", ":")) for row in catalog["sources"])
            + '\n  ]\n}\n')
        (output / "catalog.json").write_text(text, encoding="utf-8")
        (output / "schema.json").write_text(json.dumps(schema, ensure_ascii=False,
            separators=(",", ":")) + "\n", encoding="utf-8")
        if output.resolve() == (ROOT / "compatibility/current-src/native-project-sources/prusa-3").resolve():
            refresh_contract(schema)
            index_path = output.parent / "source-index.json"
            index = json.loads(index_path.read_text(encoding="utf-8"))
            index.setdefault("format_catalogs", {})["PrusaSlicer"] = {
                "application_version": VERSION, "catalog": "prusa-3/catalog.json",
                "schema": "prusa-3/schema.json", "project_format": "prusa3",
            }
            index_path.write_text(json.dumps(index, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "compatibility/current-src/native-project-sources/prusa-3")
    args = parser.parse_args()
    version = subprocess.run([str(args.binary), "--version"], capture_output=True,
                             text=True, encoding="utf-8", timeout=30)
    if version.returncode != 0 or (version.stdout + version.stderr).strip() != VERSION:
        raise SystemExit(f"Expected exact PrusaSlicer {VERSION} binary")
    collect(args.binary, args.output)


if __name__ == "__main__":
    main()
