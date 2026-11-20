"""Pictures of the CREATPRF module: the 40 pictures of the colour and monochrome profiles and of the profile editor.

    python3 OMKit/tools/mkicons_creatprf.py OMKit/resources/modules/creatprf/icons               # write the resources
    python3 OMKit/tools/mkicons_creatprf.py OMKit/resources/modules/creatprf/icons --check       # compare, write nothing
    python3 OMKit/tools/mkicons_creatprf.py OMKit/resources/modules/creatprf/icons --png DIR     # and a 1:1 PNG of each
    python3 OMKit/tools/mkicons_creatprf.py DIR --design lines                                   # the previous set

The pictures are committed in OMKit/resources/modules/creatprf/icons, and the module build takes them from there
(src/modules/Makefile, mkmod.py --res-disk): run this tool again only to change them, then check that the build and
the camera show what is expected. Requires Python 3.9+ and numpy (a manual tool: the build itself does not run it).
The output is deterministic (no randomness, no fonts of the system): --check compares it with the committed files.

Design "type" (the default; CREATPRF 1.4) - letters only, one stroke face for every picture:
  profile pictures (ids 0x1xx..0x4xx) - a badge with the profile's code: a colour profile is "C1".."C3" in a hollow
      rounded frame, a monochrome one "M1".."M3" cut out of a solid rounded plate. The strip and the large icon carry
      the same drawing (as the camera's own pairs do), the menu line, the control panel cell and the shooting screen
      sign share a smaller one;
  editor pictures (ids 0x5xx) - words: "Color" over "Level", "Grain" over Off / Low / Med / High, the grain choice bar
      the level's word alone; the dial label "Level" (the other one of Color/Level is the camera's own "Color") in the
      face of the camera's dial labels, drawn by dial_label.py as every module's dial label is.
  The face is geometry only (segments and circular arcs, round ends, one line width per size class): a narrow rounded
  grotesque with straight-sided bowls. A glyph is rasterised once per size class and placed at whole pixels, so the
  same letter is the same pixels in every picture of that class (GLYPH_MARKS lists the placements for a check).
Design "lines" (CREATPRF 1.1..1.3) - pictograms (three rings / a quartered square) and the profile number; the editor
  pictures are words in a round stroke font. Kept for comparison.

  id               place                                   size                    outline profile (design "type")
  0x100|p<<2       picture mode strip                      66 x 50                 icon
  0x100|p<<2|1     large icon of the selected cell         80 x 60                 icon
  0x200|p<<1       menu line "Picture Mode"                <= 124 x 39, cut        label
  0x300|p<<1       control panel cell                      128 x 39                scp
  0x400|p<<1       shooting screen sign                    <= 80 x 36, cut         shoot
  0x500            editor item "Color / Level"             66 x 50                 icon
  0x501..0x504     editor item "Grain" Off..High           66 x 50                 icon
  0x505..0x508     the grain choice bar Off..High          80 x 60                 icon
  0x50a            dial label "Level"                      <= 56 x 24              dial_label.py
  p: profile, 1..3 colour, 0x11..0x13 monochrome (OMK_PROF_*); "edited" variants are not drawn (the plain one is shown)

Method: a picture is painted in samples (8 x 8 per pixel) as coverage 0..1 (1 the sign); a pixel's mean gives the
meaning value (OMK_ICON_*): >= 0.75 sign (1), >= 0.4 transition 3, > 0.1 transition 4. The outline (2), outline edge
(5) and halo (6) grow from the sign by pixel rules measured on the camera's stock pictures of the same places (the
profile column above; PROFILES_TYPE tells how well each rule repeats the stock pictures). Values are 0..6 only.

Resource: type 1, id = OMK_PIC_*, file 1_<id>.bin: u16 width, u16 height (little endian), then 4-bit pixels, the high
nibble first, each row a whole number of bytes. The camera takes the height exactly and the width up to the size above.
"""
import math
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dial_label                                   # noqa: E402  the dial labels' face, shared with mkicons_film.py

# ====================================================================================================================
# Design "lines" (the previous set) and the shared parts: samples, thresholds, growth of the outline
# ====================================================================================================================

SS = 8                                              # samples per pixel along each axis
SIGN_T = (0.75, 0.40, 0.10)                         # sign, transition 3, transition 4 thresholds of a pixel's mean

# outline: (meanings it grows from, squared radius); edge: the same; halo: ("sign" | "outline", squared radius)
PROFILES = {
    "icon": {"out": ((1, 3, 4), 5), "edge": None, "halo": None},
    "label": {"out": ((1, 3, 4), 1), "edge": None, "halo": ("sign", 4)},
    "shoot": {"out": ((1, 3), 1), "edge": ((1, 3), 2), "halo": ("outline", 1)},
    "scp": {"out": None, "edge": None, "halo": None},
    "text": {"out": ((1, 3, 4), 2), "edge": None, "halo": None},
}

# preview colours of the meanings: the OSD plane palette as measured on screen frames (icons; menu line and panel cell)
ICON_RGB = {1: (254, 254, 254), 2: (0, 0, 0), 3: (170, 170, 170), 4: (85, 85, 85), 5: (3, 3, 3), 6: (5, 5, 5)}
LABEL_RGB = {1: (255, 255, 255), 2: (0, 0, 0), 3: (179, 180, 175), 4: (106, 107, 99), 5: (3, 3, 3), 6: (5, 5, 5)}
PREVIEW_BG = (29, 29, 29)

