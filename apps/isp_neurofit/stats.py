"""What two unpaired picture sets are compared by: lightness quantiles, the cast of the near-neutral pixels by lightness,
chroma by hue, and the colours of the anchors (the same kind of object marked in both sets).

A set is a list of (entry, picture): the entry is the picture's record of selection.json (its weight), the picture a
white-balanced camera RAW (h, w, 3; 1 = the sensor's white) or a scan as sRGB 0..1. Every picture counts with its weight,
whatever its size."""
from __future__ import annotations

import numpy as np

from ispkit import camera, color, model

from .look import HUE_NAMES, lab_of_linear

Q = np.array([0.5, 1, 2, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 98, 99, 99.5])     # lightness quantiles, %
CHROMA_MIN = 8.0                # a pixel counts as coloured above this chroma
HUE_SHARE_MIN = 0.015           # a hue bin is compared when both sets hold at least this share of coloured pixels
NEAR_NEUTRAL_C = 12.0           # the cast is read from the pixels below this chroma
PER_PICTURE = 8000              # pixels of a picture that go into the statistics
EV_SPAN = 3.0                   # the exposure alignment of the sets is searched within +- this many stops


def wquantile(v, w, q):
    """Weighted quantiles (q in %)."""
    o = np.argsort(v)
    cw = np.cumsum(w[o])
    return np.interp(np.asarray(q, float) / 100.0 * cw[-1], cw, v[o])


def set_stats(lab, w):
    """The statistics two sets are compared by. lab (N, 3), w (N,) - a pixel's weight (its picture's weight / pixels)."""
    L, a, b = lab.T
    c = np.hypot(a, b)
    h = np.degrees(np.arctan2(b, a)) % 360
    w = w / w.sum()
    out = {"Lq": wquantile(L, w, Q), "chroma_mean": float((c * w).sum()), "cast": [], "hue": []}
    near = c < NEAR_NEUTRAL_C
    for k in range(10):
        m = near & (L >= 10 * k) & (L < 10 * k + 10 + (k == 9))
        s = w[m].sum()
        out["cast"].append([float((a[m] * w[m]).sum() / s), float((b[m] * w[m]).sum() / s), float(s)] if s > 0.004
                           else [np.nan, np.nan, float(s)])
    col = c > CHROMA_MIN
    total = max(w[col].sum(), 1e-12)
    out["coloured_share"] = float(w[col].sum())
    hb = (np.floor((h + 15) / 30).astype(int)) % 12
    for k in range(12):
        m = col & (hb == k)
        s = w[m].sum()
        if s / total >= 0.002 and m.sum() >= 20:
            p50, p80 = wquantile(c[m], w[m], [50, 80])
            out["hue"].append([float(s / total), float(p50), float(p80), float((L[m] * w[m]).sum() / s)])
        else:
            out["hue"].append([float(s / total), np.nan, np.nan, np.nan])
    out["cast"], out["hue"] = np.array(out["cast"]), np.array(out["hue"])
    return out


def distance(r, s):
    """How far the statistics r (a rendered RAW set) are from s (the scans): tone - mean |dL*| over the quantiles 2..98 %
    with the medians aligned (key_dL - the difference of the medians, reported apart: exposure and subject, not the film);
    cast - mean |d(a, b)| of the near-neutral pixels over the lightness bins both sets fill; chroma - the mean
    |log2 ratio| of the chroma (median and 80th percentile) over the hue bins both sets fill, weighted by the share.
    Returns (the measures, the chroma ratio per hue bin - nan where not compared)."""
    q = (Q >= 2) & (Q <= 98)
    key = float(s["Lq"][8] - r["Lq"][8])          # the medians: the sets' key (exposure, subject), not the film
    dl = s["Lq"] - r["Lq"] - key
    ok = ~np.isnan(r["cast"][:, 0]) & ~np.isnan(s["cast"][:, 0])
    dc = s["cast"][ok, :2] - r["cast"][ok, :2]
    hk = (r["hue"][:, 0] >= HUE_SHARE_MIN) & (s["hue"][:, 0] >= HUE_SHARE_MIN) & ~np.isnan(r["hue"][:, 1] + s["hue"][:, 1])
    ratio = np.full(12, np.nan)
    ratio[hk] = np.sqrt(s["hue"][hk, 1] * s["hue"][hk, 2] / (r["hue"][hk, 1] * r["hue"][hk, 2]))
    wt = np.minimum(r["hue"][hk, 0], s["hue"][hk, 0])
    d = {"tone_dL": float(np.abs(dl[q]).mean()), "key_dL": key,
         "tone_quantiles": {f"{p:g}": [round(float(x), 1), round(float(y), 1)] for p, x, y in zip(Q, r["Lq"], s["Lq"])},
         "cast_dab": float(np.hypot(dc[:, 0], dc[:, 1]).mean()) if ok.any() else 0.0,
         "cast_by_L": {f"{10 * k}-{10 * k + 10}": [round(float(v), 1) for v in (s["cast"][k, :2] - r["cast"][k, :2])]
                       for k in range(10) if ok[k]},
         "chroma_log2": float((np.abs(np.log2(ratio[hk])) * wt).sum() / wt.sum()) if hk.any() else 0.0,
         "chroma_ratio_by_hue": {HUE_NAMES[k]: round(float(ratio[k]), 2) for k in range(12) if hk[k]},
         "chroma_mean": [round(r["chroma_mean"], 1), round(s["chroma_mean"], 1)]}
    d["score"] = round(d["tone_dL"] + d["cast_dab"] + 10 * d["chroma_log2"], 2)
    return d, ratio


