"""Model of the OM-5 Mark II colour path, built from the firmware research (OMKit/docs/platform/om5m2/color-pipeline.md):

  linear camera RGB -> MATRIX 3x3 (s16 Q7, rows sum to 128) -> GAMMA per channel R/G/B (256 nodes -> 12 bit)
  -> RGB->YCbCr BT.601 with the saturation factor k on Cb/Cr (Q8: 144k, 182k) -> TONE curve on Y (256 nodes -> 10 bit;
  node i at Y = i/256 - the curve is indexed by the top 8 bits of Y, measured on the camera with black-and-white
  presets) -> 12 SECTORS (local adjusters, below) -> YCbCr 8 bit -> display RGB.

On the camera the look's matrix F is not applied after the frame's matrix S+80: core writes one Q7 matrix
round(F * S+80) (put_matrix in OMKit/src/platform/om5m2/fw1100/ispdrv.c) and the hardware applies it to the
white-balanced RAW, with one clip at the gamma's input. forward() gets the table signal (after S+80, not clipped) and
applies F in floats - the same up to the rounding of the product; run_hardware() applies the rounded product, as the
camera does.

The sector block as the firmware programs it (firmware 1.1: builder 0x603e78fd/0x603e7c3f, loader 0x6037ccd2, writer
0x6037c519): every sector k has a hue window [h0, h0+hw) (32768 = a turn), a chroma window [c0, c0+cw) and a luma window
[y0, y0+yw) (widths up to 1023, hw up to 32767 - a whole turn; a luma window of width 0 is off, weight 1); a 16-node
weight profile over each window, zero outside: node values in 1/64 steps, -1..63/64, linearly interpolated - the
hardware word is the node value and the slope to the next node, which core builds from the values (measured on the
camera); the last node slopes to 0 at the window's edge (a hue window over the whole turn: to node 0). A hue and a chroma
profile for each of the three ADDITIVE operations - chroma (A_sat), hue (A_hue, 32768 = a turn) and luma (A_lum) - and
one luma profile per sector:
    w_op,k = PH_op,k(h) * PC_op,k(C) * PY_k(Y)
    dC = sum A_sat,k w_sat,k;  dh = sum A_hue,k w_hue,k;  dY = sum A_lum,k w_lum,k
The luma profile weighs all three operations (camera experiments, interpolated like the others). In 10-bit units: Y
0..1023 after the tone curve (the luma window's coordinate is the JPEG luma), Cb/Cr +-512 after k, C = |CbCr|,
h = atan2(Cr, Cb). The hardware stores the window widths as reciprocals E<<8|M (M = round(2^(E+8)/w) in [128, 255]), so
the effective width is 2^(E+8)/M (eff_width).

Which of these quantities a fit varies, and which are shared by the sectors, is a Cfg: the stock geometry (the profile
table COLOR 0x60561040 of firmware 1.1) with shared profiles up to everything per sector. The forward pass gives the
analytic Jacobian of the output over the fitted vector (chain rule over the stages; Packer maps the vector to State).

Camera experiments set the additive amplitudes (A_sat 256 = +0.25 of C, a C ramp profile makes it a gain; A_lum 128 =
+0.12 of Y; A_hue 2731 = +30 degrees), the profile slots of every operation, the interpolation, the luma window and
profile and the order "k, then sectors" (the menu saturation is a uniform chroma gain before the sector block). The
matrix: core >= 1.00 writes it (without it the camera's JPEGs were 4.8 dE off the model, with it 1.3). Assumptions (not
established): the node domain of the gamma curves (DOMAINS; i/255 or i/256 of the full scale is one scale of the input,
inside camera.FULL_SCALE), the C metric and the place of the tone curve before the sectors."""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from .backend import B as _B        # the Jacobian's backend: GPU float32 or CPU float64 (backend.py)

TURN = 32768
FULL_TURN = 32767        # the widest hue window (15-bit field): its hue profiles wrap from node 15 to node 0
# Stock sector geometry and hue profile words (node | slope), the profile table COLOR 0x60561040 (firmware 1.1), in sector
# order (windows monotone around the circle; the profile editor's saturation slot i is sector 6 * (i % 2) + i // 2)
SECTOR_H0 = np.array([12197, 8192, 3095, 28945, 24212, 16111, 9466, 6736, 30401, 27489, 20753, 15019])
SECTOR_HW = np.array([3823, 3959, 5006, 6872, 4642, 8055, 5461, 2685, 9057, 2867, 6690, 5643])
SECTOR_HUE_WORDS = np.array([
    [6, 1541, 2822, 4357, 5638, 7173, 8454, 9989, 11270, 12805, 14086, 15866, 14322, 10739, 7410, 3826],
    [12, 3084, 6157, 9484, 12556, 15869, 15098, 13562, 12027, 10746, 9210, 7674, 6138, 4603, 3322, 1786],
    [5, 1286, 2821, 4102, 5637, 6917, 8198, 9733, 11014, 12549, 13830, 15614, 15089, 11250, 7665, 3826],
    [19, 4882, 9491, 14340, 15611, 14331, 13051, 11771, 10491, 9211, 7931, 6651, 5371, 4091, 2811, 1531],
    [6, 1541, 2822, 4357, 5638, 7173, 8454, 9990, 11525, 12806, 14341, 15865, 14066, 10483, 7154, 3571],
    [7, 1799, 3590, 5127, 6919, 8711, 10503, 12295, 14086, 15867, 14582, 12023, 9719, 7415, 5110, 2551],
    [8, 2056, 4104, 6152, 8199, 9992, 12040, 14088, 16376, 14328, 12280, 10233, 8440, 6392, 4344, 2296],
    [7, 1800, 3847, 5639, 7431, 9224, 11271, 13063, 14850, 15608, 13559, 11255, 8952, 6903, 4600, 2551],
    [7, 1798, 3335, 5126, 6663, 8454, 9991, 11782, 13319, 15105, 15606, 13046, 10486, 7926, 5366, 2806],
    [8, 2056, 4103, 5896, 7944, 9992, 12039, 13832, 16122, 14584, 12536, 10488, 8440, 6392, 4344, 2296],
    [8, 2055, 3848, 5895, 7688, 9736, 11783, 13576, 15868, 14840, 12792, 10744, 8695, 6392, 4344, 2296],
    [22, 5654, 11282, 16123, 14844, 13819, 12539, 11259, 9980, 8955, 7675, 6395, 5115, 3836, 2811, 1531]])
