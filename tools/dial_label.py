"""Dial labels of the OM-5 Mark II editors: one face for every module's label over the dials.

The labels the camera shows over the two dials of the Color Creator layout (widgets 0x3d9 and 0x3db, 56 x 24 at
(403, 85) and (499, 85), under the dial pictograms 0x181 and 0x3d8) are pictures. The firmware's own - "Color" 4221,
"Vivid" 4222, "Hi" 4215, "Sh" 4216, "Mid" 4339 - share one face: capitals and ascenders from row 2 to the base line at
21.25 (row 21 a part of a pixel), lower case from row 8, lines 2..2.5 px, a 1 px outline with its edge and halo (rows 0
and 23 the edge and the halo at most). A module's label must look the same - the same face, size and rows - so that it
stands at the same distance under its dial pictogram as the camera's own. This file draws a word in that face; the
generators of the modules (mkicons_creatprf.py, mkicons_film.py) take their labels from here, and nothing else should
draw one.

    python3 OMKit/tools/dial_label.py Level Prev       # print the words as meaning values (a check by eye)

The rule for a label (OMKit/docs/platform/om5m2/ui.md, section 4.5):
  - prefer the firmware's own word (the numbers above): it is the reference;
  - otherwise draw it here: letters of GLYPHS only (add one by the same metrics if needed), the picture - ink, outline,
    halo, a clear column at each side - at most 56 px wide, the stock widget; label() refuses a wider one;
  - no descenders (g, j, p, q, y): the 24 rows end 2.75 px under the base line, room for the outline and the halo only;
  - a word that does not fit or needs a descender gets a shorter word or an abbreviation, never a smaller size.

The face is geometry fitted to the firmware's pictures, not copied from them: rectangles for stems and bars, polygons
for diagonals, rings of superellipses for bowls and arches, all in pixels, y down; glyphs start at whole pixels, so the
left sides of stems are sharp as in the camera's labels. A picture is sampled 8 x 8 a pixel; a pixel's coverage gives
the sign (>= 0.75 -> 1) and its transitions (>= 0.4 -> 3, > 0.1 -> 4); the outline (2), its edge (5) and the halo (6)
grow from the sign by the pixel rules measured on the stock dial labels (RINGS). Values are OMK_ICON_* meanings; the
icon palette maps them to the palette indices of the stock labels. Standard library only; deterministic.
"""
import sys

H = 24                                              # the label's height: the camera takes it exactly
WIDGET_W = 56                                       # the stock widget: the widest picture it shows whole
SS = 8                                              # samples per pixel along each axis
SIGN_T = (0.75, 0.40, 0.10)                         # sign, transition 3, transition 4 thresholds of a pixel's mean
MARGIN = 1                                          # clear columns at each side of the halo (the stock "Color": 1)

# Vertical metrics, px from the top (measured on 4215, 4216, 4221, 4222, 4339)
TOP = 2.0                                           # capitals, ascenders: row 2 full, row 1 clear
XH = 8.0                                            # lower case: row 8 full
BASE = 21.25                                        # the base line: rows to 20 full, row 21 a quarter
OVER = 0.3                                          # round letters go this far over the top and under the base line
STEM = 2.25                                         # vertical stems
BAR = 2.0                                           # horizontal bars and the thin parts of bowls
BOWL = 2.4                                          # the sides of bowls


# ---------- shapes: each is (bounding box, test(x, y) -> inside) ----------

def rect(x0, y0, x1, y1):
    return (x0, y0, x1, y1), lambda x, y: x0 <= x < x1 and y0 <= y < y1


def poly(pts):
    """A simple polygon (even-odd)"""
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]

    def inside(x, y):
        c, j = False, len(pts) - 1
        for i in range(len(pts)):
            (xi, yi), (xj, yj) = pts[i], pts[j]
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                c = not c
            j = i
        return c
    return (min(xs), min(ys), max(xs), max(ys)), inside


def _superellipse(x0, y0, x1, y1, n):
    cx, cy, a, b = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    return lambda x, y: a > 0 and b > 0 and abs((x - cx) / a) ** n + abs((y - cy) / b) ** n <= 1


def ring(x0, y0, x1, y1, side=BOWL, bar=BAR, n=2.4, keep=None):
    """A bowl: the superellipse in (x0, y0, x1, y1) less the one inset by side (left, right) and bar (top, bottom);
    keep(x, y) - the part that stays (None: all)"""
    out = _superellipse(x0, y0, x1, y1, n)
    hole = _superellipse(x0 + side, y0 + bar, x1 - side, y1 - bar, n)
    return (x0, y0, x1, y1), lambda x, y: out(x, y) and not hole(x, y) and (keep is None or keep(x, y))


def blob(x0, y0, x1, y1, n=3.0):
    """A filled superellipse (the dot of an i)"""
    return (x0, y0, x1, y1), _superellipse(x0, y0, x1, y1, n)


