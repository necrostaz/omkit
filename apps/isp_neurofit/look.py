"""The film look fitted from scans (ScanLook) and its neutral start.

A look is a small parametric function of the scene (linear BT.709, middle grey 0.18, the camera's headroom above 1 kept)
to display sRGB, stored as look.json:

    tone_ev, tone_L         the display L* of a neutral at 25 exposure nodes (stops from grey, -8..+4)
    cast_L, cast_a, cast_b  a* and b* added at five lightness nodes (0, 25, 50, 75, 100)
    sat, sat_L              the overall chroma gain, and a gain over the same lightness nodes
    hue_deg, hue_sat,       12 nodes over the CIELAB hue circle, every 30 degrees: a chroma gain, a hue turn (degrees)
    hue_shift, hue_lum      and a lightness offset (L*), addressed by the colour's hue before the look's own hue changes

The neutral start is the model's stand-in for the camera's Natural, not its curve: plain sRGB of the scene up to 0.5,
then a shoulder that brings the camera's headroom to the display white. A look is fitted to the camera's tables like any
other look (ispkit.train.fit_look)."""
from __future__ import annotations

import hashlib
import json

import numpy as np

from ispkit import camera, color

TONE_EV = np.linspace(-8.0, 4.0, 25)
CAST_L = np.array([0.0, 25.0, 50.0, 75.0, 100.0])
HUE_DEG = np.arange(12) * 30.0  # CIELAB hue angles of the hue nodes
NEUTRAL_C = 10.0                # hue operations fade in from the neutral axis up to this chroma
HUE_NAMES = ("red-magenta 0", "red 30", "orange 60", "yellow 90", "yellow-green 120", "green 150", "green-cyan 180",
             "cyan 210", "cyan-blue 240", "blue 270", "violet 300", "magenta 330")


def lab_of_linear(d):
    """Linear display RGB (BT.709) -> CIELAB D65."""
    return color.lab(color.srgb_encode(d))


def linear_of_lab(lab):
    """CIELAB D65 -> linear BT.709 (not clipped)."""
    fy = (lab[:, 0] + 16) / 116
    f = np.stack([fy + lab[:, 1] / 500, fy, fy - lab[:, 2] / 200], axis=1)
    e, k = 216 / 24389, 24389 / 27
    xyz = np.where(f ** 3 > e, f ** 3, (116 * f - 16) / k) * color.WHITE_D65
    return xyz @ np.linalg.inv(color.M709).T


def y_of_l(L):
    """CIELAB L* -> relative luminance Y."""
    L = np.clip(L, 0, 100)
    return np.where(L > 8, ((L + 16) / 116) ** 3, L * 27 / 24389)


def natural_tone():
    """The neutral start's tone: plain sRGB of the scene up to 0.5, then a shoulder that brings the camera's headroom
    (the scene's 2.54 at the sensor's white) to the display white. L* at the nodes TONE_EV."""
    x = 0.18 * 2.0 ** TONE_EV
    top, knee, width = 1.06 * 0.18 / camera.GREY, 0.5, 0.5
    sh = knee + (1 - knee) * (1 - np.exp(-(x - knee) / width)) / (1 - np.exp(-(top - knee) / width))
    d = np.clip(np.where(x <= knee, x, sh), 0, 1)
    return color.lab(np.repeat(color.srgb_encode(d)[:, None], 3, axis=1))[:, 0]


def natural_look() -> dict:
    """look.json of the neutral start."""
    return {"version": 1, "tone_ev": TONE_EV.tolist(), "tone_L": np.round(natural_tone(), 3).tolist(),
            "cast_L": CAST_L.tolist(), "cast_a": [0.0] * 5, "cast_b": [0.0] * 5,
            "sat": 1.0, "sat_L": [1.0] * 5,
            "hue_deg": HUE_DEG.tolist(), "hue_sat": [1.0] * 12, "hue_shift": [0.0] * 12, "hue_lum": [0.0] * 12,
            "notes": []}


def _periodic(h, values):
    """Linear interpolation of 12 node values over the hue circle, h in degrees."""
    v = np.asarray(values, float)
    p = (h % 360.0) / 30.0
    i = np.floor(p).astype(int) % 12
    f = p - np.floor(p)
    return v[i] * (1 - f) + v[(i + 1) % 12] * f


