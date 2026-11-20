"""Pictures of the FILMLOOK module (docs: OMKit design note "film look").

    python3 OMKit/tools/mkicons_film.py OMKit/resources/modules/filmlook/icons                 # write the resources
    python3 OMKit/tools/mkicons_film.py OMKit/resources/modules/filmlook/icons --sheet x.png   # and a preview sheet
    python3 OMKit/tools/mkicons_film.py OMKit/resources/modules/filmlook/icons --check         # compare, write nothing

The pictures are committed in OMKit/resources/modules/filmlook/icons, and the module build takes them from there
(src/modules/Makefile, mkmod.py --res-disk): run this tool again only to change them.

Design "negative F": an upright piece of film - a plate with a row of sprocket holes down each side and a letter F cut
out of the frame, so the F shows dark, as on a negative (in the panel cell, in the background colour). The editor item
adds two small arrows at the sides: the dials switch the preset. Twelve pictures, laid out like the stock picture mode
icons of the same places (sign height and line width measured on them):

  id 0  picture mode strip   66 x 50   outline 2 px (round, r^2 = 5), no edge or halo      profile icon
  id 1  large (selected)     80 x 60   as id 0                                              profile icon
  id 2  menu line            <= 124 x 39, cut to the content + 2 columns; outline 1 px + halo profile label
  id 3  control panel cell   128 x 39  no outline, the antialiased sign only                profile scp
  id 4  shooting screen      <= 80 x 36, cut as id 2; outline 1 px, edge on diagonals, halo  profile shoot
  id 5  editor item          66 x 50   as id 0, with the arrows                             profile icon
  id 6  rear dial's label    <= 56 x 24  "Prev", in place of the Color Creator's "Color"      dial_label.py
  id 7  front dial's label   <= 56 x 24  "Next", in place of "Vivid"                          dial_label.py
  id 8  Color/Level item     66 x 50   "Color" over "Level", as the profiles' Color Level item   profile text
  id 9  grain item           66 x 50   "Film" over "Grain"                                       profile text
  id 10 grain: tile dial     <= 56 x 24  "Tile", over the dial that steps the tile            dial_label.py
  id 11 grain: strength dial <= 56 x 24  "Level", over the dial of Off, Low, Med, High        dial_label.py

The dial labels (ids 6, 7, 10, 11) are drawn by dial_label.py in the face, size and rows of the camera's own labels
("Color", "Vivid"), as every module's dial label is; "Tile" and "Level" stand for "Type" and "Strength" (FILMLOOK 1.9),
which do not fit: "Type" has descenders, "Strength" is wider than the stock 56 px widget.

Method: shapes are boolean masks of samples, 4 x 4 per pixel (sample centres at (i + 0.5) / 4), built from rectangles
(optionally with round corners), the letter's strokes and capsules; a pixel's coverage gives the sign (>= 0.75 -> 1) and
its transitions (>= 0.4 -> 3, > 0.1 -> 4); the outline (2), edge (5) and halo (6) grow from the sign by pixel rules
measured on the stock pictures. Meaning values (OMK_ICON_*): 0 clear, 1 sign, 2 outline, 3 / 4 transitions
sign -> outline, 5 edge of the outline, 6 halo. A mask row is an int bit set (bit i = sample column i), so the tool needs
nothing beyond the standard library.

Resource: type 1, id = OMK_LOOK_PIC_* (0 strip, 1 large, 2 menu label, 3 panel cell, 4 shooting screen, 5 editor item),
file 1_<id>.bin (pack()): u16 width, u16 height, then 4-bit pixels, the high nibble first.
Deterministic: the same input gives the same bytes.
"""
import math
import struct
import sys
import zlib
from bisect import bisect_left, bisect_right
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dial_label                                   # noqa: E402  the dial labels' face, shared with mkicons_creatprf.py

SS = 4                                              # samples per pixel along each axis