# ---------- the glyphs: (ink width, left side, right side, shapes(x)) ----------
# Sides are the space a glyph keeps at its left and right: "s" a straight stem, "r" a round side, "d" a diagonal. Two
# glyphs' ink is GAP[left glyph's right side + right glyph's left side] apart (measured: H-i 3.4, o-l 1.7, C-o 0.7).
GAP = {"ss": 3.25, "sr": 2.75, "sd": 2.25, "rs": 1.75, "rr": 1.0, "rd": 1.5, "ds": 2.25, "dr": 1.5, "dd": 1.25}


def _x_round(x, w, top):
    """An o-like bowl of width w from x, its top at top (with the overshoot)"""
    return ring(x, top - OVER, x + w, BASE + OVER)


def g_C(x):
    w = 11.3
    cx = x + w / 2
    return [ring(x, TOP - OVER, x + w, BASE + OVER, n=2.3, keep=lambda px, py: not (px > cx + 0.8 and 4.4 < py < 18.6))]


def g_o(x):
    return [_x_round(x, 11.3, XH)]


def g_e(x):
    w = 10.8
    cx = x + w / 2
    mid = (XH + BASE) / 2 + 0.2
    return [ring(x, XH - OVER, x + w, BASE + OVER, keep=lambda px, py: not (px > cx + 0.6 and mid + BAR < py < BASE - 1.6)),
            rect(x + 1.0, mid, x + w - 0.8, mid + BAR)]


def g_l(x):
    return [rect(x, TOP, x + STEM, BASE)]


def g_i(x):
    return [rect(x, XH, x + STEM, BASE), blob(x - 0.25, TOP - 0.2, x + STEM + 0.3, TOP + 2.55)]


def _arch(x, w, top_y):
    """The top of an n: the upper half of a bowl from the left stem's outer side to the right stem"""
    yc = top_y - OVER + 6.0
    return ring(x, top_y - OVER, x + w, top_y - OVER + 12.0, side=STEM, keep=lambda px, py: py < yc and px > x + 0.8)


def g_n(x):
    w = 9.5
    return [rect(x, XH, x + STEM, BASE), _arch(x, w, XH), rect(x + w - STEM, XH + 5.0, x + w, BASE)]


def g_h(x):
    w = 9.5
    return [rect(x, TOP, x + STEM, BASE), _arch(x, w, XH), rect(x + w - STEM, XH + 5.0, x + w, BASE)]


def g_m(x):
    w = 16.2
    k = (w - STEM) / 2                              # the middle stem's left side
    return [rect(x, XH, x + STEM, BASE), _arch(x, k + STEM, XH), rect(x + k, XH + 5.0, x + k + STEM, BASE),
            _arch(x + k, w - k, XH), rect(x + w - STEM, XH + 5.0, x + w, BASE)]


def g_u(x):
    w = 9.5
    yc = BASE + OVER - 6.0
    return [rect(x, XH, x + STEM, BASE - 5.0), rect(x + w - STEM, XH, x + w, BASE),
            ring(x, BASE + OVER - 12.0, x + w, BASE + OVER, side=STEM, keep=lambda px, py: py > yc and px < x + w - 0.8)]


def g_r(x):
    w = 6.6
    return [rect(x, XH, x + STEM, BASE),
            ring(x, XH - OVER, x + 2 * (w - 0.4), XH - OVER + 12.0, side=STEM,
                 keep=lambda px, py: py < XH - OVER + 6.0 and x + 0.8 < px < x + w)]


def g_d(x):
    w = 10.0
    return [ring(x, XH - OVER, x + w, BASE + OVER), rect(x + w - STEM, TOP, x + w, BASE)]


def g_L(x):
    return [rect(x, TOP, x + STEM + 0.15, BASE), rect(x, BASE - BAR, x + 9.0, BASE)]


def g_F(x):
    return [rect(x, TOP, x + STEM + 0.2, BASE), rect(x, TOP, x + 9.0, TOP + BAR + 0.25),
            rect(x, 10.0, x + 7.5, 10.0 + BAR + 0.5)]


def g_H(x):
    w = 12.25
    return [rect(x, TOP, x + STEM, BASE), rect(x + w - STEM - 0.25, TOP, x + w, BASE), rect(x, 10.45, x + w, 10.45 + 1.8)]


def g_N(x):
    w = 12.25
    return [rect(x, TOP, x + STEM, BASE), rect(x + w - STEM, TOP, x + w, BASE),
            poly([(x, TOP), (x + 2.6, TOP), (x + w, BASE), (x + w - 2.6, BASE)])]


