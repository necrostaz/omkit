"""What the camera's FILMLOOK editors show for a preset, with the same integer arithmetic as the module
(OMKit/src/modules/filmlook/edits.c, edits_show): whether the preset is black and white, every sector's own saturation in
editor steps, the hue and the swatch colour of every sector's centre, the sector mask and the tone curve the curve editor
draws. Only the unedited preset is modelled here (the module's edits are applied on the camera)."""
from __future__ import annotations

import math
import struct

ISPB_SIZE = 5064
OFF_CB, OFF_GAMMA_G, OFF_TONE, OFF_MASK, OFF_REC = 28, 32 + 512, 1568, 2080, 2088
REC, REC_SAT, SAT_STEP = 24, 20, 51            # a sector record; its chroma addition; one editor step of it
# The 64 points of the curve editor: positions on the gamma G table in 1/16 node
CURVE_AT = (0, 0, 2, 5, 9, 15, 23, 32, 44, 56, 71, 88, 106, 127, 149, 174, 200, 229, 259, 292, 327, 364, 403, 445, 488,
            534, 582, 633, 685, 740, 798, 857, 919, 984, 1050, 1120, 1191, 1265, 1342, 1421, 1502, 1586, 1672, 1761, 1852,
            1946, 2043, 2142, 2243, 2347, 2454, 2563, 2675, 2789, 2907, 3026, 3149, 3274, 3401, 3532, 3665, 3800, 3939,
            4079)


def _rd16(b, o):
    return b[o] | b[o + 1] << 8


def _at16(b, off, p):
    """A table of 256 nodes at p (1/16 node), as at16 in edits.c."""
    i, f = p >> 4, p & 15
    if i >= 255:
        a, c = _rd16(b, off + 2 * 254), _rd16(b, off + 2 * 255)
        return c + ((c - a) * ((i - 255) * 16 + f) // 16 if c > a else 0)
    a, c = _rd16(b, off + 2 * i), _rd16(b, off + 2 * i + 2)
    return (a * (16 - f) + c * f) // 16


def _c_div(a, b):
    """C division (towards zero) for a non-negative b."""
    return a // b if a >= 0 else -((-a) // b)


def curve(ispb: bytes) -> list[int]:
    """The curve the editor draws: output 0..255 at 64 points (the tone curve through the gamma G table)."""
    out = []
    for p in CURVE_AT:
        v = _at16(ispb, OFF_TONE, _at16(ispb, OFF_GAMMA_G, p))
        out.append(255 if v >= 1023 else v * 255 // 1023)
    return out


def show(ispb: bytes) -> dict:
    """What the editors show for an ISP block: mono (the Cb gain is 0), vivid (the edits' shifts: 0 here), base (every
    sector's own saturation in steps, -127..127), mask (the sector mask), hue (degrees) and rgb (the swatch) of every
    sector's centre, curve (the tone curve)."""
    vivid, base, hue, rgb = [], [], [], []
    sin64 = [round(4096 * math.sin(2 * math.pi * k / 64)) for k in range(64)]
    for i in range(12):
        o = OFF_REC + REC * i
        a = struct.unpack_from('<h', ispb, o + REC_SAT)[0]
        a = (a + SAT_STEP // 2) // SAT_STEP if a >= 0 else -((-a + SAT_STEP // 2) // SAT_STEP)
        base.append(max(-127, min(127, a)))
        vivid.append(0)
        centre = (_rd16(ispb, o) + _rd16(ispb, o + 2) // 2) % 32768
        hue.append(centre * 360 // 32768)
        k = centre * 64 // 32768
        cb = _c_div(56 * sin64[(k + 16) % 64], 4096)
        cr = _c_div(56 * sin64[k], 4096)
        c = (140 + _c_div(cr * 1402, 1000), 140 - _c_div(cb * 344 + cr * 714, 1000), 140 + _c_div(cb * 1772, 1000))
        rgb.append([max(0, min(255, v)) for v in c])
    return {"mono": _rd16(ispb, OFF_CB) == 0, "vivid": vivid, "base": base, "mask": _rd16(ispb, OFF_MASK) & 0x0FFF,
            "hue": hue, "rgb": rgb, "curve": curve(ispb)}
