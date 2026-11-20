"""The bundled FILMLOOK preset set (OMKit/resources/modules/filmlook/presets): check it against its manifest and copy it
to a card.

    python3 OMKit/tools/presetset.py check [DIR]
    python3 OMKit/tools/presetset.py copy DEST [DIR]

DIR is the set folder (default: OMKit/resources/modules/filmlook/presets) with manifest.json and the .ISP files it lists.
`check` verifies every listed file: its size and SHA-256, and the file itself (docs/formats/isp.md: header and its CRC,
the directory, every section's CRC, the platform, the name, the ISP block's size and layout, the cassette header, META
as plain ASCII key=value lines without the key look) - the checks FILMLOOK makes when it lists the presets, plus the manifest's name. A .ISP
in DIR that the manifest does not list is an error too, so the folder and the manifest cannot drift apart. `copy`
checks first, then writes the listed files to DEST (the card's OMKIT/FILMLOOK). Other .ISP files in DEST are kept:
they are the user's own presets.

Standard library only: a release must not depend on the tools that made the presets.
"""
import hashlib
import json
import shutil
import struct
import sys
import zlib
from pathlib import Path

SET_DIR = Path(__file__).resolve().parent.parent / "resources" / "modules" / "filmlook" / "presets"
MAX_PRESETS = 64                                # OMK_LOOK_MAX: FILMLOOK lists at most 64 presets
NAME_MAX = 39                                   # OMK_NAME_MAX - 1: a long file name with its extension
MAX_FILE = 16383                                # FILMLOOK reads at most 16 KiB less a byte
HEADER, DIR_ENTRY, MAX_SECTIONS = 64, 16, 16
PLATFORM, ISPB_SIZE, ISPB_LAYOUT = "OM5M2", 5064, 1
FAT_BAD = set('"*/:<>?\\|')


def _crc(b: bytes) -> int:
    return zlib.crc32(b) & 0xFFFFFFFF


def parse(data: bytes) -> dict:
    """Header and sections of a format 1 .isp; ValueError with the reason if it is not valid."""
    if len(data) < HEADER or data[:4] != b"OISP":
        raise ValueError("no OISP magic")
    if len(data) > MAX_FILE:
        raise ValueError(f"{len(data)} bytes: larger than FILMLOOK reads")
    _, vmaj, _, plat, count, flags, size, diroff, crc, raw_name = struct.unpack_from("<4sHH8sHHIII32s", data)
    if vmaj != 1:
        raise ValueError(f"format {vmaj}")
    if size != len(data) or flags or not 1 <= count <= MAX_SECTIONS or diroff != HEADER:
        raise ValueError(f"size field {size}, flags {flags}, {count} sections, directory at {diroff}")
    directory = data[diroff:diroff + DIR_ENTRY * count]
    if _crc(data[:28] + bytes(4) + data[32:HEADER] + directory) != crc:
        raise ValueError("header CRC")
    if plat.rstrip(b"\0").decode("ascii", "replace") != PLATFORM:
        raise ValueError(f"platform {plat!r}")
    name = raw_name.split(b"\0")[0]
    if (not name or raw_name[len(name):].strip(b"\0") or len(name) > 31 or name != name.strip()
            or not all(32 <= c < 127 for c in name)):
        raise ValueError("name: 1..31 printable ASCII characters, no spaces at the ends")
    sections = {}
    for i in range(count):
        tag, off, n, scrc = struct.unpack_from("<4sIII", directory, DIR_ENTRY * i)
        payload = data[off:off + n]
        if off % 16 or len(payload) != n or _crc(payload) != scrc or tag in sections:
            raise ValueError(f"section {tag!r}: offset, size, CRC or a repeated tag")
        sections[tag] = payload
    ispb = sections.get(b"ISPB")
    if ispb is None or len(ispb) != ISPB_SIZE or struct.unpack_from("<H", ispb)[0] != ISPB_LAYOUT:
        raise ValueError("no ISP block of the OM5M2 layout 1")
    out = {"name": name.decode("ascii"), "sections": [t.decode("ascii", "replace") for t in sections]}
    cani = sections.get(b"CANI")
    if cani is not None:
        if len(cani) < 64 or struct.unpack_from("<HHHH", cani) != (1, 1, 256, 256):
            raise ValueError("cassette: not a version 1 drawing of 256 x 256")
        if struct.unpack_from("<I", cani, 12)[0] != len(cani) - 64:
            raise ValueError("cassette: its size field")
    meta = sections.get(b"META")
    if meta is not None:
        try:
            text = meta.decode("ascii")
        except UnicodeDecodeError:
            raise ValueError("META: not plain ASCII") from None
        keys = [ln.split("=", 1)[0] for ln in text.splitlines()]
        if any("=" not in ln for ln in text.splitlines()):
            raise ValueError("META: a line without key=value")
        if "look" in keys:
            raise ValueError("META: the key look (the source look's name) is not written in a release file")
    return out


def check(folder: Path = SET_DIR) -> list[dict]:
    """The manifest's entries after every check; SystemExit with all problems found otherwise."""
    man = json.loads((folder / "manifest.json").read_text(encoding="ascii"))
    presets, bad = man["presets"], []
    files = [p["file"] for p in presets]
    if len(set(f.upper() for f in files)) != len(files) or not 1 <= len(files) <= MAX_PRESETS:
        bad.append(f"{len(files)} entries or a repeated file name (1..{MAX_PRESETS} distinct)")
    names = [p["name"] for p in presets]
    if len(set(names)) != len(names):
        bad.append("a repeated preset name")
    for e in presets:
        f = e["file"]
        stem, dot, ext = f.rpartition(".")
        if (not dot or ext != "ISP" or not stem or len(f) > NAME_MAX or stem != stem.upper() or stem[0] in ". "
                or not all(32 < ord(c) < 127 and c not in FAT_BAD for c in stem)):
            bad.append(f"{f}: not an upper-case .ISP name of up to {NAME_MAX} characters")
            continue
        p = folder / f
        if not p.is_file():
            bad.append(f"{f}: missing")
            continue
        data = p.read_bytes()
        if len(data) != e["size"] or hashlib.sha256(data).hexdigest() != e["sha256"]:
            bad.append(f"{f}: size or SHA-256 differs from the manifest")
            continue
        try:
            got = parse(data)
        except ValueError as err:
            bad.append(f"{f}: {err}")
            continue
        if got["name"] != e["name"]:
            bad.append(f"{f}: name {got['name']!r} differs from the manifest")
    extra = sorted(p.name for p in folder.iterdir() if p.suffix.upper() == ".ISP" and p.name not in files)
    if extra:
        bad.append(f"not in the manifest: {', '.join(extra)}")
    if bad:
        raise SystemExit("preset set check failed:\n  " + "\n  ".join(bad))
    return presets


def copy(dest: Path, folder: Path = SET_DIR) -> list[dict]:
    presets = check(folder)
    dest.mkdir(parents=True, exist_ok=True)
    for e in presets:
        shutil.copyfile(folder / e["file"], dest / e["file"])
    return presets


def main(argv: list[str]) -> int:
    if len(argv) >= 1 and argv[0] == "check" and len(argv) <= 2:
        presets = check(Path(argv[1]) if len(argv) == 2 else SET_DIR)
        for e in presets:
            print(f"{e['file']:17} {e['name']}")
        print(f"{len(presets)} presets: OK")
        return 0
    if len(argv) in (2, 3) and argv[0] == "copy":
        presets = copy(Path(argv[1]), Path(argv[2]) if len(argv) == 3 else SET_DIR)
        print(f"{argv[1]}: {len(presets)} presets")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
