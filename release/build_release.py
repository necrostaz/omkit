"""Build an OMKit release: one ZIP for one platform and firmware, for camera owners.

    python3 OMKit/release/build_release.py --version 0.1.0 [--platform om5m2/fw1100] [--card DIR] [--out DIR] [--test]

The ZIP (OMKit-<version>-<platform>-<firmware>.zip, one top folder of the same name) holds:
  INSTALL.txt      the short step-by-step install guide of the platform (release/platforms/<platform>/<firmware>/
                   INSTALL.txt with the version filled in); the full guide stays in the OMKit documentation
                   (docs/platform/<platform>/<firmware>/install.md)
  CHANGELOG.md     OMKit/CHANGELOG.md: the changes of every release (Keep a Changelog)
  RELEASE.md       release notes: versions of core and the modules, contents, the patched file's expected SHA-256
  RELEASE.json     the same for programs
  LICENSE, LICENSE-CC-BY-4.0, NOTICE   OMKit's licences and notice (required, except with --test)
  OMKIT/           the card folder: CORE.BIN, MODULES/, GRAIN/, FILMLOOK/, OMKIT.INI (the bundled modules on)
  firmware_tool/   omkit_firmware.py, platform.json, patch.json and the launchers for Windows, macOS and Linux
  SHA256SUMS.txt   SHA-256 of every other file (sha256sum -c / shasum -a 256 -c)

No firmware file of any kind goes into the ZIP: users make the patched file on their own computer with firmware_tool.

The card folder is built with `make -C OMKit card` (on Windows through WSL) into the output folder, or taken from --card.
Either way it is checked: CORE.BIN's header, sum and version; exactly the modules of RELEASE_MODULES in the Makefile,
with their names and versions; the grain tiles and FILMLOOK presets equal to their manifests (tools/grainset.py,
tools/presetset.py); OMKIT.INI byte for byte core's sample with those modules on (tools/cardini.py); nothing else.
--test builds without LICENSE and from a modified tree; the ZIP is then named and marked TEST and must never be
published. Output: OMKit/build/release/ (next to the Makefile's default OUT, OMKit/build).

Next to the ZIP the builder writes <top>.notes.md, the text of the GitHub release: the version's section of
CHANGELOG.md (only what changed since the previous release), the component versions and the ZIP's SHA-256. A release
needs a dated section for its version at the top of CHANGELOG.md and an empty Unreleased section; a --test build
takes the version's section if there is one, else Unreleased, else the newest release's section.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import time
import zipfile
from pathlib import Path

OMKIT = Path(__file__).resolve().parents[1]
RELEASE = OMKIT / "release"
sys.path.insert(0, str(OMKIT / "tools"))
import cardini  # noqa: E402
import grainset  # noqa: E402
import presetset  # noqa: E402

DEFAULT_OUT = OMKIT / "build" / "release"
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?$")
USER_FILES = ["omkit_firmware.py", "get_firmware_windows.bat", "get_firmware_macos.command", "get_firmware_linux.sh"]
EXECUTABLE = (".sh", ".command")
CORE_MAGIC, CORE_ABI, CORE_MAX = 0x434B4D4F, 1, 0x300000      # tools/pack.py
OMM_MAGIC = 0x4D4B4D4F                                        # tools/mkmod.py: 'OMKM'
MAX_FILE = 16 << 20                                           # no file of a release is near this; firmware files are
MAX_EDIT_BYTES = 4096                                         # the patch is the loader's code only
TEST_MARK = "TEST-BUILD-NOT-FOR-PUBLISHING.txt"
CHANGE_GROUPS = ("Added", "Changed", "Deprecated", "Removed", "Fixed", "Security")   # Keep a Changelog 1.1.0


def fail(msg: str) -> None:
    raise SystemExit("build_release: " + msg)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# --- versions and the card folder --------------------------------------------------------------------------------

def core_version() -> str:
    m = re.search(r'#define\s+CORE_VERSION\s+"([^"]+)"', (OMKIT / "src/core/include/omk_core.h").read_text("utf-8"))
    if not m:
        fail("CORE_VERSION not found in omk_core.h")
    return m.group(1)


def release_modules() -> dict[str, str]:
    m = re.search(r"^RELEASE_MODULES\s*:=\s*(.+)$", (OMKIT / "Makefile").read_text("utf-8"), re.M)
    if not m:
        fail("RELEASE_MODULES not found in the Makefile")
    return {name.upper(): ver for name, _, ver in (w.partition(":") for w in m.group(1).split())}


def wsl_path(p: Path) -> str:
    r = subprocess.run(["wsl", "wslpath", "-a", "-u", str(p).replace("\\", "/")], capture_output=True, text=True)
    if r.returncode:
        fail(f"wslpath failed for {p}: {r.stderr.strip()}")
    return r.stdout.strip()


def build_card(work: Path) -> Path:
    """make -C OMKit card into the folder work: core build in work/core, card in work/card/OMKIT."""
    core, card = work / "core", work / "card" / "OMKIT"
    if os.name == "nt":
        q = lambda p: "'" + wsl_path(p) + "'"
        cmd = ["wsl", "bash", "-lc", f"make -C {q(OMKIT)} card BUILD=release OUT={q(core)} CARD={q(card)}"]
    else:
        cmd = ["make", "-C", str(OMKIT), "card", "BUILD=release", f"OUT={core}", f"CARD={card}"]
    print("building the card:", " ".join(cmd), flush=True)
    if subprocess.run(cmd).returncode:
        fail("make card failed")
    return card


def check_core(data: bytes, version: str) -> None:
    if len(data) < 20:
        fail("CORE.BIN is too short")
    magic, length, total, abi = struct.unpack_from("<4I", data)
    body = data[16:]
    if (magic, abi) != (CORE_MAGIC, CORE_ABI) or length != len(body) or not 0 < length <= CORE_MAX - 16 or length % 4:
        fail("CORE.BIN: bad header")
    if sum(struct.unpack(f"<{length // 4}I", body)) & 0xFFFFFFFF != total:
        fail("CORE.BIN: body sum differs from the header")
    if b"\0" + version.encode() + b"\0" not in body:
        fail(f"CORE.BIN is not core {version} (CORE_VERSION of the source): rebuild the card")


def check_module(name: str, data: bytes, version: str) -> None:
    magic, _fmt, _api, raw, ver = struct.unpack_from("<IHH8sI", data)
    major, _, minor = version.partition(".")
    if magic != OMM_MAGIC or raw.rstrip(b"\0").decode("ascii", "replace") != name:
        fail(f"MODULES/{name}.OMM: not the module {name}")
    if ver != (int(major) << 16 | int(minor or 0)):
        fail(f"MODULES/{name}.OMM: version {ver >> 16}.{ver & 0xFFFF}, the Makefile says {version}")


def check_set(card_dir: Path, entries: list[dict], ext: str, what: str) -> None:
    want = {e["file"]: e["sha256"] for e in entries}
    have = {p.name for p in card_dir.iterdir()} if card_dir.is_dir() else set()
    if have != set(want):
        fail(f"{card_dir.name}/: {what} differ from the manifest (missing {sorted(set(want) - have)}, "
             f"extra {sorted(have - set(want))})")
    for name, digest in want.items():
        if not name.upper().endswith(ext) or sha256((card_dir / name).read_bytes()) != digest:
            fail(f"{card_dir.name}/{name}: differs from the manifest")


def check_card(card: Path) -> dict:
    """Every check of the card folder; returns the contents for the release notes."""
    if card.name != "OMKIT" or not card.is_dir():
        fail(f"{card}: not an OMKIT folder")
    core_ver, modules = core_version(), release_modules()
    top = {p.name for p in card.iterdir()}
    if top != {"CORE.BIN", "MODULES", "GRAIN", "FILMLOOK", "OMKIT.INI"}:
        fail(f"{card}: expected CORE.BIN, MODULES, GRAIN, FILMLOOK, OMKIT.INI and nothing else, found {sorted(top)}")
    check_core((card / "CORE.BIN").read_bytes(), core_ver)
    mods = {p.name for p in (card / "MODULES").iterdir()}
    if mods != {f"{m}.OMM" for m in modules}:
        fail(f"MODULES/: {sorted(mods)}, the Makefile's RELEASE_MODULES give {sorted(modules)}")
    for name, ver in modules.items():
        check_module(name, (card / "MODULES" / f"{name}.OMM").read_bytes(), ver)
    try:
        ini = cardini.make(list(modules))
    except cardini.IniError as e:
        fail(f"OMKIT.INI cannot be made: {e}")
    if (card / "OMKIT.INI").read_bytes() != ini:
        fail("OMKIT.INI is not core's sample with the modules of RELEASE_MODULES on (tools/cardini.py): rebuild the card")
    tiles, presets = grainset.check(), presetset.check()
    check_set(card / "GRAIN", tiles, ".GRN", "grain tiles")
    check_set(card / "FILMLOOK", presets, ".ISP", "presets")
    return {"core": core_ver, "modules": modules, "grain_tiles": len(tiles), "presets": len(presets)}


# --- the firmware tool data --------------------------------------------------------------------------------------

def check_platform(pdir: Path) -> tuple[dict, dict]:
    platform = json.loads((pdir / "platform.json").read_text("utf-8"))
    patch = json.loads((pdir / "patch.json").read_text("utf-8"))
    off = platform["official_file"]
    if (patch["input"] != {"size": off["size"], "sha256": off["sha256"]}
            or (patch["platform"], patch["firmware"]) != (platform["platform"], platform["firmware"])):
        fail("platform.json and patch.json do not belong together")
    total = 0
    for comp in patch["components"]:
        for e in comp["edits"]:
            if set(e) != {"name", "address", "offset", "size", "bytes"}:
                fail(f"patch.json: edit {e.get('name')} has fields {sorted(e)} (only new bytes, never old ones)")
            n = len(bytes.fromhex(" ".join(e["bytes"])))
            if n != e["size"]:
                fail(f"patch.json: edit {e['name']} size")
            total += n
    if total > MAX_EDIT_BYTES:
        fail(f"patch.json: {total} bytes of edits, more than the loader can be")
    return platform, patch


# --- texts -------------------------------------------------------------------------------------------------------

LINK = re.compile(r"\]\(([^)\s]*)\)")
CHANGE_HEAD = re.compile(r"^## \[([^\]]+)\](?: - (\S+))?$")


def changelog_sections(text: str) -> list[tuple[str, str, str]]:
    """CHANGELOG.md's sections as (version or 'Unreleased', date or '', body), top to bottom, after checking the form:
    '# Changelog', then '## [Unreleased]' first, then '## [X.Y.Z] - YYYY-MM-DD' newest first; '###' groups only of
    Keep a Changelog; no relative links (the text is read in the ZIP and on GitHub, where they would point nowhere)."""
    lines = text.replace("\r\n", "\n").split("\n")
    if lines[0] != "# Changelog":
        fail("CHANGELOG.md: the first line must be '# Changelog'")
    for target in LINK.findall(text):
        if not re.match(r"^https?://", target):
            fail(f"CHANGELOG.md: link '{target}' is not absolute; the ZIP and the GitHub release have no docs folder")
    sections, cur = [], None
    for ln in lines[1:]:
        if ln.startswith("## "):
            m = CHANGE_HEAD.match(ln)
            if not m:
                fail(f"CHANGELOG.md: bad heading {ln!r}: '## [Unreleased]' or '## [X.Y.Z] - YYYY-MM-DD'")
            cur = [m.group(1), m.group(2) or "", []]
            sections.append(cur)
        elif cur is not None:
            if ln.startswith("### ") and ln[4:] not in CHANGE_GROUPS:
                fail(f"CHANGELOG.md: group {ln!r}; groups are {', '.join(CHANGE_GROUPS)}")
            cur[2].append(ln)
    if not sections or sections[0][0] != "Unreleased" or sections[0][1]:
        fail("CHANGELOG.md: the first section must be '## [Unreleased]' (without a date)")
    seen = []
    for ver, date, _ in sections[1:]:
        if not VERSION_RE.match(ver) or not re.match(r"^\d{4}-\d{2}-\d{2}$", date):
            fail(f"CHANGELOG.md: section [{ver}] needs a version MAJOR.MINOR.PATCH and a date YYYY-MM-DD")
        try:
            time.strptime(date, "%Y-%m-%d")
        except ValueError:
            fail(f"CHANGELOG.md: section [{ver}]: no such date {date}")
        if ver in seen:
            fail(f"CHANGELOG.md: section [{ver}] twice")
        seen.append(ver)
    return [(v, d, "\n".join(b).strip("\n")) for v, d, b in sections]


def changes_of(text: str, version: str, test: bool) -> str:
    """The body of CHANGELOG.md's section for this release: what changed since the previous one."""
    sections = changelog_sections(text)
    found = {v: b for v, _, b in sections}
    if not test:
        if version not in found:
            fail(f"CHANGELOG.md has no section [{version}]: move the Unreleased changes to '## [{version}] - <date>'")
        if sections[1][0] != version:
            fail(f"CHANGELOG.md: [{version}] must be the newest section, under Unreleased")
        if found["Unreleased"]:
            fail("CHANGELOG.md: the Unreleased section is not empty; move its changes into the release's section")
    body = found.get(version) or found["Unreleased"]
    if not body and test and len(sections) > 1:
        body = sections[1][2]                   # a test build right after a release: the newest release's changes
    if not body:
        fail(f"CHANGELOG.md: no changes for {version}")
    return body