# id: (width, or (None, max width) - cut to the content, height, profile, sign box (x0, y0, x1, y1), line width)
PICTURES = {
    0: (66, 50, "icon", (8, 10, 58, 40), 3.0),
    1: (80, 60, "icon", (15, 15, 65, 45), 3.0),
    2: ((None, 124), 39, "label", (2, 5, 52, 33), 3.0),
    3: (128, 39, "scp", ((128 - 50) / 2, 8, (128 - 50) / 2 + 50, 34), 3.0),
    4: ((None, 80), 36, "shoot", (1, 7, 39, 29), 2.5),
    5: (66, 50, "icon", (18, 12, 48, 38), 3.0),
    6: ((None, 56), 24, "dial", "Prev", None),
    7: ((None, 56), 24, "dial", "Next", None),
    8: (66, 50, "text", ("Color", "Level"), 2.2),
    9: (66, 50, "text", ("Film", "Grain"), 2.2),
    10: ((None, 56), 24, "dial", "Tile", None),
    11: ((None, 56), 24, "dial", "Level", None),
}
# The words of the item pictures (ids 8, 9): letters as strokes in a box 0..w x 0..1 (y down, 1 = the cap height; lower
# case from 0.3)
GLYPHS = {
    "r": (0.42, [(0, 0.3, 0, 1), (0, 0.48, 0.16, 0.33), (0.16, 0.33, 0.42, 0.3)]),
    "e": (0.58, [(0.02, 0.64, 0.56, 0.64), (0.56, 0.64, 0.56, 0.5), (0.56, 0.5, 0.42, 0.3), (0.42, 0.3, 0.16, 0.3),
                 (0.16, 0.3, 0.02, 0.47), (0.02, 0.47, 0.02, 0.83), (0.02, 0.83, 0.17, 1), (0.17, 1, 0.52, 1)]),
    "v": (0.56, [(0, 0.3, 0.28, 1), (0.28, 1, 0.56, 0.3)]),
    "C": (0.68, [(0.68, 0.16, 0.5, 0), (0.5, 0, 0.2, 0), (0.2, 0, 0.02, 0.2), (0.02, 0.2, 0.02, 0.8), (0.02, 0.8, 0.2, 1),
                 (0.2, 1, 0.5, 1), (0.5, 1, 0.68, 0.84)]),
    "o": (0.56, [(0.16, 0.3, 0.4, 0.3), (0.4, 0.3, 0.56, 0.46), (0.56, 0.46, 0.56, 0.84), (0.56, 0.84, 0.4, 1),
                 (0.4, 1, 0.16, 1), (0.16, 1, 0, 0.84), (0, 0.84, 0, 0.46), (0, 0.46, 0.16, 0.3)]),
    "l": (0.1, [(0.05, 0, 0.05, 1)]),
    "L": (0.52, [(0, 0, 0, 1), (0, 1, 0.52, 1)]),
    "n": (0.54, [(0, 0.3, 0, 1), (0, 0.46, 0.16, 0.3), (0.16, 0.3, 0.38, 0.3), (0.38, 0.3, 0.54, 0.46), (0.54, 0.46, 0.54, 1)]),
    "F": (0.55, [(0, 0, 0, 1), (0, 0, 0.55, 0), (0, 0.47, 0.46, 0.47)]),
    "i": (0.1, [(0.05, 0.3, 0.05, 1), (0.05, 0.06, 0.05, 0.12)]),
    "m": (0.8, [(0, 0.3, 0, 1), (0, 0.44, 0.13, 0.3), (0.13, 0.3, 0.27, 0.3), (0.27, 0.3, 0.4, 0.44), (0.4, 0.44, 0.4, 1),
                (0.4, 0.44, 0.53, 0.3), (0.53, 0.3, 0.67, 0.3), (0.67, 0.3, 0.8, 0.44), (0.8, 0.44, 0.8, 1)]),
    "G": (0.68, [(0.68, 0.16, 0.5, 0), (0.5, 0, 0.2, 0), (0.2, 0, 0.02, 0.2), (0.02, 0.2, 0.02, 0.8), (0.02, 0.8, 0.2, 1),
                 (0.2, 1, 0.5, 1), (0.5, 1, 0.68, 0.84), (0.68, 0.84, 0.68, 0.56), (0.68, 0.56, 0.4, 0.56)]),
    "a": (0.54, [(0.06, 0.36, 0.2, 0.3), (0.2, 0.3, 0.4, 0.3), (0.4, 0.3, 0.54, 0.44), (0.54, 0.44, 0.54, 1),
                 (0.54, 0.62, 0.18, 0.62), (0.18, 0.62, 0, 0.75), (0, 0.75, 0, 0.88), (0, 0.88, 0.15, 1), (0.15, 1, 0.4, 1),
                 (0.4, 1, 0.54, 0.88)]),
}
CAP = 16.0                  # px
RIGHT_MARGIN = 2            # clear columns right of the outline of a cut picture (stock: 2..3)

# Coverage thresholds of the sign (as mkicons_creatprf.py): >= 0.75 sign, >= 0.4 transition 3, > 0.1 transition 4
SIGN_T = (0.75, 0.40, 0.10)

