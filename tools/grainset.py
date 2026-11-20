"""The bundled grain tile set (OMKit/resources/modules/filmlook/grain): check it against its manifest and copy it to a card.

    python3 OMKit/tools/grainset.py check [DIR]
    python3 OMKit/tools/grainset.py copy DEST [DIR]

DIR is the set folder (default: OMKit/resources/modules/filmlook/grain) with manifest.json and the .GRN containers it lists.
`check` verifies every listed file: its size and SHA-256, and the container itself (docs/formats/grn.md: header and
its CRC, name, side, gains, pixel CRC, largest value and mean) - the same checks core makes at power-on, plus the
manifest's name, side and gains. A .GRN in DIR that the manifest does not list is an error too, so the folder and the
manifest cannot drift apart. `copy` checks first, then writes the listed files to DEST (the card's OMKIT/GRAIN) and
removes every other .GRN there: core shows all containers it finds in that folder.

Standard library only: a release must not depend on the generator that made the tiles.
"""
import hashlib
import json
import shutil
import struct
import sys
import zlib
from pathlib import Path

SET_DIR = Path(__file__).resolve().parent.parent / "resources" / "modules" / "filmlook" / "grain"
MAX_TILES = 32                                  # core reads at most 32 containers
HEAD = 64
HEAD_FMT = "<4sHHII24sHHBBH3HHII"               # docs/formats/grn.md, section 2
SIDES = (128, 192, 256, 384, 512, 768)          # multiples of 64 that divide 1536
TILE_GAIN_MAX, LEVEL_GAIN_MAX, VMAX = 1024, 2047, 63


def parse(data: bytes) -> dict:
    """Header and pixels of a format 2 container; ValueError with the reason if it is not valid."""
    if len(data) < HEAD or data[:4] != b"OMGR":
        raise ValueError("no OMGR magic")
    (_, ver, head, size, hcrc, raw_name, side, mean256, vmax, flags, gain, l1, l2, l3, res1, pcrc,
     res2) = struct.unpack_from(HEAD_FMT, data)
    if ver != 2 or head != HEAD:
        raise ValueError(f"version {ver}, header size {head}")
    if zlib.crc32(data[:12] + bytes(4) + data[16:HEAD]) != hcrc:
        raise ValueError("header CRC")
    if side not in SIDES or size != HEAD + side * side or size != len(data):
        raise ValueError(f"side {side}, size field {size}, file {len(data)}")
    if flags or res1 or res2 or vmax > VMAX:
        raise ValueError("flags, reserved fields or largest value")
    name = raw_name.split(b"\0")[0]
    if (not name or raw_name[len(name):].strip(b"\0") or raw_name[23] or name != name.strip()
            or not all(32 <= c < 127 for c in name)):
        raise ValueError("name")
    if not 1 <= gain <= TILE_GAIN_MAX:
        raise ValueError(f"tile gain {gain}")
    levels = (l1, l2, l3)
    if levels == (0, 0, 0):
        levels = tuple((gain * k + 50) // 100 for k in (25, 55, 100))
    elif not (all(1 <= g <= LEVEL_GAIN_MAX for g in levels) and l1 <= l2 <= l3):
        raise ValueError(f"level gains {levels}")
    px = data[HEAD:]
    n = side * side
    if zlib.crc32(px) != pcrc or max(px) != vmax or (sum(px) * 256 + n // 2) // n != mean256:
        raise ValueError("pixel CRC, largest value or mean")
    return {"name": name.decode("ascii"), "side": side, "gain": gain, "levels": list(levels)}


def check(folder: Path = SET_DIR) -> list[dict]:
    """The manifest's entries after every check; SystemExit with all problems found otherwise."""
    man = json.loads((folder / "manifest.json").read_text(encoding="ascii"))
    tiles, bad = man["tiles"], []
    files = [t["file"] for t in tiles]
    if len(set(f.upper() for f in files)) != len(files) or not 1 <= len(files) <= MAX_TILES:
        bad.append(f"{len(files)} entries or a repeated file name (1..{MAX_TILES} distinct)")
    for t in tiles:
        p = folder / t["file"]
        stem, _, ext = t["file"].partition(".")
        if not (1 <= len(stem) <= 8 and ext == "GRN" and stem.isascii() and stem.replace("_", "").isalnum()
                and stem == stem.upper()):
            bad.append(f"{t['file']}: not an 8.3 upper-case .GRN name")
            continue
        if not p.is_file():
            bad.append(f"{t['file']}: missing")
            continue
        data = p.read_bytes()
        if len(data) != t["size"] or hashlib.sha256(data).hexdigest() != t["sha256"]:
            bad.append(f"{t['file']}: size or SHA-256 differs from the manifest")
            continue
        try:
            c = parse(data)
        except ValueError as e:
            bad.append(f"{t['file']}: {e}")
            continue
        if (c["name"], c["side"], c["gain"], c["levels"]) != (t["name"], t["side"], t["gain"], t["levels"]):
            bad.append(f"{t['file']}: header {c} differs from the manifest")
    extra = sorted(p.name for p in folder.iterdir() if p.suffix.upper() == ".GRN" and p.name not in files)
    if extra:
        bad.append(f"not in the manifest: {', '.join(extra)}")
    if bad:
        raise SystemExit("grain set check failed:\n  " + "\n  ".join(bad))
    return tiles


def copy(dest: Path, folder: Path = SET_DIR) -> list[dict]:
    tiles = check(folder)
    dest.mkdir(parents=True, exist_ok=True)
    keep = {t["file"] for t in tiles}
    for p in dest.iterdir():
        if p.suffix.upper() == ".GRN" and p.name not in keep:
            p.unlink()
    for t in tiles:
        shutil.copyfile(folder / t["file"], dest / t["file"])
    return tiles


def main(argv: list[str]) -> int:
    if len(argv) >= 1 and argv[0] == "check" and len(argv) <= 2:
        tiles = check(Path(argv[1]) if len(argv) == 2 else SET_DIR)
        for t in tiles:
            print(f"{t['file']:13} {t['name']:23} {t['side']:4}  levels {t['levels'][0]}/{t['levels'][1]}/{t['levels'][2]}")
        print(f"{len(tiles)} tiles: OK")
        return 0
    if len(argv) in (2, 3) and argv[0] == "copy":
        tiles = copy(Path(argv[1]), Path(argv[2]) if len(argv) == 3 else SET_DIR)
        print(f"{argv[1]}: {len(tiles)} tiles")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