C_WIN = 1023             # stock chroma window 0..1023 (10 bit)
Y_WIN = 1023             # stock luma window 0..1023 (1024 reads as 0: the window off - camera experiments)
NP = 16                  # nodes per profile
# Which operations (chroma, hue, luma) the luma profile weighs: all three (camera experiments: luma, chroma, hue)
Y_OPS = (True, True, True)
# Overlapping sectors (camera experiments and real-scene shots): sectors 0..5 and 6..11
# are two banks (the two halves of the weight tables); within a bank a pixel gets one sector - the lowest-numbered whose
# window holds it - and the two banks add up. SECTOR_MODE: "sum" (every sector adds, an earlier model),
# "first_h" / "first_hc" / "first_hcy" (one sector of all 12, held by the hue / hue and chroma / hue, chroma and luma
# windows), the same with "_bank" (one per bank). Which windows decide: hue and chroma (a sector whose chroma window
# misses the pixel leaves it to the next one, measured on the camera); whether the luma window does is not known
SECTOR_MODE = "first_hc_bank"
MASK_ALL = 0x0FFF        # the sector mask (S+976, ISPB +2080): all 12 on. A sector that is off is skipped - it neither acts nor
#                          holds a pixel of its bank (hypothesis: every file and camera experiment so far had all 12 on)
Q = 64                   # profile node steps per 1.0: the hardware holds -64..63 (measured on the camera)
PROF_STEP = 16384 // Q   # a node step in the .isp block's Q14 values
PROF_ONE = (Q - 1) * PROF_STEP     # the largest profile value, 63/64
PROF_MIN = -Q * PROF_STEP          # -1.0
# The stock hue profiles as node values (Q14): the high byte of the words
SECTOR_HUE_Q14 = (SECTOR_HUE_WORDS >> 8) * PROF_STEP
OPS = ("sat", "hue", "lum")
# Profile order of a sector in the hardware tables (and in the .isp block): H/C of chroma, H/C of hue, H/C of luma, Y
PROFILE_ORDER = ("h_sat", "c_sat", "h_hue", "c_hue", "h_lum", "c_lum", "y")
# Internal units of the amplitudes: chroma/1023 (with a ramp profile over C a gain), hue in degrees, luma/1023
SA = np.array([1023.0, TURN / 360.0, 1023.0])

# Node domain of the gamma curves: node position t in [0, 1] for a linear value m, back, and dt/dm.
# lin: uniform in linear input - consistent with the stock curve 0x60533138 (slope ~5 at black, like the BT.709 toe) and
# with a 14-bit linear input (firmware research); sqrt: the alternative, for the sensitivity check
DOMAINS = {
    "lin": (lambda m: m, lambda t: t, lambda m: np.ones_like(m)),
    "sqrt": (lambda m: np.sqrt(np.maximum(m, 0)), lambda t: t * t, lambda m: 0.5 / np.sqrt(np.maximum(m, 1e-12))),
}

KG = 33     # gamma knots per channel (interpolated to the 256 hardware nodes)
KT = 17     # tone knots
# Knot positions on the node axis: dense near black, where the curves bend most (the hardware nodes are uniform)
GAMMA_KNOTS = np.concatenate([[0], np.geomspace(1 / 255, 1, KG - 1)])
TONE_KNOTS = np.linspace(0, 1, KT)          # on the Y axis
TONE_SCALE = 256                            # the tone curve's node i stands at Y = i / TONE_SCALE (above node 255: the
TONE_AT = np.arange(256) / TONE_SCALE       # last segment continued)
GAMMA_AT = np.linspace(0, 1, 256)           # the gamma's node i at i/255 of the full scale (camera.FULL_SCALE)
RAMP = np.arange(NP) / NP                   # a profile over C that makes an additive operation a gain: A * C / 1023
#                                             (exact in 1/64 steps; node 15 slopes to 0 only past C = 15/16 of the window)


def _interp_matrix(pos, at):
    """(n, K): the hardware nodes (at their positions at) from the knots, linear (np.interp is linear in the knot
    values)."""
    return np.stack([np.interp(at, pos, np.eye(len(pos))[k]) for k in range(len(pos))], axis=1)


AG = _interp_matrix(GAMMA_KNOTS, GAMMA_AT)
AT = _interp_matrix(TONE_KNOTS, TONE_AT)


def nodes(knots):
    """The 256 hardware nodes of gamma (KG) or tone (KT) knots."""
    if len(knots) == KG:
        return np.interp(GAMMA_AT, GAMMA_KNOTS, knots)
    return np.interp(TONE_AT, TONE_KNOTS, knots)


