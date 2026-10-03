#!/usr/bin/env python3
"""Check that the Neroued consumer produced a readable 3MF with Fat Cat parts."""

from __future__ import annotations

import sys
import zipfile
import json
import math
import xml.etree.ElementTree as ET
from pathlib import Path


CORE = "{http://schemas.microsoft.com/3dmanufacturing/core/2015/02}"
PRODUCTION = "{http://schemas.microsoft.com/3dmanufacturing/production/2015/06}"


def _transform(vertex: tuple[float, float, float], element: ET.Element) -> tuple[float, float, float]:
    values = [float(value) for value in element.get("transform", "1 0 0 0 1 0 0 0 1 0 0 0").split()]
    if len(values) != 12 or not all(map(math.isfinite, values)):
        raise RuntimeError("invalid serialized geometry transform")
    return tuple(sum(vertex[row] * values[row * 3 + column] for row in range(3)) + values[9 + column]
                 for column in range(3))


def verify_cube_placement(archive: zipfile.ZipFile) -> tuple[tuple[float, ...], tuple[float, ...]]:
    """Check actual serialized geometry after component and production transforms."""
    root = ET.fromstring(archive.read("3D/3dmodel.model"))
    item = root.find(f"{CORE}build/{CORE}item")
    assembly = root.find(f"{CORE}resources/{CORE}object[@id='{item.get('objectid')}']")
    component = assembly.find(f"{CORE}components/{CORE}component")
    path = component.get(f"{PRODUCTION}path").lstrip("/")
    source = ET.fromstring(archive.read(path))
    mesh = source.find(f"{CORE}resources/{CORE}object[@id='{component.get('objectid')}']/{CORE}mesh")
    vertices = [_transform(_transform(tuple(float(vertex.get(axis)) for axis in "xyz"), component), item)
                for vertex in mesh.findall(f"{CORE}vertices/{CORE}vertex")]
    if not vertices or any(not all(map(math.isfinite, vertex)) for vertex in vertices):
        raise RuntimeError("consumer cube has no finite serialized vertices")
    lower = tuple(min(vertex[axis] for vertex in vertices) for axis in range(3))
    upper = tuple(max(vertex[axis] for vertex in vertices) for axis in range(3))
    if any(not math.isclose(upper[axis] - lower[axis], 20, abs_tol=1e-5) for axis in range(3)):
        raise RuntimeError("serialized consumer cube is not 20 mm in each dimension")
    if not math.isclose(lower[2], 0, abs_tol=1e-5):
        raise RuntimeError("consumer cube bottom is not on the build plate")
    project = json.loads(archive.read("Metadata/project_settings.config"))
    if project.get("printable_area"):
        points = [tuple(map(float, point.split("x"))) for point in project["printable_area"]]
        if any(lower[axis] <= min(point[axis] for point in points) or
               upper[axis] >= max(point[axis] for point in points) for axis in range(2)):
            raise RuntimeError("serialized consumer cube exceeds or touches the printable area boundary")
    if "printable_height" in project and upper[2] > float(project["printable_height"]):
        raise RuntimeError("serialized consumer cube exceeds the printable height")
    return lower, upper


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_3mf.py PATH")
    path = Path(sys.argv[1])
    with zipfile.ZipFile(path) as archive:
        broken = archive.testzip()
        if broken is not None:
            raise RuntimeError(f"3MF archive has a corrupt member: {broken}")
        names = set(archive.namelist())
        required = {
            "[Content_Types].xml",
            "_rels/.rels",
            "3D/3dmodel.model",
            "Metadata/project_settings.config",
            "Metadata/model_settings.config",
            "Metadata/slice_info.config",
        }
        missing = sorted(required - names)
        if missing:
            raise RuntimeError(f"3MF is missing required entries: {missing}")
        lower, upper = verify_cube_placement(archive)
    print(f"Verified serialized cube bounds: {lower} to {upper}")
    print(f"Verified 3MF archive: {path} ({len(names)} entries)")


if __name__ == "__main__":
    main()