# Outline profiles, pixel rules measured on the stock pictures:
#   out  = (meaning values the outline grows from, r^2): the outline is the pixels whose centre is within r of such a pixel;
#   edge = the same for the edge 5; halo = (from "sign" or "outline" = sign + outline, r^2) for the halo 6
PROFILES = {
    "icon": {"out": ((1, 3, 4), 5), "edge": None, "halo": None},
    "label": {"out": ((1, 3, 4), 1), "edge": None, "halo": ("sign", 4)},
    "shoot": {"out": ((1, 3), 1), "edge": ((1, 3), 2), "halo": ("outline", 1)},
    "scp": {"out": None, "edge": None, "halo": None},
    "text": {"out": ((1, 3, 4), 2), "edge": None, "halo": None},     # the icon palette, a 1 px outline: letters stay apart
}


class Grid:
    """Samples of a w x h picture: xs, ys - sample centres in pixels (pixel x covers [x, x + 1)); a mask is a list of
    h * SS ints, bit i of row j set = sample (i, j) inside"""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.xs = [(i + 0.5) / SS for i in range(w * SS)]
        self.ys = [(j + 0.5) / SS for j in range(h * SS)]

    def empty(self):
        return [0] * len(self.ys)

    def cols(self, x0, x1):
        """Sample columns i with x0 <= xs[i] <= x1: (first, last + 1)"""
        return bisect_left(self.xs, x0), bisect_right(self.xs, x1)

    def rows(self, y0, y1):
        return bisect_left(self.ys, y0), bisect_right(self.ys, y1)


def m_or(a, b):
    return [p | q for p, q in zip(a, b)]


def m_andnot(a, b):
    return [p & ~q for p, q in zip(a, b)]


def rect(g, x0, y0, x1, y1, r=0.0):
    """Rectangle [x0, x1] x [y0, y1], corners rounded with radius r"""
    m = g.empty()
    if r <= 0:
        i0, i1 = g.cols(x0, x1)
        if i1 > i0:
            bits = ((1 << (i1 - i0)) - 1) << i0
            j0, j1 = g.rows(y0, y1)
            for j in range(j0, j1):
                m[j] = bits
        return m
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hx, hy = (x1 - x0) / 2 - r, (y1 - y0) / 2 - r
    i0, i1 = g.cols(cx - (hx + r) - 1, cx + (hx + r) + 1)        # a superset; the exact test is below
    rr = r * r
    for j in range(*g.rows(cy - (hy + r) - 1, cy + (hy + r) + 1)):
        ay = abs(g.ys[j] - cy)
        if not ay <= hy + r:
            continue
        dy = max(ay - hy, 0)
        bits = 0
        for i in range(i0, i1):
            ax = abs(g.xs[i] - cx)
            dx = max(ax - hx, 0)
            if dx * dx + dy * dy <= rr and ax <= hx + r:
                bits |= 1 << i
        m[j] = bits
    return m


def capsule(g, ax, ay, bx, by, width):
    """Segment a-b of this width with round ends"""
    m = g.empty()
    vx, vy = bx - ax, by - ay
    vv = vx * vx + vy * vy or 1e-9
    rr = (width / 2) ** 2
    pad = width / 2 + 1
    i0, i1 = g.cols(min(ax, bx) - pad, max(ax, bx) + pad)
    for j in range(*g.rows(min(ay, by) - pad, max(ay, by) + pad)):
        y = g.ys[j]
        bits = 0
        for i in range(i0, i1):
            x = g.xs[i]
            t = min(max(((x - ax) * vx + (y - ay) * vy) / vv, 0), 1)
            ex, ey = x - ax - t * vx, y - ay - t * vy
            if ex * ex + ey * ey <= rr:
                bits |= 1 << i
        m[j] = bits
    return m


def chevron(g, cx, cy, h, w, s, right):
    """Arrow head "<" / ">" h high and w wide, line s"""
    tip = cx + w / 2 if right else cx - w / 2
    back = cx - w / 2 if right else cx + w / 2
    return m_or(capsule(g, back, cy - h / 2, tip, cy, s), capsule(g, tip, cy, back, cy + h / 2, s))


def letter_f(g, x0, y0, w, h, s):
    """Letter F, grotesque: even line s, square ends (as the stock icons' digits), middle bar at 0.47 h"""
    my = y0 + h * 0.47
    m = m_or(rect(g, x0, y0, x0 + s, y0 + h), rect(g, x0, y0, x0 + w, y0 + s))
    return m_or(m, rect(g, x0, my - s / 2, x0 + w * 0.84, my + s / 2))


