"""The .isp profile file (format 1.0, spec: OMKit/docs/formats/isp.md): writer and reader.

A small binary file the camera applies for free: a header (display name, target platform), resources (an optional icon)
and the platform's ISP block - the colour path tables in the camera's own formats. All little-endian; sections are
16-byte aligned and listed in a directory with their CRC-32, so the camera reads only what it needs."""
from __future__ import annotations

import struct
import warnings
import zlib

import numpy as np

from . import model

MAGIC = b"OISP"
VERSION = (1, 0)
HEADER = 64
DIR_ENTRY = 16
ALIGN = 16

# OM5M2 ISP block (tag ISPB), layout 1
OM5M2_LAYOUT = 1
PRESENT_MATRIX, PRESENT_GAMMA, PRESENT_TONE, PRESENT_SATURATION, PRESENT_SECTORS = 1, 2, 4, 8, 16
NATURAL = 2                                   # MODE_COLORTONE value of Natural: the base mode
BASE_MODES = {NATURAL: "Natural"}             # ISPB +4: the base modes by name
ISPB_SIZE = 5064

ICON_KINDS = {"mode": 1, "mode_large": 2, "panel": 3, "shoot": 4, "label": 5}


recip = model.recip      # the sector record's reciprocal width field (E<<8 | M)


def _crc(b: bytes) -> int:
    return zlib.crc32(b) & 0xFFFFFFFF


def _pad(b: bytes) -> bytes:
    return b + b"\0" * (-len(b) % ALIGN)


def ispb_om5m2(hw: model.Hardware, base_mode=NATURAL, matrix=None) -> bytes:
    """The OM5M2 ISP block from quantised tables (model.Hardware). matrix: mark the look's matrix F present (core >= 1.00:
    S+80 = F * S+80 of the frame); None - when it is not the identity."""
    if matrix is None:
        matrix = not np.array_equal(np.asarray(hw.matrix_q7), np.eye(3, dtype=int) * 128)
    present = PRESENT_GAMMA | PRESENT_TONE | PRESENT_SATURATION | PRESENT_SECTORS | (PRESENT_MATRIX if matrix else 0)
    for name, top in (("hw", model.FULL_TURN), ("cw", 1023), ("c0", 1023), ("yw", 1023), ("y0", 1023)):
        v = np.asarray(getattr(hw, name))
        if v.min() < 0 or v.max() > top:
            raise ValueError(f"sector field {name} outside 0..{top} (a width of 1024 reads as 0 on the camera)")
    out = struct.pack("<HHHH", OM5M2_LAYOUT, present, base_mode, 0)
    out += struct.pack("<9h", *hw.matrix_q7.ravel().tolist()) + b"\0\0"
    out += struct.pack("<HH", hw.k_q, int(round(hw.k_q * 182 / 144)))
    out += np.asarray(hw.gamma12, "<u2").tobytes() + np.asarray(hw.tone10, "<u2").tobytes()
    out += struct.pack("<HHI", int(getattr(hw, "mask", model.MASK_ALL)) & model.MASK_ALL, 0, 0)   # the sector mask
    for k in range(12):
        out += struct.pack("<9H3h", int(hw.h0[k]), int(hw.hw[k]), int(hw.c0[k]), int(hw.cw[k]), int(hw.y0[k]),
                           int(hw.yw[k]), recip(int(hw.hw[k])), recip(int(hw.cw[k])), recip(int(hw.yw[k])),
                           int(hw.a_hue[k]), int(hw.a_sat[k]), int(hw.a_lum[k]))
    # 7 profiles of 16 nodes per sector, model.PROFILE_ORDER: H/C of chroma, H/C of hue, H/C of luma, Y; node values Q14,
    # -1.0..63/64 (core rounds them to 1/64 and builds the hardware words: value | slope to the next node)
    prof = np.asarray(hw.prof)
    if prof.min() < model.PROF_MIN or prof.max() > model.PROF_ONE:
        raise ValueError(f"profile values {prof.min()}..{prof.max()} outside {model.PROF_MIN}..{model.PROF_ONE}")
    out += prof.astype("<i2").reshape(12, 7, 16).tobytes()
    assert len(out) == ISPB_SIZE, len(out)
    return out


