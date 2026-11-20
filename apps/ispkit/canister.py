"""FILMLOOK cassette pictures in the "8 bit" style. A cassette is drawn as a pixel picture on the 256x256 surface of the
editor (palette layers, rectangles, stepped diagonals, the 5x8 font font5 that the camera driver draws with), turned into CANI v1 drawing
records (cani.py; format: OMKit/docs/formats/isp.md section 5a) and checked back by a renderer that does what the camera
does.

  generic(index, name, iso[, brand, proc])
                          - one of the 30 generic pictures (GENERICS, fixed order: 6 cassettes with a line of their
                            kind, 19 text-free pictures, 5 text-free instant packs, cartridges and a wooden cassette)
                            with a profile's name and ISO baked into the drawing: font5 glyphs as filled pixel
                            rectangles, scale 2x2, else the narrow 1x2 and truncation (fit_text) - the camera prints
                            nothing itself, the section is an ordinary drawing; brand and proc - other lines for rows A
                            and C;
  decode(blob)            - the records of a section (checked by cani.parse first, as core would);
  render(records)         - the camera's drawing: OpenVG fill (non-zero / even-odd) sampled 4x4 per pixel over the
                            backing colour; also strokes and curves of drawings made elsewhere (strokes approximately:
                            round joins instead of the default miter);
  render_png(blob)        - a PNG preview of a section;
  sheet_png(items)        - several sections side by side with captions, to choose from;
  limits(blob)            - the section's sizes against the limits of cani.py.

Picture -> records: layer k is every pixel whose final layer is >= k and touches it (the layer and what lies on it), so
shapes stay simple and drawing the records in order gives exactly the picture. A shape becomes its pixel outlines (only
horizontal and vertical edges on whole pixels, i.e. coordinates that are multiples of 4 quarter pixels), filled non-zero;
a record over the point or segment limit is cut by a column into parts. Every pixel is then fully covered or not at all:
the GPU's antialiasing has nothing to blur. Deterministic: the same input gives the same bytes.

numpy + Pillow."""
from __future__ import annotations

import io
import math
import struct

import numpy as np
from PIL import Image, ImageDraw

from . import cani

S = cani.SURFACE                        # the surface, pixels
Q = 4                                   # quarter pixels per pixel (path scale 0.25, as editor.c)
MOVE, LINE, QUAD, CUBIC, CLOSE = 2, 4, 10, 12, 0      # OpenVG absolute segments
OP_FILL, OP_FILL_EO, OP_STROKE = 1, 2, 3
BG = (46, 52, 58)                       # backing of the previews (a translucent plate over the live view on the camera)
CROP = (16, 76, 240, 196)               # x0, y0, x1, y1 of the cassette and the camera's "n / N" line under it
COUNTER_TOP = 186                       # the camera's "n / N": top of the line, pixels from the top

# ---- font5: the 5x8 font of OMKit/src/platform/om5m2/fw1100/editor.c, chars 32..126, 5 columns per char, bit 0 the top row,
# Glyphs 0x20..0x7E of the classic 5x7 font of the Adafruit GFX Library (glcdfont.c), BSD 2-Clause, Copyright (c) 2012 Adafruit Industries; see NOTICE;
# bit 7 the descender row ----
FONT5_HEX = """
0000000000 00005f0000 0007000700 147f147f14 242a7f2a12 2313086462 3649562050 0008070300 001c224100 0041221c00
2a1c7f1c2a 08083e0808 0080703000 0808080808 0000606000 2010080402 3e5149453e 00427f4000 7249494946 2141494d33
1814127f10 2745454539 3c4a494931 4121110907 3649494936 464949291e 0000140000 0040340000 0008142241 1414141414
0041221408 0201590906 3e415d594e 7c1211127c 7f49494936 3e41414122 7f4141413e 7f49494941 7f09090901 3e41415173
7f0808087f 00417f4100 2040413f01 7f08142241 7f40404040 7f021c027f 7f0408107f 3e4141413e 7f09090906 3e4151215e
7f09192946 2649494932 03017f0103 3f4040403f 1f2040201f 3f4038403f 6314081463 0304780403 6159494d43 007f414141
0204081020 004141417f 0402010204 4040404040 0003070800 2054547840 7f28444438 3844444428 384444287f 3854545418
00087e0902 18a4a49c78 7f08040478 00447d4000 2040403d00 7f10284400 00417f4000 7c04780478 7c08040478 3844444438
fc18242418 18242418fc 7c08040408 4854545424 04043f4424 3c4040207c 1c2040201c 3c4030403c 4428102844 4c9090907c
4464544c44 0008364100 0000770000 0041360800 0201020402
"""
FONT5 = [bytes.fromhex(h) for h in FONT5_HEX.split()]
ADVANCE, CAP = 6, 7                     # font5: advance 6 columns, cap height 7 rows (row 7 is the descender)
TEXT_SCALES = ((2, 2), (1, 2))          # (sx, sy) pixels per font pixel: preferred, then the narrow fallback


def glyph(ch: str) -> np.ndarray:
    """8 x 5 mask of a font5 char; outside 32..126 - '?'"""
    c = ord(ch)
    if c < 32 or c > 126:
        c = ord("?")
    g = np.zeros((8, 5), bool)
    for x, b in enumerate(FONT5[c - 32]):
        for y in range(8):
            g[y, x] = b >> y & 1
    return g


def text_w(s: str, sx: int) -> int:
    return (len(s) * ADVANCE - 1) * sx


