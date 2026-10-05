#!/usr/bin/env python3
"""Smallest cube export: native settings, generator facts, metadata, one write."""

import argparse
from pathlib import Path

import fatcat_metadata as fatcat
import fatcat_metadata_neroued as adapter
import neroued_3mf as writer

# Reuse the complete example's geometry and safe placement, not slicer policy.
if __package__:
    from . import write_example
else:
    import write_example

create_cube_builder = write_example._create_builder
cube_material = write_example._cube_material
cube_placement = write_example._cube_placement
cube_part_metadata = write_example._model_part_request


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    output = parser.parse_args().output

    # 1. Describe the target and material; FatCat resolves native profile JSON.
    request = {
        "project_source": "fatcat_native",
        "slicer_id": "BambuStudio", "application_version": "02.08.02.61",
        "machine_uid": "bambu-lab:a1-mini", "nozzle_uid": "nozzle:0.4mm",
        "build_plate_uid": "plate:textured-pei",
        "source_materials": [
            {"name": "Bambu PLA Basic", "material_type": "PLA Basic", "colour": "#E63946"}
        ],
        "enable_prime_tower": "0",
    }
    settings = fatcat.compose_project_settings(request)
    project = settings["project_settings"]

    # 2. Your generator supplies real geometry, writer IDs and material indexes.
    builder, part_id, assembly_id = create_cube_builder(
        cube_placement(project), cube_material(request, project))
    model = {
        "slicer_id": request["slicer_id"],
        "application_version": request["application_version"],
        "assembly_id": assembly_id, "instance_id": "0",
        "identify_id": settings["metadata_defaults"]["identify_id"],
        "source_file": output.name, "active_material_count": 1,
        "parts": [cube_part_metadata(part_id)],
        "plate": {"plater_id": "1", "plater_name": "FatCat cube", "locked": False,
                  "bed_type": settings["metadata_defaults"]["plate_value"],
                  "filament_map_mode": "Auto For Flush"},
        "component_inputs": {}, "resource_roles": [],
    }

    # 3. Apply FatCat's description, then let Neroued write the package once.
    description = fatcat.compose_model_metadata(project, model)
    adapter.apply_root_model_metadata(builder, description["root_model"])
    adapter.apply_metadata_components(builder, description, {})
    output.parent.mkdir(parents=True, exist_ok=True)
    writer.write_to_file(output, builder.build())
    print(f"Wrote {output}")


if __name__ == "__main__":
    main()