GRAIN = ["Off", "Low", "Med", "High"]


# ---------- canvas ----------

class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.xs = (np.arange(w * SS) + 0.5) / SS
        self.ys = (np.arange(h * SS) + 0.5) / SS
        self.lum = np.zeros((h * SS, w * SS), np.float32)

    def window(self, x0, y0, x1, y1):
        """The samples covering a rectangle and their coordinates (X, Y)"""
        i0, i1 = np.searchsorted(self.xs, [x0, x1])
        j0, j1 = np.searchsorted(self.ys, [y0, y1])
        sl = (slice(j0, j1), slice(i0, i1))
        return sl, self.xs[None, i0:i1], self.ys[j0:j1, None]

    def paint(self, sl, mask, tone=1.0):
        self.lum[sl][mask] = tone

    def rect(self, x0, y0, x1, y1, tone=1.0, r=0.0):
        sl, X, Y = self.window(x0, y0, x1, y1)
        if r <= 0:
            m = np.ones(np.broadcast(X, Y).shape, bool)
        else:
            cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
            dx = np.maximum(np.abs(X - cx) - ((x1 - x0) / 2 - r), 0)
            dy = np.maximum(np.abs(Y - cy) - ((y1 - y0) / 2 - r), 0)
            m = dx * dx + dy * dy <= r * r
        self.paint(sl, m, tone)

    def frame(self, x0, y0, x1, y1, s, tone=1.0, r=0.0):
        """A frame s thick inside the rectangle; what was inside it stays"""
        keep = self.lum.copy()
        self.rect(x0, y0, x1, y1, tone, r)
        sl, X, Y = self.window(x0 + s, y0 + s, x1 - s, y1 - s)
        ri = max(r - s, 0)
        if ri <= 0:
            m = np.ones(np.broadcast(X, Y).shape, bool)
        else:
            cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
            dx = np.maximum(np.abs(X - cx) - ((x1 - x0) / 2 - s - ri), 0)
            dy = np.maximum(np.abs(Y - cy) - ((y1 - y0) / 2 - s - ri), 0)
            m = dx * dx + dy * dy <= ri * ri
        self.lum[sl][m] = keep[sl][m]

    def ring(self, cx, cy, r, s, tone=1.0):
        sl, X, Y = self.window(cx - r - s, cy - r - s, cx + r + s, cy + r + s)
        d = np.sqrt((X - cx) ** 2 + (Y - cy) ** 2)
        self.paint(sl, np.abs(d - r) <= s / 2, tone)

    def capsule(self, ax, ay, bx, by, s, tone=1.0):
        p = s / 2 + 0.5
        sl, X, Y = self.window(min(ax, bx) - p, min(ay, by) - p, max(ax, bx) + p, max(ay, by) + p)
        vx, vy = bx - ax, by - ay
        t = np.clip(((X - ax) * vx + (Y - ay) * vy) / (vx * vx + vy * vy or 1e-9), 0, 1)
        self.paint(sl, (X - ax - t * vx) ** 2 + (Y - ay - t * vy) ** 2 <= (s / 2) ** 2, tone)

    def polyline(self, pts, s, tone=1.0):
        for (ax, ay), (bx, by) in zip(pts[:-1], pts[1:]):
            self.capsule(ax, ay, bx, by, s, tone)

    def values(self, profile):
        """Meaning values (h x w, uint8) with the outline, edge and halo of the profile"""
        b = self.lum.reshape(self.h, SS, self.w, SS).mean(axis=(1, 3))
        v = np.zeros((self.h, self.w), np.uint8)
        v[b > SIGN_T[2]] = 4
        v[b >= SIGN_T[1]] = 3
        v[b >= SIGN_T[0]] = 1
        return outline(v, PROFILES[profile])


def grow(m, r2):
    """Dilate a mask by a disc of squared radius r2"""
    R = int(math.isqrt(r2))
    p = np.pad(m, R)
    H, W = m.shape
    out = np.zeros_like(m)
    for dy in range(-R, R + 1):
        for dx in range(-R, R + 1):
            if dx * dx + dy * dy <= r2:
                out |= p[R + dy:R + dy + H, R + dx:R + dx + W]
    return out


def outline(v, p):
    v = v.copy()
    s = np.isin(v, (1, 3, 4))
    outl = grow(np.isin(v, p["out"][0]), p["out"][1]) & ~s if p["out"] else np.zeros_like(s)
    if p["halo"]:
        src, r2 = p["halo"]
        v[grow(s | outl if src == "outline" else s, r2) & ~s] = 6
    if p["edge"]:
        v[grow(np.isin(v, p["edge"][0]), p["edge"][1]) & ~s & ~outl] = 5
    v[outl] = 2
    return v


# ---------- stroke font ----------
# Box 0..w x 0..1, y down, 1 = the capital height, lower case from 0.3. A stroke is ("L", x0, y0, x1, y1, ...) a polyline
# or ("A", cx, cy, rx, ry, a0, a1) an arc of an ellipse in degrees, point = (cx + rx cos a, cy + ry sin a).