def fit_text(s: str, w: int) -> tuple[tuple[int, int], str]:
    """Scale and text for a box w pixels wide: the first of TEXT_SCALES at which the whole line fits; else the last one
    and the line cut to what fits. Upper case, as on the named cassettes; chars outside font5 become '?'"""
    s = "".join(c if 32 <= ord(c) <= 126 else "?" for c in (s or "").upper())
    for sx, sy in TEXT_SCALES:
        if text_w(s, sx) <= w:
            return (sx, sy), s
    sx, sy = TEXT_SCALES[-1]
    return (sx, sy), s[:(w + sx) // (ADVANCE * sx)]


# ---- the pictures' parameters (a stylisation, not logos): label colour, band colours, text colour, label style, shape;
# a generic also has its own lines of row A (brand) and row C (proc), empty on the text-free ones. Optional, for a
# picture of its own (none of the generics uses them): iso_window=False - no ISO window (the ISO must then be empty),
# band_line - the colour of a 1-pixel line along both edges of row B, deco - a pattern on the label (DECO_CLIPS) ----
P = dict

# Generic pictures: indices and order are fixed (cani.GENERICS, existing .isp files), new ones only appended.
#   0-5   cassettes with a line of their kind in rows A and C (no makers): the name and ISO come from the profile;
#   6-24  pictures of reference looks without any text; a preset's own brand, name, ISO and process code are
#         drawn on one of them (generic() with brand and proc);
#   25-29 text-free instant packs and cartridges (colour, black-and-white) and an old-process cassette.
GENERICS = [
    P(id="filmlook", brand="FILM LOOK", proc="135", label=0x3a3f46, band=[0xc8ccd0], text=0x1a1d22, style="band"),
    P(id="yellow", brand="COLOR 135", proc="C-41", label=0xf2c21a, band=[0xc8202a], text=0xffffff, style="band"),
    P(id="green", brand="COLOR 135", proc="C-41", label=0x2f9a4a, band=[0xf2f2f2], text=0x1f6a34, style="band"),
    P(id="bw", brand="B&W 135", proc="B&W", label=0x1c1c1c, band=[0xe8e8e8, 0xe8e8e8], text=0xf0f0f0,
      style="stripes"),
    P(id="slide", brand="SLIDE 135", proc="E-6", label=0xe8eef4, band=[0x2a64b4], text=0xffffff, style="band"),
    P(id="cine", brand="CINE 35MM", proc="ECN-2", label=0x8c9096, band=[0xe07820], text=0x1a1a1a, style="band"),
    # pictures of reference looks, text-free
    P(id="p_c200", label=0x2f9a4a, band=[0xf2f2f2], text=0x1f6a34, style="band"),
    P(id="p_pro400h", label=0xe9eef0, band=[0x2e7d6f], text=0xffffff, style="band"),
    P(id="p_provia", label=0x1f4fa0, band=[0xffffff], text=0xffffff, style="split"),
    P(id="p_velvia", label=0x5a2a7a, band=[0xc8327a], text=0xffffff, style="split"),
    P(id="p_xtra400", label=0x23824a, band=[0xd8282a], text=0xffffff, style="band"),
    P(id="p_doublex", label=0x3a3a3a, band=[0xd0d0d0, 0xd0d0d0], text=0xf0f0f0, style="stripes"),
    P(id="p_ekta100", label=0xe8eef4, band=[0x2a64b4], text=0xffffff, style="band"),
    P(id="p_ektar", label=0xd8452a, band=[0xf0b020], text=0x101010, style="band"),
    P(id="p_gold200", label=0xf2c21a, band=[0xc8202a], text=0xffffff, style="band"),
    P(id="p_kchrome", label=0xf2c21a, band=[0xc8202a], text=0x101010, style="split"),
    P(id="p_portra", label=0xece2cc, band=[0xd8962c], text=0x4a3420, style="band"),
    P(id="p_portra_push", label=0xece2cc, band=[0xd8962c, 0x4a3420], text=0x4a3420, style="stripes"),
    P(id="p_trix", label=0xf2c21a, band=[0x101010], text=0x101010, style="split"),
    P(id="p_umax400", label=0xf2c21a, band=[0x1c3c8c, 0xc8202a], text=0x1c3c8c, style="stripes"),
    P(id="p_verita", label=0x2a8a8a, band=[0xf0f0f0], text=0x1c5c5c, style="band"),
    P(id="p_v200t", label=0xb4b8bc, band=[0xe07820], text=0x202020, style="band"),
    P(id="p_v250d", label=0xb4b8bc, band=[0x2a64b4], text=0xffffff, style="band"),
    P(id="p_v500t", label=0xb4b8bc, band=[0xc8202a], text=0xffffff, style="band"),
    P(id="p_v50d", label=0xb4b8bc, band=[0xf0c020], text=0x202020, style="band"),
    # instant film: a pack (a box seen at an angle, a stripe of colours), a mini cartridge (rounded corners, the yellow
    # mark it is aligned by); an old process: an ordinary cassette in wood and brass (pal: brass caps, a dark spool; two
    # lines of wood grain on the label)
    P(id="instant", shape="pack", label=0xf2f0ea, band=[0xe03c31, 0xf08a24, 0xf6c628, 0x5aa83c, 0x2e6db4],
      text=0x1a1a1a, style="rainbow"),
    P(id="instant_bw", shape="pack", label=0x1c1c1c, band=[0xf0f0f0, 0xbcbcbc, 0x8a8a8a, 0x5e5e5e],
      text=0xf0f0f0, style="rainbow"),
    P(id="mini", shape="cartridge", label=0xf4f6f8, band=[0x4aa8e0], text=0xffffff, style="band"),
    P(id="mini_bw", shape="cartridge", label=0x2a2a2e, band=[0xb8bcc0], text=0x1a1a1a, style="band"),
    P(id="wetplate", label=0x8c5a2c, band=[0xe6d8b8], text=0x3a2414, style="band", grain=True,
      pal=dict(cap=0xc8a040, cap_hi=0xdec58c, cap_lo=0x8c702d, spool=0x2a2826, slot=0x6a6660)),
]
GENERIC_INDEX = {g["id"]: i for i, g in enumerate(GENERICS)}

# ---- colours ----
def rgb3(c: int):
    return (c >> 16) & 255, (c >> 8) & 255, c & 255


def luma(c: int) -> float:
    r, g, b = rgb3(c)
    return 0.299 * r + 0.587 * g + 0.114 * b


def ink(bg: int, want: int, dark=0x101010, light=0xffffff) -> int:
    """The wanted text colour if it stands out from the background, else a light or a dark one"""
    if abs(luma(bg) - luma(want)) >= 90:
        return want
    return dark if luma(bg) > 140 else light


def mix(a: int, b: int, t: float) -> int:
    ra, ga, ba = rgb3(a)
    rb, gb, bb = rgb3(b)
    return (int(ra + (rb - ra) * t + .5) << 16) | (int(ga + (gb - ga) * t + .5) << 8) | int(ba + (bb - ba) * t + .5)


def palette(p: dict) -> dict:
    """Roles -> colour (8-bit console style): K outline, caps, spool, film, label, bands, light and shade rows, inks"""
    label, band = p["label"], p["band"][0]
    band2 = p["band"][1] if len(p["band"]) > 1 else band
    K = 0x101418
    return dict(K=K, cap=0xa4a8ac, cap_hi=0xe4e6e8, cap_lo=0x64686c, spool=0x34383c, slot=0x8a8e92,
                film=0x4a3018, perf=0xc8a070, label=label, band=band, band2=band2,
                label_hi=mix(label, 0xffffff, .35), label_lo=mix(label, 0x000000, .3),
                band_hi=mix(band, 0xffffff, .35), band_lo=mix(band, 0x000000, .3), iso_bg=K, iso_ink=0xf4f4f4)


# ---- the pixel picture ----
class Art:
    """Palette picture of the surface: ids[y, x] - layer number (layer order = drawing order), -1 empty"""

    def __init__(self):
        self.ids = np.full((S, S), -1, np.int16)
        self.cols: list[int] = []

    def layer(self, rgb: int) -> int:
        self.cols.append(rgb)
        return len(self.cols) - 1

    def rect(self, x0, y0, x1, y1, lay):
        self.ids[y0:y1, x0:x1] = lay

    def outlined(self, m, fill, line):
        """Shape m in colour fill with a 1-pixel line around it (4 neighbours: the corners of the line cut off)"""
        d = m.copy()
        d[1:, :] |= m[:-1, :]
        d[:-1, :] |= m[1:, :]
        d[:, 1:] |= m[:, :-1]
        d[:, :-1] |= m[:, 1:]
        self.ids[d & ~m] = line
        self.ids[m] = fill

    def text(self, s, x, top, sx, sy, lay):
        for i, ch in enumerate(s):
            b = np.kron(glyph(ch), np.ones((sy, sx), bool))
            h, w = b.shape
            xx = x + i * ADVANCE * sx
            sub = self.ids[top:top + h, xx:xx + w]
            sub[b[:sub.shape[0], :sub.shape[1]]] = lay

    def text_box(self, s, x, y, w, h, align, lay):
        """A line in a box as fit_text places it: x by align, top = y + (h - 7 sy) // 2 (the descender row may go sy
        below)"""
        if not s:
            return
        (sx, sy), s = fit_text(s, w)
        tw = text_w(s, sx)
        xx = x + {"left": 0, "center": (w - tw) // 2}[align]
        self.text(s, xx, y + (h - CAP * sy) // 2, sx, sy, lay)

    def image(self, bg=BG) -> np.ndarray:
        pal = np.array([rgb3(c) for c in self.cols] + [bg], np.uint8)
        return pal[np.where(self.ids < 0, len(self.cols), self.ids)]


def rmask(x0, y0, x1, y1):
    m = np.zeros((S, S), bool)
    m[y0:y1, x0:x1] = True
    return m


# ---- geometry, pixels: body 180 wide (the name field is 166 = 14 font5 chars at scale 2), cassette 20..234 ----
BODY, CAP_L, CAP_R = (42, 88, 222, 168), (30, 84, 43, 172), (221, 84, 234, 172)
SPOOL, SLOT = (20, 116, 31, 140), (22, 127, 29, 129)
TONGUE, TAPER = (54, 166, 196, 178), 170            # film tongue; its end steps in from this row, 1 pixel a row
PERF = (171, 173, 4, 10, 60, 180)                   # sprocket holes: rows y0..y1, width, pitch, x from..to
SPLIT_STEP, CAP_COL = 4, 2                          # split label: diagonal step; caps: light / dark column width
SPLIT_CLEAR = "KODACHROME"                          # the split diagonal starts right of a name this long at scale 2
ROWS = (24, 24, 23)                                 # label rows: A brand + ISO window, B name, C process code
HL, SH, MARGIN = 2, 5, 6                            # light rows on top, shade rows at the bottom, text margin
ISO_BOX = (68, 20)                                  # ISO window, outer size
SCALE_BRAND, SCALE_PROC = 1, 1                      # brand and process code: font5 at 1
NAME_W = BODY[2] - BODY[0] - 2 - 2 * MARGIN         # the name field, 166 pixels
RAINBOW_ROWS = 3                                    # "rainbow" label: rows per stripe, the stripes centred in row C
# The other shapes keep the body (the label with its rows and fields) and move it by (dx, dy) to stay centred in CROP
# above the "n / N" line: equal gaps left and right, above and below (pack 20 / 13, cartridge 22 / 15 pixels)
BODY_AT = {"cassette": (0, 0), "pack": (-6, 5), "cartridge": (-4, 3)}
PACK_DEPTH = 4                                      # pack: the top and right faces, 1 pixel a row
MINI_R = 4                                          # cartridge: corners cut in steps of 1 pixel


# ---- decorations on the label: a picture's own pattern (its parameter "deco", a list of items drawn in order after the
# label style's bands and the band line, before the light and shade rows and the texts). Coordinates in pixels relative
# to the label's inner field (x from its left edge, y from its top: light rows 0-1, row A from 2, row B from 26, row C
# from 50, shade from 73; the field is 178 x 78). Each item has a "kind" and "clip" - where it may draw: "label"
# (default, the whole field), "A", "B", "C", "AC" (the rows without the light and shade rows, which stay the label's)
# or a box (x0, y0, x1, y1). A colour of None leaves the pixel as it is. Pixels are tested at their centres; the items
# never draw into the name's text box (2 pixels around the drawn line), the brand and process lines nor the ISO
# window, so the texts stay on their plain fields. Layers: one per colour of an item (a ring of "rings": one per ring),
# in the order first used - a layer is a record, and an outline costs points in every layer between the two colours it
# parts, so a pattern costs least when its neighbouring colours are neighbouring layers (stripes as a palindrome:
# a b c b a ...). Kinds:
#   waves  - horizontal stripes of `colours` (cycled), `thick` rows each, starting at row `y`, their edges a sine of
#            amplitude `amp` and period `period` (pixels) in steps of `step` columns, shifted by `phase` columns;
#   rays   - wedges of `colours` (cycled) around (cx, cy): `n` of them, turned by `phase` (turns), twisted by `twist`
#            turns per pixel of radius (a spiral), in blocks of `cell` pixels;
#   rings  - concentric discs: `radii` (largest first) in `colours`, centre (cx, cy) (a pixel corner: an even width);
#   flower - `n` discs of radius `r` at distance `dist` around (cx, cy), turned by `phase` turns, in `colour`, and a
#            centre disc of radius `core` in `core_colour`;
#   discs  - `items` [(cx, cy, r, colour, rim, shine)]: a disc of radius r, a rim of 1 pixel in rim (None: none) and a
#            2 x 2 shine at its upper left in shine (None: none) - bubbles, dots;
#   drips  - from row `y` down, in `colour`: columns of `widths` (cycled) each running down `lengths` rows (cycled), the
#            ends rounded by a pixel - a melting edge.
DECO_CLIPS = {"label": (0, 0, 178, 78), "A": (0, 2, 178, 26), "B": (0, 26, 178, 50), "C": (0, 50, 178, 73)}


def _deco(a: Art, items, origin, keep, layer_of) -> None:
    """Draws the decoration items onto a (see above); origin - the inner field's top left, keep - the pixels they must
    not touch, layer_of(colour, key) - the layer of a colour in a part of an item (key: item number, part)"""
    ox, oy = origin
    yy, xx = np.mgrid[0:S, 0:S]
    X, Y = xx - ox + .5, yy - oy + .5                        # pixel centres in the field's coordinates

    def clip_mask(c):
        if c == "AC":
            return clip_mask("A") | clip_mask("C")
        x0, y0, x1, y1 = DECO_CLIPS[c] if isinstance(c, str) else c
        return (X >= x0) & (X < x1) & (Y >= y0) & (Y < y1)

    def put(m, colour, part=0):
        if colour is not None:
            a.ids[m & clip & ~keep] = layer_of(colour, (n_item, part))

    def disc(cx, cy, r):
        return (X - cx) ** 2 + (Y - cy) ** 2 < r * r

    for n_item, d in enumerate(items):
        clip = clip_mask(d.get("clip", "label"))
        kind = d["kind"]
        if kind == "waves":
            xq = np.floor(X / d["step"]) * d["step"]
            off = np.rint(d["amp"] * np.sin(2 * np.pi * (xq + d.get("phase", 0)) / d["period"]))
            k = np.floor((Y - d.get("y", 0) - off) / d["thick"]).astype(int) % len(d["colours"])
            for i, col in enumerate(d["colours"]):
                put(k == i, col)
        elif kind == "rays":
            cell = d.get("cell", 1)
            bx, by = (np.floor(X / cell) + .5) * cell - d["cx"], (np.floor(Y / cell) + .5) * cell - d["cy"]
            turn = np.arctan2(by, bx) / (2 * np.pi) + d.get("phase", 0) + d.get("twist", 0) * np.hypot(bx, by)
            k = np.floor(turn * d["n"]).astype(int) % len(d["colours"])
            for i, col in enumerate(d["colours"]):
                put(k == i, col)
        elif kind == "rings":
            for i, (r, col) in enumerate(zip(d["radii"], d["colours"])):
                put(disc(d["cx"], d["cy"], r), col, i)      # a layer per ring: inner rings lie on outer ones
        elif kind == "flower":
            for i in range(d["n"]):
                t = 2 * np.pi * (i / d["n"] + d.get("phase", 0))
                put(disc(d["cx"] + d["dist"] * math.cos(t), d["cy"] + d["dist"] * math.sin(t), d["r"]), d["colour"])
            put(disc(d["cx"], d["cy"], d["core"]), d["core_colour"])
        elif kind == "discs":
            for cx, cy, r, col, rim, shine in d["items"]:
                put(disc(cx, cy, r), rim if rim is not None else col)
                put(disc(cx, cy, r - 1) if rim is not None else disc(cx, cy, r), col)
                sx, sy = math.floor(cx - r / 2), math.floor(cy - r / 2)
                put((X >= sx) & (X < sx + 2) & (Y >= sy) & (Y < sy + 2) & disc(cx, cy, r - 1), shine)
        elif kind == "drips":
            col = np.floor(X).astype(int)
            length = np.zeros(S, int)
            widths, lengths = d["widths"], d["lengths"]
            x, i = 0, 0
            while x < DECO_CLIPS["label"][2]:
                w = widths[i % len(widths)]
                length[max(x + ox, 0):x + ox + w] = lengths[i % len(lengths)]
                if w > 2 and lengths[i % len(lengths)] > 1:      # a rounded end: the end row a pixel narrower each side
                    length[x + ox] -= 1
                    length[x + ox + w - 1] -= 1
                x, i = x + w, i + 1
            run = length[np.clip(col + ox, 0, S - 1)]
            put((Y >= d["y"]) & (Y < d["y"] + run), d["colour"])
        else:
            raise ValueError(f"decoration {kind}")


def rounded(x0, y0, x1, y1, r):
    """A rectangle with its corners cut in r steps of one pixel"""
    m = rmask(x0, y0, x1, y1)
    for i in range(r):
        cut = r - i
        for y in (y0 + i, y1 - 1 - i):
            m[y, x0:x0 + cut] = False
            m[y, x1 - cut:x1] = False
    return m


def draw(p: dict, name: str, iso: str, brand: str | None = None, proc: str | None = None) -> Art:
    """The pixel picture of parameters p with this name and ISO; brand and proc (rows A and C) by default p's own lines
    (none on a text-free generic)"""
    a = Art()
    c = palette(p)
    c.update(p.get("pal", {}))                       # a generic's own colours of the caps and the spool (old process)
    lay = {}
    brand = p.get("brand", "") if brand is None else brand
    proc = p.get("proc", "") if proc is None else proc
    shape = p.get("shape", "cassette")
    dx, dy = BODY_AT[shape]

    def L(role, rgb=None):
        lay[role] = a.layer(c[role] if rgb is None else rgb)
        return lay[role]

    # z order: outline (silhouette) -> what lies behind the body (film, spool, caps; the box's faces)
    # -> label -> bands -> light and shade -> what lies on the label -> text
    Lk = L("K")
    bx0, by0, bx1, by1 = BODY[0] + dx, BODY[1] + dy, BODY[2] + dx, BODY[3] + dy
    if shape == "cassette":
        x0, y0, x1, y1 = TONGUE                      # film tongue with a stepped end and sprocket holes
        tm = rmask(x0, y0, x1, y1)
        for y in range(TAPER, y1):
            tm[y, x1 - (y - TAPER + 1):x1] = False
        a.outlined(tm, L("film"), Lk)
        py0, py1, pw, pitch, px0, px1 = PERF
        Lp = L("perf")
        for x in range(px0, px1, pitch):
            a.rect(x, py0, x + pw, py1, Lp)
        x0, y0, x1, y1 = SPOOL                       # spool head
        a.outlined(rmask(x0 + 1, y0 + 1, x1 - 1, y1 - 1), L("spool"), Lk)
        a.rect(*SLOT, L("slot"))
        Lc, Lch, Lcl = L("cap"), L("cap_hi"), L("cap_lo")    # caps: a light and a dark column, cut corners
        for x0, y0, x1, y1 in (CAP_L, CAP_R):
            a.outlined(rmask(x0 + 1, y0 + 1, x1 - 1, y1 - 1), Lc, Lk)
            a.rect(x0 + 1, y0 + 1, x0 + 1 + CAP_COL, y1 - 1, Lch)
            a.rect(x1 - 1 - CAP_COL, y0 + 1, x1 - 1, y1 - 1, Lcl)
    elif shape == "pack":                            # the top and right faces of the box
        faces = np.zeros((S, S), bool)
        for d in range(1, PACK_DEPTH + 1):
            faces |= rmask(bx0 + 1 + d, by0 + 1 - d, bx1 - 1 + d, by1 - 1 - d)
        face = mix(p["label"], 0x000000, .45) if luma(p["label"]) > 80 else mix(p["label"], 0xffffff, .2)
        a.outlined(faces, L("face", face), Lk)          # darker than the front; on a dark box lighter, to be seen
    elif shape != "cartridge":                       # a cartridge is the body alone (its corners rounded below)
        raise ValueError(f"shape {shape}")
    ix0, iy0, ix1, iy1 = bx0 + 1, by0 + 1, bx1 - 1, by1 - 1      # the body is the label
    a.rect(bx0, by0, bx1, by1, Lk)
    a.rect(ix0, iy0, ix1, iy1, L("label"))
    rA0 = iy0 + HL
    rB0 = rA0 + ROWS[0]
    rC0 = rB0 + ROWS[1]
    assert rC0 + ROWS[2] + SH == iy1
    cx0, cx1 = ix0 + MARGIN, ix1 - MARGIN
    name_bg = c["label"]
    iso_window = p.get("iso_window", True)           # False: none (a picture without the ISO line)
    bw, bh = ISO_BOX                                 # ISO window: right in row A, centred in the row
    ox1 = cx1
    ox0 = ox1 - bw
    oy0 = rA0 + (ROWS[0] - bh) // 2
    oy1 = oy0 + bh
    ib = (ox0 + 1, oy0 + 1, ox1 - 1, oy1 - 1)        # the window's inner field
    iso_cap = CAP * TEXT_SCALES[0][1]
    baseline = ib[1] + (ib[3] - ib[1] - iso_cap) // 2 + iso_cap      # the baseline of row A
    ptop = rC0 + (ROWS[2] - CAP * SCALE_PROC) // 2   # process code: left in row C, centred in the row
    if not iso_window and iso:
        raise ValueError(f"ISO {iso!r}: this picture has no ISO window")
    style = p["style"]
    if style == "band":
        a.rect(ix0, rB0, ix1, rC0, L("band"))
        name_bg = c["band"]
    elif style == "stripes":
        a.rect(ix0, rB0, ix1, rB0 + 2, L("band"))
        a.rect(ix0, rC0 - 2, ix1, rC0, L("band2"))
    elif style == "split":
        # the lower right corner in another colour; a stepped 45-degree diagonal right of the longest name of the
        # split presets in row B, and never into row A (the ISO window)
        name_end = cx0 + text_w(SPLIT_CLEAR, TEXT_SCALES[0][0])
        xb = max(name_end + 2 * SPLIT_STEP - (iy1 - rC0), ix1 - (iy1 - rB0))
        Lb = L("band")
        for y in range(iy0, iy1):
            xs = xb + ((iy1 - y + SPLIT_STEP - 1) // SPLIT_STEP) * SPLIT_STEP
            if xs < ix1:
                a.rect(max(xs, ix0), y, ix1, y + 1, Lb)
    elif style == "rainbow":                         # stripes of the band colours across row C, centred in it
        y = rC0 + (ROWS[2] - RAINBOW_ROWS * len(p["band"])) // 2
        for k, col in enumerate(p["band"]):
            a.rect(ix0, y, ix1, y + RAINBOW_ROWS, L("band") if k == 0 else L(f"stripe{k}", col))
            y += RAINBOW_ROWS
    else:
        raise ValueError(f"label style {style}")
    if p.get("band_line") is not None:               # a line of 1 pixel along the edges of row B (the name's band)
        Ll = L("band_line", p["band_line"])
        a.rect(ix0, rB0, ix1, rB0 + 1, Ll)
        a.rect(ix0, rC0 - 1, ix1, rC0, Ll)
    if p.get("deco"):                                # the picture's own pattern (DECO_CLIPS above)
        keep = np.zeros((S, S), bool)
        if iso_window:
            keep[oy0:oy1, ox0:ox1] = True
        if name:
            (sx, sy), shown = fit_text(name, cx1 - cx0)
            ntop = rB0 + (ROWS[1] - CAP * sy) // 2
            keep[ntop - 2:ntop + 8 * sy + 2, cx0 - 2:cx0 + text_w(shown, sx) + 2] = True
        for line, sc, top in ((brand, SCALE_BRAND, baseline - CAP * SCALE_BRAND), (proc, SCALE_PROC, ptop)):
            if line:                                 # rows A and C: the brand and process lines
                keep[top - 2:top + 8 * sc + 2, cx0 - 2:cx0 + text_w(line, sc) + 2] = True
        deco_lay: dict = {}

        def deco_layer(col, key):
            if (col, key) not in deco_lay:
                deco_lay[col, key] = a.layer(col)
            return deco_lay[col, key]
        _deco(a, p["deco"], (ix0, iy0), keep, deco_layer)
    for role, hi, lo in (("label", "label_hi", "label_lo"), ("band", "band_hi", "band_lo")):
        if role not in lay:                          # light on top, shade at the bottom: a shade of each colour
            continue
        base = lay[role]
        if c[hi] != c[role]:
            Lh = L(hi)
            sub = a.ids[iy0:iy0 + HL, ix0:ix1]
            sub[sub == base] = Lh
        if c[lo] != c[role]:
            Lo = L(lo)
            sub = a.ids[iy1 - SH:iy1, ix0:ix1]
            sub[sub == base] = Lo
    if shape == "cartridge":                         # rounded corners; the yellow mark right in row C
        a.ids[rmask(bx0, by0, bx1, by1) & ~rounded(bx0, by0, bx1, by1, MINI_R)] = -1
        a.ids[rounded(bx0, by0, bx1, by1, MINI_R) & ~rounded(ix0, iy0, ix1, iy1, MINI_R - 1)] = Lk
        my = rC0 + (ROWS[2] - 8) // 2
        a.outlined(rmask(cx1 - 13, my + 1, cx1 - 1, my + 7), L("mark", 0xf2c21a), Lk)
    if p.get("grain"):                               # two lines of wood grain in rows A and C (old process)
        Lg = L("grain", mix(p["label"], 0x000000, .25))
        a.rect(cx0 + 24, rA0 + 5, ox0 - 12, rA0 + 6, Lg)
        a.rect(cx0 + 64, rC0 + 15, cx1 - 24, rC0 + 16, Lg)
    name_ink, label_ink = ink(name_bg, p["text"]), ink(c["label"], p["text"], dark=0x101418)
    if iso_window:
        a.rect(ox0, oy0, ox1, oy1, L("iso_bg"))
    if cx0 + text_w(brand, SCALE_BRAND) + MARGIN > (ox0 if iso_window else cx1 + MARGIN):
        raise ValueError(f"brand {brand!r}: longer than row A" + (" left of the ISO window" if iso_window else ""))
    if cx0 + text_w(proc, SCALE_PROC) > cx1:
        raise ValueError(f"process code {proc!r}: longer than row C")
    a.text(brand, cx0, baseline - CAP * SCALE_BRAND, SCALE_BRAND, SCALE_BRAND, L("brand_ink", label_ink))
    a.text(proc, cx0, ptop, SCALE_PROC, SCALE_PROC, L("proc_ink", label_ink))
    a.text_box(name, cx0, rB0, cx1 - cx0, ROWS[1], "left", L("name_ink", name_ink))
    if iso_window:
        a.text_box(iso, ib[0], ib[1], ib[2] - ib[0], ib[3] - ib[1], "center", L("iso_ink", c["iso_ink"]))
    return a


# ---- picture -> records ----
def trace(mask: np.ndarray) -> list[list[tuple[int, int]]]:
    """Outlines of a pixel region: edges on pixel borders, clockwise (y down), holes counter-clockwise; where parts touch
    at a corner the walk turns right so they do not merge. Vertices only at corners"""
    m = np.zeros((mask.shape[0] + 2, mask.shape[1] + 2), bool)
    m[1:-1, 1:-1] = mask
    inn = m[1:-1, 1:-1]
    out: dict = {}

    def add(a, b):
        out.setdefault(a, []).append(b)
    for y, x in zip(*np.nonzero(inn & ~m[:-2, 1:-1])):
        add((x, y), (x + 1, y))
    for y, x in zip(*np.nonzero(inn & ~m[1:-1, 2:])):
        add((x + 1, y), (x + 1, y + 1))
    for y, x in zip(*np.nonzero(inn & ~m[2:, 1:-1])):
        add((x + 1, y + 1), (x, y + 1))
    for y, x in zip(*np.nonzero(inn & ~m[1:-1, :-2])):
        add((x, y + 1), (x, y))
    loops = []
    while out:
        start = min(out)                          # deterministic: the leftmost-topmost vertex
        prev, cur = start, out[start].pop()
        if not out[start]:
            del out[start]
        pts = [start]
        while cur != start:
            pts.append(cur)
            cands = out[cur]
            if len(cands) == 1:
                nxt = cands.pop()
            else:
                dx, dy = cur[0] - prev[0], cur[1] - prev[1]
                want = (cur[0] - dy, cur[1] + dx)  # turn right (y down)
                nxt = want if want in cands else cands[0]
                cands.remove(nxt)
            if not cands:
                del out[cur]
            prev, cur = cur, nxt
        k = len(pts)
        loops.append([pts[i] for i in range(k)
                      if (pts[i - 1][0] - pts[i][0]) * (pts[(i + 1) % k][1] - pts[i][1])
                      != (pts[i - 1][1] - pts[i][1]) * (pts[(i + 1) % k][0] - pts[i][0])])
    return loops


def mask_records(mask, color) -> list[dict]:
    loops = trace(mask)
    npt = sum(len(lp) for lp in loops)
    nseg = npt + 2 * len(loops)                   # MOVE + LINE * (n - 1) + CLOSE
    if npt > cani.MAX_PTS or nseg > cani.MAX_SEG:     # cut by a column: holes stay in one record with their outline
        xs = np.nonzero(mask.any(0))[0]
        cut = (xs[0] + xs[-1] + 1) // 2
        left = mask.copy()
        left[:, cut:] = False
        right = mask.copy()
        right[:, :cut] = False
        return mask_records(left, color) + mask_records(right, color)
    seg, xy = [], []
    for lp in loops:
        for i, (x, y) in enumerate(lp):
            seg.append(MOVE if i == 0 else LINE)
            xy.append((int(x) * Q, (S - int(y)) * Q))
        seg.append(CLOSE)
    return [dict(op=OP_FILL, cap=0, color=color << 8 | 255, width=0.0, seg=seg, xy=xy)]


def grow(seed: np.ndarray, allowed: np.ndarray) -> np.ndarray:
    """The parts of allowed (4-connected) that touch seed: a layer takes the pixels lying on it only where they join it -
    else islands of upper layers (letters on a band) would add outlines"""
    cur = seed & allowed
    while True:
        nxt = cur.copy()
        nxt[1:, :] |= cur[:-1, :]
        nxt[:-1, :] |= cur[1:, :]
        nxt[:, 1:] |= cur[:, :-1]
        nxt[:, :-1] |= cur[:, 1:]
        nxt &= allowed
        if (nxt == cur).all():
            return cur
        cur = nxt


def vectorize(art: Art) -> list[dict]:
    ids = art.ids
    present = sorted(set(np.unique(ids).tolist()) - {-1})
    remap = np.full(len(art.cols) + 1, -1, np.int16)  # visible layers in a row of one colour are one layer
    cols, prev = [], None
    for k in present:
        if art.cols[k] != prev:
            cols.append(art.cols[k])
            prev = art.cols[k]
        remap[k] = len(cols) - 1
    ids = remap[ids]                              # -1 -> remap[-1] = -1
    recs = []
    for k, col in enumerate(cols):
        recs += mask_records(grow(ids == k, ids >= k), col)
    return recs


# ---- the CANI section (kind 1, a drawing) ----
class TooBig(ValueError):
    """The drawing does not fit the section's limits (cani.MAX_RECORDS, MAX_BYTES)"""


def encode(recs, brand: str, name: str, iso: str) -> bytes:
    """Records -> section; raises TooBig over the record or byte limit, ValueError on any other check of cani.parse"""
    body = b""
    for r in recs:
        segs = bytes(r["seg"])
        segs += b"\0" * (-len(segs) % 4)
        xy = b"".join(struct.pack("<hh", x, y) for x, y in r["xy"])
        width = struct.unpack("<I", struct.pack("<f", r["width"]))[0]
        body += struct.pack("<BBHIIHH", r["op"], r["cap"], 0, r["color"], width, len(r["seg"]), len(r["xy"])) + segs + xy
    if len(recs) > cani.MAX_RECORDS or len(body) > cani.MAX_BYTES:
        raise TooBig(f"{len(recs)} records, {len(body)} bytes (limits {cani.MAX_RECORDS}, {cani.MAX_BYTES})")
    head = struct.pack("<HHHHHHI", cani.VERSION, cani.KIND_DRAW, S, S, len(recs), 0, len(body))
    blob = head + cani._text(brand, 16) + cani._text(name, 24) + cani._text(iso, 8) + body
    cani.parse(blob)
    return blob


def generic(index: int, name: str = "", iso: str = "", brand: str | None = None, proc: str | None = None) -> bytes:
    """The section of generic picture `index` (GENERICS: 0-5 cassettes with a line of their kind, 6-24 pictures of
    reference looks, 25-29 instant film and an old-process cassette) with this name and ISO drawn on its label; brand and proc
    replace the generic's own lines of rows A and C (the header's brand is the line drawn). Bounded: a name whose glyphs
    would take the drawing over the byte limit is drawn cut by one char at a time until it fits; the header keeps the
    name as given. Only names near the 27-char maximum of the field can need it: 17 chars take ~4 KB, random 27-char
    names with a 4-char ISO ~5-6 KB (1 in 300 is cut), 27 x 'X' with a 7-char ISO ~9 KB"""
    if not 0 <= index < len(GENERICS):
        raise ValueError(f"no generic cassette {index} (0..{len(GENERICS) - 1})")
    g = GENERICS[index]
    brand = g.get("brand", "") if brand is None else brand
    iso = (iso or "")[:7]                           # as many as the header's char[8] holds
    shown = fit_text(name, NAME_W)[1]
    while True:
        try:
            return encode(vectorize(draw(g, shown, iso, brand, proc)), brand, name, iso)
        except TooBig:
            if not shown:
                raise
            shown = shown[:-1].rstrip()


HEAD_NAME, HEAD_BRAND, HEAD_ISO = 23, 15, 7     # the CANI header's char[24], char[16], char[8], zero terminated


def full_font_chars() -> int:
    """How many characters of the name line fit in the full 2x2 font (else the narrow 1x2 one): 14."""
    return (NAME_W // TEXT_SCALES[0][0] + 1) // ADVANCE


def make_generic(index, name="", iso="", brand="", proc=""):
    """A generic cassette with the user's texts, checked as a page shows them: (section bytes or None, report, errors,
    warnings). index - GENERICS; name - the name line; iso - the ISO window; brand, proc - rows A and C, empty: the
    picture's own line. Errors (no section): characters outside printable ASCII (font5 has no others), an ISO
    longer than 7 characters, a brand or process line too long for its row (the generator says so). Warnings: a name or
    brand longer than the header's field (the header keeps the first characters, the drawing what fits), an empty name,
    a name drawn narrow (more than full_font_chars()) or cut."""
    errors, warnings = [], []
    try:
        index = int(index)
    except (TypeError, ValueError):
        index = -1
    if not 0 <= index < len(GENERICS):
        return None, {}, [f"no generic cassette {index} (0..{len(GENERICS) - 1})"], []
    g = GENERICS[index]
    name, iso, brand, proc = (str(v or "") for v in (name, iso, brand, proc))
    for label, v in (("name", name), ("ISO", iso), ("brand", brand), ("process", proc)):
        bad = sorted({c for c in v if not 32 <= ord(c) < 127})
        if bad:
            errors.append(f"{label}: only printable ASCII (font5 has no other characters), not "
                          + " ".join(repr(c) for c in bad))
    if len(iso) > HEAD_ISO:
        errors.append(f"ISO: at most {HEAD_ISO} characters")
    if len(name) > HEAD_NAME:
        warnings.append(f"name: longer than {HEAD_NAME} characters - the file's header keeps the first {HEAD_NAME}, the "
                        "drawing what fits")
    if len(brand) > HEAD_BRAND:
        warnings.append(f"brand: longer than {HEAD_BRAND} characters - the file's header keeps the first {HEAD_BRAND}")
    if not name.strip():
        warnings.append("name: empty - the label shows only the ISO")
    rep = {"index": index, "key": g["id"], "label": cani.GENERICS[index][1], "brand_default": g.get("brand", ""),
           "proc_default": g.get("proc", ""), "full_font": full_font_chars()}
    (sx, sy), shown = fit_text(name, NAME_W)
    rep["shown"], rep["font"] = shown, "full" if (sx, sy) == TEXT_SCALES[0] else "narrow"
    if shown != name.upper():
        warnings.append(f"name: does not fit on the label - drawn as '{shown}'")
    elif (sx, sy) != TEXT_SCALES[0]:
        warnings.append(f"name: drawn in the narrow font - the full font holds at most {full_font_chars()} characters")
    if errors:
        return None, rep, errors, warnings
    try:
        blob = generic(index, name, iso, brand or None, proc or None)
    except ValueError as e:              # a brand or process line too long for its row; over the section's limits
        return None, rep, errors + [str(e)], warnings
    rep["limits"] = limits(blob)
    rep["size"] = len(blob)
    return blob, rep, errors, warnings


# ---- back from bytes ----
_NPTS = {MOVE: 1, LINE: 1, QUAD: 2, CUBIC: 3, CLOSE: 0}


def decode(blob: bytes) -> list[dict]:
    """The records of a section, after the checks of cani.parse (as core would make them)"""
    info = cani.parse(blob)
    if info["kind"] != cani.KIND_DRAW:
        raise ValueError(f"kind {info['kind']} is not a drawing")
    _, _, _, _, count, _, _ = struct.unpack_from("<HHHHHHI", blob)
    off, recs = cani.HEADER, []
    for _ in range(count):
        op, cap, _, color, wb, ns, npt = struct.unpack_from("<BBHIIHH", blob, off)
        segs = list(blob[off + 16:off + 16 + ns])
        off += 16 + ns + (-ns % 4)
        xy = struct.unpack_from(f"<{2 * npt}h", blob, off)
        off += 4 * npt
        recs.append(dict(op=op, cap=cap, color=color, width=struct.unpack("<f", struct.pack("<I", wb))[0],
                         seg=segs, xy=list(zip(xy[0::2], xy[1::2]))))
    return recs


def limits(blob: bytes) -> dict:
    """The section's sizes against the limits (cani.MAX_*)"""
    recs = decode(blob)
    out = dict(records=len(recs), bytes=len(blob) - cani.HEADER, max_seg=max(len(r["seg"]) for r in recs),
               max_pts=max(len(r["xy"]) for r in recs))
    out["ok"] = (out["records"] <= cani.MAX_RECORDS and out["bytes"] <= cani.MAX_BYTES and
                 out["max_seg"] <= cani.MAX_SEG and out["max_pts"] <= cani.MAX_PTS)
    return out


# ---- the camera's drawing: OpenVG fill sampled 4x4 per pixel ----
CURVE_STEPS = 16                                # line pieces per curve segment


def subpaths(rec) -> list[tuple[list[tuple[float, float]], bool]]:
    """The record's path as polylines in quarter pixels, curves flattened: [(points, closed)]"""
    out, cur, i = [], [], 0
    xy = rec["xy"]
    for s in rec["seg"]:
        if s == MOVE:
            if len(cur) > 1:
                out.append((cur, False))
            cur = [xy[i]]
        elif s == LINE:
            cur.append(xy[i])
        elif s in (QUAD, CUBIC):
            ctrl = [cur[-1]] + list(xy[i:i + _NPTS[s]])
            for k in range(1, CURVE_STEPS + 1):
                t = k / CURVE_STEPS
                pts = ctrl
                while len(pts) > 1:                 # de Casteljau
                    pts = [(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t) for a, b in zip(pts, pts[1:])]
                cur.append(pts[0])
        elif s == CLOSE and cur:
            out.append((cur, True))
            cur = [cur[0]]
        i += _NPTS[s]
    if len(cur) > 1:
        out.append((cur, False))
    return out


def _circle(cx, cy, r, n=16):
    return [(cx + r * math.cos(-2 * math.pi * k / n), cy + r * math.sin(-2 * math.pi * k / n)) for k in range(n)]


def stroke_polygons(rec) -> list[list[tuple[float, float]]]:
    """A stroke as polygons of one orientation (their non-zero union): a quad per piece, round joins (OpenVG's default
    miter is not modelled), caps BUTT 0 / ROUND 1 / SQUARE 2 on open ends"""
    r = rec["width"] * Q / 2
    polys = []
    for pts, closed in subpaths(rec):
        pieces = list(zip(pts, pts[1:] + pts[:1])) if closed else list(zip(pts, pts[1:]))
        for (ax, ay), (bx, by) in pieces:
            ln = math.hypot(bx - ax, by - ay)
            if not ln:
                continue
            nx, ny = -(by - ay) / ln * r, (bx - ax) / ln * r
            polys.append([(ax + nx, ay + ny), (bx + nx, by + ny), (bx - nx, by - ny), (ax - nx, ay - ny)])
        joins = pts if closed else pts[1:-1]
        polys += [_circle(x, y, r) for x, y in joins]
        if not closed and len(pts) > 1:
            for (ex, ey), (qx, qy) in ((pts[0], pts[1]), (pts[-1], pts[-2])):
                if rec["cap"] == 1:
                    polys.append(_circle(ex, ey, r))
                elif rec["cap"] == 2:
                    ln = math.hypot(ex - qx, ey - qy) or 1
                    dx, dy = (ex - qx) / ln * r, (ey - qy) / ln * r
                    nx, ny = -dy, dx
                    polys.append([(ex + nx, ey + ny), (ex + dx + nx, ey + dy + ny), (ex + dx - nx, ey + dy - ny),
                                  (ex - nx, ey - ny)])
    for p in polys:                                  # one orientation, so overlaps never cancel under non-zero
        area = sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(p, p[1:] + p[:1]))
        if area > 0:
            p.reverse()
    return polys


def coverage(polys, nonzero=True) -> np.ndarray:
    """Covered share of the 16 samples of each pixel of the surface (rows from the top) for closed polygons in quarter
    pixels, y up; sample centres at (i + 0.5) / 4"""
    N = S * Q
    acc = np.zeros((N, N + 1), np.int32)             # [sample row j (y up), column]
    for poly in polys:
        for (xa, ya), (xb, yb) in zip(poly, poly[1:] + poly[:1]):
            if ya == yb:
                continue
            d = 1 if yb > ya else -1
            lo, hi = min(ya, yb), max(ya, yb)
            j0, j1 = max(int(math.ceil(lo - .5)), 0), min(int(math.ceil(hi - .5)), N)   # rows with centres in [lo, hi)
            if j0 >= j1:
                continue
            if xa == xb:
                acc[j0:j1, min(max(int(math.floor(xa - .5)) + 1, 0), N)] += d
            else:
                js = np.arange(j0, j1)
                xc = xa + (js + .5 - ya) * (xb - xa) / (yb - ya)
                np.add.at(acc, (js, np.clip(np.floor(xc - .5).astype(int) + 1, 0, N)), d)
    wind = np.cumsum(acc, axis=1)[:, :N]
    inside = (wind != 0) if nonzero else (wind % 2 != 0)
    return inside.reshape(S, Q, S, Q).mean(axis=(1, 3))[::-1]


def record_coverage(rec) -> np.ndarray:
    if rec["op"] == OP_STROKE:
        return coverage(stroke_polygons(rec))
    return coverage([pts for pts, _ in subpaths(rec)], rec["op"] == OP_FILL)


def render(recs, bg=BG) -> tuple[np.ndarray, int]:
    """(256 x 256 x 3 picture, number of partly covered pixels) of the records drawn in order over bg"""
    img = np.empty((S, S, 3), np.float64)
    img[:] = bg
    partial = 0
    for r in recs:
        cov = record_coverage(r)
        partial += int(((cov > 0) & (cov < 1)).sum())
        col = np.array([(r["color"] >> 24) & 255, (r["color"] >> 16) & 255, (r["color"] >> 8) & 255], float)
        a = (r["color"] & 255) / 255 * cov[..., None]
        img = img * (1 - a) + col * a
    return np.clip(np.rint(img), 0, 255).astype(np.uint8), partial


def counter(img: np.ndarray, s: str, rgb=(180, 180, 180)):
    """The camera's "n / N" under the cassette: font5 at 1, centred on x 128"""
    tw = text_w(s, 1)
    x = 128 - (tw + 1) // 2
    for i, ch in enumerate(s):
        g = glyph(ch)
        sub = img[COUNTER_TOP:COUNTER_TOP + 8, x + i * ADVANCE:x + i * ADVANCE + 5]
        sub[g[:sub.shape[0], :sub.shape[1]]] = rgb


def _crop(img, crop):
    if crop == "auto":                               # CROP; a drawing reaching out of it - with 8 pixels around it
        crop = CROP
        drawn = (img != np.array(BG, np.uint8)).any(-1)
        if drawn.any():
            ys, xs = np.nonzero(drawn)
            x0, y0, x1, y1 = int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1
            if x0 < CROP[0] or y0 < CROP[1] or x1 > CROP[2] or y1 > CROP[3]:
                crop = (max(min(x0 - 8, CROP[0]), 0), max(min(y0 - 8, CROP[1]), 0),
                        min(max(x1 + 8, CROP[2]), S), min(max(y1 + 8, CROP[3]), S))
    if crop:
        x0, y0, x1, y1 = crop
        img = img[y0:y1, x0:x1]
    return img


def _png(im: Image.Image) -> bytes:
    buf = io.BytesIO()
    im.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def render_png(blob: bytes, scale: int = 2, crop="auto", label: str | None = None) -> bytes:
    """PNG of a section as the camera draws it: crop "auto" (the cassette, or wider if the drawing is), CROP-like
    (x0, y0, x1, y1) or None (the whole surface); nearest-neighbour scale; label - the "n / N" line under it"""
    img, _ = render(decode(blob))
    if label:
        counter(img, label)
    im = Image.fromarray(_crop(img, crop))
    if scale > 1:
        im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
    return _png(im)


def sheet_png(items, cols: int = 4, title: str = "", scale: int = 1) -> bytes:
    """items: [(section bytes, caption)] -> one PNG sheet, each cassette as the camera draws it (crop CROP), with its
    "n / N" line; captions and the title in Pillow's default font (ASCII)"""
    tiles = []
    for k, (blob, caption) in enumerate(items):
        img, _ = render(decode(blob))
        counter(img, f"{k + 1} / {len(items)}")
        t = Image.fromarray(_crop(img, CROP))
        if scale > 1:
            t = t.resize((t.width * scale, t.height * scale), Image.NEAREST)
        tile = Image.new("RGB", (t.width, t.height + 16), (30, 30, 30))
        tile.paste(t, (0, 0))
        ImageDraw.Draw(tile).text((4, t.height + 2), caption.encode("ascii", "replace").decode(), fill=(200, 200, 200))
        tiles.append(tile)
    pad, top = 6, 22 if title else 0
    w, h = tiles[0].width, tiles[0].height
    rows = (len(tiles) + cols - 1) // cols
    out = Image.new("RGB", (cols * (w + pad) + pad, rows * (h + pad) + pad + top), (22, 22, 22))
    if title:
        ImageDraw.Draw(out).text((pad, 5), title.encode("ascii", "replace").decode(), fill=(230, 230, 230))
    for i, t in enumerate(tiles):
        out.paste(t, (pad + (i % cols) * (w + pad), top + pad + (i // cols) * (h + pad)))
    return _png(out)
