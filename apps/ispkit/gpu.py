"""The render of quantised tables on the GPU: model.run_hardware (the forward pass without the Jacobian) in torch, float64
- the same arithmetic in the same order as model.forward, so the 8-bit output is model.run_hardware's up to a rounding
tie (a last-bit difference of atan2, hypot, sin, cos or of a matrix product on the GPU).

model.py stays the one definition of the colour path: this is its copy for speed, and every table set is checked against
model.run_hardware on a probe of colours before the GPU renders it (check); a set that differs by more than one 8-bit code
(a mode this copy does not know, model.SECTOR_MODE) is rendered by model.run_hardware on the CPU, with a message. Without
torch or CUDA, or with ISPKIT_GPU=0 (backend.py), it is not used.

Measured: a RAW of 1310 x 978 (766 thousand distinct colours) - 0.25 s on four CPU threads, about 0.02 s on a desktop
GPU."""
from __future__ import annotations

import hashlib

import numpy as np

from . import model
from .backend import B, torch

PROBE = 4096                 # colours of the picture checked on the CPU, besides the grid
_checked = {}                # table set -> it renders as model.run_hardware does


def available():
    return B.name == "gpu"


def warm():
    """CUDA's first use takes seconds (the context, the kernels): an app calls this at the start, on a thread of its own."""
    st, cfg = _state(model.quantise(model.State(gamma=np.tile(np.linspace(0, 1, model.KG), (3, 1)),
                                                tone=np.linspace(0, 1, model.KT))), ("matrix", "sat", "sectors"))
    _gpu(np.full((16, 3), 0.3), st, cfg, "lin")


def _t(a):
    return torch.as_tensor(np.asarray(a, np.float64), device=B.dev)


def _prof(p, idx, wrap=False):
    """model._prof without the derivative: the 16-node profile's value at idx (zero outside)."""
    inside = (idx >= 0) & (idx < model.NP)
    i = torch.clamp(torch.floor(idx), 0, model.NP - 1).long()
    f = torch.where(inside, idx - i, 0.0)
    p = np.asarray(p, float)
    pp = _t(np.append(p, p[0] if wrap else 0.0))
    a, b = pp[i], pp[i + 1]
    return torch.where(inside, a + (b - a) * f, 0.0), inside


def forward(x, st: model.State, cfg: model.Cfg, domain="lin", eff=()):
    """model.forward(x, st, cfg, domain=domain, eff=eff) for x a (N, 3) float64 tensor on the GPU."""
    M = st.matrix() if cfg.matrix else np.eye(3)
    mr = x @ _t(M).T
    m = torch.clamp(mr, 0, 1)
    gn = _t(np.clip(st.gamma_nodes(), 0, 1))
    if domain == "lin":
        t = m * 255
    elif domain == "sqrt":
        t = torch.sqrt(torch.clamp(m, min=0)) * 255
    else:
        raise ValueError(domain)
    i = torch.clamp(t.long(), max=254)
    f = t - i
    g = torch.empty_like(m)
    for c in range(3):
        g[:, c] = gn[c][i[:, c]] * (1 - f[:, c]) + gn[c][i[:, c] + 1] * f[:, c]
    y = 0.299 * g[:, 0] + 0.587 * g[:, 1] + 0.114 * g[:, 2]
    kk = 0.0 if cfg.mono == "neutral" else st.k if cfg.sat else 1.0
    cb = (g[:, 2] - y) / 1.772 * kk
    cr = (g[:, 0] - y) / 1.402 * kk
    tn = _t(np.clip(st.tone_nodes(), 0, 1))
    ty = torch.clamp(y, 0, 1) * model.TONE_SCALE
    j = torch.clamp(ty.long(), max=254)
    u = ty - j
    y2 = tn[j] * (1 - u) + tn[j + 1] * u
    if cfg.sectors:
        TURN, NP, mode = model.TURN, model.NP, model.SECTOR_MODE
        y10, cb10, cr10 = y2 * 1023, cb * 1024, cr * 1024
        h = torch.remainder(torch.atan2(cr10, cb10) * TURN / (2 * np.pi), TURN)
        c = torch.hypot(cb10, cr10)
        D = torch.zeros((3, len(x)), dtype=x.dtype, device=x.device)
        taken = torch.zeros(len(x), dtype=torch.bool, device=x.device)
        for s in range(12):
            if s == 6 and mode.endswith("_bank"):
                taken[:] = False
            if not st.on[s]:
                continue
            hw, h0 = st.HW[s] * TURN / 360, st.H0[s] * TURN / 360
            cw, c0 = st.CW[s] * 1023, st.C0[s] * 1023
            yw, y0 = st.YW[s] * 1023, st.Y0[s] * 1023
            hw = model.eff_width(hw) if "h" in eff else hw
            cw = model.eff_width(cw) if "c" in eff else cw
            if yw <= 0:
                yw, wy, iy = 1.0, 1.0, None
            else:
                yw = model.eff_width(yw) if "y" in eff else yw
                iy = (y10 - y0) * NP / yw
                wy, _ = _prof(st.PY[s], iy)
            ih = torch.remainder(h - h0, TURN) * NP / hw
            ic = (c - c0) * NP / cw
            wrap = round(st.HW[s] * TURN / 360) >= model.FULL_TURN
            fr = 1.0
            if mode != "sum":
                fr = (~taken).double()
                held = ih < NP
                if mode.startswith(("first_hc", "first_hcy")):
                    held &= (ic >= 0) & (ic < NP)
                if mode.startswith("first_hcy") and st.YW[s] > 0:
                    held &= (iy >= 0) & (iy < NP)
                taken |= held
            for o in range(3):
                Aeff = float(st.A[o, s] * model.SA[o])
                wh, hin = _prof(st.PH[o, s], ih, wrap)
                if not bool(hin.any()):
                    continue
                wh = wh * fr
                wc, _ = _prof(st.PC[o, s], ic)
                wyo = wy if model.Y_OPS[o] else 1.0
                D[o] += Aeff * wh * wc * wyo
        c2 = torch.clamp(c + D[0], min=0)
        h2 = h + D[1]
        y3r = y10 + D[2]
        phi = h2 * 2 * np.pi / TURN
        Y, CB, CR = torch.clamp(y3r, 0, 1023) / 1023, c2 * torch.cos(phi) / 1024, c2 * torch.sin(phi) / 1024
    else:
        Y, CB, CR = y2, cb, cr
    r = Y + 1.402 * CR
    b = Y + 1.772 * CB
    gg = (Y - 0.299 * r - 0.114 * b) / 0.587
    return torch.clamp(torch.stack([r, gg, b], dim=1), 0, 1)