def unwrap(text: str) -> str:
    """Joins the wrapped lines of a paragraph or list item: GitHub shows every line break of a release text."""
    out: list[str] = []
    for ln in text.split("\n"):
        prev = out[-1] if out else ""
        starts = not ln.strip() or ln.startswith(("#", "- ", "* ", "|", "```")) or re.match(r"^\d+\. ", ln)
        if not starts and prev.strip() and not prev.startswith(("#", "|", "```")):
            out[-1] = prev + " " + ln.strip()
        else:
            out.append(ln)
    return "\n".join(out)


def github_notes(info: dict, digest: str) -> str:
    """The text of the GitHub release (its title is set on GitHub: 'OMKit <version>')."""
    mods = ", ".join(f"{m} {v}" for m, v in info["modules"].items())
    test = "**TEST BUILD - not for publishing.**\n\n" if info["test"] else ""
    return f"""{test}OMKit {info['version']} for the {info['camera']}, firmware {info['firmware_version']}.

{unwrap(info['changes'])}

---

- **Versions:** core {info['core']}, {mods}.
- **Install:** extract the ZIP and follow `INSTALL.txt`. Already installed OMKit? No flashing again: update the `OMKIT` folder on the card and keep your `OMKIT.INI` (`INSTALL.txt`, part B).
- **All changes:** `CHANGELOG.md` in the ZIP.
- **ZIP SHA-256:** `{digest}`
"""


