"""A look given as a 3D LUT in a .cube file: a function from the scene (linear BT.709, middle grey 0.18) to the display
(sRGB-encoded RGB) - what the fitter copies into the camera's tables.

A .cube file holds only the table, not how its input and output are encoded. The input encoding (CUBE_IN) says how the
scene is brought to the LUT's input, the output encoding (CUBE_OUT) how the LUT's output is read as display light. The
supported ones (all with BT.709 / sRGB primaries):

    input   cineon   Cineon log (printing density: white 685, black 95, 300 codes per decade, of code 1023)
            srgb     sRGB (the scene clipped to 0..1, then the sRGB curve) - a look made for developed pictures
            linear   linear light, 0..1
            acescct  the ACEScct curve, but on BT.709 primaries (not on the ACES AP1 primaries)
    output  bt1886   Rec.709 / BT.1886 display, gamma 2.4
            srgb     sRGB display
            gamma22  a display of gamma 2.2

guess() picks the encodings from the file's comments and title; the user can override them. The file's DOMAIN_MIN /
DOMAIN_MAX (or LUT_3D_INPUT_RANGE) are honoured; the lookup is trilinear. A 1D LUT, or a file with a 1D shaper before
its 3D table, is refused."""
from __future__ import annotations

from pathlib import Path

import numpy as np

from . import color

MAX_SIZE = 129                 # the largest LUT_3D_SIZE accepted (129^3 rows: about 50 MB of text)


def _acescct(x):
    x = np.maximum(x, 1e-10)
    return np.where(x <= 0.0078125, 10.5402377416545 * x + 0.0729055341958355, (np.log2(x) + 9.72) / 17.52)


# How a .cube's input expects the scene and how its output is encoded: key -> (label, function)
CUBE_IN = {
    "cineon": ("Cineon log", color.cineon_encode),
    "srgb": ("sRGB", color.srgb_encode),
    "linear": ("linear", lambda x: x),
    "acescct": ("ACEScct curve, BT.709 primaries", _acescct),
}
CUBE_OUT = {
    "bt1886": ("Rec.709 gamma 2.4", lambda v: color.srgb_encode(np.clip(v, 0, 1) ** 2.4)),
    "srgb": ("sRGB", lambda v: np.clip(v, 0, 1)),
    "gamma22": ("gamma 2.2", lambda v: color.srgb_encode(np.clip(v, 0, 1) ** 2.2)),
}


def guess(text: str):
    """(input, output) encodings from the file's text: "cineon" or "acescct" named in it -> that input, else sRGB; a
    Cineon input with "2.4", "rec709" or "rec.709" in the text -> a Rec.709 gamma 2.4 output, else sRGB."""
    low = text.lower()
    ein = "cineon" if "cineon" in low else "acescct" if "acescct" in low else "srgb"
    eout = "bt1886" if ein == "cineon" and ("2.4" in low or "rec709" in low or "rec.709" in low) else "srgb"
    return ein, eout


class CubeTarget:
    """A 3D .cube LUT as a look: the scene is encoded for its input (CUBE_IN), its output decoded to sRGB (CUBE_OUT).
    enc_in / enc_out None: from the file's text (guess). Raises ValueError for a file that is not a 3D LUT."""

    def __init__(self, path: Path | None = None, enc_in=None, enc_out=None, text=None, name=None):
        if text is None:
            text = Path(path).read_text(encoding="utf-8", errors="replace")
        self.name = name or (Path(path).stem.replace(" ", "_") if path else "look")
        gin, gout = guess(text)
        self.enc_in, self.enc_out = enc_in or gin, enc_out or gout
        if self.enc_in not in CUBE_IN:
            raise ValueError(f"unknown input encoding {self.enc_in!r} (one of {', '.join(CUBE_IN)})")
        if self.enc_out not in CUBE_OUT:
            raise ValueError(f"unknown output encoding {self.enc_out!r} (one of {', '.join(CUBE_OUT)})")
        self.title = ""
        n = 0
        lo, hi = np.zeros(3), np.ones(3)
        rows = []
        for line in text.splitlines():
            t = line.strip().split()
            if not t or t[0].startswith("#"):
                continue
            if t[0] == "TITLE":
                self.title = line.strip()[5:].strip().strip('"')
            elif t[0] == "LUT_3D_SIZE":
                n = int(t[1])
                if not 2 <= n <= MAX_SIZE:
                    raise ValueError(f"LUT_3D_SIZE {n}: only 2..{MAX_SIZE} are supported")
            elif t[0] == "LUT_1D_SIZE":
                raise ValueError("a 1D LUT (or a 1D shaper before the 3D table): only plain 3D LUTs are supported")
            elif t[0] == "DOMAIN_MIN":
                lo = np.array([float(v) for v in t[1:4]])
            elif t[0] == "DOMAIN_MAX":
                hi = np.array([float(v) for v in t[1:4]])
            elif t[0] == "LUT_3D_INPUT_RANGE":
                lo, hi = np.full(3, float(t[1])), np.full(3, float(t[2]))
            elif t[0][0] in "0123456789.-" and len(t) == 3:
                rows.append([float(v) for v in t])
        if not n or len(rows) != n ** 3:
            raise ValueError(f"not a 3D .cube: size {n}, {len(rows)} rows")
        if np.any(hi <= lo):
            raise ValueError("DOMAIN_MAX must be above DOMAIN_MIN in every channel")
        self.n, self.lo, self.hi = n, lo, hi
        self.lut = np.array(rows).reshape(n, n, n, 3)    # index [b][g][r], red fastest in the file
        self.desc = f"{CUBE_IN[self.enc_in][0]} in, {CUBE_OUT[self.enc_out][0]} out, {n}^3"

    def apply(self, v):
        """Trilinear lookup on the declared domain, v (N, 3)."""
        p = np.clip((v - self.lo) / (self.hi - self.lo), 0, 1) * (self.n - 1)
        i = np.minimum(np.floor(p).astype(int), self.n - 2)
        f = p - i
        out = 0
        for dr in (0, 1):
            for dg in (0, 1):
                for db in (0, 1):
                    w = (f[:, 0] if dr else 1 - f[:, 0]) * (f[:, 1] if dg else 1 - f[:, 1]) * (f[:, 2] if db else 1 - f[:, 2])
                    out = out + w[:, None] * self.lut[i[:, 2] + db, i[:, 1] + dg, i[:, 0] + dr]
        return out

    def __call__(self, x709):
        v = CUBE_IN[self.enc_in][1](np.clip(x709, 0, 1))
        return CUBE_OUT[self.enc_out][1](self.apply(v))
