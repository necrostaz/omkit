"""Levenberg-Marquardt fit of the colour path model (model.py) to a target look.

Residuals: CIELAB differences under the CIEDE2000 metric linearised at the target colour (|T (Lab - Lab_target)|^2 ~ dE00^2,
T fixed per point), or plain CIELAB (dE76). The Jacobian of the data residuals is analytic (model.forward) and stays on
the dense backend (backend.py: the GPU in float32 when there is one) up to the normal equations J^T J, J^T r; the
regulariser's - finite differences (it does not run the model, so it is cheap); the P x P system is solved in float64."""
from __future__ import annotations

import time

import numpy as np

from . import color, model
from .backend import B as _B

K_MAX = 1.5          # soft limit of the saturation factor k: larger k with negative sector chroma is a degenerate pair


def init_state(target, domain):
    """Neutral axis exact from the start: gamma knot t of channel c = target(grey at t)[c]; tone = identity."""
    inv = model.DOMAINS[domain][1]
    t = model.GAMMA_KNOTS
    out = target(np.repeat(inv(t)[:, None], 3, axis=1))
    return model.State(gamma=out.T.copy(), tone=np.linspace(0, 1, model.KT))


def de2000_metric(lab_t):
    """(N, 3, 3) T with |T (lab - lab_t)|^2 ~ CIEDE2000^2 near lab_t (the metric linearised at the target colour)."""
    L, a, b = lab_t.T
    C = np.hypot(a, b)
    G = 0.5 * (1 - np.sqrt(C ** 7 / (C ** 7 + 25 ** 7)))
    ap = (1 + G) * a
    Cp = np.hypot(ap, b)
    hp = np.degrees(np.arctan2(b, ap)) % 360
    T_ = (1 - 0.17 * np.cos(np.radians(hp - 30)) + 0.24 * np.cos(np.radians(2 * hp)) + 0.32 * np.cos(np.radians(3 * hp + 6))
          - 0.20 * np.cos(np.radians(4 * hp - 63)))
    dth = 30 * np.exp(-(((hp - 275) / 25) ** 2))
    Rc = 2 * np.sqrt(Cp ** 7 / (Cp ** 7 + 25 ** 7))
    Sl = 1 + 0.015 * (L - 50) ** 2 / np.sqrt(20 + (L - 50) ** 2)
    Sc = 1 + 0.045 * Cp
    Sh = 1 + 0.015 * Cp * T_
    Rt = -np.sin(np.radians(2 * dth)) * Rc
    # radial and tangential directions in (a', b); da' = (1 + G) da
    big = Cp > 1e-6
    ur = np.where(big[:, None], np.stack([ap, b], 1) / np.where(big, Cp, 1)[:, None], np.array([1.0, 0.0]))
    ut = np.stack([-ur[:, 1], ur[:, 0]], 1)
    dC = np.stack([ur[:, 0] * (1 + G), ur[:, 1]], 1) / Sc[:, None]
    dH = np.stack([ut[:, 0] * (1 + G), ut[:, 1]], 1) / Sh[:, None]
    # dE^2 = (dL/Sl)^2 + e2^2 + e3^2 + Rt e2 e3 = (dL/Sl)^2 + (e2 + Rt/2 e3)^2 + (1 - Rt^2/4) e3^2
    T = np.zeros((len(L), 3, 3))
    T[:, 0, 0] = 1 / Sl
    T[:, 1, 1:] = dC + (Rt / 2)[:, None] * dH
    T[:, 2, 1:] = dH * np.sqrt(np.maximum(1 - Rt ** 2 / 4, 1e-6))[:, None]
    return T