def release_notes(info: dict) -> str:
    mods = "\n".join(f"| `{m}` | {v} |" for m, v in info["modules"].items())
    test = "\n**TEST BUILD - not for publishing.**\n" if info["test"] else ""
    return f"""# OMKit {info['version']} for the {info['camera']}, firmware {info['firmware_version']}
{test}
Install guide: `INSTALL.txt`. Changes: `CHANGELOG.md`. Checksums of every file: `SHA256SUMS.txt`.

| Part | Version |
|---|---|
| `core` | {info['core']} |
{mods}

The card folder `OMKIT/` holds `CORE.BIN`, the modules above in `MODULES/`, {info['grain_tiles']} grain tiles in
`GRAIN/`, {info['presets']} FILMLOOK presets in `FILMLOOK/` and the settings file `OMKIT.INI`, in which the modules above
are on. Updating an earlier OMKit release for this firmware needs no firmware tool and no firmware update: copy these
files into the card's `OMKIT` folder, all but `OMKIT.INI`, to keep your settings (`INSTALL.txt`, part B).

`firmware_tool/` makes the update files from the official firmware {info['firmware_version']}
(`{info['official_name']}`, SHA-256 `{info['official_sha256']}`). The patched file it writes must have the SHA-256
`{info['patched_sha256']}` ({info['patched_size']} bytes); the tool checks this itself.
"""


