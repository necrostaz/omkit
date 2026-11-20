"""Colour helpers: transfer functions, primaries, CIELAB and CIEDE2000 (numpy only)."""
from __future__ import annotations

import numpy as np

# Linear RGB -> XYZ (D65)
M709 = np.array([[0.4124564, 0.3575761, 0.1804375],
                 [0.2126729, 0.7151522, 0.0721750],
                 [0.0193339, 0.1191920, 0.9503041]])
M2020 = np.array([[0.6369580, 0.1446169, 0.1688810],
                  [0.2627002, 0.6779981, 0.0593017],
                  [0.0000000, 0.0280727, 1.0609851]])
# ProPhoto (ROMM, D50) -> XYZ D50, then Bradford D50 -> D65
MPRO_D50 = np.array([[0.7976749, 0.1351917, 0.0313534],
                     [0.2880402, 0.7118741, 0.0000857],
                     [0.0000000, 0.0000000, 0.8252100]])
BRAD_D50_D65 = np.array([[0.9555766, -0.0230393, 0.0631636],
                         [-0.0282895, 1.0099416, 0.0210077],
                         [0.0122982, -0.0204830, 1.3299098]])
M_P3 = np.array([[0.4865709, 0.2656677, 0.1982173],
                 [0.2289746, 0.6917385, 0.0792869],
                 [0.0000000, 0.0451134, 1.0439444]])
M_ADOBE = np.array([[0.5767309, 0.1855540, 0.1881852],
                    [0.2973769, 0.6273491, 0.0752741],
                    [0.0270343, 0.0706872, 0.9911085]])
WHITE_D65 = np.array([0.95047, 1.0, 1.08883])

M709_TO_2020 = np.linalg.solve(M2020, M709)
MPRO_TO_709 = np.linalg.solve(M709, BRAD_D50_D65 @ MPRO_D50)


def srgb_encode(x):
    x = np.clip(x, 0.0, 1.0)
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1 / 2.4) - 0.055)


def srgb_decode(v):
    v = np.clip(v, 0.0, 1.0)
    return np.where(v <= 0.04045, v / 12.92, np.power((v + 0.055) / 1.055, 2.4))


def cineon_encode(x):
    """Scene linear -> Cineon log code / 1023 (white 685, black 95, 300 codes per decade)."""
    black = 10 ** ((95 - 685) / 300)
    return (685 + 300 * np.log10(np.maximum(x, 0) * (1 - black) + black)) / 1023


def lab(rgb_encoded):
    """sRGB-encoded RGB (N,3) -> CIELAB D65."""
    xyz = srgb_decode(rgb_encoded) @ M709.T / WHITE_D65
    e = 216 / 24389
    k = 24389 / 27
    f = np.where(xyz > e, np.cbrt(xyz), (k * xyz + 16) / 116)
    return np.stack([116 * f[:, 1] - 16, 500 * (f[:, 0] - f[:, 1]), 200 * (f[:, 1] - f[:, 2])], axis=1)


def de2000(lab1, lab2):
    L1, a1, b1 = lab1.T
    L2, a2, b2 = lab2.T
    C1, C2 = np.hypot(a1, b1), np.hypot(a2, b2)
    Cm = (C1 + C2) / 2
    G = 0.5 * (1 - np.sqrt(Cm ** 7 / (Cm ** 7 + 25 ** 7)))
    a1p, a2p = (1 + G) * a1, (1 + G) * a2
    C1p, C2p = np.hypot(a1p, b1), np.hypot(a2p, b2)
    h1p = np.degrees(np.arctan2(b1, a1p)) % 360
    h2p = np.degrees(np.arctan2(b2, a2p)) % 360
    dLp, dCp = L2 - L1, C2p - C1p
    dh = h2p - h1p
    dh = np.where(np.abs(dh) > 180, dh - 360 * np.sign(dh), dh)
    dh = np.where(C1p * C2p == 0, 0, dh)
    dHp = 2 * np.sqrt(C1p * C2p) * np.sin(np.radians(dh / 2))
    Lpm = (L1 + L2) / 2
    Cpm = (C1p + C2p) / 2
    hs = h1p + h2p
    hpm = np.where(C1p * C2p == 0, hs, np.where(np.abs(h1p - h2p) > 180, np.where(hs < 360, hs + 360, hs - 360) / 2, hs / 2))
    T = (1 - 0.17 * np.cos(np.radians(hpm - 30)) + 0.24 * np.cos(np.radians(2 * hpm)) + 0.32 * np.cos(np.radians(3 * hpm + 6))
         - 0.20 * np.cos(np.radians(4 * hpm - 63)))
    dth = 30 * np.exp(-(((hpm - 275) / 25) ** 2))
    Rc = 2 * np.sqrt(Cpm ** 7 / (Cpm ** 7 + 25 ** 7))
    Sl = 1 + 0.015 * (Lpm - 50) ** 2 / np.sqrt(20 + (Lpm - 50) ** 2)
    Sc = 1 + 0.045 * Cpm
    Sh = 1 + 0.015 * Cpm * T
    Rt = -np.sin(np.radians(2 * dth)) * Rc
    return np.sqrt((dLp / Sl) ** 2 + (dCp / Sc) ** 2 + (dHp / Sh) ** 2 + Rt * (dCp / Sc) * (dHp / Sh))