def plain(d):
    """Floats of a measures dict rounded to 2 places (for the JSON files)."""
    return {k: (round(v, 2) if isinstance(v, float) else v) for k, v in d.items()}


def sample(items):
    """The pixels a set is measured by, with a weight per pixel: every picture counts with its weight, whatever its
    size; at most PER_PICTURE pixels of a picture (a regular stride)."""
    px, ws = [], []
    for e, a in items:
        v = a.reshape(-1, 3)
        v = v[::max(1, len(v) // PER_PICTURE)]
        px.append(v)
        ws.append(np.full(len(v), float(e.get("weight", 1.0)) / len(v)))
    return np.concatenate(px), np.concatenate(ws)


def pixels(items, lab_of):
    """Lab pixels of a set and their weights."""
    px, w = sample(items)
    return lab_of(px), w


def raw_stats(raws, lab_of, median=None):
    """The statistics of the RAW set rendered by lab_of, and the exposure it was compared at. Unpaired sets say nothing
    about their relative exposure (how the scenes were lit and metered, how the scans were levelled), so with median
    given - the scans' median L* - the RAW set is first exposed (a gain on the RAW, clipped at the sensor's white, as
    the camera would give it) until its rendered median is there. That exposure is a nuisance value of the comparison:
    it is never part of the look. Returns (set_stats, EV)."""
    px, w = sample(raws)

    def at(ev):
        return lab_of(np.minimum(px * 2.0 ** ev, 1.0))
    ev = 0.0
    if median is not None:
        lo, hi = -EV_SPAN, EV_SPAN
        for _ in range(12):
            ev = 0.5 * (lo + hi)
            if wquantile(at(ev)[:, 0], w, [50])[0] < median:
                lo = ev
            else:
                hi = ev
        ev = 0.5 * (lo + hi)
    return set_stats(at(ev), w), ev


def scan_lab(a):
    """sRGB 0..1 pixels of a scan -> CIELAB."""
    return color.lab(a)


def look_lab(look):
    """RAW pixels through a ScanLook -> CIELAB."""
    return lambda raw: look.lab(camera.scene_of_raw(raw))


def tables_srgb(hw, use):
    """RAW pixels through the quantised tables (ispkit.model.run_hardware, 8-bit as the camera) -> display sRGB."""
    return lambda raw: model.run_hardware(camera.table_of_raw(raw), hw, "lin", use)


def box_of(a, box):
    """The pixels of a picture inside a box given in fractions of the frame: x0, y0, x1, y1."""
    x0, y0, x1, y1 = box
    h, w = a.shape[:2]
    return a[int(h * y0):max(int(h * y0) + 1, int(np.ceil(h * y1))), int(w * x0):max(int(w * x0) + 1, int(np.ceil(w * x1)))]


def anchor_means(sel, raws, scans, raw_lab):
    """{class: {"raw": (Lab mean, boxes), "scan": ...}} of the anchors of selection.json; raw_lab renders RAW pixels to
    Lab."""
    by_raw = {e["file"]: a for e, a in raws}
    by_scan = {e["file"]: a for e, a in scans}
    acc = {}
    for an in sel.get("anchors", []):
        src = by_raw if an["set"] == "raw" else by_scan
        if an["file"] not in src:
            continue
        px = box_of(src[an["file"]], an["box"]).reshape(-1, 3)
        lab = raw_lab(px) if an["set"] == "raw" else scan_lab(px)
        # the median of a box: a few pixels of something else in it do not move the anchor
        acc.setdefault(an["class"], {}).setdefault(an["set"], []).append(np.median(lab, axis=0))
    return {c: {k: (np.mean(v, axis=0), len(v)) for k, v in d.items()} for c, d in acc.items()}


def anchor_report(means):
    """The anchor classes compared: Lab of both sets, dL, chroma ratio, hue difference, dE."""
    out = {}
    for c, d in means.items():
        if "raw" not in d or "scan" not in d:
            out[c] = {"note": "marked in one set only - not used", "sets": {k: v[1] for k, v in d.items()}}
            continue
        r, s = d["raw"][0], d["scan"][0]
        cr, cs = np.hypot(*r[1:]), np.hypot(*s[1:])
        dh = (np.degrees(np.arctan2(s[2], s[1]) - np.arctan2(r[2], r[1])) + 180) % 360 - 180
        out[c] = {"render_lab": np.round(r, 1).tolist(), "scan_lab": np.round(s, 1).tolist(),
                  "boxes": [d["raw"][1], d["scan"][1]], "dL": round(float(s[0] - r[0]), 1),
                  "chroma_ratio": round(float(cs / max(cr, 1e-6)), 2), "dhue_deg": round(float(dh), 1),
                  "dE_ab": round(float(np.hypot(s[1] - r[1], s[2] - r[2])), 1),
                  "dE2000": round(float(color.de2000(r[None], s[None])[0]), 1)}
    return out


def base_hue(look, sel, raws, cls):
    """The hue an anchor class is addressed by in the look's hue nodes: of the tone-mapped scene, BEFORE the look's own
    hue operations (the mean direction over the class's RAW boxes)."""
    by = {e["file"]: a for e, a in raws}
    hs = []
    for an in sel.get("anchors", []):
        if an["set"] == "raw" and an["class"] == cls and an["file"] in by:
            lab = np.median(lab_of_linear(look.base(camera.scene_of_raw(box_of(by[an["file"]], an["box"]).reshape(-1, 3)))), axis=0)
            hs.append(np.exp(1j * np.arctan2(lab[2], lab[1])))
    return float(np.degrees(np.angle(np.mean(hs))) % 360)


def picture_stats(lab):
    """One picture's summary for analysis.json."""
    L, a, b = lab.T
    c = np.hypot(a, b)
    h = np.degrees(np.arctan2(b, a)) % 360
    col = c > CHROMA_MIN
    hb = (np.floor((h + 15) / 30).astype(int)) % 12
    return {"L_pct_2_10_50_90_98": np.round(np.percentile(L, [2, 10, 50, 90, 98]), 1).tolist(),
            "mean_a": round(float(a.mean()), 1), "mean_b": round(float(b.mean()), 1),
            "chroma_mean": round(float(c.mean()), 1), "chroma_p95": round(float(np.percentile(c, 95)), 1),
            "white_share": round(float((L > 98).mean()), 3), "black_share": round(float((L < 3).mean()), 3),
            "coloured_share": round(float(col.mean()), 3),
            "hue_share": [round(float((col & (hb == k)).mean()), 3) for k in range(12)]}


def auto_tags(st):
    """First tags of a picture from its lightness: low_key / mid_key / high_key, flat, mono (to be replaced by the scene's
    kind)."""
    med = st["L_pct_2_10_50_90_98"][2]
    tags = ["low_key" if med < 35 else "high_key" if med > 65 else "mid_key"]
    if st["L_pct_2_10_50_90_98"][4] - st["L_pct_2_10_50_90_98"][0] < 45:
        tags.append("flat")
    if st["chroma_mean"] < 3:
        tags.append("mono")
    return tags


def border(a):
    """The share of near-black or near-white pixels in the outer 2 % frame of a scan: a film rebate or a scanner mask."""
    h, w = a.shape[:2]
    k = max(1, round(min(h, w) * 0.02))
    edge = np.concatenate([a[:k].reshape(-1, 3), a[-k:].reshape(-1, 3), a[:, :k].reshape(-1, 3), a[:, -k:].reshape(-1, 3)])
    y = edge.mean(axis=1)
    return float(((y < 0.03) | (y > 0.985)).mean())