def g_M(x):
    w = 16.0
    m = w / 2
    return [rect(x, TOP, x + STEM, BASE), rect(x + w - STEM, TOP, x + w, BASE),
            poly([(x, TOP), (x + 2.6, TOP), (x + m + 1.3, BASE - 2.6), (x + m + 1.3, BASE), (x + m - 1.3, BASE)]),
            poly([(x + w, TOP), (x + w - 2.6, TOP), (x + m - 1.3, BASE - 2.6), (x + m - 1.3, BASE), (x + m + 1.3, BASE)])]


def g_P(x):
    w = 10.2
    mid = 11.6
    return [rect(x, TOP, x + STEM + 0.1, BASE), rect(x, TOP, x + 5.0, TOP + BAR), rect(x, mid - BAR, x + 5.0, mid),
            ring(x - 1.0, TOP, x + w, mid, keep=lambda px, py: px > x + 4.5)]


def g_V(x):
    w = 13.6
    m = w / 2
    return [poly([(x, TOP), (x + 2.6, TOP), (x + m + 1.5, BASE), (x + m - 1.5, BASE)]),
            poly([(x + w, TOP), (x + w - 2.6, TOP), (x + m - 1.5, BASE), (x + m + 1.5, BASE)])]


def g_v(x):
    w = 10.0
    m = w / 2
    return [poly([(x, XH), (x + 2.4, XH), (x + m + 1.2, BASE), (x + m - 1.2, BASE)]),
            poly([(x + w, XH), (x + w - 2.4, XH), (x + m - 1.2, BASE), (x + m + 1.2, BASE)])]


def g_x(x):
    w = 10.0
    return [poly([(x + 0.2, XH), (x + 2.7, XH), (x + w, BASE), (x + w - 2.5, BASE)]),
            poly([(x + w - 0.2, XH), (x + w - 2.7, XH), (x, BASE), (x + 2.5, BASE)])]


def g_t(x):
    w = 7.0
    return [rect(x + 1.2, 4.2, x + 1.2 + STEM, BASE - 3.0), rect(x, XH, x + w - 0.6, XH + BAR),
            ring(x + 1.2, BASE + OVER - 9.0, x + w + 3.0, BASE + OVER, side=STEM, bar=BAR,
                 keep=lambda px, py: py > BASE + OVER - 4.5 and px < x + w)]


def g_T(x):
    w = 11.0
    k = round((w - STEM) / 2)                       # the stem at a whole pixel, as all stems
    return [rect(x, TOP, x + w, TOP + BAR + 0.25), rect(x + k, TOP, x + k + STEM, BASE)]


def g_k(x):
    w = 9.6
    return [rect(x, TOP, x + STEM, BASE), poly([(x + 1.5, 15.2), (x + w - 2.9, XH), (x + w, XH), (x + 1.5, 17.6)]),
            poly([(x + 4.0, 12.6), (x + 6.3, 11.2), (x + w + 0.2, BASE), (x + w - 2.7, BASE)])]


def g_S(x):
    w = 9.8
    cx = x + w / 2
    yc = (TOP + BASE) / 2
    top = ring(x, TOP - OVER, x + w, yc + 1.0, keep=lambda px, py: not (px > cx + 0.8 and TOP + 2.2 < py < yc - 3.0)
               and not (px > cx and py > yc - 3.5) and not (px < cx - 0.5 and py > yc))
    bot = ring(x, yc - 1.0, x + w, BASE + OVER, keep=lambda px, py: not (px < cx - 0.8 and yc + 3.0 < py < BASE - 2.2)
               and not (px < cx and py < yc + 3.5) and not (px > cx + 0.5 and py < yc))
    spine = poly([(x + 0.8, yc - 4.6), (x + 3.4, yc - 4.6), (x + w - 0.8, yc + 4.6), (x + w - 3.4, yc + 4.6)])
    return [top, bot, spine]


# character: (ink width, left side, right side, shapes)
GLYPHS = {
    "C": (11.3, "r", "r", g_C), "F": (9.0, "s", "d", g_F), "H": (12.25, "s", "s", g_H), "L": (9.0, "s", "d", g_L),
    "M": (16.0, "s", "s", g_M), "N": (12.25, "s", "s", g_N), "P": (10.2, "s", "r", g_P), "S": (9.8, "r", "r", g_S),
    "T": (11.0, "d", "d", g_T), "V": (13.6, "d", "d", g_V),
    "d": (10.0, "r", "s", g_d), "e": (10.8, "r", "r", g_e), "h": (9.5, "s", "s", g_h), "i": (STEM, "s", "s", g_i),
    "k": (9.6, "s", "d", g_k), "l": (STEM, "s", "s", g_l), "m": (16.2, "s", "s", g_m), "n": (9.5, "s", "s", g_n),
    "o": (11.3, "r", "r", g_o), "r": (6.6, "s", "d", g_r), "t": (7.0, "d", "r", g_t), "u": (9.5, "s", "s", g_u),
    "v": (10.0, "d", "d", g_v), "x": (10.0, "d", "d", g_x),
}


