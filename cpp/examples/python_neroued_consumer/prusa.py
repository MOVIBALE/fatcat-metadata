#!/usr/bin/env python3
"""Write a native PrusaSlicer 3 project using FatCat and one Neroued write.

用 FatCat 合成 PrusaSlicer 3 原生项目，Neroued 只写包一次。
"""
from __future__ import annotations

import argparse
from pathlib import Path
import uuid

import fatcat_metadata as fatcat
import fatcat_metadata_neroued as adapter
import neroued_3mf as writer

from fatcat_metadata_examples.write_example import _cube_mesh


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--machine", default="prusa:mk4s")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sources = fatcat.list_machines("PrusaSlicer")["sources"]
    source = next((row for row in sources if row["machine_uid"] == args.machine), None)
    if source is None:
        raise SystemExit("Use fatcat catalog --slicer PrusaSlicer to select a machine")
    count = min(4, source["hardware_settings"]["material_slot_count"])
    colours = ["#E63946", "#FFD166", "#118AB2", "#FFFFFF"][:count]
    request = {
        "project_source": "fatcat_native", "slicer_id": "PrusaSlicer",
        "application_version": "3.0.0-alpha12", "machine_uid": args.machine,
        "nozzle_uid": source["nozzle_uid"],
        "source_materials": [{"name": f"Colour {i + 1}", "material_type": "PLA",
            "colour": colour, "native_settings": {"temperature": 237,
                "first_layer_temperature": 237, "extrusion_multiplier": 0.91}}
            for i, colour in enumerate(colours)],
        "process_settings": {"layer_height": 0.2, "fill_density": "35%",
            "perimeters": 3, "external_perimeter_speed": 60},
        "enable_prime_tower": count > 1,
        "wipe_tower_x": 15, "wipe_tower_y": 15,
    }
    result = fatcat.compose_project_settings(request)
    project = result["project_settings"]
    project["project"]["id"] = str(uuid.uuid4())
    builder = writer.DocumentBuilder()
    builder.set_unit(writer.Unit.Millimeter)
    materials = builder.add_base_material_group([
        writer.BaseMaterial(f"Colour {i + 1}", writer.Color.from_hex(c))
        for i, c in enumerate(colours)])
    parts = []
    components = []
    for i in range(count):
        mesh = _cube_mesh()
        # Caller-owned geometry: stacked 3 mm layers with a common 20 mm square.
        mesh.vertices = [writer.Vec3f(v.x, v.y, v.z * 0.15 + i * 3) for v in mesh.vertices]
        object_id = builder.add_mesh_object(f"Colour {i + 1}", mesh, materials, i)
        parts.append({"part_id": object_id, "name": f"Colour {i + 1}", "material_index": i})
        components.append(writer.Component(object_id, writer.Transform.identity()))
    assembly_id = builder.add_component_object("Prusa native demo", components)
    bed = source["hardware_settings"]["bed"]
    builder.add_build_item(assembly_id, writer.Transform.translation(bed["width"] / 2 - 10,
        bed["height"] / 2 - 10, 0), uuid=str(uuid.uuid4()))
    description = fatcat.compose_model_metadata(project, {
        "slicer_id": "PrusaSlicer", "application_version": "3.0.0-alpha12",
        "assembly_id": assembly_id, "parts": parts})
    adapter.apply_root_model_metadata(builder, description["root_model"])
    adapter.apply_metadata_components(builder, description, {})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    writer.write_to_file(args.output, builder.build())
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