GLYPHS = {
    "1": (0.40, [("L", 0.04, 0.26, 0.32, 0.0, 0.32, 1.0)]),
    "2": (0.62, [("A", 0.30, 0.30, 0.30, 0.30, 190, 400), ("L", 0.53, 0.493, 0.0, 1.0, 0.62, 1.0)]),
    "3": (0.60, [("A", 0.29, 0.255, 0.27, 0.255, 205, 450), ("A", 0.29, 0.755, 0.29, 0.245, 270, 520)]),
    "C": (0.68, [("A", 0.36, 0.5, 0.36, 0.5, 48, 312)]),
    "O": (0.74, [("A", 0.37, 0.5, 0.37, 0.5, 0, 360)]),
    "G": (0.72, [("A", 0.36, 0.5, 0.36, 0.5, 0, 312), ("L", 0.72, 0.5, 0.42, 0.5)]),
    "M": (0.80, [("L", 0, 1, 0, 0, 0.4, 0.72, 0.8, 0, 0.8, 1)]),
    "L": (0.50, [("L", 0, 0, 0, 1, 0.5, 1)]),
    "F": (0.54, [("L", 0.54, 0, 0, 0, 0, 1), ("L", 0, 0.47, 0.45, 0.47)]),
    "H": (0.64, [("L", 0, 0, 0, 1), ("L", 0.64, 0, 0.64, 1), ("L", 0, 0.5, 0.64, 0.5)]),
    "o": (0.56, [("A", 0.28, 0.65, 0.28, 0.35, 0, 360)]),
    "l": (0.0, [("L", 0, 0, 0, 1)]),
    "i": (0.0, [("L", 0, 0.3, 0, 1), ("L", 0, 0.03, 0, 0.05)]),
    "r": (0.36, [("L", 0, 0.3, 0, 1), ("A", 0.27, 0.57, 0.27, 0.27, 180, 290)]),
    "n": (0.52, [("L", 0, 0.3, 0, 1), ("A", 0.26, 0.56, 0.26, 0.26, 180, 360), ("L", 0.52, 0.56, 0.52, 1)]),
    "h": (0.52, [("L", 0, 0, 0, 1), ("A", 0.26, 0.56, 0.26, 0.26, 180, 360), ("L", 0.52, 0.56, 0.52, 1)]),
    "e": (0.56, [("A", 0.28, 0.65, 0.28, 0.35, 42, 360), ("L", 0, 0.65, 0.56, 0.65)]),
    "v": (0.54, [("L", 0, 0.3, 0.27, 1, 0.54, 0.3)]),
    "w": (0.80, [("L", 0, 0.3, 0.2, 1, 0.4, 0.42, 0.6, 1, 0.8, 0.3)]),
    "t": (0.36, [("L", 0.12, 0.06, 0.12, 0.8), ("A", 0.32, 0.8, 0.2, 0.2, 180, 80), ("L", 0, 0.3, 0.34, 0.3)]),
    "f": (0.36, [("L", 0.12, 1, 0.12, 0.2), ("A", 0.32, 0.2, 0.2, 0.2, 180, 280), ("L", 0, 0.3, 0.34, 0.3)]),
    "a": (0.54, [("A", 0.27, 0.65, 0.27, 0.35, 0, 360), ("L", 0.54, 0.3, 0.54, 1)]),
    "d": (0.54, [("A", 0.27, 0.65, 0.27, 0.35, 0, 360), ("L", 0.54, 0, 0.54, 1)]),
    "g": (0.54, [("A", 0.27, 0.65, 0.27, 0.35, 0, 360), ("L", 0.54, 0.3, 0.54, 1.08), ("A", 0.27, 1.08, 0.27, 0.22, 0, 150)]),
    " ": (0.16, []),
}
DIGIT_W = 0.62                                      # the place of a digit (digits are centred in it)


def _points(st):
    if st[0] == "L":
        return [(st[i], st[i + 1]) for i in range(1, len(st), 2)]
    _, cx, cy, rx, ry, a0, a1 = st
    n = max(4, int(abs(a1 - a0) / 12))
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * k / n)), cy + ry * math.sin(math.radians(a0 + (a1 - a0) * k / n)))
            for k in range(n + 1)]


def auto_gap(cap, s):
    """Letter spacing in capitals: the gap between neighbouring strokes is at least 1.8 px and 0.1 capital"""
    return (s + max(1.8, 0.1 * cap)) / cap


def text_width(text, cap, gap, fixed=None):
    """Width along the stroke axes (without the line width)"""
    return sum(((fixed if fixed and c.isdigit() else GLYPHS[c][0]) + gap) * cap for c in text) - gap * cap


def draw_text(cv, text, x, top, height, s, gap=None, tone=1.0, fixed=None):
    """A line of text: the outer top of the capitals at top, the outer capital height (with the line s) height"""
    cap = height - s
    gap = auto_gap(cap, s) if gap is None else gap
    y0 = top + s / 2
    x += s / 2
    for c in text:
        w, strokes = GLYPHS[c]
        adv = fixed if fixed and c.isdigit() else w
        ox = x + (adv - w) / 2 * cap
        for st in strokes:
            cv.polyline([(ox + px * cap, y0 + py * cap) for px, py in _points(st)], s, tone)
        x += (adv + gap) * cap
    return x - gap * cap + s / 2


def outer_width(text, height, s, gap=None, fixed=None):
    return text_width(text, height - s, auto_gap(height - s, s) if gap is None else gap, fixed) + s


# ---------- profile pictures ----------

def polar(cx, cy, r, deg):
    return cx + math.cos(math.radians(deg)) * r, cy + math.sin(math.radians(deg)) * r