def compose_q7(f_q7, base_q7):
    """The matrix the camera applies: core's put_matrix (fw1100/ispdrv.c) - F * S+80 in Q7, every product rounded half away
    from zero, the row sum brought back to 128 on the diagonal."""
    f, b = np.asarray(f_q7, int), np.asarray(base_q7, int)
    m = np.zeros((3, 3), int)
    for r in range(3):
        for c in range(3):
            acc = int(f[r] @ b[:, c])
            m[r, c] = (acc + 64) >> 7 if acc >= 0 else -((-acc + 64) >> 7)
        m[r, r] += 128 - m[r].sum()
    return m


def off_of(m):
    """The six off-diagonals of a matrix whose rows sum to 1 (State.off)."""
    m = np.asarray(m, float)
    return np.array([m[0, 1], m[0, 2], m[1, 0], m[1, 2], m[2, 0], m[2, 1]])


def recip(width: int) -> int:
    """The sector record's reciprocal field: E<<8 | M, M = round(2^(E+8)/width) in [128, 255] (builder 0x603e78fd)."""
    if width <= 0:
        return 0
    for e in range(16):
        m = round(2 ** (e + 8) / width)
        if 128 <= m <= 255:
            return e << 8 | m
    raise ValueError(width)


def eff_width(width):
    """The window width the hardware applies: 2^(E+8)/M from the reciprocal field."""
    r = recip(int(round(width)))
    return 2.0 ** ((r >> 8) + 8) / (r & 0xFF) if r else float(width)


# ------------------------------------------------------------------ what a fit varies
@dataclass(frozen=True)
class Cfg:
    matrix: bool = True      # the 3x3 matrix
    sat: bool = True         # the factor k on Cb/Cr
    sectors: bool = True     # the sector block (amplitudes A of the three operations)
    pc: str = "fixed"        # profiles over C: fixed (ramp) | shared (one per operation) | sector (per sector and operation)
    py: str = "fixed"        # profile over Y: fixed (1) | shared | sector
    ph: str = "stock"        # hue profiles: stock | sector_op (per sector and operation)
    win_h: bool = False      # hue windows h0/hw
    win_c: bool = False      # chroma windows c0/cw
    win_y: bool = False      # luma windows y0/yw
    mono: str = ""           # a black-and-white look: "neutral" - k = 0 (Cb = Cr = 0 on the camera),
    #                          "toned" - k = 1, the tint from the gamma curves; k is not fitted. A fitter holds the
    #                          tone curve at the identity, with the matrix the rows equal, and for "neutral" the gamma
    #                          curves equal


def cfg_of_use(use) -> Cfg:
    """The blocks a quantised table set is run with (model.run_hardware(..., use))."""
    return Cfg(matrix="matrix" in use, sat="sat" in use, sectors="sectors" in use)


@dataclass
class State:
    """The colour path in continuous values (what a fit varies). Units: A in SA units; windows: H0/HW in degrees,
    C0/CW/Y0/YW in 1/1023."""
    gamma: np.ndarray                    # (3, KG) knots or (3, 256) nodes, 0..1
    tone: np.ndarray                     # (KT,) knots or (256,) nodes
    off: np.ndarray = field(default_factory=lambda: np.zeros(6))   # matrix off-diagonals; the diagonal keeps rows at 1
    k: float = 1.0
    A: np.ndarray = field(default_factory=lambda: np.zeros((3, 12)))
    PH: np.ndarray = field(default_factory=lambda: np.repeat((SECTOR_HUE_Q14 / 16384.0)[None], 3, axis=0))
    PC: np.ndarray = field(default_factory=lambda: np.tile(RAMP, (3, 12, 1)))
    PY: np.ndarray = field(default_factory=lambda: np.ones((12, NP)))
    H0: np.ndarray = field(default_factory=lambda: SECTOR_H0 * 360.0 / TURN)
    HW: np.ndarray = field(default_factory=lambda: SECTOR_HW * 360.0 / TURN)
    C0: np.ndarray = field(default_factory=lambda: np.zeros(12))
    CW: np.ndarray = field(default_factory=lambda: np.full(12, C_WIN / 1023))
    Y0: np.ndarray = field(default_factory=lambda: np.zeros(12))
    YW: np.ndarray = field(default_factory=lambda: np.full(12, Y_WIN / 1023))
    on: np.ndarray = field(default_factory=lambda: np.ones(12, bool))   # the sector mask (ISPB +2080): off - skipped

    def copy(self):
        return State(**{k: (v.copy() if isinstance(v, np.ndarray) else v) for k, v in self.__dict__.items()})

    def matrix(self):
        o = self.off
        m = np.array([[0, o[0], o[1]], [o[2], 0, o[3]], [o[4], o[5], 0]], dtype=float)
        m[np.diag_indices(3)] = 1 - m.sum(axis=1)
        return m

    def gamma_nodes(self):
        return self.gamma @ (AG if self.gamma.shape[1] == KG else np.eye(256)).T

    def tone_nodes(self):
        return (AT if len(self.tone) == KT else np.eye(256)) @ self.tone