def make_reg(cfg: model.Cfg, st0: model.State, smooth=0.02, k_max=K_MAX):
    """Physical solutions only: smooth, non-decreasing curves; the tone curve near the identity in the shadows (crushing Y
    while the chroma stays makes coloured blacks) and nowhere steeper than TONE_SLOPE_MAX; moderate sector operations;
    smooth profiles within signed Q14; k <= k_max (soft). A black-and-white look (cfg.mono): the tone curve at the
    identity and, with the matrix, the rows equal - one weighted sum of the channels, like the film's sensitivity, then
    the gamma curves (equal for a neutral look; a toned print's tint comes from their differences)."""
    kt = len(st0.tone) == model.KT
    n_t = 4 if kt else 48
    tone_at = model.TONE_KNOTS if kt else model.TONE_AT
    tone_id = tone_at.copy()
    dpos = np.diff(tone_at)

    def reg(st: model.State):
        out = [np.diff(st.gamma, 2, axis=1).ravel() * smooth * 30, np.diff(st.tone, 2) * smooth * 100,
               np.minimum(np.diff(st.gamma, axis=1), 0).ravel() * 300, np.minimum(np.diff(st.tone), 0) * 300,
               (st.tone[:n_t] - tone_id[:n_t]) * 50,
               np.maximum(np.diff(st.tone) / dpos - TONE_SLOPE_MAX, 0) * TONE_SLOPE_W,
               (np.maximum(st.gamma - 1, 0) + np.maximum(-st.gamma, 0)).ravel() * CURVE_OUT,
               (np.maximum(st.tone - 1, 0) + np.maximum(-st.tone, 0)) * CURVE_OUT]
        if cfg.mono:
            out.append((st.tone - tone_id) * MONO_W)
            if cfg.matrix:
                m = st.matrix()
                out.append((m[1:] - m[0]).ravel() * MONO_W)
                if cfg.mono == "neutral":
                    out.append((st.gamma[1:] - st.gamma[0]).ravel() * MONO_W)
        if cfg.sat and k_max:
            out.append(np.atleast_1d(max(st.k - k_max, 0.0) * 100))
        if cfg.sectors:
            a_sat, a_hue, a_lum = st.A
            out += [a_sat * A_REG[0], a_hue * A_REG[1], a_lum * A_REG[2]]
            for kind, arr in (("pc", st.PC), ("ph", st.PH)):
                mode = getattr(cfg, kind)
                if mode in ("fixed", "stock"):
                    continue
                for o in range(3):
                    p = arr[o] if mode in ("sector", "sector_op") else arr[o, :1]
                    out += [np.diff(p, 2, axis=1).ravel() * 5, np.maximum(np.abs(p) - PMAX, 0).ravel() * 100]
            if cfg.py != "fixed":
                p = st.PY if cfg.py == "sector" else st.PY[:1]
                out += [np.diff(p, 2, axis=1).ravel() * 5, np.maximum(np.abs(p) - PMAX, 0).ravel() * 100]
        return np.concatenate(out)

    return reg


PMAX = (model.Q - 1) / model.Q            # profile nodes in 1/64 steps: at most 63/64 (the hardware's range)
# Sector amplitudes within what camera experiments have shown the model holds (chroma +-0.5 of C, hue +-100 degrees, luma
# +-0.125). A fit with the sectors adding up stacked overlapping sectors with large opposite
# amplitudes that cancel in the model - on the camera one sector of a bank acts (model.SECTOR_MODE)
A_MAX = np.array([0.5, 100.0, 0.125])
# The weight of the sector amplitudes in the cost (chroma, hue in degrees, luma): the sectors take only what the global
# part (gamma, tone, k) leaves
# The tone curve's slope: a gamma squeezed into a strip of Y and stretched back by the tone curve renders the same in the
# model and multiplies every error between the two stages on the camera (measured with black-and-white presets: the
# signal squeezed into 9-13 % of Y, slopes 10-12, 1.2 L* of error from the tone curve's node layout alone)
TONE_SLOPE_MAX = 2.0
TONE_SLOPE_W = 20.0
MONO_W = 300.0           # a look without colour: how hard the tone identity and the equal rows and curves are held
CURVES_HARD = False      # True: the curves clipped to 0..1 at every step; False: a soft penalty (make_reg), clipped at the end
CURVE_OUT = 0.0          # no pull needed: the model clips the nodes as the tables do (weights 0/30/300 compared on six looks)
A_REG = np.array([0.5, 0.03, 0.5])   # hue x3 of the earlier 0.01: smaller rotations, ColorChecker 0.94 -> 0.82


def project(st: model.State):
    """Everything within what the hardware holds: the gamma and tone curves in 0..1 (12 and 10 bit - with k near 0, as in
    a black-and-white look, only the weighted sum of the channel curves is seen, and a fit would push single curves
    far outside; the tables clip them), k >= 0, the windows within their fields (h0/hw 15 bit,
    c0/cw/y0/yw 10 bit), the profiles within -1..PMAX - a larger weight is not representable, the amplitude carries it
    instead; the amplitudes within A_MAX."""
    if CURVES_HARD:
        st.gamma = np.clip(st.gamma, 0.0, 1.0)
        st.tone = np.clip(st.tone, 0.0, 1.0)
    st.k = max(float(st.k), 0.0)
    for nm in ("PH", "PC", "PY"):
        setattr(st, nm, np.clip(getattr(st, nm), -1.0, PMAX))
    st.A = np.clip(st.A, -A_MAX[:, None], A_MAX[:, None])
    st.H0 = np.mod(st.H0, 360.0)
    st.HW = np.clip(st.HW, 5.0, 359.0)
    st.C0 = np.clip(st.C0, 0.0, 0.5)
    st.CW = np.clip(st.CW, 0.05, 1.0)
    st.Y0 = np.clip(st.Y0, 0.0, 0.9)
    st.YW = np.clip(st.YW, 0.1, 1.0)
    return st


