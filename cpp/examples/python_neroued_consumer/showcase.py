#!/usr/bin/env python3
"""Export the same cube with custom process settings for a selected slicer/printer."""

from __future__ import annotations

import argparse
import json
import uuid
from pathlib import Path

import fatcat_metadata as fatcat
import fatcat_metadata_neroued as adapter
import neroued_3mf as writer

if __package__:
    from . import write_example as geometry
else:
    import write_example as geometry


def main() -> None:
    targets = fatcat.list_targets()["targets"]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--slicer", choices=[t["slicer_id"] for t in targets], required=True)
    parser.add_argument("--machine", required=True, help="machine UID returned by fatcat catalog")
    parser.add_argument("--nozzle", default="nozzle:0.4mm")
    parser.add_argument("--plate", default="plate:textured-pei")
    parser.add_argument("--filament-profile", help="exact PLA preset name returned by fatcat choices")
    parser.add_argument("--colour", default="#007F87")
    parser.add_argument("--name", default="FatCat custom process cube")
    parser.add_argument("--process-json", type=Path,
                        default=Path(__file__).with_name("showcase_process.json"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    target = next(t for t in targets if t["slicer_id"] == args.slicer)
    _, process = geometry._load_json(args.process_json)
    selection = {
        **process,
        **target,
        "project_source": "fatcat_native",
        "machine_uid": args.machine,
        "nozzle_uid": args.nozzle,
        "build_plate_uid": args.plate,
        "source_materials": [{"name": "PLA", "material_type": "PLA", "colour": args.colour}],
    }
    if args.filament_profile:
        selection["native_filament_profile_names"] = [args.filament_profile]
    settings = fatcat.compose_project_settings(selection)
    project = settings["project_settings"]

    # The generator owns geometry and real writer IDs. FatCat owns slicer settings.
    placement = geometry._cube_placement(project)
    builder, part_id, assembly_id = geometry._create_builder(
        placement, geometry._cube_material(selection, project),
        name=args.name)
    part = geometry._model_part_request(part_id)
    part["name"] = args.name
    model = {
        **target,
        "assembly_id": assembly_id, "instance_id": "0",
        "identify_id": settings["metadata_defaults"]["identify_id"],
        "source_file": args.output.name, "active_material_count": 1,
        "parts": [part],
        "plate": {"plater_id": "1", "plater_name": args.name, "locked": False,
                  "bed_type": settings["metadata_defaults"]["plate_value"],
                  "filament_map_mode": "Auto For Flush"},
        "component_inputs": {}, "resource_roles": [],
    }
    defaults = settings["metadata_defaults"]
    if "slice_uuid_seed_prefix" in defaults:
        model["slice_uuid"] = str(uuid.uuid4())
    if defaults["plate_summary"]:
        model["component_inputs"]["plate_summary"] = {
            "bbox_all": [placement[0], placement[1], placement[0] + 20, placement[1] + 20],
            "filament_colors": project["filament_colour"],
            "nozzle_diameter": project["nozzle_diameter"],
            "layer_height": project["layer_height"], "name": args.name,
            "bed_type": defaults["plate_value"],
        }
    description = fatcat.compose_model_metadata(project, model)
    adapter.apply_root_model_metadata(builder, description["root_model"])
    adapter.apply_metadata_components(builder, description, {})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    writer.write_to_file(args.output, builder.build())

    # Leave the final input/output facts beside the 3MF for readers to inspect.
    receipt = {"selection": selection, "composition": settings, "model": model}
    args.output.with_suffix(".settings.json").write_text(
        json.dumps(receipt, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {args.output}")
    print(f"Target: {args.slicer} {target['application_version']} / {project['printer_model']}")
    options = fatcat.list_project_options({
        key: selection[key]
        for key in ("slicer_id", "application_version", "machine_uid", "nozzle_uid")
    })
    aliases = options["process_settings_contract"]["aliases"]
    print(json.dumps({aliases.get(key, key): project.get(aliases.get(key, key))
                      for key in process}, indent=2))


if __name__ == "__main__":
    main()