class Packer:
    """The fitted vector <-> State for a Cfg. Sector quantities have column maps (-1: not fitted); shared quantities map
    all sectors to the same columns."""

    def __init__(self, cfg: Cfg, st: State):
        self.cfg = cfg
        n = 0

        def add(size):
            nonlocal n
            n += size
            return n - size

        self.i_gamma = add(st.gamma.size)
        self.ng = st.gamma.shape[1]
        self.i_tone = add(st.tone.size)
        self.i_off = add(6) if cfg.matrix else None
        self.i_k = add(1) if cfg.sat else None
        self.cm = {}
        if cfg.sectors:
            cmA = -np.ones((3, 12), int)
            for o in range(3):
                cmA[o] = add(12) + np.arange(12)
            self.cm["A"] = cmA
            per = 12 * (NP - 1)
            cmPC = -np.ones((3, 12, NP), int)
            cmPH = -np.ones((3, 12, NP), int)
            for o in range(3):
                if cfg.pc == "shared":
                    cmPC[o, :, 1:] = (add(NP - 1) + np.arange(NP - 1))[None]
                elif cfg.pc == "sector":
                    cmPC[o, :, 1:] = add(per) + np.arange(per).reshape(12, NP - 1)
                if cfg.ph == "sector_op":
                    cmPH[o, :, 1:] = add(per) + np.arange(per).reshape(12, NP - 1)
            self.cm["PC"], self.cm["PH"] = cmPC, cmPH
            cmPY = -np.ones((12, NP), int)
            if cfg.py == "shared":
                cmPY[:] = (add(NP) + np.arange(NP))[None]
            elif cfg.py == "sector":
                cmPY[:] = add(12 * NP) + np.arange(12 * NP).reshape(12, NP)
            self.cm["PY"] = cmPY
            for nm, on in (("H0", cfg.win_h), ("HW", cfg.win_h), ("C0", cfg.win_c), ("CW", cfg.win_c),
                           ("Y0", cfg.win_y), ("YW", cfg.win_y)):
                self.cm[nm] = (add(12) + np.arange(12)) if on else -np.ones(12, int)
        self.n = n

    def vector(self, st: State):
        v = np.zeros(self.n)
        v[self.i_gamma:self.i_gamma + st.gamma.size] = st.gamma.ravel()
        v[self.i_tone:self.i_tone + st.tone.size] = st.tone
        if self.i_off is not None:
            v[self.i_off:self.i_off + 6] = st.off
        if self.i_k is not None:
            v[self.i_k] = st.k
        for nm, cm in self.cm.items():
            sel = cm >= 0
            v[cm[sel]] = getattr(st, nm)[sel]
        return v

    def state(self, v, base: State):
        st = base.copy()
        st.gamma = v[self.i_gamma:self.i_gamma + st.gamma.size].reshape(st.gamma.shape).copy()
        st.tone = v[self.i_tone:self.i_tone + st.tone.size].copy()
        if self.i_off is not None:
            st.off = v[self.i_off:self.i_off + 6].copy()
        if self.i_k is not None:
            st.k = float(v[self.i_k])
        for nm, cm in self.cm.items():
            sel = cm >= 0
            getattr(st, nm)[sel] = v[cm[sel]]
        return st


def _prof(p, idx, wrap=False):
    """A 16-node profile at fractional index idx: value, d/didx, node, fraction, inside (zero outside). Linear between the
    nodes (the hardware word's slope, measured on the camera); past node 15 to 0 at the window's edge, or with wrap (a hue
    window over the whole turn) to node 0 - the callers map the Jacobian column of node NP to node 0 then."""
    inside = (idx >= 0) & (idx < NP)
    i = np.clip(np.floor(idx).astype(int), 0, NP - 1)
    f = np.where(inside, idx - i, 0.0)
    p = np.asarray(p, float)
    pp = np.append(p, p[0] if wrap else 0.0)
    a, b = pp[i], pp[i + 1]
    return np.where(inside, a + (b - a) * f, 0.0), np.where(inside, b - a, 0.0), i, f, inside


def _front(x, st: State, cfg: Cfg, domain="lin"):
    """The path up to the sector block (matrix, gamma, YCbCr with k, tone) and what the Jacobian needs of it."""
    fwd, _, dfwd = DOMAINS[domain]
    M = st.matrix() if cfg.matrix else np.eye(3)
    mr = x @ M.T
    mmask = (mr > 0) & (mr < 1)
    m = np.clip(mr, 0, 1)
    # the tables hold 0..1 (12 and 10 bit): a node outside is clipped, as the hardware has it - no gradient through it
    gn_raw = st.gamma_nodes()
    gin = ((gn_raw >= 0) & (gn_raw <= 1)).astype(float)
    gn = np.clip(gn_raw, 0, 1)
    t = fwd(m) * 255
    i = np.minimum(t.astype(int), 254)
    f = t - i
    g = np.empty_like(m)
    sl = np.empty_like(m)
    for c in range(3):
        g[:, c] = gn[c][i[:, c]] * (1 - f[:, c]) + gn[c][i[:, c] + 1] * f[:, c]
        sl[:, c] = (gn[c][i[:, c] + 1] - gn[c][i[:, c]]) * 255 * mmask[:, c]
    sl = sl * dfwd(m)
    y = 0.299 * g[:, 0] + 0.587 * g[:, 1] + 0.114 * g[:, 2]
    kk = 0.0 if cfg.mono == "neutral" else st.k if cfg.sat else 1.0
    cb = (g[:, 2] - y) / 1.772 * kk
    cr = (g[:, 0] - y) / 1.402 * kk
    tn_raw = st.tone_nodes()
    tin = ((tn_raw >= 0) & (tn_raw <= 1)).astype(float)
    tn = np.clip(tn_raw, 0, 1)
    ymask = (y > 0) & (y < 1)
    ty = np.clip(y, 0, 1) * TONE_SCALE
    j = np.minimum(ty.astype(int), 254)
    u = ty - j
    y2 = tn[j] * (1 - u) + tn[j + 1] * u
    return m, mmask, gn, gin, i, f, g, sl, y, kk, cb, cr, tn, tin, ymask, j, u, y2