def _state(h: model.Hardware, use):
    """model.run_hardware's state and blocks (model.hardware_state)."""
    st, use = model.hardware_state(h, use)
    return st, model.cfg_of_use(use)


def _gpu(x, st, cfg, domain, chunk=1 << 20):
    out = []
    for a in range(0, len(x), chunk):
        o = forward(_t(x[a:a + chunk]), st, cfg, domain, eff=("h", "c", "y"))
        out.append((torch.round(o * 255) / 255).cpu().numpy())
    return np.concatenate(out) if out else np.zeros((0, 3))


def _key(h: model.Hardware, domain, use):
    d = hashlib.sha256(repr((domain, tuple(use))).encode())
    for v in (h.matrix_q7, h.gamma12, h.tone10, [h.k_q], h.a_sat, h.a_hue, h.a_lum, h.h0, h.hw, h.c0, h.cw, h.y0, h.yw,
              h.prof, [getattr(h, "mask", model.MASK_ALL)]):
        d.update(np.asarray(v, np.int64).tobytes())
    return d.hexdigest()


def check(x, h: model.Hardware, domain, use):
    """Whether the GPU renders this table set as model.run_hardware: the colours of a grid of the table signal (inside and
    beyond 0..1) and PROBE of the picture's, at most one 8-bit code apart. Kept per table set."""
    k = _key(h, domain, use)
    if k not in _checked:
        v = np.linspace(-0.1, 1.2, 15)
        grid = np.stack(np.meshgrid(v, v, v, indexing="ij"), -1).reshape(-1, 3)
        pick = np.random.default_rng(0).choice(len(x), min(PROBE, len(x)), replace=False)
        probe = np.concatenate([grid, x[pick]])
        st, cfg = _state(h, use)
        diff = np.abs(_gpu(probe, st, cfg, domain) - model.run_hardware(probe, h, domain, use)).max() * 255
        _checked[k] = diff < 1.5
        if not _checked[k]:
            print(f"ispkit.gpu: the GPU copy of the model differs from model.run_hardware by {diff:.0f} codes - "
                  "these tables are rendered on the CPU", flush=True)
    return _checked[k]


def run_hardware(x, h: model.Hardware, domain, use):
    """model.run_hardware(x, h, domain, use) on the GPU; None when the check fails (the caller renders on the CPU)."""
    if not check(x, h, domain, use):
        return None
    st, cfg = _state(h, use)
    return _gpu(np.asarray(x, float), st, cfg, domain)
