"""The fitting recipe: the training set, its weights, the recipes and their stages, and the fitter's version.

A look is fitted independently of any one picture, as the camera's tables are: on a training set of colours in the
table signal (camera.py), with the cost CIEDE2000 between what the look wants and what the tables give (fit.py). The
result is the quantised table set (model.Hardware) that an .isp file carries (ispfile.ispb_om5m2).

    fit_look(look, recipe)      a look (a function of the scene, e.g. cube.CubeTarget) -> model.Hardware
    training_set(quick, prior)  the colours and weights fitted on
    evaluate(look, hw, x)       what the look wants and what the tables give, for scene colours
    mono_kind(out)              whether a look is black and white (neutral or toned)

The fitter's version (version()) is a hash of the sources the fit depends on: an app keeps fits per (look, options,
version, prior), so a changed fitter never serves an old fit."""
from __future__ import annotations

import hashlib
from dataclasses import replace
from pathlib import Path

import numpy as np

from . import __version__, camera, color, fit, model
from . import prior as prior_mod

HERE = Path(__file__).resolve().parent
SOURCES = ("color.py", "model.py", "fit.py", "train.py", "cube.py", "camera.py", "backend.py", "prior.py")

# The recipes - what a fit varies, in stages; the look's own 3x3 matrix is an option of every recipe (the camera then
# applies one matrix: the look's times its own). name: (label, description, blocks the tables are run with without the
# matrix, stages - model.Cfg with the matrix off; a stage starts from the previous one's result). All but "curves" start
# with the global part (gamma, tone, k and the matrix when on); the sectors then take only what it leaves.
_GLOBAL = model.Cfg(sectors=False, matrix=False)
RECIPES = {
    "curves": ("1. Curves", "Only the gamma curves R, G, B and the tone curve on luma (with the matrix: a 3x3 matrix "
               "before them). The fastest; holds tone and overall colour balance, not single hues.", (),
               [model.Cfg(matrix=False, sat=False, sectors=False)]),
    "curves+sat": ("2. Curves + saturation", "The curves plus the saturation factor k (one gain of the colour "
                   "difference signals). No hue sectors.", ("sat",), [model.Cfg(sectors=False, matrix=False)]),
    "full": ("3. Full", "Then the 12 hue sectors with the camera's own windows: each sector's chroma, hue and luma "
             "additions are fitted; how they fade with chroma stays the camera's own.", ("sat", "sectors"),
             [_GLOBAL, model.Cfg(matrix=False)]),
    "full+profiles": ("4. Full + profiles", "As Full, plus how each operation fades with chroma (one profile per "
                      "operation, shared by all sectors).", ("sat", "sectors"),
                      [_GLOBAL, model.Cfg(pc="shared", matrix=False)]),
    "best": ("5. Best", "As Full + profiles, then a chroma profile for every sector and free hue windows (position and "
             "width). The default: with the matrix the most accurate and the most robust.", ("sat", "sectors"),
             [_GLOBAL, model.Cfg(pc="shared", matrix=False), model.Cfg(pc="sector", win_h=True, matrix=False)]),
    "best+all": ("6. Best + all", "Everything in the sectors is free: hue and luma profiles of every sector, chroma and "
                 "luma windows. Many parameters: with the matrix it usually lands worse than Best.",
                 ("sat", "sectors"),
                 [_GLOBAL, model.Cfg(pc="shared", matrix=False),
                  model.Cfg(pc="sector", ph="sector_op", py="sector", win_h=True, win_c=True, win_y=True, matrix=False)]),
}
DEFAULT = "best"
MATRIX = True                # the matrix option by default
STALL = 2.0                  # fit_state: the first stage ending this many times above the curves stage is stuck
# A black-and-white look (mono_kind): on the training set the target's a*b* depend on L* alone - within MONO_DEV of
# their mean at the same L* (99th percentile; black-and-white film looks 0.00-0.20, colour looks 50-75); neutral when
# its chroma C*ab stays below MONO_C as well (a toned print: about 1). Every recipe is then one stage, model.Cfg(mono=...)
MONO_DEV = 1.0
MONO_C = 0.25
DOMAIN = "lin"               # the gamma curves' node domain: linear (model.DOMAINS)

CC_SRGB = np.array([[115, 82, 68], [194, 150, 130], [98, 122, 157], [87, 108, 67], [133, 128, 177], [103, 189, 170],
                    [214, 126, 44], [80, 91, 166], [193, 90, 99], [94, 60, 108], [157, 188, 64], [224, 163, 46],
                    [56, 61, 150], [70, 148, 73], [175, 54, 60], [231, 199, 31], [187, 86, 149], [8, 133, 161],
                    [243, 243, 242], [200, 200, 200], [160, 160, 160], [122, 122, 121], [85, 85, 85], [52, 52, 52]]) / 255.0
PRIOR_WEIGHT = 1000.0      # the photo prior's total weight (about the weight of the RGB cube)


def use_of(variant, matrix=MATRIX):
    """The blocks the tables are run with (model.run_hardware(..., use))."""
    return (("matrix",) if matrix else ()) + RECIPES[variant][2]