def _hcy(y2, cb, cr):
    """The sector block's coordinates: y10 (0..1023), h (32768 a turn), C (10 bit) and cb10, cr10."""
    y10, cb10, cr10 = y2 * 1023, cb * 1024, cr * 1024
    return np.mod(np.arctan2(cr10, cb10) * TURN / (2 * np.pi), TURN), np.hypot(cb10, cr10), y10, cb10, cr10


def _geom(st: State, s: int, h, c, y10, eff):
    """Sector s at the pixels: its widths (effective where eff says), the fractional node indices over its hue, chroma and
    luma windows, the luma weight, whether its hue profiles wrap, and whether its windows hold the pixel (SECTOR_MODE)."""
    N = len(h)
    hw, h0 = st.HW[s] * TURN / 360, st.H0[s] * TURN / 360
    cw, c0 = st.CW[s] * 1023, st.C0[s] * 1023
    yw, y0 = st.YW[s] * 1023, st.Y0[s] * 1023
    hw = eff_width(hw) if "h" in eff else hw
    cw = eff_width(cw) if "c" in eff else cw
    if yw <= 0:
        # the luma window off (width 0, reciprocal 0): weight 1 - all 12 sectors of the profile table COLOR
        yw, iy = 1.0, np.zeros(N)
        wy, dwy, yi = np.ones(N), np.zeros(N), np.zeros(N, int)
        yf, yin = np.zeros(N), np.zeros(N, bool)
    else:
        yw = eff_width(yw) if "y" in eff else yw
        iy = (y10 - y0) * NP / yw
        wy, dwy, yi, yf, yin = _prof(st.PY[s], iy)
    ih = np.mod(h - h0, TURN) * NP / hw
    ic = (c - c0) * NP / cw
    wrap = round(st.HW[s] * TURN / 360) >= FULL_TURN
    held = ih < NP
    if SECTOR_MODE.startswith(("first_hc", "first_hcy")):
        held &= (ic >= 0) & (ic < NP)
    if SECTOR_MODE.startswith("first_hcy") and st.YW[s] > 0:
        held &= (iy >= 0) & (iy < NP)
    return hw, cw, yw, ih, ic, iy, wy, dwy, yi, yf, yin, wrap, held


def sector_input(x, st: State, cfg: Cfg, domain="lin"):
    """The sector block's input for the table signal x (N, 3): h (32768 a turn), C (10 bit), y10 (0..1023), cb10, cr10 -
    the path of forward() up to the sectors."""
    *_, kk, cb, cr, _tn, _tin, _ym, _j, _u, y2 = _front(x, st, cfg, domain)
    return _hcy(y2, cb, cr)


def sector_weights(h, c, y10, st: State, eff=()):
    """What the sector block does at (h, c, y10), as forward() runs it: W (3, 12, N) - the weight of operation o (chroma,
    hue, luma) of sector s, zero where a lower sector of the bank holds the pixel (SECTOR_MODE) or the sector is off;
    held (12, N) - the sector's windows hold the pixel; won (12, N) - it is the one of its bank that acts; D (3, N) - the
    additions sum_s A W in 10-bit chroma, 32768-a-turn hue and 10-bit luma."""
    N = len(h)
    W = np.zeros((3, 12, N))
    held = np.zeros((12, N), bool)
    won = np.zeros((12, N), bool)
    taken = np.zeros(N, bool)
    for s in range(12):
        if s == 6 and SECTOR_MODE.endswith("_bank"):
            taken[:] = False
        if not st.on[s]:
            continue
        hw, cw, yw, ih, ic, iy, wy, *_r, wrap, hs = _geom(st, s, h, c, y10, eff)
        fr = 1.0
        held[s] = hs
        if SECTOR_MODE != "sum":
            fr = (~taken).astype(float)
            won[s] = hs & ~taken
            taken |= hs
        else:
            won[s] = hs
        for o in range(3):
            wh = _prof(st.PH[o, s], ih, wrap)[0] * fr
            wc = _prof(st.PC[o, s], ic)[0]
            W[o, s] = wh * wc * (wy if Y_OPS[o] else 1.0)
    D = np.einsum("os,osn->on", st.A * SA[:, None], W)
    return W, held, won, D


def output_of(h2, c2r, y3r):
    """Display RGB (N, 3) of the sector block's output: hue h2 (32768 a turn), chroma c2r and luma y3r (10 bit, before the
    clipping), as forward() makes it."""
    phi = h2 * 2 * np.pi / TURN
    c2 = np.maximum(c2r, 0)
    return _rgb(np.clip(y3r, 0, 1023) / 1023, c2 * np.cos(phi) / 1024, c2 * np.sin(phi) / 1024)


def _rgb(Y, CB, CR):
    r = Y + 1.402 * CR
    b = Y + 1.772 * CB
    gg = (Y - 0.299 * r - 0.114 * b) / 0.587
    return np.stack([r, gg, b], axis=1)


