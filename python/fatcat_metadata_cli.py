"""Launch the shared native CLI installed with the wheel. 启动随包安装的原生命令。"""

from pathlib import Path
import subprocess
import sys


def main() -> int:
    """Forward arguments to the native CLI. 将参数转交原生命令行。"""
    import fatcat_metadata

    root = Path(fatcat_metadata.__file__).resolve().parent / "fatcat_metadata_data"
    executable = root / "bin" / ("fatcat.exe" if sys.platform == "win32" else "fatcat")
    try:
        args = sys.argv[1:]
        # Default to wheel data; an explicit override remains caller-owned.
        data_args = [] if (not args or args[0] in {"--help", "-h", "--version"}
                           or "--data-root" in args) else ["--data-root", str(root)]
        return subprocess.run([str(executable), *args, *data_args], check=False).returncode
    except OSError as error:
        print(f"fatcat: cannot launch the installed native CLI: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