def icon_section(icons) -> bytes:
    """icons: list of (kind name, width, height, pixels) - pixels (h, w) of OMK_ICON_* values 0..15, packed 4 bits per
    pixel, high nibble first, every row a whole number of bytes (the OMKit module picture format)."""
    entries, data = [], b""
    base = 4 + 16 * len(icons)
    for kind, w, h, px in icons:
        px = np.asarray(px, np.uint8).reshape(h, w)
        rows = []
        for r in px:
            r = np.concatenate([r, [0]]) if w % 2 else r
            rows.append(((r[0::2] << 4) | r[1::2]).astype(np.uint8).tobytes())
        blob = _pad(b"".join(rows))
        entries.append(struct.pack("<HHHHII", ICON_KINDS[kind], w, h, 0, base + len(data), len(b"".join(rows))))
        data += blob
    return struct.pack("<HH", len(icons), 0) + b"".join(entries) + data


META_DROPPED = ("look",)     # META keys no longer written: the source look's name (older files may still carry it)


def meta_of(payload: bytes) -> dict:
    """META payload -> dict of its key=value lines. Keys of META_DROPPED are left out: readers do not use them"""
    out = dict(x.split("=", 1) for x in payload.decode("utf-8", "replace").splitlines() if "=" in x)
    for k in META_DROPPED:
        out.pop(k, None)
    return out


def write(name: str, platform: str, ispb: bytes, icons=None, meta: dict | None = None, extra=()) -> bytes:
    """extra: more sections as (tag, payload), e.g. (b"CANI", the cassette picture of cani.py). META: key=value lines
    (provenance); a key of META_DROPPED is never written - left out with a warning"""
    nb = name.encode("utf-8")
    if len(nb) > 31:
        raise ValueError("the name is longer than 31 bytes in UTF-8")
    bad = [k for k in META_DROPPED if meta and k in meta]
    if bad:
        warnings.warn(f"META key(s) {', '.join(bad)} not written (no longer part of META)", stacklevel=2)
        meta = {k: v for k, v in meta.items() if k not in META_DROPPED}
    sections = [(b"ISPB", ispb)]
    if icons:
        sections.append((b"ICON", icon_section(icons)))
    if meta:
        sections.append((b"META", "".join(f"{k}={v}\n" for k, v in meta.items()).encode("utf-8")))
    sections += [(bytes(t), bytes(p)) for t, p in extra]
    offset = HEADER + DIR_ENTRY * len(sections)
    offset += -offset % ALIGN
    directory, body = b"", b""
    for tag, payload in sections:
        directory += struct.pack("<4sIII", tag, offset + len(body), len(payload), _crc(payload))
        body += _pad(payload)
    gap = b"\0" * (-(HEADER + len(directory)) % ALIGN)
    size = HEADER + len(directory) + len(gap) + len(body)
    head = struct.pack("<4sHH8sHHIII32s", MAGIC, *VERSION, platform.encode("ascii"), len(sections), 0, size, HEADER, 0,
                       nb)
    crc = _crc(head + directory)
    head = head[:28] + struct.pack("<I", crc) + head[32:]
    return head + directory + gap + body