TEST_BANNER = "\n*** TEST BUILD - NOT A RELEASE, DO NOT PUBLISH OR GIVE TO CAMERA OWNERS ***\n"
TEST_TEXT = ("This is a TEST build of OMKit, made for checking the release process. It is not a release and must not be "
             "published or given to camera owners.\n")


# --- assembling --------------------------------------------------------------------------------------------------

def eol(name: str, data: bytes) -> bytes:
    if name.endswith(".bat"):
        return data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    if name.endswith((".sh", ".command", ".py", ".json", ".md", ".txt")):
        return data.replace(b"\r\n", b"\n")
    return data


def is_firmware(data: bytes) -> bool:
    """A firmware container component header: 'OE', model, then a payload length that fits the file."""
    if len(data) < 64 or data[:2] != b"OE":
        return False
    length = struct.unpack_from("<I", data, 0x0C)[0]
    return 0 < length and length % 16 == 0 and 64 + length <= len(data)


def git_state() -> tuple[str, bool, int]:
    """(commit, clean, commit time) of the tree holding OMKit; ('', False, now) without git."""
    try:
        head = subprocess.run(["git", "-C", str(OMKIT), "rev-parse", "HEAD"], capture_output=True, text=True,
                              check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(OMKIT), "status", "--porcelain", "--", "."], capture_output=True,
                               text=True, check=True).stdout.strip()
        when = int(subprocess.run(["git", "-C", str(OMKIT), "log", "-1", "--format=%ct"], capture_output=True,
                                  text=True, check=True).stdout.strip())
        return head, not dirty, when
    except (OSError, subprocess.CalledProcessError, ValueError):
        return "", False, int(time.time())


