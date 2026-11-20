"""The cassette picture of a FILMLOOK preset: the CANI section of an .isp file (OMKit/docs/formats/isp.md, section 5a).
One kind is written:

  kind 1 - a drawing: a list of OpenVG fill/stroke records. The cassette generator canister.py makes them - the 30
           generic pictures with a profile's name and ISO baked in (generic() below) - or a drawing made elsewhere.

Kind 3 (a reference to a generic cassette whose name and ISO the camera would print at run time) is not supported:
parse() refuses it with a message saying so.

Header, 64 bytes, little endian: u16 version 1, u16 kind 1, u16 surface width 256, u16 height 256, u16 record count, u16
flags 0, u32 bytes of records, char[16] brand, char[24] name, char[8] ISO (plain ASCII, zero padded). Records follow (16
bytes + segments + coordinates)."""
from __future__ import annotations

import re
import struct

VERSION = 1
KIND_DRAW = 1
KIND_GENERIC = 3                                # not supported; parse() refuses it
SURFACE = 256
HEADER = 64
MAX_RECORDS = 64
MAX_BYTES = 6144                                # bytes of records; a generic with a long baked-in name needs up to ~5 KB
MAX_SEG = 352
MAX_PTS = 320
_COORDS = {0: 0, 2: 1, 4: 1, 10: 2, 12: 3}      # OpenVG absolute segments: CLOSE, MOVE_TO, LINE_TO, QUAD_TO, CUBIC_TO

# Fixed ids and order (canister.GENERICS; new ones are appended, never renumbered); labels as the apps show them
GENERICS = [
    ("filmlook", "Film Look (neutral)"),
    ("yellow", "Yellow, Kodak-like"),
    ("green", "Green, Fuji-like"),
    ("bw", "Black-and-white film"),
    ("slide", "Slide film"),
    ("cine", "Motion picture film"),
    # 6-24: pictures of reference looks, without text
    ("p_c200", "Green, white band"),
    ("p_pro400h", "Light, teal band"),
    ("p_provia", "Blue, white corner"),
    ("p_velvia", "Purple, crimson corner"),
    ("p_xtra400", "Green, red band"),
    ("p_doublex", "Dark grey, light stripes"),
    ("p_ekta100", "Light, blue band"),
    ("p_ektar", "Red, orange band"),
    ("p_gold200", "Yellow, red band"),
    ("p_kchrome", "Yellow, red corner"),
    ("p_portra", "Cream, ochre band"),
    ("p_portra_push", "Cream, two stripes"),
    ("p_trix", "Yellow, black corner"),
    ("p_umax400", "Yellow, blue and red stripes"),
    ("p_verita", "Teal, white band"),
    ("p_v200t", "Grey, orange band"),
    ("p_v250d", "Grey, blue band"),
    ("p_v500t", "Grey, red band"),
    ("p_v50d", "Grey, yellow band"),
    # 25-29: instant film and an old process
    ("instant", "Instant film pack (colour)"),
    ("instant_bw", "Instant film pack (black and white)"),
    ("mini", "Mini instant cartridge (colour)"),
    ("mini_bw", "Mini instant cartridge (black and white)"),
    ("wetplate", "Old process, cassette (wood, brass)"),
]
DEFAULT_GENERIC = 0
_ID = {k: i for i, (k, _) in enumerate(GENERICS)}

_KEYS = [(("kodak", "portra", "ektar", "gold", "ultramax", "colorplus", "ektachrome", "kodachrome", "tri-x", "trix",
           "t-max", "tmax", "vision", "double-x"), 1),
         (("fuji", "velvia", "provia", "astia", "superia", "acros", "pro 400h", "c200", "x-tra", "eterna", "neopan"), 2)]
_BW = ("tri-x", "trix", "double-x", "hp5", "fp4", "delta", "acros", "t-max", "tmax", "neopan", "pan ", "panf", "ilford",
       "foma", "rollei", "b&w", "bw", "mono", "kentmere", "ortho", "collodion")
_SLIDE = ("velvia", "provia", "astia", "ektachrome", "kodachrome", "slide", "e-6", "e6", "agfachrome", "fujichrome")
_CINE = ("vision", "cinestill", "eterna", "5207", "5219", "5203", "5213", "7203", "7207", "800t", "500t", "250d", "50d",
         "200t")
_OLD = ("collodion", "wet plate", "wetplate", "wet-plate", "tintype", "ambrotype", "daguerre")
_MINI = ("instax",)
_INSTANT = ("polaroid", "instant", "sx-70", "sx70", "i-type", "itype", "impossible", "fp-100", "fp100", "fp-3000",
            "fp3000", "peel-apart")