def clip_round(cv, cx, cy, a, r):
    """Clear everything outside a rounded square of side 2a"""
    sl, X, Y = cv.window(cx - a - 2, cy - a - 2, cx + a + 2, cy + a + 2)
    dx = np.maximum(np.abs(X - cx) - (a - r), 0)
    dy = np.maximum(np.abs(Y - cy) - (a - r), 0)
    cv.paint(sl, (dx * dx + dy * dy > r * r) | (np.abs(Y - cy) > a + 1e-6) | (np.abs(X - cx) > a + 1e-6), 0.0)


def pictogram(cv, mono, cx, cy, R, s):
    """Centre (cx, cy), half size R, line s"""
    if mono:                                        # a frame with two filled quarters: black and white
        a, r = R * 0.92, R * 0.28
        cv.rect(cx - a, cy - a, cx, cy)
        cv.rect(cx, cy, cx + a, cy + a)
        clip_round(cv, cx, cy, a, r)
        cv.frame(cx - a, cy - a, cx + a, cy + a, s, r=r)
    else:                                           # three overlapping rings
        rr, off = R * 0.60, R * 0.46
        for k in range(3):
            x, y = polar(cx, cy + 0.06 * R, off, -90 + 120 * k)
            cv.ring(x, y, rr - s / 2, s)


def sign_digit(cv, mono, n, H, xc=None, x0=None):
    """The pictogram on the left, the number on the right: centred on xc or from the left edge x0"""
    yc = H / 2
    R = round(0.30 * H if H >= 50 else 0.32 * H)
    gap = 0.13 * H
    dh = round(0.56 * H)
    s = max(2.4, 0.060 * H)                         # the number's line
    dw = DIGIT_W * (dh - s) + s
    total = 2 * R + gap + dw
    left = xc - total / 2 if x0 is None else x0
    pictogram(cv, mono, left + R, yc, R, max(2.0, 0.050 * H))
    draw_text(cv, str(n), left + 2 * R + gap, round(yc - dh / 2), dh, s, fixed=DIGIT_W)


# ---------- editor pictures ----------

def word_line(cv, text, H, height, s, xc=None, x0=None, top=None, gap=None):
    w = outer_width(text, height, s, gap, fixed=DIGIT_W)
    left = xc - w / 2 if x0 is None else x0
    draw_text(cv, text, left, round((H - height) / 2) if top is None else top, height, s, gap, fixed=DIGIT_W)


def two_lines(cv, W, H, a, b):
    """Two centred lines: capitals 15 px of 50, lines 21 px apart (scaled to H)"""
    k = H / 50
    s = 2.2 * k
    h1 = round(15 * k)
    word_line(cv, a, H, h1, s, xc=W / 2, top=round(5 * k))
    word_line(cv, b, H, h1, s, xc=W / 2, top=round(26 * k))


# ====================================================================================================================
# Design "type": one stroke face, badges with the profile's code, words
# ====================================================================================================================

# Outline, edge and halo of the camera's stock pictures as growth rules: each is a union of (meanings, squared radius)
# discs, taken in the order outline, edge, halo, never over the sign or an earlier ring. Fitted to stock pictures of
# the same places (wrong pixels of all the ring's pixels):
#   icon   strip, large icon, column item: outline exact in 7 pictures of 7
#   label  menu line: outline + halo together exact in 2 of 2 (the dark surround); their split 77 of 523 pixels off
#   shoot  shooting screen: outline + edge + halo together exact in 2 of 2; the outline alone 19 of 308 off
# (the dial labels' rule - outline 90 of 837 pixels off, the whole surround 59 of 917 - is dial_label.RINGS)
PROFILES_TYPE = {
    "icon": {"out": (((1, 3, 4), 5),), "edge": (), "halo": ()},
    "label": {"out": (((1,), 2), ((1, 3, 4), 1)), "edge": (), "halo": (((1,), 5),)},
    "shoot": {"out": (((1,), 2),), "edge": (((1, 3), 2),), "halo": (((1,), 5),)},
    "scp": {"out": (), "edge": (), "halo": ()},
}


def outline_type(v, p, dark):
    """Rings of profile p around the sign; dark - pixels that are outline wherever they are not the sign (the inside
    of a frame, letters cut out of a plate), only where the profile has an outline at all"""
    v = v.copy()
    s = np.isin(v, (1, 3, 4))
    taken = s.copy()
    if p["out"]:
        taken |= dark
    rings = []
    for key in ("out", "edge", "halo"):
        m = np.zeros_like(s)
        for vals, r2 in p[key]:
            m |= grow(np.isin(v, vals), r2)
        m &= ~taken
        taken |= m
        rings.append(m)
    if p["out"]:
        rings[0] |= dark & ~s
    for m, value in zip(rings, (2, 5, 6)):
        v[m] = value
    return v


# ---------- the stroke face ----------
# A glyph is a list of centre lines (polylines; arcs are sampled every 3 degrees or finer) drawn with a round pen of
# the class's line width. Glyph box: x 0..W, y 0 the top of the capitals, cap their bottom (the base line), the
# x-height xh above the base line, the descender desc below it; all whole pixels, so the outer edges of stems and bars
# fall on pixel borders. The centre lines stay half a line inside the box: L, R, T, B (base), X (x-height), D.

class Box:
    def __init__(self, face, W):
        h = face.s / 2
        self.W, self.h, self.cap = W, h, face.cap
        self.L, self.R, self.T, self.B = h, W - h, h, face.cap - h
        self.X, self.D = face.cap - face.xh + h, face.cap + face.desc - h
        self.cx = W / 2


