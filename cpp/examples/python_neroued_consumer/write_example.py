#!/usr/bin/env python3
"""Write a small 3MF through Fat Cat metadata and the neroued writer."""

from __future__ import annotations

import argparse
import json
import math
import uuid
from pathlib import Path
from typing import Any, TYPE_CHECKING

import fatcat_metadata
import fatcat_metadata_neroued

if TYPE_CHECKING:
    import neroued_3mf as n3mf


_EXAMPLE_DIR = Path(__file__).resolve().parent
_BUILTIN_REQUEST = _EXAMPLE_DIR / "builtin_project_request.json"
_CUSTOM_REQUEST = _EXAMPLE_DIR / "project_request.json"
_CUBE_VERTICES = (
    (0.0, 0.0, 0.0),
    (20.0, 0.0, 0.0),
    (20.0, 20.0, 0.0),
    (0.0, 20.0, 0.0),
    (0.0, 0.0, 20.0),
    (20.0, 0.0, 20.0),
    (20.0, 20.0, 20.0),
    (0.0, 20.0, 20.0),
)


def _cube_mesh() -> n3mf.Mesh:
    import neroued_3mf as n3mf

    triangles = (
        (0, 2, 1), (0, 3, 2),
        (4, 5, 6), (4, 6, 7),
        (0, 1, 5), (0, 5, 4),
        (1, 2, 6), (1, 6, 5),
        (2, 3, 7), (2, 7, 6),
        (3, 0, 4), (3, 4, 7),
    )
    mesh = n3mf.Mesh()
    mesh.vertices = [n3mf.Vec3f(*vertex) for vertex in _CUBE_VERTICES]
    mesh.triangles = [n3mf.IndexTriangle(*triangle) for triangle in triangles]
    return mesh


def _number(value: float) -> str:
    return format(value, ".15g")


def _load_json(path: Path) -> tuple[str, dict[str, Any]]:
    raw = path.read_text(encoding="utf-8")
    value = json.loads(raw)
    if not isinstance(value, dict):
        raise ValueError(f"Expected a JSON object in {path}")
    return raw, value


def _model_part_request(object_id: int) -> dict[str, Any]:
    minimum = tuple(min(vertex[axis] for vertex in _CUBE_VERTICES) for axis in range(3))
    maximum = tuple(max(vertex[axis] for vertex in _CUBE_VERTICES) for axis in range(3))
    center = tuple((minimum[axis] + maximum[axis]) / 2 for axis in range(3))
    matrix = (
        f"1 0 0 {_number(maximum[0])} "
        f"0 1 0 {_number(maximum[1])} "
        f"0 0 1 {_number(maximum[2])} 0 0 0 1"
    )
    return {
        "part_id": object_id,
        "name": "Consumer cube",
        "material_index": 0,
        "source_object_id": 0,
        "source_volume_id": 0,
        "matrix": matrix,
        "source_offset_x": _number(center[0]),
        "source_offset_y": _number(center[1]),
        "source_offset_z": _number(center[2]),
    }


def _cube_placement(project: dict[str, Any],
                    bed_center: tuple[float, float] | None = None) -> tuple[float, float, float]:
    """Center this example on the selected printable rectangle, without editing it."""
    area = project.get("printable_area")
    bounds = None
    if area:
        try:
            points = [tuple(float(value) for value in point.split("x")) for point in area]
        except (AttributeError, TypeError, ValueError) as error:
            raise ValueError("printable area must describe a finite rectangle") from error
        if len(points) != 4 or any(len(point) != 2 or not all(map(math.isfinite, point)) for point in points):
            raise ValueError("printable area must describe a finite rectangle")
        lower = tuple(min(point[axis] for point in points) for axis in range(2))
        upper = tuple(max(point[axis] for point in points) for axis in range(2))
        if set(points) != {(lower[0], lower[1]), (lower[0], upper[1]),
                           (upper[0], lower[1]), (upper[0], upper[1])} or any(
                               lower[axis] >= upper[axis] for axis in range(2)):
            raise ValueError("this cube example requires a rectangular printable area")
        bounds = lower, upper
        if bed_center is None:
            bed_center = tuple((lower[axis] + upper[axis]) / 2 for axis in range(2))
    if bed_center is None:
        raise ValueError("project has no printable area; supply generator --bed-center X Y")
    if len(bed_center) != 2 or not all(map(math.isfinite, bed_center)):
        raise ValueError("bed center must contain two finite coordinates")
    minimum = tuple(min(vertex[axis] for vertex in _CUBE_VERTICES) for axis in range(3))
    maximum = tuple(max(vertex[axis] for vertex in _CUBE_VERTICES) for axis in range(3))
    translation = tuple(bed_center[axis] - (minimum[axis] + maximum[axis]) / 2
                        for axis in range(2)) + (-minimum[2],)
    if bounds and any(minimum[axis] + translation[axis] <= bounds[0][axis] or
                      maximum[axis] + translation[axis] >= bounds[1][axis] for axis in range(2)):
        raise ValueError("cube placement exceeds the selected printable area")
    if "printable_height" in project:
        height = float(project["printable_height"])
        if not math.isfinite(height) or maximum[2] + translation[2] > height:
            raise ValueError("cube exceeds the selected printable height")
    return translation