LICENCE_FILES = ("LICENSE", "LICENSE-CC-BY-4.0", "NOTICE")   # OMKit/: Apache-2.0, CC BY 4.0 (docs, data), the notice

def collect(args, platform_dir: Path, card: Path) -> tuple[dict[str, bytes], dict]:
    platform, patch = check_platform(platform_dir)
    content = check_card(card)
    commit, clean, when = git_state()
    if not args.test and not clean:
        fail("the OMKit tree has uncommitted changes (or is not a git checkout); commit first, or use --test")
    licences = [OMKIT / n for n in LICENCE_FILES]
    missing = [str(p) for p in licences if not p.is_file()]
    if not args.test and missing:
        fail(f"{', '.join(missing)} missing: a release needs the licences and the notice (use --test for a test build)")

    files: dict[str, bytes] = {}
    for p in sorted(card.rglob("*")):
        if p.is_file():
            files["OMKIT/" + p.relative_to(card).as_posix()] = p.read_bytes()
    for name in USER_FILES:
        files["firmware_tool/" + name] = eol(name, (RELEASE / "user" / name).read_bytes())
    for name in ("platform.json", "patch.json"):
        files["firmware_tool/" + name] = eol(name, (platform_dir / name).read_bytes())
    guide = (platform_dir / "INSTALL.txt").read_text("ascii")
    for key in ("{version}", "{test}"):
        if guide.count(key) != 1:
            fail(f"{platform_dir / 'INSTALL.txt'}: {key} must appear exactly once")
    guide = guide.replace("{version}", args.version).replace("{test}", TEST_BANNER if args.test else "")
    files["INSTALL.txt"] = guide.replace("\r\n", "\n").replace("\n", "\r\n").encode("ascii")   # CRLF: any text editor
    for p in licences:
        if p.is_file():
            files[p.name] = eol(p.name + ".txt", p.read_bytes())
    changelog = (OMKIT / "CHANGELOG.md").read_text("utf-8")
    changes = changes_of(changelog, args.version, args.test)
    files["CHANGELOG.md"] = eol("CHANGELOG.md", changelog.encode("utf-8"))
    if args.test:
        files[TEST_MARK] = TEST_TEXT.encode("ascii")
    info = {
        "name": "OMKit", "version": args.version, "test": args.test,
        "platform": platform["platform"], "firmware": platform["firmware"], "camera": platform["camera"],
        "firmware_version": platform["firmware_version_text"],
        "core": content["core"], "modules": content["modules"],
        "grain_tiles": content["grain_tiles"], "presets": content["presets"],
        "official_name": platform["official_file"]["name"], "official_sha256": platform["official_file"]["sha256"],
        "patched_size": patch["output"]["size"], "patched_sha256": patch["output"]["sha256"],
        "source_commit": commit, "source_clean": clean,
    }
    files["RELEASE.md"] = release_notes(info).encode("utf-8")
    files["RELEASE.json"] = (json.dumps(info, indent=1) + "\n").encode("utf-8")
    info["changes"] = changes
    for name, data in files.items():
        if len(data) > MAX_FILE or is_firmware(data) or re.search(r"(^|/)(OLY_|E\d{3}\d{4}\.BIN$)", name, re.I):
            fail(f"{name}: looks like a firmware file; a release never holds one")
    files["SHA256SUMS.txt"] = "".join(f"{sha256(files[n])}  {n}\n" for n in sorted(files)).encode("ascii")
    info["time"] = when
    return files, info


