"""Run the OMKit ISP neurofit command line from this skill's folder, wherever the folder was copied to:

    python <this skill's folder>/scripts/neurofit.py <command> [options]

The tool itself lives in an OMKit checkout (OMKit/apps/isp_neurofit). Its folder OMKit/apps is found, in this order:
1. the environment variable OMKIT_APPS;
2. config.json next to this script, written by the installer (OMKit/apps/isp_neurofit/install_skill.py);
3. this script's own place, when the skill is used inside the checkout (OMKit/apps/isp_neurofit/skill/isp-neurofit).
config.json may also name the Python interpreter the installer ran with: the tool is run with it when the current one
lacks the packages (numpy, Pillow, rawpy)."""
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def config() -> dict:
    try:
        return json.loads((HERE / "config.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def apps_folder(cfg: dict):
    """OMKit/apps, or None."""
    candidates = [os.environ.get("OMKIT_APPS"), cfg.get("apps"), HERE.parents[3] if len(HERE.parents) > 3 else None]
    for c in candidates:
        if c and (Path(c) / "isp_neurofit" / "cli.py").is_file() and (Path(c) / "ispkit").is_dir():
            return Path(c).resolve()
    return None


def has_packages() -> bool:
    try:
        import numpy  # noqa: F401
        import PIL  # noqa: F401
        return True
    except ImportError:
        return False


def main() -> int:
    cfg = config()
    apps = apps_folder(cfg)
    if apps is None:
        print("error: the OMKit checkout is not found. Install the skill with "
              "'python OMKit/apps/isp_neurofit/install_skill.py --dest <skills folder>' from the checkout, or set the "
              "environment variable OMKIT_APPS to the checkout's OMKit/apps folder.", file=sys.stderr)
        return 2
    run = apps / "isp_neurofit" / "run.py"
    py = cfg.get("python")
    if not has_packages() and py and Path(py).is_file() and Path(py).resolve() != Path(sys.executable).resolve():
        return subprocess.run([py, str(run), *sys.argv[1:]]).returncode
    if not has_packages():
        print(f"error: this Python lacks numpy or Pillow: python -m pip install -r \"{apps / 'requirements.txt'}\"",
              file=sys.stderr)
        return 2
    sys.path.insert(0, str(apps))
    from isp_neurofit.cli import main as cli_main
    return cli_main(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