def _arc(cx, cy, r, a0, a1):
    """Points of a circular arc from angle a0 to a1, degrees, y down (0 right, 90 down, 180 left, 270 up)"""
    n = max(2, math.ceil(abs(a1 - a0) / 3))
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * k / n)), cy + r * math.sin(math.radians(a0 + (a1 - a0) * k / n)))
            for k in range(n + 1)]


def _bowl(L, T, R, B):
    """A closed bowl: straight sides, half circles at the top and the bottom (or the sides, if it is wider than tall)"""
    r = min(R - L, B - T) / 2
    p = _arc(R - r, T + r, r, -90, 0) + _arc(R - r, B - r, r, 0, 90) + _arc(L + r, B - r, r, 90, 180) + \
        _arc(L + r, T + r, r, 180, 270)
    return p + [p[0]]


def _bar_y(b, y):
    """The centre of a horizontal bar near y with its top edge on a pixel border"""
    return math.floor(y - b.h + 0.5) + b.h


def _g_C(b):
    r = (b.R - b.L) / 2
    return [_arc(b.cx, b.T + r, r, 322, 180) + _arc(b.cx, b.B - r, r, 180, 38)]


def _g_G(b):
    r = (b.R - b.L) / 2
    mid = _bar_y(b, b.T + 0.55 * (b.B - b.T))
    return [_arc(b.cx, b.T + r, r, 322, 180) + _arc(b.cx, b.B - r, r, 180, 0) + [(b.R, mid), (b.cx + 0.05 * b.W, mid)]]


def _g_O(b):
    return [_bowl(b.L, b.T, b.R, b.B)]


def _g_M(b):
    return [[(b.L, b.B), (b.L, b.T), (b.cx, b.T + 0.62 * (b.B - b.T)), (b.R, b.T), (b.R, b.B)]]


def _g_L(b):
    return [[(b.L, b.T), (b.L, b.B), (b.R, b.B)]]


def _g_F(b):
    mid = _bar_y(b, b.T + 0.47 * (b.B - b.T))
    return [[(b.R, b.T), (b.L, b.T), (b.L, b.B)], [(b.L, mid), (b.R - 0.14 * b.W, mid)]]


def _g_H(b):
    mid = _bar_y(b, (b.T + b.B) / 2)
    return [[(b.L, b.T), (b.L, b.B)], [(b.R, b.T), (b.R, b.B)], [(b.L, mid), (b.R, mid)]]


def _digit_1(b):
    w = round(0.48 * b.cap)
    w += (b.W + w) % 2                              # the stem's right edge on a pixel border
    left, right = (b.W - w) / 2 + b.h, (b.W + w) / 2 - b.h
    return [[(left, b.T + 0.3 * (b.B - b.T)), (right, b.T), (right, b.B)]]


def _digit_2(b):
    r = (b.R - b.L) / 2
    cy = b.T + r
    d = math.hypot(b.L - b.cx, b.B - cy)            # the diagonal leaves the bowl along its tangent
    a = math.degrees(math.atan2(b.B - cy, b.L - b.cx) - math.acos(r / d))
    return [_arc(b.cx, cy, r, 195, 360 + a) + [(b.L, b.B), (b.R, b.B)]]


def _digit_3(b):
    half = (b.B - b.T) / 2
    r2 = min(0.265 * (b.B - b.T), (b.R - b.L) / 2)   # the lower bowl a little larger than the upper one
    r1 = half - r2
    off = (b.R - b.L - 2 * r2) / 2
    return [_arc(b.R - off - r1, b.T + r1, r1, 205, 450) + _arc(b.R - off - r2, b.B - r2, r2, 270, 515)]


def _g_o(b):
    return [_bowl(b.L, b.X, b.R, b.B)]


def _g_l(b):
    return [[(b.L, b.T), (b.L, b.B)]]


def _g_i(b):
    return [[(b.L, b.X), (b.L, b.B)], [(b.L, b.T), (b.L, b.T + 0.5)]]     # the dot a little taller than round


def _g_r(b):
    r = (b.R - b.L) / 1.5
    return [[(b.L, b.X), (b.L, b.B)], _arc(b.L + r, b.X + r, r, 180, 300)]


def _g_n(b, top=None):
    r = (b.R - b.L) / 2
    return [[(b.L, b.X if top is None else top), (b.L, b.B)], _arc(b.cx, b.X + r, r, 180, 360) + [(b.R, b.B)]]


def _g_h(b):
    return _g_n(b, b.T)


def _g_e(b):
    r = (b.R - b.L) / 2
    mid = (b.X + b.B) / 2
    return [[(b.L, mid), (b.R, mid)] + _arc(b.cx, b.X + r, r, 0, -180) + _arc(b.cx, b.B - r, r, 180, 42)]


def _g_v(b):
    return [[(b.L, b.X), (b.cx, b.B), (b.R, b.X)]]


def _g_w(b):
    q = (b.R - b.L) / 4
    return [[(b.L, b.X), (b.L + q, b.B), (b.cx, b.X + 0.12 * (b.B - b.X)), (b.R - q, b.B), (b.R, b.X)]]


def _stem_x(b):
    return math.floor(0.3 * (b.W - 2 * b.h)) + b.h   # the stem of f and t: its left edge on a pixel border