def forward(x, st: State, cfg: Cfg, pk: Packer | None = None, domain="lin", eff=()):
    """x (N, 3) the table signal - what the tables get: the camera's linear signal after its matrix S+80, 1 = the
    gamma's full scale (camera.py; a scene is brought there by camera.table_of_scene) -> display RGB (N, 3); with pk
    also the Jacobian (N, 3, pk.n) - an array of the dense backend (backend.B: a torch tensor on the GPU or numpy).
    eff: window kinds ("h", "c", "y") applied with the hardware's effective width."""
    N = len(x)
    jac = pk is not None
    P = pk.n if jac else 0
    m, mmask, gn, gin, i, f, g, sl, y, kk, cb, cr, tn, tin, ymask, j, u, y2 = _front(x, st, cfg, domain)

    if jac:
        V, Ix = _B.V, _B.I
        Jg = _B.zeros((N, 3, P))
        Ag = V(AG if pk.ng == KG else np.eye(256))
        for c in range(3):
            Jg[:, c, pk.i_gamma + c * pk.ng: pk.i_gamma + (c + 1) * pk.ng] = \
                Ag[Ix(i[:, c])] * V((1 - f[:, c]) * gin[c][i[:, c]])[:, None] + \
                Ag[Ix(i[:, c] + 1)] * V(f[:, c] * gin[c][i[:, c] + 1])[:, None]
        if pk.i_off is not None:
            # m0 = x0 + o0(x1-x0) + o1(x2-x0); m1 = x1 + o2(x0-x1) + o3(x2-x1); m2 = x2 + o4(x0-x2) + o5(x1-x2)
            dm = np.zeros((N, 3, 6))
            dm[:, 0, 0], dm[:, 0, 1] = x[:, 1] - x[:, 0], x[:, 2] - x[:, 0]
            dm[:, 1, 2], dm[:, 1, 3] = x[:, 0] - x[:, 1], x[:, 2] - x[:, 1]
            dm[:, 2, 4], dm[:, 2, 5] = x[:, 0] - x[:, 2], x[:, 1] - x[:, 2]
            Jg[:, :, pk.i_off:pk.i_off + 6] += V(sl[:, :, None] * dm)
        Jy = 0.299 * Jg[:, 0] + 0.587 * Jg[:, 1] + 0.114 * Jg[:, 2]
        Jcb = (Jg[:, 2] - Jy) / 1.772 * kk
        Jcr = (Jg[:, 0] - Jy) / 1.402 * kk
        del Jg
        if pk.i_k is not None:
            Jcb[:, pk.i_k] += V((g[:, 2] - y) / 1.772)
            Jcr[:, pk.i_k] += V((g[:, 0] - y) / 1.402)
        Jy2 = Jy * V((tn[j + 1] - tn[j]) * TONE_SCALE * ymask)[:, None]
        At = V(AT if len(st.tone) == KT else np.eye(256))
        Jy2[:, pk.i_tone:pk.i_tone + At.shape[1]] += At[Ix(j)] * V((1 - u) * tin[j])[:, None] + \
            At[Ix(j + 1)] * V(u * tin[j + 1])[:, None]

    if cfg.sectors:
        h, c, y10, cb10, cr10 = _hcy(y2, cb, cr)
        D = np.zeros((3, N))
        dD = np.zeros((3, 3, N))      # d D_op / d (h, c, y10)
        if jac:
            JD = _B.zeros((3, N, P + 1))      # the last column collects the quantities not fitted
            ar = Ix(np.arange(N))
        taken = np.zeros(N, bool)             # a sector already holds the pixel (SECTOR_MODE first_*)
        for s in range(12):
            if s == 6 and SECTOR_MODE.endswith("_bank"):
                taken[:] = False              # the second half of the tables (sectors 6..11) is a bank of its own
            if not st.on[s]:
                continue                      # off in the sector mask
            hw, cw, yw, ih, ic, iy, wy, dwy, yi, yf, yin, wrap, held = _geom(st, s, h, c, y10, eff)
            fr = 1.0
            if SECTOR_MODE != "sum":
                fr = (~taken).astype(float)
                taken |= held
            for o in range(3):
                Aeff = st.A[o, s] * SA[o]
                wh, dwh, hi, hf, hin = _prof(st.PH[o, s], ih, wrap)
                if not hin.any():
                    continue
                wh, dwh = wh * fr, dwh * fr
                wc, dwc, ci, cf, cin = _prof(st.PC[o, s], ic)
                lum = Y_OPS[o]
                wyo = wy if lum else 1.0
                D[o] += Aeff * wh * wc * wyo
                dD[o, 0] += Aeff * dwh * NP / hw * wc * wyo
                dD[o, 1] += Aeff * wh * dwc * NP / cw * wyo
                if lum:
                    dD[o, 2] += Aeff * wh * wc * dwy * NP / yw
                if not jac:
                    continue
                Jrow = JD[o]
                col = pk.cm["A"][o, s]
                if col >= 0:
                    Jrow[:, col] += V(SA[o] * wh * wc * wyo)
                for nm, ii, ff, inn, fac, wr in (("PH", hi, hf, hin, Aeff * wc * wyo * fr, wrap),
                                                 ("PC", ci, cf, cin, Aeff * wh * wyo, False)):
                    cm = pk.cm[nm][o, s]
                    if (cm >= 0).any():
                        cmx = np.where(cm >= 0, cm, P)
                        cmx = np.append(cmx, cmx[0] if wr else P)
                        Jrow[ar, Ix(cmx[ii])] += V(fac * (1 - ff) * inn)
                        Jrow[ar, Ix(cmx[np.minimum(ii + 1, NP)])] += V(fac * ff * inn)
                cm = pk.cm["PY"][s]
                if lum and (cm >= 0).any():
                    fac = Aeff * wh * wc
                    cmx = np.append(np.where(cm >= 0, cm, P), P)
                    Jrow[ar, Ix(cmx[yi])] += V(fac * (1 - yf) * yin)
                    Jrow[ar, Ix(cmx[np.minimum(yi + 1, NP)])] += V(fac * yf * yin)
                for nm0, nmw, dv, idx, w_, unit, other in (("H0", "HW", dwh, ih, hw, TURN / 360, wc * wyo),
                                                          ("C0", "CW", dwc, ic, cw, 1023.0, wh * wyo),
                                                          ("Y0", "YW", dwy, iy, yw, 1023.0, wh * wc * lum)):
                    if pk.cm[nm0][s] >= 0:
                        Jrow[:, pk.cm[nm0][s]] += V(-Aeff * other * dv * NP / w_ * unit + np.zeros(N))
                    if pk.cm[nmw][s] >= 0:
                        Jrow[:, pk.cm[nmw][s]] += V(-Aeff * other * dv * idx / w_ * unit + np.zeros(N))
        c2r = c + D[0]
        cmask = c2r > 0
        c2 = np.maximum(c2r, 0)
        h2 = h + D[1]
        y3r = y10 + D[2]
        y3mask = (y3r > 0) & (y3r < 1023)
        phi = h2 * 2 * np.pi / TURN
        Y, CB, CR = np.clip(y3r, 0, 1023) / 1023, c2 * np.cos(phi) / 1024, c2 * np.sin(phi) / 1024
        if jac:
            # the output as a scaling and rotation of (cb, cr): out = s R(th) z, s = c2/c, th = D_hue - stable at neutrals
            big = c > 1e-9
            cs0 = np.where(big, c, 1.0)
            uz = np.stack([cb10, cr10], axis=1) / cs0[:, None] * big[:, None]
            tz = np.stack([-cr10, cb10], axis=1) / cs0[:, None] * big[:, None]
            Jz = _B.stack([Jcb, Jcr], 1) * 1024
            Jy10 = Jy2 * 1023
            Jc = _B.einsum("ni,nip->np", V(uz), Jz)
            Jh = _B.einsum("ni,nip->np", V(tz), Jz) * V(TURN / (2 * np.pi) / cs0)[:, None]
            JDt = [V(dD[o, 0])[:, None] * Jh + V(dD[o, 1])[:, None] * Jc + V(dD[o, 2])[:, None] * Jy10 + JD[o, :, :P]
                   for o in range(3)]
            del JD
            s_ = np.where(big, c2 / cs0, np.maximum(1 + dD[0, 1], 0))
            # zero at the neutrals (big false) and where the chroma is clipped at 0
            Js = (JDt[0] - V(D[0] / cs0)[:, None] * Jc) * V(big * cmask / cs0)[:, None]
            th = D[1] * 2 * np.pi / TURN
            ct, st_ = np.cos(th), np.sin(th)
            Rz = np.stack([ct * cb10 - st_ * cr10, st_ * cb10 + ct * cr10], axis=1)
            dRz = np.stack([-st_ * cb10 - ct * cr10, ct * cb10 - st_ * cr10], axis=1)
            Jth = JDt[1] * (2 * np.pi / TURN)
            ctv, stv = V(ct)[:, None], V(st_)[:, None]
            RJz = _B.stack([ctv * Jz[:, 0] - stv * Jz[:, 1], stv * Jz[:, 0] + ctv * Jz[:, 1]], 1)
            Jo = (V(s_)[:, None, None] * RJz + V(Rz)[:, :, None] * Js[:, None, :]
                  + V(s_[:, None] * dRz)[:, :, None] * Jth[:, None, :]) / 1024
            Jout = _B.cat([((Jy10 + JDt[2]) * V(y3mask / 1023)[:, None])[:, None], Jo], 1)
    else:
        Y, CB, CR = y2, cb, cr
        if jac:
            Jout = _B.stack([Jy2, Jcb, Jcr], 1)
    raw = _rgb(Y, CB, CR)
    out = np.clip(raw, 0, 1)
    if not jac:
        return out
    T = np.array([[1, 0, 1.402], [0, 0, 0], [1, 1.772, 0]], float)
    T[1] = (np.array([1, 0, 0]) - 0.299 * T[0] - 0.114 * T[2]) / 0.587
    return out, _B.einsum("ij,njp->nip", T, Jout) * V((raw > 0) & (raw < 1))[:, :, None]