def default_generic(look_name: str, mono: bool = False) -> int:
    """A generic for a look by its name: an old process (wet plate), instant film (mini, then pack; black-and-white or
    colour), black-and-white, motion picture, slide, then the maker's colour, else Film Look"""
    n = (look_name or "").lower()
    bw = mono or any(k in n for k in _BW) or "3000b" in n
    if any(k in n for k in _OLD):
        return _ID["wetplate"]
    if any(k in n for k in _MINI):
        return _ID["mini_bw" if bw else "mini"]
    if any(k in n for k in _INSTANT):
        return _ID["instant_bw" if bw else "instant"]
    if bw:
        return _ID["bw"]
    if any(k in n for k in _CINE):
        return _ID["cine"]
    if any(k in n for k in _SLIDE):
        return _ID["slide"]
    for keys, g in _KEYS:
        if any(k in n for k in keys):
            return g
    return DEFAULT_GENERIC


def guess_iso(look_name: str) -> str:
    """The film speed from a look's name: the last 2-4 digit number, with a letter after it (500T, 400H)"""
    m = re.findall(r"(?<![\d.])(\d{2,4})([A-Za-z]?)(?![A-Za-z\d.])", (look_name or "").replace("_", " "))
    return (m[-1][0] + m[-1][1].upper()) if m else ""


def _text(s: str, n: int) -> bytes:
    b = "".join(c if 32 <= ord(c) < 127 else "?" for c in (s or "")).encode("ascii")[:n - 1]
    return b + b"\0" * (n - len(b))


def generic(index: int, name: str = "", iso: str = "", brand: str | None = None, proc: str | None = None) -> bytes:
    """The CANI section (kind 1) of generic picture `index` with the profile's name and ISO drawn on its label; brand and
    proc - other lines for rows A and C (canister.generic)"""
    from . import canister          # here, not at the top: canister imports this module
    return canister.generic(index, name, iso, brand, proc)


def parse(data: bytes) -> dict:
    """Check a CANI section as core would (raises ValueError): the header, and every record's sizes, segments and
    coordinate ranges"""
    if len(data) < HEADER:
        raise ValueError("shorter than the 64-byte header")
    ver, kind, w, h, count, flags, size = struct.unpack_from("<HHHHHHI", data)
    texts = [data[o:o + n].split(b"\0")[0].decode("ascii", "replace") for o, n in ((16, 16), (32, 24), (56, 8))]
    out = {"version": ver, "kind": kind, "brand": texts[0], "name": texts[1], "iso": texts[2]}
    if ver != VERSION:
        raise ValueError(f"version {ver}, expected {VERSION}")
    if (w, h) != (SURFACE, SURFACE):
        raise ValueError(f"surface {w}x{h}, expected {SURFACE}x{SURFACE}")
    if kind == KIND_GENERIC:
        raise ValueError("kind 3 (a reference to a generic cassette) is no longer supported: a generic cassette is a "
                         "drawing (kind 1) with the name and ISO baked in - make the file again")
    if kind != KIND_DRAW:
        raise ValueError(f"kind {kind} is not supported")
    if flags:
        raise ValueError(f"flags {flags:#x}, expected 0")
    if not 1 <= count <= MAX_RECORDS or size > MAX_BYTES or len(data) != HEADER + size:
        raise ValueError(f"{count} records / {size} bytes: outside the limits ({MAX_RECORDS} records, {MAX_BYTES} "
                         f"bytes) or not the section's size")
    o = HEADER
    for r in range(count):
        if o + 16 > len(data):
            raise ValueError(f"record {r}: cut short")
        op, caps, _, _, width, nseg, npts = struct.unpack_from("<BBHIIHH", data, o)
        if op not in (1, 2, 3) or caps > 2:
            raise ValueError(f"record {r}: operation {op} / caps {caps}")
        if op == 3 and not 0x3c000000 <= width <= 0x41800000:
            raise ValueError(f"record {r}: stroke width bits {width:#x}")
        if not 1 <= nseg <= MAX_SEG or npts > MAX_PTS:
            raise ValueError(f"record {r}: {nseg} segments, {npts} points")
        segs = data[o + 16:o + 16 + nseg]
        if len(segs) != nseg or segs[0] != 2 or any(s not in _COORDS for s in segs):
            raise ValueError(f"record {r}: bad segments")
        if sum(_COORDS[s] for s in segs) != npts:
            raise ValueError(f"record {r}: {npts} points do not match the segments")
        c = o + 16 + nseg + (-nseg % 4)
        if c + 4 * npts > len(data):
            raise ValueError(f"record {r}: cut short")
        xy = struct.unpack_from(f"<{2 * npts}h", data, c)
        if any(not 0 <= v <= 4 * SURFACE for v in xy):
            raise ValueError(f"record {r}: a coordinate outside the surface")
        o = c + 4 * npts
    if o != len(data):
        raise ValueError("the records do not fill the section")
    out["records"] = count
    return out