def _g_t(b):
    x = _stem_x(b)
    r = b.R - x
    return [[(x, b.X - 0.2 * b.cap)] + _arc(x + r, b.B - r, r, 180, 65), [(b.L, b.X), (b.R, b.X)]]


def _g_f(b):
    x = _stem_x(b)
    r = b.R - x
    return [[(x, b.B)] + _arc(x + r, b.T + r, r, 180, 295), [(b.L, b.X), (b.R, b.X)]]


def _g_a(b):
    return [_bowl(b.L, b.X, b.R, b.B), [(b.R, b.X), (b.R, b.B)]]


def _g_d(b):
    return [_bowl(b.L, b.X, b.R, b.B), [(b.R, b.T), (b.R, b.B)]]


def _g_g(b):
    r = (b.R - b.L) / 2
    return [_bowl(b.L, b.X, b.R, b.B), [(b.R, b.X)] + _arc(b.cx, b.D - r, r, 0, 140)]


# glyph: (width in capital heights; None - one stem, "digit" - the class's digit place), sides for the letter gap
# (s straight stem, r round or open), centre lines
FACE_GLYPHS = {
    "C": (0.62, "rr", _g_C), "G": (0.64, "rr", _g_G), "O": (0.66, "rr", _g_O), "M": (0.80, "ss", _g_M),
    "L": (0.50, "sr", _g_L), "F": (0.52, "sr", _g_F), "H": (0.66, "ss", _g_H),
    "1": ("digit", "rr", _digit_1), "2": ("digit", "rr", _digit_2), "3": ("digit", "rr", _digit_3),
    "o": (0.56, "rr", _g_o), "e": (0.56, "rr", _g_e), "a": (0.56, "rs", _g_a), "d": (0.56, "rs", _g_d),
    "g": (0.56, "rs", _g_g), "n": (0.54, "ss", _g_n), "h": (0.54, "ss", _g_h), "r": (0.38, "sr", _g_r),
    "v": (0.56, "rr", _g_v), "w": (0.84, "rr", _g_w), "t": (0.40, "rr", _g_t), "f": (0.40, "rr", _g_f),
    "l": (None, "ss", _g_l), "i": (None, "ss", _g_i),
}


def stroke_cover(w, h, paths, s):
    """Coverage (h x w, 0..1) of centre lines drawn with a round pen s wide"""
    X = ((np.arange(w * SS) + 0.5) / SS)[None, :]
    Y = ((np.arange(h * SS) + 0.5) / SS)[:, None]
    m = np.zeros((h * SS, w * SS), bool)
    for pts in paths:
        for (ax, ay), (bx, by) in zip(pts[:-1], pts[1:]):
            vx, vy = bx - ax, by - ay
            t = np.clip(((X - ax) * vx + (Y - ay) * vy) / (vx * vx + vy * vy or 1e-9), 0, 1)
            m |= (X - ax - t * vx) ** 2 + (Y - ay - t * vy) ** 2 <= (s / 2) ** 2
    return m.reshape(h, SS, w, SS).mean(axis=(1, 3)).astype(np.float32)


def round_rect_cover(w, h, r):
    """Coverage of a rounded rectangle filling w x h, corner radius r"""
    X = ((np.arange(w * SS) + 0.5) / SS)[None, :]
    Y = ((np.arange(h * SS) + 0.5) / SS)[:, None]
    dx = np.maximum(np.abs(X - w / 2) - (w / 2 - r), 0)
    dy = np.maximum(np.abs(Y - h / 2) - (h / 2 - r), 0)
    return (dx * dx + dy * dy <= r * r).reshape(h, SS, w, SS).mean(axis=(1, 3)).astype(np.float32)


class Face:
    """A size class of the face. cap, xh, desc - outer heights of the capitals, of the lower case and the descender;
    s the line; gap the letter gap (one more between two straight stems); space the word space; digit the place of a
    digit; widths - glyph widths set by hand; badge (width, height, frame line, corner radius, plate inset) of a
    profile's code: the solid plate of a monochrome profile is smaller than the frame by the inset on every side"""

    def __init__(self, name, cap, xh, desc, s, gap, space, digit, widths=None, badge=None):
        self.name, self.cap, self.xh, self.desc, self.s = name, cap, xh, desc, s
        self.gap, self.space, self.digit, self.widths, self.badge = gap, space, digit, widths or {}, badge
        self._glyphs = {}

    def glyph(self, c):
        """(coverage (cap + desc) x W, sides)"""
        if c not in self._glyphs:
            frac, sides, fn = FACE_GLYPHS[c]
            W = self.widths.get(c) or (self.digit if frac == "digit" else math.ceil(self.s) if frac is None
                                       else round(frac * self.cap))
            self._glyphs[c] = (stroke_cover(W, self.cap + self.desc, fn(Box(self, W)), self.s), sides)
        return self._glyphs[c]

    def layout(self, text):
        """[(character, x)] and the width of a line"""
        out, x, prev = [], 0, None
        for c in text:
            if c == " ":
                x += self.space
                prev = None
                continue
            cov, sides = self.glyph(c)
            if prev is not None:
                x += self.gap + (1 if prev == "s" and sides[0] == "s" else 0)
            out.append((c, x))
            x += cov.shape[1]
            prev = sides[1]
        return out, x