def read(data: bytes) -> dict:
    """Parse and check a .isp file: returns the header fields and the section payloads; raises ValueError."""
    if len(data) < HEADER or data[:4] != MAGIC:
        raise ValueError("not an .isp file")
    magic, vmaj, vmin, plat, count, flags, size, diroff, crc, name = struct.unpack_from("<4sHH8sHHIII32s", data)
    if vmaj != VERSION[0]:
        raise ValueError(f"format {vmaj}.{vmin} is not supported")
    if size != len(data):
        raise ValueError("file size does not match the header")
    directory = data[diroff:diroff + DIR_ENTRY * count]
    if _crc(data[:28] + b"\0\0\0\0" + data[32:HEADER] + directory) != crc:
        raise ValueError("header CRC mismatch")
    out = {"version": (vmaj, vmin), "platform": plat.rstrip(b"\0").decode("ascii"), "flags": flags,
           "name": name.split(b"\0")[0].decode("utf-8"), "sections": {}}
    for i in range(count):
        tag, off, n, scrc = struct.unpack_from("<4sIII", directory, DIR_ENTRY * i)
        payload = data[off:off + n]
        if len(payload) != n or _crc(payload) != scrc:
            raise ValueError(f"section {tag!r}: CRC mismatch")
        out["sections"][tag.decode("ascii")] = payload
    return out


def peek_header(data: bytes) -> dict:
    """The header's fields read without the checks of read() - for showing a file read() refuses; {} when the file
    does not even start like one"""
    if len(data) < HEADER or data[:4] != MAGIC:
        return {}
    _, vmaj, vmin, plat, count, flags, size, diroff, crc, name = struct.unpack_from("<4sHH8sHHIII32s", data)
    return {"version": f"{vmaj}.{vmin}", "platform": plat.rstrip(b"\0").decode("ascii", "replace"), "sections_count": count,
            "size_header": size, "header_crc": f"{crc:08X}", "name": name.split(b"\0")[0].decode("utf-8", "replace")}


def list_sections(data: bytes) -> list:
    """The directory as the file lists it, without the checks of read(): tag, offset, size, CRC-32 and whether the
    section's data matches it"""
    try:
        count, diroff = struct.unpack_from("<H", data, 16)[0], struct.unpack_from("<I", data, 24)[0]
        out = []
        for i in range(count):
            tag, off, n, scrc = struct.unpack_from("<4sIII", data, diroff + DIR_ENTRY * i)
            payload = data[off:off + n]
            out.append({"tag": tag.decode("ascii", "replace"), "offset": off, "size": n, "crc": f"{scrc:08X}",
                        "ok": len(payload) == n and _crc(payload) == scrc})
        return out
    except struct.error:
        return []


def hardware_from_ispb(b: bytes) -> model.Hardware:
    """Back from the OM5M2 block to model.Hardware (exact round trip: per-sector windows and profiles)."""
    layout, present, base, _ = struct.unpack_from("<HHHH", b)
    if layout != OM5M2_LAYOUT:
        raise ValueError(f"OM5M2 ISP block layout {layout} is not supported")
    m = np.array(struct.unpack_from("<9h", b, 8)).reshape(3, 3)
    kcb, _kcr = struct.unpack_from("<HH", b, 28)
    gamma = np.frombuffer(b, "<u2", 768, 32).reshape(3, 256).astype(int)
    tone = np.frombuffer(b, "<u2", 256, 1568).astype(int)
    recs = np.array([struct.unpack_from("<9H3h", b, 2088 + 24 * k) for k in range(12)])
    prof = np.frombuffer(b, "<i2", 12 * 7 * 16, 2376).reshape(12, 7, 16).astype(int)
    hw = model.Hardware(m, gamma, tone, kcb, recs[:, 10], recs[:, 9], recs[:, 11])
    hw.h0, hw.hw, hw.c0, hw.cw, hw.y0, hw.yw = (recs[:, i].astype(int) for i in range(6))
    hw.prof = prof
    hw.mask = struct.unpack_from("<H", b, 2080)[0] & model.MASK_ALL
    return hw


def use_of_present(present: int) -> tuple:
    """The blocks model.run_hardware runs a block's tables with: the ones the block marks present (ISPB +2), as the camera
    applies them - matrix, saturation (k), sectors. Gamma and tone always run (the file's tables)."""
    return ((("matrix",) if present & PRESENT_MATRIX else ()) + (("sat",) if present & PRESENT_SATURATION else ())
            + (("sectors",) if present & PRESENT_SECTORS else ()))