def _cube_material(request: dict[str, Any], project: dict[str, Any]) -> tuple[str, str]:
    palette = request.get('source_materials')
    if palette:
        return palette[0]['name'], palette[0]['colour']
    return project['filament_settings_id'][0], project['filament_colour'][0]


def _create_builder(placement: tuple[float, float, float],
                    material: tuple[str, str], production: bool = False) -> tuple[n3mf.DocumentBuilder, int, int]:
    import neroued_3mf as n3mf

    builder = n3mf.DocumentBuilder()
    builder.set_unit(n3mf.Unit.Millimeter)
    builder.set_language("en-US")
    builder.add_namespace("p", "http://schemas.microsoft.com/3dmanufacturing/production/2015/06")

    material_group_id = builder.add_base_material_group(
        [n3mf.BaseMaterial(material[0], n3mf.Color.from_hex(material[1]))]
    )
    object_id = builder.add_mesh_object(
        "Consumer cube", _cube_mesh(), material_group_id, 0
    )
    builder.set_object_uuid(object_id, str(uuid.uuid4()))
    builder.set_component_transform(object_id, n3mf.Transform.identity())

    if production:
        builder.enable_production(n3mf.Transform.translation(*placement))
        builder.set_production_merge_objects(True)
        builder.add_build_item(object_id, uuid=str(uuid.uuid4()))
        assembly_id = object_id + 1
    else:
        # Original 0.4.0 preserves material groups in core model resources.
        # Its production serialization omits them; use its public core assembly API.
        assembly_id = builder.add_component_object(
            "Consumer cube assembly", [n3mf.Component(object_id, n3mf.Transform.identity())])
        builder.set_object_uuid(assembly_id, str(uuid.uuid4()))
        builder.add_build_item(assembly_id, n3mf.Transform.translation(*placement),
                               uuid=str(uuid.uuid4()))
    return builder, object_id, assembly_id


def main() -> None:
    import neroued_3mf as n3mf

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--project-json",
        type=Path,
        default=None,
        help="optional explicit source slicer project settings JSON",
    )
    parser.add_argument(
        "--project-request-json",
        type=Path,
        default=None,
        help="target and source selection request JSON; defaults for the selected source mode",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--production", action="store_true",
                        help="external production model layout; requires a writer with material serialization support")
    parser.add_argument("--bed-center", type=float, nargs=2, metavar=("X", "Y"),
                        help="generator placement center; required if the custom project has no printable area")
    args = parser.parse_args()

    request_path = args.project_request_json or (
        _BUILTIN_REQUEST if args.project_json is None else _CUSTOM_REQUEST
    )
    project_request_json, project_request = _load_json(request_path)
    if args.project_json is None:
        project_result_json = fatcat_metadata.compose_project_settings(
            project_request_json
        )
    else:
        project_json, _ = _load_json(args.project_json)
        project_result_json = fatcat_metadata.compose_project_settings(
            project_json, project_request_json
        )
    project_result = json.loads(project_result_json)
    final_project_json = project_result["project_settings_json"]
    final_project = json.loads(final_project_json)
    placement = _cube_placement(final_project, tuple(args.bed_center) if args.bed_center else None)
    builder, object_id, assembly_id = _create_builder(
        placement, _cube_material(project_request, final_project), args.production)

    _, model_request = _load_json(_EXAMPLE_DIR / "request.json")
    model_request["slicer_id"] = project_request["slicer_id"]
    model_request["application_version"] = project_request["application_version"]
    model_request["assembly_id"] = assembly_id
    model_request["identify_id"] = project_result["metadata_defaults"]["identify_id"]
    model_request["source_file"] = args.output.name
    model_request["active_material_count"] = len(
        final_project.get("filament_settings_id", [])
    )
    model_request["plate"]["bed_type"] = project_result["metadata_defaults"][
        "plate_value"
    ]
    model_request["parts"] = [
        _model_part_request(object_id)
    ]

    description = json.loads(
        fatcat_metadata.compose_model_metadata(
            final_project_json,
            json.dumps(model_request),
        )
    )
    fatcat_metadata_neroued.apply_root_model_metadata(
        builder, description["root_model"]
    )
    fatcat_metadata_neroued.apply_metadata_components(builder, description, {})

    args.output.parent.mkdir(parents=True, exist_ok=True)
    n3mf.write_to_file(args.output, builder.build())
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