def layout(text):
    """[(character, x of its ink)] from 0, and the ink width"""
    out, x, prev = [], 0.0, None
    for c in text:
        if c not in GLYPHS:
            raise ValueError(f"{c!r} is not in the dial label face: use another word")
        w, left, right, _ = GLYPHS[c]
        if prev is not None:
            x = round(x + GAP[prev + left])     # glyphs start at whole pixels: their stems' left sides are sharp
        out.append((c, x))
        x += w
        prev = right
    return out, x


def coverage(text, w, x0):
    """Coverage (rows of floats, H x w) of the word with its ink from x0"""
    shapes = []
    for c, x in layout(text)[0]:
        shapes += GLYPHS[c][3](x0 + x)
    inv = 1.0 / SS
    hit = [[0] * w for _ in range(H)]       # a sample is inside the word if any shape takes it
    seen = [[None] * w for _ in range(H)]
    for (bx0, by0, bx1, by1), inside in shapes:
        for py in range(max(0, int(by0)), min(H, int(by1) + 1)):
            for px in range(max(0, int(bx0)), min(w, int(bx1) + 1)):
                s = seen[py][px]
                if s is None:
                    s = seen[py][px] = bytearray(SS * SS)
                for j in range(SS):
                    y = py + (j + 0.5) * inv
                    for i in range(SS):
                        if not s[j * SS + i] and inside(px + (i + 0.5) * inv, y):
                            s[j * SS + i] = 1
    for py in range(H):
        for px in range(w):
            if seen[py][px] is not None:
                hit[py][px] = sum(seen[py][px])
    return [[hit[y][x] / (SS * SS) for x in range(w)] for y in range(H)]


def _grow(sel, r2):
    h, w = len(sel), len(sel[0])
    r = int(r2 ** 0.5)
    offs = [(dx, dy) for dy in range(-r, r + 1) for dx in range(-r, r + 1) if dx * dx + dy * dy <= r2]
    out = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if sel[y][x]:
                for dx, dy in offs:
                    if 0 <= y + dy < h and 0 <= x + dx < w:
                        out[y + dy][x + dx] = True
    return out


# The rings around the sign as on the stock dial labels: (meanings grown from, squared radius), unions, taken in the
# order outline, edge, halo, never over the sign or an earlier ring (outline 90 of 837 pixels off the stock labels,
# the whole surround 59 of 917)
RINGS = ((2, (((1,), 2), ((1, 3), 1))), (5, (((1,), 4), ((1, 3), 2), ((1, 3, 4), 1))),
         (6, (((1,), 5), ((1, 3), 4), ((1, 3, 4), 2))))


def values(cov):
    """Meaning values (rows of ints) from coverage: the sign, then the outline, its edge and the halo"""
    h, w = len(cov), len(cov[0])
    v = [[1 if c >= SIGN_T[0] else 3 if c >= SIGN_T[1] else 4 if c > SIGN_T[2] else 0 for c in row] for row in cov]
    taken = [[v[y][x] != 0 for x in range(w)] for y in range(h)]
    sign = [row[:] for row in v]
    for value, rules in RINGS:
        m = [[False] * w for _ in range(h)]
        for vals, r2 in rules:
            g = _grow([[sign[y][x] in vals for x in range(w)] for y in range(h)], r2)
            m = [[m[y][x] or g[y][x] for x in range(w)] for y in range(h)]
        for y in range(h):
            for x in range(w):
                if m[y][x] and not taken[y][x]:
                    v[y][x] = value
                    taken[y][x] = True
    return v


def label(text):
    """A dial label: (width, rows of meaning values), 24 rows, cut to the halo plus MARGIN columns each side, so the
    camera, which centres it in the widget, centres the ink. ValueError if it does not fit the stock widget"""
    ink = layout(text)[1]
    w = int(ink) + 16
    v = values(coverage(text, w, (w - round(ink)) // 2))
    used = [x for x in range(w) if any(v[y][x] for y in range(H))]
    x0, x1 = used[0] - MARGIN, used[-1] + 1 + MARGIN
    if any(k not in (0, 5, 6) for k in v[0] + v[-1]):
        raise ValueError(f"{text!r}: more than the edge and the halo in the top or the bottom row")
    if x1 - x0 > WIDGET_W:
        raise ValueError(f"{text!r}: {x1 - x0} px wide, the widget shows {WIDGET_W}: take a shorter word or an abbreviation")
    return x1 - x0, [row[x0:x1] for row in v]


def main(argv):
    for text in argv or ["Level"]:
        w, rows = label(text)
        print(f"{text}: {w} x {H}")
        for row in rows:
            print("  " + "".join(".#o+:-~"[k] for k in row))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