# ------------------------------------------------------------------ the hardware tables
@dataclass
class Hardware:
    """The tables as the camera would get them (quantised): the sector records S+688+24k and the 7 profiles per sector."""
    matrix_q7: np.ndarray                # 3x3 int, rows sum to 128
    gamma12: np.ndarray                  # (3, 256) int 0..4095
    tone10: np.ndarray                   # (256,) int 0..1023
    k_q: int                             # Cb factor 144k (Cr 182k derives from the same k)
    a_sat: np.ndarray                    # 12 int, sector record +20 (chroma, 10-bit units)
    a_hue: np.ndarray                    # 12 int, +18 (32768 = a turn; 455 = 5 degrees)
    a_lum: np.ndarray                    # 12 int, +22 (luma, 10-bit units: camera experiments)
    h0: np.ndarray = field(default_factory=lambda: SECTOR_H0.copy())         # 12 int, +0 (15 bit)
    hw: np.ndarray = field(default_factory=lambda: SECTOR_HW.copy())         # +2
    c0: np.ndarray = field(default_factory=lambda: np.zeros(12, int))        # +4
    cw: np.ndarray = field(default_factory=lambda: np.full(12, C_WIN))       # +6
    y0: np.ndarray = field(default_factory=lambda: np.zeros(12, int))        # +8
    yw: np.ndarray = field(default_factory=lambda: np.full(12, Y_WIN))       # +10
    prof: np.ndarray = field(default_factory=lambda: stock_profiles())      # (12, 7, 16) int Q14, PROFILE_ORDER
    mask: int = MASK_ALL                 # the sector mask, ISPB +2080 / S+976: bit k - sector k on

    @property
    def prof_q14(self):
        """The profiles of sector 0 by name (u_sat, u_hue, u_lum, v_y) - for code that knew only shared profiles."""
        p = self.prof[0]
        return {"u_sat": p[1], "u_hue": p[3], "u_lum": p[5], "v_y": p[6]}