# The classes. Lines: the stock digits and letters are 2.5..2.8 px in the strip and the menu line, 2..2.5 px in the
# column items and dial labels (measured on the stock pictures); a cut-out letter needs the heavier end of that.
FACE_A = Face("A", cap=20, xh=14, desc=5, s=3.0, gap=3, space=7, digit=12, badge=(45, 34, 3, 7, 1))
FACE_B = Face("B", cap=16, xh=11, desc=4, s=2.5, gap=3, space=6, digit=10, widths={"M": 14}, badge=(37, 26, 2, 5, 2))
FACE_T = Face("T", cap=16, xh=11, desc=4, s=2.25, gap=2, space=8, digit=10)
FACES = {f.name: f for f in (FACE_A, FACE_B, FACE_T)}
# The plate inset: a solid plate looks larger and heavier than a frame of the same size. 1 px where both stand in one
# row (the strip), 2 px where a sign stands alone (menu line, control panel, shooting screen) and the plate would
# otherwise be the heaviest sign of the screen.

GLYPH_MARKS = {}                                    # id: [(character, class, x, y, width, height, cut out)]


class Cover:
    """A picture as coverage per pixel; shapes are placed at whole pixels"""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.a = np.zeros((h, w), np.float32)
        self.dark = np.zeros((h, w), bool)
        self.marks = []

    def stamp(self, cov, x, y, cut=False):
        if x != int(x) or y != int(y):
            raise ValueError(f"a shape at a part of a pixel: {x}, {y}")
        x, y = int(x), int(y)
        h, w = cov.shape
        if x < 0 or y < 0 or x + w > self.w or y + h > self.h:
            raise ValueError("a shape outside the picture")
        part = self.a[y:y + h, x:x + w]
        if cut:
            np.minimum(part, 1 - cov, out=part)
        else:
            np.maximum(part, cov, out=part)

    def text(self, face, text, x, top, cut=False):
        for c, dx in face.layout(text)[0]:
            cov = face.glyph(c)[0]
            self.stamp(cov, x + dx, top, cut)
            self.marks.append((c, face.name, int(x + dx), int(top), cov.shape[1], cov.shape[0], cut))

    def values(self, profile):
        v = np.zeros((self.h, self.w), np.uint8)
        v[self.a > SIGN_T[2]] = 4
        v[self.a >= SIGN_T[1]] = 3
        v[self.a >= SIGN_T[0]] = 1
        return outline_type(v, PROFILES_TYPE[profile], self.dark)


def badge(cv, face, mono, n, x, y):
    """The profile's code at (x, y): "C<n>" in a hollow frame, "M<n>" cut out of a plate. The inside of the frame and
    the cut-out letters are marked dark: the outline fills them evenly (left to the growth rules, the 3..4 px gaps
    would be filled anyway, but for ragged islands of the background)"""
    bw, bh, t, r, i = face.badge
    code = ("M" if mono else "C") + str(n)
    width = face.layout(code)[1]
    tx, ty = x + (bw - width) / 2, y + (bh - face.cap) / 2
    if mono:
        plate = round_rect_cover(bw - 2 * i, bh - 2 * i, r - i)
        cv.stamp(plate, x + i, y + i)
        cv.text(face, code, tx, ty, cut=True)
        cv.dark[y + i:y + bh - i, x + i:x + bw - i] |= plate >= 0.5
    else:
        frame = round_rect_cover(bw, bh, r)
        inner = round_rect_cover(bw - 2 * t, bh - 2 * t, r - t)
        frame[t:bh - t, t:bw - t] -= inner
        cv.stamp(frame, x, y)
        cv.text(face, code, tx, ty)
        cv.dark[y + t:y + bh - t, x + t:x + bw - t] |= inner >= 0.5