def lab_jac(rgb):
    """CIELAB of sRGB-encoded rgb and d Lab / d rgb (N, 3, 3)."""
    v = np.clip(rgb, 0, 1)
    dlin = np.where(v <= 0.04045, 1 / 12.92, 2.4 / 1.055 * np.power((v + 0.055) / 1.055, 1.4))
    xyz = color.srgb_decode(v) @ color.M709.T / color.WHITE_D65
    e, kk = 216 / 24389, 24389 / 27
    fx = np.where(xyz > e, np.cbrt(xyz), (kk * xyz + 16) / 116)
    dfx = np.where(xyz > e, 1 / 3 * np.cbrt(np.maximum(xyz, 1e-12)) ** -2, kk / 116)
    lab = np.stack([116 * fx[:, 1] - 16, 500 * (fx[:, 0] - fx[:, 1]), 200 * (fx[:, 1] - fx[:, 2])], axis=1)
    Lf = np.array([[0, 116, 0], [500, -500, 0], [0, 200, -200]], float)
    return lab, np.einsum("ij,nj,jk,nk->nik", Lf, dfx, color.M709 / color.WHITE_D65[:, None], dlin)


def fit(target_out, x, st0: model.State, cfg: model.Cfg, domain="lin", weights=None, metric="de2000", iters=40,
        reg=None, windows_projected=True, tol=1e-4, log=None, info=None):
    """Fit the quantities cfg varies, starting from st0. Returns the continuous State. info (a dict, optional) gets the
    cost at the start and at the end ("cost0", "cost", regulariser included) and the number of accepted steps ("steps"):
    0 means the stage did not move from st0."""
    lab_t = color.lab(target_out)
    w = np.ones(len(x)) if weights is None else weights
    sw = np.sqrt(w)[:, None]
    pk = model.Packer(cfg, st0)
    reg = reg or make_reg(cfg, st0)
    T = de2000_metric(lab_t) if metric == "de2000" else None
    # windows that are not fitted run with the hardware's effective width; fitted ones - continuously
    eff = tuple(k for k, on in (("h", cfg.win_h), ("c", cfg.win_c), ("y", cfg.win_y)) if not on)
    proj = True             # every stage within the hardware's range (project)

    def resid(v, need_j):
        """The residuals; with need_j also the normal equations H = J^T J, g = J^T r (float64)."""
        st = pk.state(v, st0)
        if need_j:
            rgb, Jr = model.forward(x, st, cfg, pk, domain, eff)
            lab, Jl = lab_jac(rgb)
            # the point's whole linear chain (Lab of RGB, the metric, the weight) as one 3x3, applied once
            L = Jl * sw[:, :, None]
        else:
            lab = color.lab(model.forward(x, st, cfg, domain=domain, eff=eff))
        d = lab - lab_t
        if T is not None:
            d = np.einsum("nij,nj->ni", T, d)
            if need_j:
                L = np.einsum("nij,njk->nik", T, Jl) * sw[:, :, None]
        rr = reg(st)
        rd = (d * sw).ravel()
        r = np.concatenate([rd, rr])
        if not need_j:
            return r
        J = _B.einsum("nij,njp->nip", _B.V(L), Jr).reshape(-1, pk.n)
        del Jr
        H, g = _B.normal(J, rd)
        del J
        Jreg = np.empty((rr.size, pk.n))
        for jj in range(pk.n):
            v2 = v.copy()
            v2[jj] += 1e-6
            Jreg[:, jj] = (reg(pk.state(v2, st0)) - rr) / 1e-6
        return r, H + Jreg.T @ Jreg, g + Jreg.T @ rr

    t0 = time.time()
    v = pk.vector(st0)
    r, H, g = resid(v, True)
    cost = r @ r
    cost0, steps = cost, 0
    lam = 1e-2
    for it in range(iters):
        improved = False
        for _ in range(8):
            v2 = v + np.linalg.solve(H + lam * (np.diag(np.diag(H)) + 1e-6 * np.eye(pk.n)), -g)
            if proj and windows_projected:
                v2 = pk.vector(project(pk.state(v2, st0)))
            r2 = resid(v2, False)
            c2 = r2 @ r2
            if c2 < cost:
                rel = (cost - c2) / cost
                v, cost = v2, c2
                steps += 1
                lam = max(lam / 3, 1e-7)
                improved = True
                break
            lam *= 4
        if log:
            log(f"    iter {it}: cost {cost:.1f} lam {lam:.1e} ({time.time() - t0:.1f} s)")
        if not improved or rel < tol:
            break
        r, H, g = resid(v, True)
    if info is not None:
        info.update(cost0=float(cost0), cost=float(cost), steps=steps)
    return pk.state(v, st0)
