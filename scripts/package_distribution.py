"""Package reviewed wheels/SDK with hashes, without publishing. 打包并留存来源，不发布。"""

import argparse
from email.parser import BytesParser
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
from tarfile import open as open_tar
import tomllib
from zipfile import ZipFile


def main() -> None:
    """Prepare local distribution files and their receipt. 准备本地发行文件和清单。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-prefix", type=Path)
    parser.add_argument("--dist-dir", "--wheel-dir", dest="dist_dir", type=Path,
                        help="directory containing Python wheels and/or a source distribution")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if args.sdk_prefix is None and args.dist_dir is None:
        parser.error("provide --sdk-prefix or --dist-dir")
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
    if args.dist_dir is not None:
        distributions = sorted(args.dist_dir.glob("*.whl")) + sorted(args.dist_dir.glob("*.tar.gz"))
        if not distributions:
            parser.error("distribution directory contains no Python distributions")
        for distribution in distributions:
            destination = output / distribution.name
            if distribution.resolve() != destination:
                shutil.copy2(distribution, destination)
            artifact = {"filename": destination.name}
            if destination.suffix == ".whl":
                with ZipFile(destination) as package:
                    info = next(name for name in package.namelist()
                                if name.endswith(".dist-info/WHEEL"))
                    metadata = next(name for name in package.namelist()
                                    if name.endswith(".dist-info/METADATA"))
                    identity = BytesParser().parsebytes(package.read(metadata))
                    tags = [line[5:] for line in package.read(info).decode("utf-8").splitlines()
                            if line.startswith("Tag: ")]
                artifact.update(kind="python-wheel", tags=tags)
            else:
                with open_tar(destination) as package:
                    metadata = next(member for member in package.getmembers()
                                    if member.name.count("/") == 1 and member.name.endswith("/PKG-INFO"))
                    with package.extractfile(metadata) as stream:
                        identity = BytesParser().parsebytes(stream.read())
                artifact.update(kind="python-sdist")
            if identity["Name"] != project["project"]["name"] or identity["Version"] != version:
                parser.error(f"distribution identity does not match this checkout: {destination.name}")
            artifacts.append(artifact)
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