class ScanLook:
    """A look for the fitter: scene (linear BT.709, grey 0.18, the camera's headroom above 1 kept) -> display sRGB.
    1. the tone curve, per channel (a neutral lands exactly on tone_L; colours lose chroma towards the ends, as on film);
    2. in CIELAB of that: chroma x sat x sat_L(L) x hue_sat(h), hue + hue_shift(h), L + hue_lum(h) - the three hue
       operations fading out below NEUTRAL_C; then the cast a/b(L) added."""

    def __init__(self, p: dict, name="scan_look"):
        self.p = p
        self.name = name
        self.desc = f"scan look {self.key()}"

    def key(self):
        """A short hash of the parameters (the notes left out): names the look in the history and the file's META."""
        d = {k: v for k, v in self.p.items() if k != "notes"}
        return hashlib.sha256(json.dumps(d, sort_keys=True).encode()).hexdigest()[:12]

    def base(self, x709):
        """Step 1 only: display linear RGB."""
        p = self.p
        ev = np.log2(np.maximum(np.asarray(x709, float), 1e-9) / 0.18)
        return y_of_l(np.interp(ev, p["tone_ev"], p["tone_L"]))

    def lab(self, x709):
        p = self.p
        lab = lab_of_linear(self.base(x709))
        L, a, b = lab.T
        c = np.hypot(a, b)
        h = np.degrees(np.arctan2(b, a))
        w = np.clip(c / NEUTRAL_C, 0, 1)
        gain = p["sat"] * np.interp(L, p["cast_L"], p["sat_L"]) * (1 + w * (_periodic(h, p["hue_sat"]) - 1))
        h2 = np.radians(h + w * _periodic(h, p["hue_shift"]))
        c2 = c * np.maximum(gain, 0)
        return np.stack([np.clip(L + w * _periodic(h, p["hue_lum"]), 0, 100),
                         c2 * np.cos(h2) + np.interp(L, p["cast_L"], p["cast_a"]),
                         c2 * np.sin(h2) + np.interp(L, p["cast_L"], p["cast_b"])], axis=1)

    def __call__(self, x709):
        x = np.asarray(x709, float).reshape(-1, 3)
        return color.srgb_encode(np.clip(linear_of_lab(self.lab(x)), 0, 1))


def pin_grey(p: dict, log=print) -> dict:
    """Slide the tone curve along the exposure axis until the scene's grey (0 EV) lands where the neutral start puts it.
    Unpaired sets do not say how the film was exposed: the comparison exposes the RAW set to the scans' median THROUGH
    the look, so a darker curve and a larger exposure of the comparison explain the scans equally well, and the estimate
    drifts to a dark curve the camera then applies to pictures as shot. The curve's shape is the film's; its place on
    the exposure axis is fixed here by convention."""
    ev, t = np.array(p["tone_ev"], float), np.array(p["tone_L"], float)
    target = float(np.interp(0.0, TONE_EV, natural_tone()))
    e0 = float(np.interp(target, t, ev))                     # the exposure the curve gives the neutral grey at
    if abs(e0) < 0.005:
        return p
    q = dict(p)
    q["tone_L"] = np.interp(ev + e0, ev, t).tolist()
    log(f"grey pinned: the curve slid {-e0:+.2f} EV, grey {float(np.interp(0.0, ev, t)):.1f} -> {target:.1f}")
    return q


def check(p: dict) -> list:
    """Problems of a hand-edited look.json: missing keys, wrong lengths, a decreasing tone curve. [] when it is fine."""
    sizes = {"tone_ev": 25, "tone_L": 25, "cast_L": 5, "cast_a": 5, "cast_b": 5, "sat_L": 5, "hue_deg": 12,
             "hue_sat": 12, "hue_shift": 12, "hue_lum": 12}
    out = []
    for k, n in sizes.items():
        v = p.get(k)
        if not isinstance(v, list) or len(v) != n or not all(isinstance(x, (int, float)) for x in v):
            out.append(f"{k}: a list of {n} numbers expected")
    if not isinstance(p.get("sat"), (int, float)):
        out.append("sat: a number expected")
    if not out and (np.diff(np.asarray(p["tone_L"], float)) < 0).any():
        out.append("tone_L: the tone curve must not decrease")
    return out