def centred(cv, face, text, top):
    cv.text(face, text, (cv.w - face.layout(text)[1]) // 2, top)


def render_type(pic):
    """-> (meaning values before the cut, glyph marks)"""
    pid, name, kind, W, H, cut, arg = pic
    cv = Cover(W, H)
    if kind == "mode":                              # the strip and the large icon: the same drawing
        bw, bh = FACE_A.badge[:2]
        badge(cv, FACE_A, arg[0], arg[1], (W - bw) // 2, (H - bh) // 2)
    elif kind in ("label", "panel", "shoot"):
        bw, bh = FACE_B.badge[:2]
        left = 3 - (FACE_B.badge[4] if arg[0] else 0)   # a left-aligned place: the plate starts where the frame does
        badge(cv, FACE_B, arg[0], arg[1], (W - bw) // 2 if kind == "panel" else left, (H - bh) // 2)
    elif kind == "item":                            # two lines 6 px apart
        centred(cv, FACE_T, arg[0], 5)
        centred(cv, FACE_T, arg[1], 5 + FACE_T.cap + 6)
    else:                                           # "bar": the level's word alone, in the large class
        centred(cv, FACE_A, arg[1], (H - FACE_A.cap) // 2)
    return cv.values(PROFILE_TYPE_OF[kind]), cv.marks


PROFILE_TYPE_OF = {"mode": "icon", "panel": "scp", "label": "label", "shoot": "shoot", "item": "icon", "bar": "icon"}


# ====================================================================================================================
# The set
# ====================================================================================================================

DESIGNS = ("type", "lines")


def pictures():
    """[(id, name, kind, width (the largest one for a cut kind), height, cut to the content, argument)]"""
    out = []
    for mono in (0, 1):
        for n in (1, 2, 3):
            prof = mono << 4 | n
            nm = ("mono" if mono else "color") + str(n)
            out += [(0x100 | prof << 2, nm + "_mode", "mode", 66, 50, False, (mono, n)),
                    (0x100 | prof << 2 | 1, nm + "_large", "mode", 80, 60, False, (mono, n)),
                    (0x200 | prof << 1, nm + "_label", "label", 124, 39, True, (mono, n)),
                    (0x300 | prof << 1, nm + "_panel", "panel", 128, 39, False, (mono, n)),
                    (0x400 | prof << 1, nm + "_shoot", "shoot", 80, 36, True, (mono, n))]
    out.append((0x500, "color_level", "item", 66, 50, False, ("Color", "Level")))
    out += [(0x501 + g, "grain_" + GRAIN[g].lower(), "item", 66, 50, False, ("Grain", GRAIN[g])) for g in range(4)]
    out += [(0x505 + g, "grain_bar_" + GRAIN[g].lower(), "bar", 80, 60, False, ("Grain", GRAIN[g])) for g in range(4)]
    out.append((0x50a, "dial_level", "dial", 56, 24, True, "Level"))   # 0x509 "Color Filter": the camera's "Color" now
    return out


PROFILE_OF = {"mode": "icon", "panel": "scp", "label": "label", "shoot": "shoot", "item": "text", "bar": "text"}


def render_lines(pic):
    """Design "lines" -> meaning values before the cut"""
    pid, name, kind, W, H, cut, arg = pic
    cv = Canvas(W, H)
    if kind in ("mode", "panel"):
        sign_digit(cv, arg[0], arg[1], H, xc=W / 2)
    elif kind in ("label", "shoot"):
        sign_digit(cv, arg[0], arg[1], H, x0=3)
    else:
        two_lines(cv, W, H, *arg)
    return cv.values(PROFILE_OF[kind])


def render(pic, design="type"):
    """-> meaning values (h x w, uint8)"""
    pid, name, kind, W, H, cut, arg = pic
    if kind == "dial":                              # every design: the dial labels' own face, cut and centred there
        GLYPH_MARKS.pop(pid, None)
        return np.array(dial_label.label(arg)[1], np.uint8)
    if design == "type":
        v, marks = render_type(pic)
    else:
        v, marks = render_lines(pic), []
    if v[:, 0].any() or v[:, -1].any() or v[0].any() or v[-1].any():
        raise ValueError(f"{pid:#x} {name}: the picture touches its border")
    if cut:
        used = np.nonzero(v.any(axis=0))[0]
        v = v[:, :min(W, used[-1] + 3)]
    if design == "type":
        GLYPH_MARKS[pid] = marks
    return v


def pack(v):
    """Module resource: u16 width, u16 height (little endian), 4-bit pixels, the high nibble first, whole bytes per row"""
    h, w = v.shape
    row = (w + 1) // 2
    a = np.zeros((h, row * 2), np.uint8)
    a[:, :w] = v & 15
    return struct.pack("<HH", w, h) + ((a[:, 0::2] << 4) | a[:, 1::2]).astype(np.uint8).tobytes()


def check(pic, v):
    """The camera's limits: the height exactly, the width up to the place's (exactly for the strip), values 0..6"""
    pid, name, kind, W, H, cut, _ = pic
    h, w = v.shape
    if h != H or w > W or (not cut and w != W) or int(v.max()) > 6:
        raise ValueError(f"{pid:#x} {name}: {w} x {h}, values up to {int(v.max())}; the camera takes "
                         f"{'up to ' if cut else ''}{W} x {H}, values 0..6")


def build(design="type"):
    """{id: (name, file bytes, values)}"""
    out = {}
    for pic in pictures():
        v = render(pic, design)
        check(pic, v)
        out[pic[0]] = (pic[1], pack(v), v)
    return out


def png(path, v, palette):
    """A 1:1 preview in the camera's colours on the strip background (standard library PNG writer)"""
    h, w = v.shape
    rgb = np.empty((h, w, 3), np.uint8)
    rgb[:] = PREVIEW_BG
    for k, c in palette.items():
        rgb[v == k] = c
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    out = Path(argv[0])
    design = argv[argv.index("--design") + 1] if "--design" in argv else DESIGNS[0]
    if design not in DESIGNS:
        print(f"unknown design {design}: one of {', '.join(DESIGNS)}")
        return 2
    pics = build(design)
    if "--check" in argv:                           # the committed files are exactly what this tool draws
        bad = [f"1_{pid}.bin" for pid, (_, data, _) in pics.items()
               if not (out / f"1_{pid}.bin").is_file() or (out / f"1_{pid}.bin").read_bytes() != data]
        extra = sorted(f.name for f in out.glob("*.bin") if int(f.stem.split("_")[-1]) not in pics) if out.is_dir() else []
        for name in bad + extra:
            print(f"differs or missing: {name}" if name in bad else f"not drawn by this tool: {name}")
        print(f"{len(pics)} pictures, {len(bad)} differ, {len(extra)} extra files in {out}")
        return 1 if bad or extra else 0
    out.mkdir(parents=True, exist_ok=True)
    for pid, (_, data, _) in pics.items():
        (out / f"1_{pid}.bin").write_bytes(data)
    if "--png" in argv:
        d = Path(argv[argv.index("--png") + 1])
        d.mkdir(parents=True, exist_ok=True)
        for pid, (name, _, v) in pics.items():
            png(d / f"{pid:#05x}_{name}.png", v, LABEL_RGB if (pid & 0xf00) in (0x200, 0x300) else ICON_RGB)
    print(f"{len(pics)} pictures -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