def stages_of(variant, matrix=MATRIX):
    return [replace(c, matrix=matrix) for c in RECIPES[variant][3]]


def version():
    """The fitter's version: ispkit's version and a hash of the fit's sources with the line ends as in git (LF)."""
    h = hashlib.sha256()
    for name in SOURCES:
        h.update((HERE / name).read_bytes().replace(b"\r\n", b"\n"))
    return f"{__version__}-{h.hexdigest()[:8]}"


def grid(n):
    v = color.srgb_decode(np.linspace(0, 1, n))
    r, g, b = np.meshgrid(v, v, v, indexing="ij")
    return np.stack([r.ravel(), g.ravel(), b.ravel()], axis=1)


def _prior(prior):
    """True - the bundled prior; a path - that file; a prior.Prior - itself; None / False - none."""
    if prior is True:
        return prior_mod.load()
    if prior is None or prior is False:
        return None
    if isinstance(prior, prior_mod.Prior):
        return prior
    return prior_mod.load(prior)


def training_set(quick=False, prior=True):
    """The colours fitted on, in the table signal (camera.py), and their weights:
    - the RGB cube of the table signal (13^3, quick 9^3), weighted down with chroma (1 at grey, 1/2 at C*ab = 35):
      photographs hold few strongly saturated colours;
    - ColorChecker's 24 patches at exposures from -4 to +3 stops around the metered one (those within the signal's
      0..1), weight 4 each;
    - the neutral axis from 2^-12 to the full scale, 64 points of weight 200: it carries the look's tone and exposure;
    - the photo prior (prior.py; 24^3 cells, quick 16^3) with the total weight PRIOR_WEIGHT, its colours brought to the
      signal as metered scenes - what the sensor delivers, not clipped after the camera's matrix: saturated colours
      leave 0..1 there, and the look's matrix gets them as the camera's does."""
    cube = grid(9 if quick else 13)
    lab = color.lab(color.srgb_encode(cube))
    x = [cube]
    w = [1 / (1 + (np.hypot(lab[:, 1], lab[:, 2]) / 35) ** 2)]
    cc = np.concatenate([camera.table_of_scene(color.srgb_decode(CC_SRGB) * 2.0 ** k) for k in range(-4, 4)])
    cc = cc[(cc.max(axis=1) <= 1) & (cc.min(axis=1) >= 0)]
    x.append(cc)
    w.append(np.full(len(cc), 4.0))
    greys = np.repeat(np.concatenate([[0], np.geomspace(2.0 ** -12, 1, 63)])[:, None], 3, axis=1)
    x.append(greys)
    w.append(np.full(len(greys), 200.0))
    pr = _prior(prior)
    if pr is not None:
        xp, wp = pr.cells(16 if quick else 24)
        if len(xp):
            xs = camera.sensor(camera.table_of_scene(xp))
            x.append(xs)
            w.append(wp * PRIOR_WEIGHT)
    return np.concatenate(x), np.concatenate(w)


def camera_target(look):
    """The look as the camera should render it, on the table signal (camera.CameraTarget; one per look)."""
    ct = getattr(look, "_camera_target", None)
    if ct is None:
        ct = camera.CameraTarget(look)
        look._camera_target = ct
    return ct


def evaluate(look, hw, x709, domain=DOMAIN, use=None):
    """Scene colours (linear BT.709, grey 0.18) through the camera: (what the look wants, what the tables give)."""
    s = camera.table_of_scene(x709)
    return camera_target(look)(s), model.run_hardware(s, hw, domain, use if use is not None else use_of(DEFAULT))


def accuracy(look, hw, use, domain=DOMAIN):
    """CIEDE2000 between the look and the tables on ColorChecker's 24 patches and on the RGB cube (17^3, every colour
    of the sRGB gamut's corners included): {"colorchecker": de, "cube": de}."""
    out = {}
    for k, x in (("colorchecker", color.srgb_decode(CC_SRGB)), ("cube", grid(17))):
        out[k] = color.de2000(*(color.lab(v) for v in evaluate(look, hw, x, domain, use)))
    return out


def mono_kind(out):
    """The target's display RGB on the training set: "neutral", "toned" (a black-and-white look) or "" (MONO_DEV, MONO_C)."""
    lab = color.lab(out)
    b = np.clip((lab[:, 0] / 2).astype(int), 0, 50)
    n = np.maximum(np.bincount(b, minlength=51), 1)
    mean = np.stack([np.bincount(b, lab[:, i], 51) / n for i in (1, 2)], 1)
    if np.percentile(np.hypot(*(lab[:, 1:] - mean[b]).T), 99) >= MONO_DEV:
        return ""
    return "neutral" if np.percentile(np.hypot(lab[:, 1], lab[:, 2]), 99) < MONO_C else "toned"


def plan(variant=DEFAULT, matrix=MATRIX, mono=""):
    """How many fit stages a recipe runs (at least; a stuck first stage adds a rerun): for the progress shown."""
    if mono:
        return 1
    stages = stages_of(variant, matrix)
    return len(stages) + (stages[0] != replace(RECIPES["curves"][3][0], matrix=matrix))


