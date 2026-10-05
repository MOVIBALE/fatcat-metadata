"""Package reviewed wheels/SDK with hashes, without publishing. 打包并留存来源，不发布。"""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tomllib
from zipfile import ZipFile


def main() -> None:
    """Prepare local distribution files and their receipt. 准备本地发行文件和清单。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-prefix", type=Path)
    parser.add_argument("--wheel-dir", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if args.sdk_prefix is None and args.wheel_dir is None:
        parser.error("provide --sdk-prefix or --wheel-dir")
    root = Path(__file__).resolve().parent.parent
    project = tomllib.loads((root / "pyproject.toml").read_text(encoding="utf-8"))
    version = project["project"]["version"]
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    status = subprocess.check_output(["git", "status", "--porcelain"], cwd=root, text=True)
    dirty = bool(status.strip())
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    artifacts = []
    if args.sdk_prefix is not None:
        prefix = args.sdk_prefix.resolve()
        suffix = ".exe" if sys.platform == "win32" else ""
        command = prefix / "bin" / f"fatcat{suffix}"
        provenance = json.loads(subprocess.check_output(
            [str(command), "--version"], text=True))
        if provenance != {"version": version, "source_revision": revision, "source_dirty": dirty}:
            parser.error("SDK build provenance does not match this checkout; rebuild before packaging")
        name = (f"fatcat-metadata-sdk-{version}-{sys.platform}-"
                f"{platform.machine().lower()}-{revision[:12]}")
        if dirty:
            name += "-dirty"
        archive = shutil.make_archive(
            str(output / name), "zip" if sys.platform == "win32" else "gztar",
            root_dir=prefix)
        artifacts.append({"filename": Path(archive).name, "kind": "cpp-sdk-and-cli"})
    if args.wheel_dir is not None:
        wheels = sorted(args.wheel_dir.glob("*.whl"))
        if not wheels:
            parser.error("wheel directory contains no wheels")
        for wheel in wheels:
            destination = output / wheel.name
            if wheel.resolve() != destination:
                shutil.copy2(wheel, destination)
            with ZipFile(destination) as package:
                info = next(name for name in package.namelist()
                            if name.endswith(".dist-info/WHEEL"))
                tags = [line[5:] for line in package.read(info).decode("utf-8").splitlines()
                        if line.startswith("Tag: ")]
            artifacts.append({"filename": destination.name, "kind": "python-wheel", "tags": tags})
    for artifact in artifacts:
        with (output / artifact["filename"]).open("rb") as stream:
            artifact["sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
    manifest = root / "compatibility/current-src/translations/supported-targets.json"
    receipt = {
        "version": version, "source_revision": revision, "source_dirty": dirty,
        "targets": json.loads(manifest.read_text(encoding="utf-8"))["targets"],
        "artifacts": artifacts,
    }
    (output / "distribution.json").write_text(
        json.dumps(receipt, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Prepared {len(artifacts)} files in {output}")


if __name__ == "__main__":
    main()