def write_zip(path: Path, top: str, files: dict[str, bytes], when: int) -> None:
    stamp = time.gmtime(max(when, 315532800))[:6]           # ZIP times start in 1980
    tmp = path.with_name(path.name + ".tmp")
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in sorted(files):
            zi = zipfile.ZipInfo(f"{top}/{name}", stamp)
            zi.create_system = 3                                 # Unix: the permission bits below are honoured
            zi.external_attr = (0o100755 if name.endswith(EXECUTABLE) else 0o100644) << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, files[name])
    os.replace(tmp, path)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Build an OMKit release ZIP for one platform and firmware.")
    ap.add_argument("--version", required=True, help="OMKit release version, e.g. 0.1.0")
    ap.add_argument("--platform", default="om5m2/fw1100", help="platform/firmware (a folder of release/platforms)")
    ap.add_argument("--card", type=Path, help="use this built OMKIT folder instead of running make card")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="output folder (default: OMKit/build/release)")
    ap.add_argument("--test", action="store_true", help="a TEST build: no LICENSE needed, modified tree allowed")
    args = ap.parse_args(argv)
    if not VERSION_RE.match(args.version):
        fail(f"version {args.version!r}: expected MAJOR.MINOR.PATCH[-suffix]")
    platform_dir = RELEASE / "platforms" / args.platform
    if not (platform_dir / "platform.json").is_file():
        fail(f"no platform {args.platform} in {RELEASE / 'platforms'}")
    plat, fw = args.platform.split("/")
    top = f"OMKit-{args.version}-{plat}-{fw}" + ("-TEST" if args.test else "")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    card = args.card.resolve() if args.card else build_card(out / f"{top}.build")
    files, info = collect(args, platform_dir, card)
    zpath = out / f"{top}.zip"
    write_zip(zpath, top, files, info["time"])
    digest = sha256(zpath.read_bytes())
    (out / f"{top}.zip.sha256").write_text(f"{digest}  {zpath.name}\n", encoding="ascii", newline="\n")
    (out / f"{top}.notes.md").write_text(github_notes(info, digest), encoding="utf-8", newline="\n")
    print(f"{zpath}\n  {len(files)} files, {zpath.stat().st_size} bytes, SHA-256 {digest}")
    print(f"  core {info['core']}, " + ", ".join(f"{m} {v}" for m, v in info["modules"].items()))
    if args.test:
        print("  TEST build: not for publishing")
    print(f"  GitHub release text: {out / (top + '.notes.md')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
