#!/usr/bin/env python3
"""Check that the Neroued consumer produced a readable 3MF with Fat Cat parts."""

from __future__ import annotations

import sys
import zipfile
from pathlib import Path


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
    print(f"Verified 3MF archive: {path} ({len(names)} entries)")


if __name__ == "__main__":
    main()
