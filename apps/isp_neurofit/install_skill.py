"""Install the isp-neurofit agent skill: copy its folder (skill/isp-neurofit: SKILL.md, references/, scripts/) into a
skills folder and record where this OMKit checkout is, so that the copy runs the tool from here.

    python OMKit/apps/isp_neurofit/install_skill.py --dest DIR             any agent that reads SKILL.md folders
    python OMKit/apps/isp_neurofit/install_skill.py --claude-user          ~/.claude/skills (Claude Code, every project)
    python OMKit/apps/isp_neurofit/install_skill.py --claude-project DIR   DIR/.claude/skills (Claude Code, one project)

The skill lands in <skills folder>/isp-neurofit/; scripts/config.json in it holds the absolute path of this checkout's
OMKit/apps folder and of the Python interpreter the installer ran with. An existing copy is replaced only with --force.
Moving the checkout later: install again (or set the environment variable OMKIT_APPS)."""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
APPS = HERE.parent
SKILL = HERE / "skill" / "isp-neurofit"
NAME = SKILL.name


def install(skills_dir: Path, force=False) -> Path:
    """Copy the skill into skills_dir/isp-neurofit and write its scripts/config.json; returns the installed folder."""
    skills_dir = skills_dir.expanduser().resolve()
    dest = skills_dir / NAME
    if dest.resolve() == SKILL.resolve():
        raise SystemExit("error: that is the skill's own folder in the checkout")
    if dest.exists():
        if not force:
            raise SystemExit(f"error: {dest} exists: --force replaces it")
        if not (dest / "SKILL.md").is_file():
            raise SystemExit(f"error: {dest} exists and is not a skill folder: not touched")
        shutil.rmtree(dest)
    skills_dir.mkdir(parents=True, exist_ok=True)
    shutil.copytree(SKILL, dest, ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "config.json"))
    cfg = {"apps": str(APPS), "python": sys.executable}
    (dest / "scripts" / "config.json").write_text(json.dumps(cfg, indent=1) + "\n", encoding="utf-8")
    return dest


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Install the isp-neurofit agent skill into a skills folder.")
    where = ap.add_mutually_exclusive_group(required=True)
    where.add_argument("--dest", help="the skills folder (the skill goes into DEST/isp-neurofit)")
    where.add_argument("--claude-user", action="store_true", help="Claude Code, for every project: ~/.claude/skills")
    where.add_argument("--claude-project", metavar="DIR", help="Claude Code, for one project: DIR/.claude/skills")
    ap.add_argument("--force", action="store_true", help="replace an installed copy")
    args = ap.parse_args(argv)
    if args.claude_user:
        skills = Path.home() / ".claude" / "skills"
    elif args.claude_project:
        skills = Path(args.claude_project) / ".claude" / "skills"
    else:
        skills = Path(args.dest)
    dest = install(skills, args.force)
    print(f"installed: {dest}")
    print(f"the tool: {APPS / 'isp_neurofit'} (Python {sys.executable})")
    print(f"check: python \"{dest / 'scripts' / 'neurofit.py'}\" doctor")
    return 0


if __name__ == "__main__":
    sys.exit(main())