def negative(g, b, s):
    """The sign: an upright piece of film in box b - a plate with sprocket holes down both sides and the letter F cut
    out of the frame; its width follows from the height, centred in the box"""
    x0, y0, x1, y1 = b
    H = y1 - y0
    u = H / 31
    edge, hw, inner = 2.0, max(2.4, 3 * u), max(1.8, 2.2 * u)
    hh = max(2.4, 2.8 * u)
    fw = max(9.0, 10.5 * u)                       # the letter: width; height about that of the stock icons' digits
    W = 2 * (edge + hw + inner) + fw + 2 * max(1.0, 1.5 * u)
    px0 = (x0 + x1) / 2 - W / 2
    px1 = px0 + W
    m = rect(g, px0, y0, px1, y1, r=1.5)
    pitch = hh + max(2.4, 3.2 * u)
    n = int((H - 2 * 1.5 + (pitch - hh)) // pitch)
    hy = y0 + (H - (n * pitch - (pitch - hh))) / 2
    for _ in range(n):
        for hx in (px0 + edge, px1 - edge - hw):
            m = m_andnot(m, rect(g, hx, hy, hx + hw, hy + hh, r=0.4))
        hy += pitch
    fh = H - 2 * max(2.5, 3.5 * u)
    return m_andnot(m, letter_f(g, (px0 + px1) / 2 - fw / 2 + 0.5, y0 + (H - fh) / 2, fw, fh, s))


def word(g, text, s, cap=CAP, y0=None, gap=0.24):
    """A word of an item picture: the letters' strokes, centred across (and down unless y0), the cap height cap"""
    gap = gap * cap
    width = sum(GLYPHS[c][0] * cap for c in text) + gap * (len(text) - 1)
    x = (g.w - width) / 2
    y0 = (g.h - cap) / 2 if y0 is None else y0
    m = g.empty()
    for c in text:
        lw, strokes = GLYPHS[c]
        for ax, ay, bx, by in strokes:
            m = m_or(m, capsule(g, x + ax * cap, y0 + ay * cap, x + bx * cap, y0 + by * cap, s))
        x += lw * cap + gap
    return m


def coverage_values(g, m):
    """Pixel meaning values of the sign from its sample mask: 1, 3, 4 by coverage, else 0; rows of lists"""
    w, h = g.w, g.h
    v = [[0] * w for _ in range(h)]
    for y in range(h):
        rows = m[y * SS:(y + 1) * SS]
        for x in range(w):
            sh = x * SS
            n = sum(bin((r >> sh) & ((1 << SS) - 1)).count("1") for r in rows)
            cg = n / SS ** 2
            v[y][x] = 1 if cg >= SIGN_T[0] else 3 if cg >= SIGN_T[1] else 4 if cg > SIGN_T[2] else 0
    return v


def grow(sel, r2):
    """Pixels whose centre is within sqrt(r2) of the centre of a selected pixel (sel: rows of bools)"""
    h, w = len(sel), len(sel[0])
    R = math.isqrt(r2)
    offs = [(dx, dy) for dy in range(-R, R + 1) for dx in range(-R, R + 1) if dx * dx + dy * dy <= r2]
    out = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if sel[y][x]:
                for dx, dy in offs:
                    if 0 <= y + dy < h and 0 <= x + dx < w:
                        out[y + dy][x + dx] = True
    return out


def outline(v, p):
    """Add the outline, edge and halo of profile p around the sign's pixels (1, 3, 4)"""
    h, w = len(v), len(v[0])

    def sel(vals):
        return [[v[y][x] in vals for x in range(w)] for y in range(h)]
    s = sel((1, 3, 4))
    if p["out"]:
        g = grow(sel(p["out"][0]), p["out"][1])
        outl = [[g[y][x] and not s[y][x] for x in range(w)] for y in range(h)]
    else:
        outl = [[False] * w for _ in range(h)]
    if p["halo"]:
        src, r2 = p["halo"]
        base = [[s[y][x] or outl[y][x] for x in range(w)] for y in range(h)] if src == "outline" else s
        g = grow(base, r2)
        for y in range(h):
            for x in range(w):
                if g[y][x] and not s[y][x]:
                    v[y][x] = 6
    if p["edge"]:
        g = grow(sel(p["edge"][0]), p["edge"][1])
        for y in range(h):
            for x in range(w):
                if g[y][x] and not s[y][x] and not outl[y][x]:
                    v[y][x] = 5
    for y in range(h):
        for x in range(w):
            if outl[y][x]:
                v[y][x] = 2
    return v


def render(rid):
    """-> (w, h, pixels: bytes of meaning values, row by row)"""
    width, h, prof, box, s = PICTURES[rid]
    if prof == "dial":                            # a dial label: the camera's face, cut and centred by dial_label.py
        w, rows = dial_label.label(box)
        return w, h, bytes(k for row in rows for k in row)
    wmax = width[1] if isinstance(width, tuple) else width
    g = Grid(wmax, h)
    if isinstance(box, str):
        m = word(g, box, s)
    elif isinstance(box[0], str):                 # two lines, as the profiles' "Color Level": cap 13 px, 21 px apart
        m = m_or(word(g, box[0], s, 13.0, 6.0), word(g, box[1], s, 13.0, 27.0))
    else:
        m = negative(g, box, s)
    if rid == 5:                                  # the editor item: arrows at both sides
        m = m_or(m, m_or(chevron(g, 8.5, 25, 14, 5.5, s, right=False), chevron(g, 57.5, 25, 14, 5.5, s, right=True)))
    v = outline(coverage_values(g, m), PROFILES[prof])
    w = wmax
    if isinstance(width, tuple):                  # "may be narrower": cut to the content, a margin as the stock ones
        used = [x for x in range(wmax) if any(v[y][x] for y in range(h))]
        w = min(wmax, used[-1] + 1 + RIGHT_MARGIN)
    return w, h, bytes(v[y][x] for y in range(h) for x in range(w))


def pack(w, h, px):
    """Module resource: u16 width, u16 height (little endian), 4-bit pixels, the high nibble first, whole bytes per row"""
    row = (w + 1) // 2
    out = bytearray(struct.pack("<HH", w, h))
    for y in range(h):
        b = bytearray(row)
        for x in range(w):
            v = px[y * w + x] & 15
            b[x // 2] |= v << 4 if x % 2 == 0 else v
        out += b
    return bytes(out)


def png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def sheet(icons, path, scale=3, cells=(84, 64)):
    """Preview: white sign, black outline, greys for the edges, on the dark strip background"""
    colours = {0: (24, 24, 24), 1: (255, 255, 255), 2: (0, 0, 0), 3: (190, 190, 190), 4: (90, 90, 90), 5: (10, 10, 10),
               6: (18, 18, 18)}
    cell_w, cell_h = cells
    cols = 3
    rows = (len(icons) + cols - 1) // cols
    W, H = cols * cell_w * scale, rows * cell_h * scale
    rgb = bytearray(bytes(colours[0]) * (W * H))
    for n, (_, _, w, h, px) in enumerate(icons):
        ox, oy = (n % cols) * cell_w * scale, (n // cols) * cell_h * scale
        for y in range(h * scale):
            for x in range(w * scale):
                c = colours[px[(y // scale) * w + x // scale]]
                i = ((oy + y) * W + ox + x) * 3
                rgb[i:i + 3] = bytes(c)
    png(path, W, H, bytes(rgb))


def build():
    """[(resource id, file bytes, w, h, pixels)]"""
    out = []
    for rid in PICTURES:
        w, h, px = render(rid)
        out.append((rid, pack(w, h, px), w, h, px))
    return out


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    out = Path(argv[0])
    icons = build()
    if "--check" in argv:                           # the committed files are exactly what this tool draws
        bad = [f"1_{rid}.bin" for rid, data, _, _, _ in icons
               if not (out / f"1_{rid}.bin").is_file() or (out / f"1_{rid}.bin").read_bytes() != data]
        ids = {f"1_{rid}.bin" for rid, _, _, _, _ in icons}
        extra = sorted(f.name for f in out.glob("*.bin") if f.name not in ids) if out.is_dir() else []
        for name in bad:
            print(f"differs or missing: {name}")
        for name in extra:
            print(f"not drawn by this tool: {name}")
        print(f"{len(icons)} pictures, {len(bad)} differ, {len(extra)} extra files in {out}")
        return 1 if bad or extra else 0
    out.mkdir(parents=True, exist_ok=True)
    for rid, data, _, _, _ in icons:
        (out / f"1_{rid}.bin").write_bytes(data)
    if "--sheet" in argv:
        sheet(icons, argv[argv.index("--sheet") + 1], cells=(132, 64))
    print(f"{len(icons)} pictures -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