def fit_state(target, variant=DEFAULT, domain=DOMAIN, quick=False, log=None, k_max=fit.K_MAX, prior=True, matrix=MATRIX,
              info=None):
    """The continuous State after the recipe's stages (with or without the look's matrix); target: a look on the scene -
    fitted through the camera (camera_target). A black-and-white look (mono_kind) gets one stage in every recipe
    (model.Cfg mono): the tone curve at the identity, with the matrix one weighted sum of the channels (like a film's
    sensitivity), k = 0 and one gamma curve for a neutral look, k = 1 and the tint from the gamma curves for a toned one.
    info (a dict, optional) gets "mono" (mono_kind) and "stages" (plan) before the first stage starts."""
    ct = camera_target(target)
    x, w = training_set(quick, prior)
    out = ct(x)
    st0 = fit.init_state(ct, domain)
    iters = 12 if quick else 40
    kind = mono_kind(out)
    if info is not None:
        info.update(mono=kind, stages=plan(variant, matrix, kind), iters=iters)
    if kind:
        cfg = model.Cfg(matrix=matrix, sat=False, sectors=False, mono=kind)
        if matrix:      # start within the constraint: every row the luma weights (the gamma's start is the neutral axis)
            st0.off = model.off_of(np.tile([0.299, 0.587, 0.114], (3, 1)))
        st = fit.fit(out, x, st0, cfg, domain, w, iters=iters, reg=fit.make_reg(cfg, st0, k_max=k_max), log=log)
        st.k = 0.0 if kind == "neutral" else 1.0
        return st
    stages = stages_of(variant, matrix)
    curves = replace(RECIPES["curves"][3][0], matrix=matrix)

    def run(st, cfgs, first=None):
        """The stages from st; returns the State and the last stage's cost (regulariser included)."""
        info_ = {}
        for j, cfg in enumerate(cfgs):
            if j == 0 and first is not None:
                st, info_ = first
                continue
            info_ = {}
            st = fit.fit(out, x, st, cfg, domain, w, iters=iters, reg=fit.make_reg(cfg, st, k_max=k_max), log=log,
                         info=info_)
        return st, info_.get("cost", np.inf)

    cfg1 = stages[0]
    info1 = {}
    st1 = fit.fit(out, x, st0, cfg1, domain, w, iters=iters, reg=fit.make_reg(cfg1, st0, k_max=k_max), log=log, info=info1)
    st, cost = run(st0, stages, first=(st1, info1))
    if cfg1 != curves:
        # Safeguard for the first stage, which starts from the identity: a look that swaps the colour channels can
        # stop the first stage far from the answer while "curves" alone gets close. The "curves" stage is fitted from
        # the same start; when the first stage - though it varies more - ends STALL times higher than it (stuck: 40 to
        # 120 times; working looks 1.0-1.4), or takes no step, the stages are run again from the curves result and the
        # result whose last stage ends lower is kept. Where the first stage works nothing changes
        infoc = {}
        stc = fit.fit(out, x, st0, curves, domain, w, iters=iters, reg=fit.make_reg(curves, st0, k_max=k_max), log=log,
                      info=infoc)
        if info1["steps"] == 0 or info1["cost"] > STALL * infoc["cost"]:
            if info is not None:
                info["stages"] += len(stages) - 1
            st2, cost2 = run(stc, stages)
            (log or print)(f"fit_state: {variant}: stage 1 ended at {info1['cost']:.1f} ({info1['steps']} steps), the "
                           f"curves stage at {infoc['cost']:.1f}; last stage {cost:.1f}, from the curves result "
                           f"{cost2:.1f} - {'taken' if cost2 < cost else 'the first kept'}")
            if cost2 < cost:
                st = st2
    return st


def fit_look(target, variant=DEFAULT, domain=DOMAIN, quick=False, log=None, matrix=MATRIX, prior=True, info=None):
    """A look -> the quantised tables (model.Hardware)."""
    return model.quantise(fit_state(target, variant, domain, quick, log, prior=prior, matrix=matrix, info=info))


# ------------------------------------------------------------------ the tables as plain data (the fit cache)
_HW_ARRAYS = ("matrix_q7", "gamma12", "tone10", "a_sat", "a_hue", "a_lum", "h0", "hw", "c0", "cw", "y0", "yw", "prof")


def hardware_dict(hw: model.Hardware) -> dict:
    """The tables as JSON-able integers (every field of model.Hardware)."""
    return {**{k: np.asarray(getattr(hw, k)).astype(int).tolist() for k in _HW_ARRAYS}, "k_q": int(hw.k_q),
            "mask": int(hw.mask)}


def hardware_of_dict(d: dict) -> model.Hardware:
    a = {k: np.array(d[k], dtype=int) for k in _HW_ARRAYS}
    return model.Hardware(a["matrix_q7"], a["gamma12"], a["tone10"], int(d["k_q"]), a["a_sat"], a["a_hue"], a["a_lum"],
                          a["h0"], a["hw"], a["c0"], a["cw"], a["y0"], a["yw"], a["prof"], int(d["mask"]))