def stock_profiles():
    p = np.zeros((12, 7, NP), int)
    for o in range(3):
        p[:, 2 * o] = SECTOR_HUE_Q14
        p[:, 2 * o + 1] = np.round(RAMP * 16384).astype(int)[None]
    p[:, 6] = PROF_ONE
    return p


def prof_q14(a):
    """Profile values -> the .isp block's node values (Q14 in 1/64 steps, -1..63/64): what the camera holds."""
    return (np.clip(np.round(np.asarray(a, float) * Q), -Q, Q - 1) * PROF_STEP).astype(int)


def prof_of_q14(v):
    """The .isp block's node values -> profile values as core reads them (fw1100/ispdrv.c node64: rounded to 1/64)."""
    return np.clip((np.asarray(v, int) + PROF_STEP // 2) // PROF_STEP, -Q, Q - 1) / Q


def quantise(st: State) -> Hardware:
    m = np.round(st.matrix() * 128).astype(int)
    m[np.diag_indices(3)] += 128 - m.sum(axis=1)  # the firmware's row normalisation 0x603e4f0c
    q14 = prof_q14
    prof = np.zeros((12, 7, NP), int)
    for o in range(3):
        prof[:, 2 * o] = q14(st.PH[o])
        prof[:, 2 * o + 1] = q14(st.PC[o])
    prof[:, 6] = q14(st.PY)
    a = np.clip(np.round(st.A * SA[:, None]), -32768, 32767).astype(int)
    rnd = lambda v: np.round(v * 1023).astype(int)   # noqa: E731
    return Hardware(m, np.clip(np.round(st.gamma_nodes() * 4095), 0, 4095).astype(int),
                    np.clip(np.round(st.tone_nodes() * 1023), 0, 1023).astype(int), int(round(st.k * 144)),
                    a[0], a[1], a[2],
                    np.mod(np.round(st.H0 * TURN / 360), TURN).astype(int),
                    np.clip(np.round(st.HW * TURN / 360), 1, FULL_TURN).astype(int),
                    np.clip(rnd(st.C0), 0, 1023), np.clip(rnd(st.CW), 1, 1023),
                    np.clip(rnd(st.Y0), 0, 1023), np.clip(rnd(st.YW), 0, 1023), prof,
                    int(sum(1 << k for k in range(12) if st.on[k])))


def state_of(h: Hardware) -> State:
    """The quantised tables back as a State (gamma and tone as 256 nodes)."""
    st = State(gamma=h.gamma12 / 4095.0, tone=h.tone10 / 1023.0, off=off_of(h.matrix_q7 / 128.0), k=h.k_q / 144.0)
    st.A = np.stack([h.a_sat, h.a_hue, h.a_lum]).astype(float) / SA[:, None]
    p = prof_of_q14(h.prof)
    st.PH = np.stack([p[:, 0], p[:, 2], p[:, 4]])
    st.PC = np.stack([p[:, 1], p[:, 3], p[:, 5]])
    st.PY = p[:, 6].copy()
    st.H0, st.HW = h.h0 * 360.0 / TURN, h.hw * 360.0 / TURN
    st.C0, st.CW, st.Y0, st.YW = h.c0 / 1023.0, h.cw / 1023.0, h.y0 / 1023.0, h.yw / 1023.0
    st.on = np.array([bool(h.mask >> k & 1) for k in range(12)])
    return st


def run_state(x, st: State, domain="lin", use=("matrix", "sat", "sectors"), eff=(), out8=False, chunk=200000):
    cfg = cfg_of_use(use)
    out = []
    for a in range(0, len(x), chunk):
        o = forward(x[a:a + chunk], st, cfg, domain=domain, eff=eff)
        out.append(np.round(o * 255) / 255 if out8 else o)
    return np.concatenate(out) if out else np.zeros((0, 3))


def run_hardware(x, h: Hardware, domain, use):
    """x: the table signal (N, 3; camera.py - after the frame's matrix S+80, not clipped) -> display RGB (N, 3) through
    the quantised tables as the camera runs them, 8-bit output. With the matrix: the camera's one Q7 matrix
    round(F * S+80) on the RAW (compose_q7), here applied to the table signal as round(F * S+80) * S+80^-1. The factor k
    is in every file: one other than 1 (a black-and-white look's 0) is applied whatever use says."""
    st, use = hardware_state(h, use)
    return run_state(x, st, domain, use, eff=("h", "c", "y"), out8=True)


def hardware_state(h: Hardware, use):
    """The State and the blocks run_hardware runs the quantised tables with (the matrix composed with S+80, k always
    when it is not 1); the windows then go with the hardware's effective widths (eff = ("h", "c", "y"))."""
    from . import camera
    st = state_of(h)
    use = tuple(use)
    if "matrix" in use:
        base = np.round(camera.S80 * 128).astype(int)
        st.off = off_of(compose_q7(h.matrix_q7, base) / 128.0 @ np.linalg.inv(camera.S80))
    if h.k_q != 144 and "sat" not in use:
        use += ("sat",)
    return st, use
