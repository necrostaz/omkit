"""ISP neurofit: an .isp profile for the OM-5 Mark II from two UNPAIRED picture sets - the camera's RAW files and scans
of one film stock. Documentation: OMKit/docs/apps/isp_neurofit.md.

The sets show different scenes, so nothing is compared pixel to pixel. The film's look is a small parametric function
(look.ScanLook: a tone curve over the scene's exposure, a colour cast over lightness, saturation, and twelve hue nodes)
that starts as a neutral rendering and is moved until the RAW set rendered through it has the statistics of the scans:
lightness distribution, cast of the near-neutral pixels, chroma by hue, and - where the same kind of object is marked
in both sets (skin, sky, foliage, a grey thing) - the colour of those objects. The look is then fitted to the camera's
tables by the ISP fitter's recipe (ispkit.train.fit_look) and written as an .isp; the RAW set through the quantised
tables is put next to the scans on boards, to be judged by eye.

    run.py analyze   --run NAME --raw DIR... --scans DIR... [--recursive] [--raw-every N]
    run.py cassettes --run NAME --label "FILM 400"
    run.py preset    --run NAME --title "Film 400" --file FILM400 --cassette yellow
    run.py anchors   --run NAME
    run.py init      --run NAME
    run.py estimate  --run NAME [--parts tone,cast,sat,anchors] [--strength 0.7]
    run.py fit       --run NAME [--quick] [--note "..."] [--out FILE.ISP]
    run.py status    --run NAME
    run.py restore   --run NAME --iter N
    run.py pin-grey  --run NAME
    run.py check-raw --dir DIR...

A run is a folder of the workspace (--workspace DIR, else the environment variable ISP_NEUROFIT_WORKSPACE, else
./isp_neurofit_runs in the current folder; never inside the OMKit checkout): analysis.json, contact sheets,
selection.json (which pictures are used, their tags, weights, scan crops, and the anchors - edited by hand or by an
agent), look.json (the look's parameters - edited by estimate and by hand), iter_NN/ (look, tables, .isp, boards,
metrics), history.json. Needs numpy, Pillow, rawpy."""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import warnings
from pathlib import Path

import numpy as np

from ispkit import camera, cani, canister, color, ispfile, picture, train

from . import look as lk
from . import pictures as pic
from . import stats as st
from .look import CAST_L, HUE_DEG, NEUTRAL_C, TONE_EV, HUE_NAMES, ScanLook, natural_look, pin_grey
from .stats import Q

APPS = Path(__file__).resolve().parents[1]          # OMKit/apps
OMKIT = APPS.parent                                  # the OMKit folder
MANIFEST = OMKIT / "resources" / "modules" / "filmlook" / "presets" / "manifest.json"
ENV_WORKSPACE = "ISP_NEUROFIT_WORKSPACE"
DEFAULT_WORKSPACE = "isp_neurofit_runs"
RUN_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
CAMERA_MODEL = picture.CAMERA_MODEL
ANCHOR_DHUE = 35.0             # an anchor pair further apart than this in hue, degrees, or in chroma (a ratio) is taken
ANCHOR_RATIO = 2.0             # for two different things and left alone
PARTS = ("tone", "cast", "sat", "anchors")


class Failure(Exception):
    """A user error: printed as "error: ..." with exit code 2."""


# ------------------------------------------------------------------ the workspace and a run
def checkout_roots() -> list:
    """Folders a workspace must not be in: the OMKit folder and the version-controlled checkout that holds it."""
    roots = [OMKIT]
    for p in (APPS, *APPS.parents):
        if (p / ".git").exists():
            roots.append(p)
            break
    return roots


def workspace(arg=None) -> Path:
    """The workspace folder: --workspace, else $ISP_NEUROFIT_WORKSPACE, else ./isp_neurofit_runs. Runs hold small copies
    of the user's pictures, so a workspace inside the checkout (where they could be committed) is refused."""
    w = Path(arg or os.environ.get(ENV_WORKSPACE) or DEFAULT_WORKSPACE).expanduser().resolve()
    for r in checkout_roots():
        if w == r or r in w.parents:
            raise Failure(f"the workspace {w} is inside the OMKit checkout {r}: runs hold copies of your pictures - choose "
                          f"a folder outside it (--workspace DIR or the environment variable {ENV_WORKSPACE})")
    return w


class Run:
    """A run: the folder <workspace>/<name> and its JSON files."""

    def __init__(self, name: str, ws: Path, create=True):
        if not RUN_NAME.fullmatch(name or ""):
            raise Failure(f"--run {name!r}: a name of letters, digits, '.', '_' and '-' (up to 64), not a path")
        self.ws = ws
        self.dir = ws / name
        if create:
            self.dir.mkdir(parents=True, exist_ok=True)
        elif not self.dir.is_dir():
            raise Failure(f"no run {name!r} in the workspace {ws}")

    def read(self, name, default=None):
        p = self.dir / name
        if not p.exists():
            return default
        try:
            return json.loads(p.read_text(encoding="utf-8"))
        except ValueError as e:
            raise Failure(f"{p}: not valid JSON ({e})") from None

    def write(self, name, obj):
        (self.dir / name).write_text(json.dumps(obj, indent=1, ensure_ascii=False), encoding="utf-8")

    def selection(self):
        sel = self.read("selection.json")
        if sel is None:
            raise Failure("no selection.json in the run: run analyze first")
        return sel

    def look(self):
        p = self.read("look.json")
        if p is None:
            raise Failure("no look.json in the run: run init first")
        bad = lk.check(p)
        if bad:
            raise Failure(f"{self.dir / 'look.json'}: " + "; ".join(bad))
        return p

    def raws(self, sel=None):
        """[(entry, RAW (h, w, 3))] of the pictures in use."""
        sel = sel or self.selection()
        return [(e, pic.load_raw(self.dir, pic.path_of(sel, "raw", e))) for e in sel["raw"] if e.get("use", True)]

    def scans(self, sel=None):
        """[(entry, sRGB 0..1 (h, w, 3), cropped)] of the pictures in use."""
        sel = sel or self.selection()
        return [(e, pic.crop(pic.load_scan(self.dir, pic.path_of(sel, "scans", e)), e.get("crop_lrtb")) / 255.0)
                for e in sel["scans"] if e.get("use", True)]


def raw_model(src: Path) -> str:
    """The camera model an ORF names in its header (the EXIF Model string), "" when none is found."""
    with open(src, "rb") as f:
        return picture.orf_model(f.read(262144))


# ------------------------------------------------------------------ analyze
def cmd_analyze(args):
    ws = workspace(args.workspace)
    run = Run(args.run, ws)
    old = run.read("selection.json") or {}
    anchors = list(old.get("anchors", []))
    # the sets: named now, or those of the run's last analysis
    recursive = bool(args.recursive or (old.get("recursive") and not (args.raw or args.scans)))
    every = args.raw_every or (old.get("raw_every") if not args.raw else None) or 1
    if every < 1:
        raise Failure("--raw-every: 1 or more")
    raw_src = [str(Path(s).resolve()) for s in args.raw or ()] or old.get("raw_sources") or ([old["raw_dir"]] if old.get("raw_dir") else [])
    scan_src = [str(Path(s).resolve()) for s in args.scans or ()] or old.get("scan_sources") or ([old["scan_dir"]] if old.get("scan_dir") else [])
    keep = {}
    for k in ("raw", "scans"):
        for e in old.get(k, []):
            keep[(k, str(pic.path_of(old, k, e)).lower())] = e
    raw_from = old.get("raw_from")
    if args.raw_from:
        # the RAW set of another run: its pictures with their tags, use and weights (to be balanced again to the new
        # scans), its RAW anchors, and its cache of the pictures read
        other_dir = Path(args.raw_from) if Path(args.raw_from).is_absolute() else ws / args.raw_from
        if not (other_dir / "selection.json").exists():
            raise Failure(f"--raw-from: no selection.json in {other_dir}")
        osel = json.loads((other_dir / "selection.json").read_text(encoding="utf-8"))
        raw_src, raw_from, every = [], str(args.raw_from), 1
        for e in osel["raw"]:
            p = pic.path_of(osel, "raw", e)
            raw_src.append(str(p))
            keep.setdefault(("raw", str(p).lower()), dict(e, path=str(p)))
        have = {json.dumps(a, sort_keys=True) for a in anchors}
        anchors += [a for a in osel.get("anchors", []) if a["set"] == "raw" and json.dumps(a, sort_keys=True) not in have]
        (run.dir / "cache" / "raw").mkdir(parents=True, exist_ok=True)
        for c in (other_dir / "cache" / "raw").glob("*.npy"):
            dst = run.dir / "cache" / "raw" / c.name
            if not dst.exists():
                try:
                    os.link(c, dst)
                except OSError:
                    shutil.copyfile(c, dst)
    if not raw_src or not scan_src:
        raise Failure("analyze: name the sets (--raw or --raw-from, and --scans); a run analysed before remembers its own")
    try:
        raw_files, scan_files = pic.collect(raw_src, pic.RAW_EXT, recursive), pic.collect(scan_src, pic.SCAN_EXT, recursive)
    except pic.SetError as e:
        raise Failure(str(e)) from None
    seen = len(raw_files)
    raw_files = raw_files[::every]
    raw_dir, scan_dir = Path(raw_src[0]), Path(scan_src[0])
    nat = ScanLook(natural_look())
    info = {"raw_sources": raw_src if len(raw_src) <= 20 else f"{len(raw_src)} files", "scan_sources": scan_src,
            "recursive": recursive, "raw_from": raw_from, "raw": [], "scans": [], "unreadable": []}
    sel = {"raw_dir": str(raw_dir if raw_dir.is_dir() else raw_dir.parent),
           "scan_dir": str(scan_dir if scan_dir.is_dir() else scan_dir.parent),
           "raw_sources": raw_src, "scan_sources": scan_src, "recursive": recursive, "raw_from": raw_from,
           "raw": [], "scans": [], "anchors": anchors}
    if every > 1:
        sel["raw_every"] = info["raw_every"] = every
    cells = {"raw": [], "scans": []}
    other = sorted({q.name for s in raw_src if Path(s).is_dir() for q in Path(s).iterdir()
                    if q.is_file() and q.suffix.lower() not in pic.RAW_EXT})
    batches = len({p.parent for _, p in scan_files}) > 1
    for name, p in raw_files:
        try:
            raw = pic.load_raw(run.dir, p)
        except ImportError:
            raise
        except Exception as e:     # noqa: BLE001 - whatever the RAW reader says
            info["unreadable"].append({"file": name, "error": str(e)[:200]})
            continue
        x = camera.scene_of_raw(raw).reshape(-1, 3)
        s = st.picture_stats(nat.lab(x))
        y = x @ color.M709[1]
        s.update(file=name, index=len(info["raw"]) + 1, camera=raw_model(p),
                 sensor_clipped_share=round(float((raw.reshape(-1, 3).max(axis=1) >= 0.999).mean()), 3),
                 median_ev=round(float(np.log2(max(np.median(y), 1e-6) / 0.18)), 2))
        info["raw"].append(s)
        e = keep.get(("raw", str(p).lower())) or {"use": True, "weight": 1.0, "tags": st.auto_tags(s)}
        sel["raw"].append(dict(e, file=name, path=str(p)))
        cells["raw"].append(pic.thumb(pic.render(raw, lambda r: nat(camera.scene_of_raw(r))), f"R{s['index']} {p.name}"))
    for name, p in scan_files:
        try:
            a = pic.load_scan(run.dir, p)
            meta = pic.scan_info(p)
        except Exception as e:     # noqa: BLE001 - whatever Pillow says
            info["unreadable"].append({"file": name, "error": str(e)[:200]})
            continue
        s = st.picture_stats(color.lab(a.reshape(-1, 3) / 255.0))
        s.update(file=name, index=len(info["scans"]) + 1, border_share=round(st.border(a / 255.0), 2), **meta)
        if batches:
            s["batch"] = p.parent.name
        info["scans"].append(s)
        e = keep.get(("scans", str(p).lower())) or {"use": True, "weight": 1.0, "tags": st.auto_tags(s),
                                                      "crop_lrtb": [0.03] * 4 if s["border_share"] > 0.3 else [0, 0, 0, 0]}
        e = dict(e, file=name, path=str(p))
        if batches:
            e["batch"] = p.parent.name
        sel["scans"].append(e)
        cells["scans"].append(pic.thumb(a / 255.0, f"S{s['index']} {p.name}"))
    # an anchor names its picture by the key: a key that changed (a second folder brought the same file name) is followed
    renamed = {(k, e["file"]): n["file"] for k in ("raw", "scans") for n in sel[k]
               for e in [keep.get((k, n["path"].lower()))] if e and e.get("file") and e["file"] != n["file"]}
    for an in sel["anchors"]:
        an["file"] = renamed.get(("raw" if an["set"] == "raw" else "scans", an["file"]), an["file"])
    if batches:
        # scans of several folders: a batch with another scanner, laboratory or stock stands out by its mean colour
        info["scan_batches"] = {}
        for b in sorted({x["batch"] for x in info["scans"]}):
            xs = [x for x in info["scans"] if x["batch"] == b]
            info["scan_batches"][b] = {"scans": len(xs), **{k: round(float(np.mean([x[k] for x in xs])), 1)
                                                            for k in ("mean_a", "mean_b", "chroma_mean")},
                                       "L50": round(float(np.mean([x["L_pct_2_10_50_90_98"][2] for x in xs])), 1),
                                       "L2": round(float(np.mean([x["L_pct_2_10_50_90_98"][0] for x in xs])), 1)}
    info["other_files_in_raw_dir"] = other[:50]
    info["sheets"] = {"raw": pic.sheets(cells["raw"], run.dir / "contact_raw"),
                      "scans": pic.sheets(cells["scans"], run.dir / "contact_scan")}
    run.write("selection.json", sel)
    info["sets"] = coverage(run, sel, info["scans"])
    run.write("analysis.json", info)
    s = info["sets"]
    print(f"run {run.dir}")
    print(f"RAW: {len(info['raw'])} readable" + (f" (every {every}th of {seen})" if every > 1 else "")
          + f", scans: {len(info['scans'])} readable, unreadable: {len(info['unreadable'])}")
    print("contact sheets (RAW through the neutral start; scans as they are):",
          ", ".join(str(run.dir / n) for n in info["sheets"]["raw"] + info["sheets"]["scans"]))
    foreign = [x["file"] for x in info["raw"] if x.get("camera") != CAMERA_MODEL]
    if foreign:
        print(f"NOT {CAMERA_MODEL} ({len(foreign)}): {', '.join(foreign[:12])}{' ...' if len(foreign) > 12 else ''} - "
              f"the camera model is not theirs: set use false")
    if raw_from:
        print(f"RAW set of run {raw_from}: its tags, use, weights and RAW anchors are a start - balance use and weight "
              f"again to these scans")
    for b, v in (info.get("scan_batches") or {}).items():
        print(f"scan batch {b}: {v['scans']} scans, mean a* {v['mean_a']:+}, b* {v['mean_b']:+}, chroma {v['chroma_mean']}, "
              f"L* median {v['L50']}, black {v['L2']}")
    print("hints:")
    for hnt in s["hints"]:
        print("  -", hnt)
    print(f"details: {run.dir / 'analysis.json'}; edit {run.dir / 'selection.json'} (use, weight, tags, crop_lrtb, "
          "anchors), then: anchors, init, estimate, fit")


def coverage(run: Run, sel, scans_info=()) -> dict:
    """What the two sets in use hold, where they differ, the noise floor of the comparison, and hints for the user."""
    raws, scans = run.raws(sel), run.scans(sel)
    hints = []
    if not raws or not scans:
        return {"hints": ["one of the sets is empty: nothing to fit"]}
    nat = ScanLook(natural_look())
    s = st.set_stats(*st.pixels(scans, st.scan_lab))
    r, ev = st.raw_stats(raws, st.look_lab(nat), s["Lq"][8])
    d, _ = st.distance(r, s)
    if abs(ev) > 1.0:
        hints.append(f"the RAW set needs {ev:+.1f} EV to reach the scans' median lightness: the sets differ in key (subject, "
                     "exposure, or the scans' levels); the tone curve's shape is still compared, but less reliably")
    # the noise floor: the scans split in two halves at random and compared with each other - what the distance shows for
    # two sets of the SAME film; a fit closer to the scans than this is fitted to the choice of pictures
    floor = None
    if len(scans) >= 6:
        rng = np.random.default_rng(1)
        fl = []
        for _ in range(8):
            o = rng.permutation(len(scans))
            a, b = [scans[i] for i in o[:len(o) // 2]], [scans[i] for i in o[len(o) // 2:]]
            fl.append(st.distance(st.set_stats(*st.pixels(a, st.scan_lab)), st.set_stats(*st.pixels(b, st.scan_lab)))[0])
        floor = {k: round(float(np.median([f[k] for f in fl])), 2) for k in ("tone_dL", "cast_dab", "chroma_log2", "score")}
    for name, items, least in (("RAW", raws, 8), ("scans", scans, 12)):
        if len(items) < least:
            hints.append(f"{name}: only {len(items)} pictures in use - the statistics follow single scenes; {least}+ wanted, "
                         "20-40 is comfortable")
    for k in range(12):
        a, b = r["hue"][k, 0], s["hue"][k, 0]
        if a >= 0.04 and b < st.HUE_SHARE_MIN:
            hints.append(f"hue {HUE_NAMES[k]}: {a:.0%} of the coloured RAW pixels, almost none in the scans - the film's "
                         "rendering of this colour stays unknown (scans with this colour wanted)")
        elif b >= 0.04 and a < st.HUE_SHARE_MIN:
            hints.append(f"hue {HUE_NAMES[k]}: {b:.0%} of the coloured scan pixels, almost none in the RAW set - nothing "
                         "to compare it with (RAW shots with this colour wanted)")
    lr, ls = r["Lq"][8], s["Lq"][8]
    span_r, span_s = r["Lq"][14] - r["Lq"][2], s["Lq"][14] - s["Lq"][2]
    mono = s["chroma_mean"] < 3
    if mono:
        hints.append("the scans are black-and-white: the result is a mono look (tone and toning only)")
    if abs(r["coloured_share"] - s["coloured_share"]) > 0.25 and not mono:
        hints.append(f"coloured pixels: {r['coloured_share']:.0%} of the RAW set, {s['coloured_share']:.0%} of the scans - "
                     "the sets differ in subject (colourful against plain); the saturation estimate is then unreliable")
    info = [e for e in sel["scans"] if e.get("use", True)]
    used = {e["file"] for e in info}
    small = [x["file"] for x in scans_info if x["file"] in used and max(x["width"], x["height"]) < 1000]
    if small:
        hints.append(f"{len(small)} scans are under 1000 px - enough for colour, but check they are not web thumbnails "
                     "with a look of their own")
    if any(any(e.get("crop_lrtb") or [0]) for e in info):
        hints.append("some scans look framed (rebate or scanner mask): crop_lrtb 3 % was set - check on the contact sheet")

    def keys(items, lab_of):
        """The lightness keys present: of each picture's median L* (low under 35, high over 65)."""
        out = set()
        for _, a in items:
            m = float(np.median(lab_of(a.reshape(-1, 3)[::16])[:, 0]))
            out.add("low_key" if m < 35 else "high_key" if m > 65 else "mid_key")
        return out
    keys_r, keys_s = keys(raws, st.look_lab(nat)), keys(scans, st.scan_lab)
    if keys_r != keys_s:
        hints.append(f"lightness keys differ: RAW {sorted(keys_r)}, scans {sorted(keys_s)} - the tone curve is matched by "
                     "lightness distribution, so both sets should hold the same mix of dark, middle and light scenes")
    hints.append("no pair of the same scene is needed, but the same KIND of scenes and light is: the closer the subjects of "
                 "the sets, the more of the difference is the film")
    hints.append("the strongest addition: the same kind of object in both sets, marked as anchors - skin, clear sky, "
                 "foliage, and something grey (asphalt, concrete, a white wall, a grey card)")
    return {"raw_in_use": len(raws), "scans_in_use": len(scans), "mono": bool(mono),
            "median_L": [round(float(lr), 1), round(float(ls), 1)], "L_span_2_98": [round(float(span_r), 1), round(float(span_s), 1)],
            "hue_share_raw": np.round(r["hue"][:, 0], 3).tolist(), "hue_share_scans": np.round(s["hue"][:, 0], 3).tolist(),
            "hue_names": list(HUE_NAMES), "distance_natural_to_scans": st.plain(d), "exposure_to_scans_ev": round(ev, 2),
            "noise_floor": floor, "hints": hints}


# ------------------------------------------------------------------ anchors
def cmd_anchors(args):
    """A sheet of the anchors as marked (to check the boxes by eye) and their colours through the current look."""
    from PIL import ImageDraw
    run = Run(args.run, workspace(args.workspace), create=False)
    sel = run.selection()
    raws, scans = run.raws(sel), run.scans(sel)
    look = ScanLook(run.look() if (run.dir / "look.json").exists() else natural_look())
    cells = []
    for kind, items in (("raw", raws), ("scan", scans)):
        for e, a in items:
            boxes = [an for an in sel.get("anchors", []) if an["set"] == kind and an["file"] == e["file"]]
            if not boxes:
                continue
            rgb = pic.render(a, lambda r: look(camera.scene_of_raw(r))) if kind == "raw" else a
            cell = pic.thumb(rgb, f"{kind} {e['file']}")
            h, w = a.shape[:2]
            s = min(pic.THUMB / w, pic.THUMB / h)
            ox, oy = (pic.THUMB + 8 - round(w * s)) // 2, 18 + (pic.THUMB - round(h * s)) // 2
            dr = ImageDraw.Draw(cell)
            for an in boxes:
                x0, y0, x1, y1 = an["box"]
                dr.rectangle((ox + x0 * w * s, oy + y0 * h * s, ox + x1 * w * s, oy + y1 * h * s), outline=(255, 255, 0))
                dr.text((ox + x0 * w * s + 2, oy + y0 * h * s + 1), an["class"], fill=(255, 255, 0))
            cells.append(cell)
    pic.sheet(cells, run.dir / "anchors.jpg")
    known = {("raw", e["file"]) for e, _ in raws} | {("scan", e["file"]) for e, _ in scans}
    lost = [an for an in sel.get("anchors", []) if (an.get("set"), an.get("file")) not in known]
    rep = st.anchor_report(st.anchor_means(sel, raws, scans, st.look_lab(look)))
    print(f"{len(sel.get('anchors', []))} anchors, sheet: {run.dir / 'anchors.jpg' if cells else 'none'}")
    for an in lost:
        print(f"  not used: {an.get('set')} {an.get('file')!r} is not a picture in use (check set, file and use)")
    print(json.dumps(rep, indent=1))


# ------------------------------------------------------------------ init / estimate
def cmd_init(args):
    run = Run(args.run, workspace(args.workspace))
    if (run.dir / "look.json").exists() and not args.force:
        raise Failure("look.json exists (use --force to start again from the neutral look)")
    run.write("look.json", natural_look())
    print(f"{run.dir / 'look.json'}: the neutral start")


def _smooth_monotone(v, lo=0.0, hi=100.0):
    v = np.asarray(v, float)
    v = np.concatenate([[v[0]], (v[:-2] + 2 * v[1:-1] + v[2:]) / 4, [v[-1]]])
    return np.clip(np.maximum.accumulate(v), lo, hi)


def estimate(run, p: dict, parts, strength: float, log=print, free_exposure=False) -> dict:
    """One step of the look towards the scans' statistics; every part works on what the earlier ones left. run: what
    gives selection(), raws() and scans() (a Run)."""
    sel = run.selection()
    raws, scans = run.raws(sel), run.scans(sel)
    s = st.set_stats(*st.pixels(scans, st.scan_lab))
    p = json.loads(json.dumps(p))

    median = None if free_exposure else s["Lq"][8]

    def rendered():
        return st.raw_stats(raws, st.look_lab(ScanLook(p)), median)[0]

    if "tone" in parts:
        r = rendered()
        lq = np.maximum.accumulate(r["Lq"] + np.arange(len(Q)) * 1e-6)
        t = np.array(p["tone_L"])
        # the rendered set's lightness quantiles -> the scans' (the set exposed to the scans' median first, unless the
        # exposure is free): a correction of the display L*, read at every node of the tone curve - its shape (contrast,
        # toe, shoulder, black and white levels); outside the lightness the RAW set reaches, the end corrections continue
        new = _smooth_monotone(t + strength * (np.interp(t, lq, s["Lq"]) - t))
        p["tone_L"] = np.round(new, 3).tolist()
        log(f"tone: black {t[0]:.1f} -> {p['tone_L'][0]:.1f}, grey {np.interp(0, TONE_EV, t):.1f} -> "
            f"{np.interp(0, TONE_EV, p['tone_L']):.1f}, white {t[-1]:.1f} -> {p['tone_L'][-1]:.1f}")
    if "cast" in parts:
        r = rendered()
        ok = ~np.isnan(r["cast"][:, 0]) & ~np.isnan(s["cast"][:, 0])
        if ok.sum() >= 2:
            centres = np.arange(10) * 10 + 5.0
            for i, key in enumerate(("cast_a", "cast_b")):
                d = np.interp(CAST_L, centres[ok], (s["cast"][:, i] - r["cast"][:, i])[ok])
                p[key] = np.round(np.array(p[key]) + strength * d, 2).tolist()
            log(f"cast a {p['cast_a']}, b {p['cast_b']}")
        else:
            log("cast: too few near-neutral pixels in common lightness bins - left")
    if "sat" in parts:
        r = rendered()
        if s["chroma_mean"] < 3:
            p["sat"] = 0.0
            log("sat: the scans are black-and-white - sat 0")
        else:
            _, ratio = st.distance(r, s)
            hk = ~np.isnan(ratio)
            if hk.any():
                wt = np.minimum(r["hue"][hk, 0], s["hue"][hk, 0])
                g = float(np.exp((np.log(ratio[hk]) * wt).sum() / wt.sum()))
                p["sat"] = round(float(np.clip(p["sat"] * g ** strength, 0.2, 2.0)), 3)
                rel = np.ones(12)
                rel[hk] = np.clip(ratio[hk] / g, 0.6, 1.6)
                # the bins compared pull their two neighbours by half: twelve separate gains from histograms are noisy
                rel = (np.roll(rel, 1) + 2 * rel + np.roll(rel, -1)) / 4
                p["hue_sat"] = np.round(np.clip(np.array(p["hue_sat"]) * rel ** strength, 0.4, 1.8), 3).tolist()
                log(f"sat x{g ** strength:.2f} -> {p['sat']}; hue_sat {p['hue_sat']}")
            else:
                log("sat: no hue bin filled in both sets - left")
    if "anchors" in parts:
        look = ScanLook(p)
        means = st.anchor_means(sel, raws, scans, st.look_lab(look))
        for c, d in means.items():
            if "raw" not in d or "scan" not in d:
                continue
            rl, sl = d["raw"][0], d["scan"][0]
            if c == "neutral":
                wl = np.clip(1 - np.abs(CAST_L - rl[0]) / 25.0, 0, 1)
                p["cast_a"] = np.round(np.array(p["cast_a"]) + strength * wl * (sl[1] - rl[1]), 2).tolist()
                p["cast_b"] = np.round(np.array(p["cast_b"]) + strength * wl * (sl[2] - rl[2]), 2).tolist()
                log(f"anchor neutral at L {rl[0]:.0f}: cast {sl[1] - rl[1]:+.1f}, {sl[2] - rl[2]:+.1f}")
                continue
            cr, cs = np.hypot(*rl[1:]), np.hypot(*sl[1:])
            if cr < NEUTRAL_C / 2:
                log(f"anchor {c}: the rendered colour is near neutral (C {cr:.1f}) - skipped")
                continue
            # the hue the nodes are addressed by is the hue BEFORE the look's hue operations: of the tone-mapped scene
            h0 = st.base_hue(look, sel, raws, c)
            dh = (np.degrees(np.arctan2(sl[2], sl[1]) - np.arctan2(rl[2], rl[1])) + 180) % 360 - 180
            if abs(dh) > ANCHOR_DHUE or not 1 / ANCHOR_RATIO <= cs / cr <= ANCHOR_RATIO:
                log(f"anchor {c}: hue {dh:+.0f} deg, chroma x{cs / cr:.2f} - too far apart to be the same kind of object "
                    "in the same kind of light; NOT applied (check the boxes, or set the nodes by hand if it is the film)")
                continue
            wt = np.clip(1 - np.abs((HUE_DEG - h0 + 180) % 360 - 180) / 30.0, 0, 1)
            p["hue_shift"] = np.round(np.clip(np.array(p["hue_shift"]) + strength * wt * dh, -40, 40), 2).tolist()
            p["hue_sat"] = np.round(np.clip(np.array(p["hue_sat"]) * (cs / cr) ** (strength * wt), 0.4, 1.8), 3).tolist()
            log(f"anchor {c} at hue {h0:.0f}: hue {dh:+.1f} deg, chroma x{cs / cr:.2f} (dL {sl[0] - rl[0]:+.1f} not applied: "
                "lightness is the tone curve's and the exposure's)")
    return p


def cmd_estimate(args):
    run = Run(args.run, workspace(args.workspace), create=False)
    parts = [x for x in args.parts.split(",") if x]
    bad = set(parts) - set(PARTS)
    if bad:
        raise Failure(f"unknown parts: {sorted(bad)} (known: {', '.join(PARTS)})")
    p = run.look()
    for n in range(args.rounds):
        print(f"round {n + 1}")
        p = estimate(run, p, parts, args.strength, free_exposure=args.exposure == "free")
    if args.exposure != "free" and not args.no_pin and "tone" in parts:
        p = pin_grey(p)
    p.setdefault("notes", []).append(f"estimate {','.join(parts)} x{args.rounds} strength {args.strength}")
    run.write("look.json", p)
    sel = run.selection()
    s = st.set_stats(*st.pixels(run.scans(sel), st.scan_lab))
    r, ev = st.raw_stats(run.raws(sel), st.look_lab(ScanLook(p)), None if args.exposure == "free" else s["Lq"][8])
    d, _ = st.distance(r, s)
    print(f"look {ScanLook(p).key()}: RAW through the look (exposed {ev:+.2f} EV to the scans' median) against the scans - "
          f"tone dL {d['tone_dL']:.2f}, cast {d['cast_dab']:.2f}, chroma log2 {d['chroma_log2']:.3f}, score {d['score']}")


def cmd_pin_grey(args):
    run = Run(args.run, workspace(args.workspace), create=False)
    p = pin_grey(run.look())
    p.setdefault("notes", []).append("pin-grey")
    run.write("look.json", p)


# ------------------------------------------------------------------ the preset: its names and its cassette picture
def short_of(title: str, file: str | None = None) -> str:
    short = (file or "".join(ch for ch in title.upper() if ch.isascii() and ch.isalnum())[:8] or "LOOK").upper()
    if len(short) > 8 or not short.isascii() or not short.isalnum():
        raise Failure("--file: an 8.3 name, up to 8 ASCII letters and digits")
    return short


def cassette_index(s) -> int:
    """A generic cassette by its id (cani.GENERICS: yellow, p_portra...) or its number."""
    ids = [k for k, _ in cani.GENERICS]
    s = str(s).strip()
    if s in ids:
        return ids.index(s)
    if s.isdigit() and int(s) < len(ids):
        return int(s)
    raise Failure(f"no cassette {s!r}: one of {', '.join(ids)} or 0..{len(ids) - 1}")


def cassette_of(ps: dict) -> bytes | None:
    """The CANI section of a run's preset (preset.json), or None when no cassette is chosen: the camera and the player
    then draw a generic one by the profile's name."""
    if ps.get("cassette") in (None, ""):
        return None
    blob = cani.generic(cassette_index(ps["cassette"]), ps.get("label") or "", ps.get("iso") or "",
                        ps.get("brand"), ps.get("proc"))
    cani.parse(blob)
    return blob


def bundled_presets() -> list:
    """[(file stem, name)] of the kit's bundled FILMLOOK presets (resources/modules/filmlook/presets/manifest.json); []
    when the file is not there."""
    try:
        m = json.loads(MANIFEST.read_text(encoding="utf-8"))
        return [(Path(x["file"]).stem.upper(), x.get("name", "")) for x in m.get("presets", [])]
    except (OSError, ValueError, KeyError, TypeError):
        return []


def taken_names(run: Run) -> dict:
    """8.3 names a new preset should not take: the bundled FILMLOOK presets and the presets of the workspace's other
    runs."""
    out = {stem: f"the bundled FILMLOOK preset {name!r}" for stem, name in bundled_presets()}
    for p in run.ws.glob("*/preset.json"):
        if p.parent.resolve() != run.dir.resolve():
            try:
                out.setdefault(json.loads(p.read_text(encoding="utf-8"))["file"], f"the preset of run {p.parent.name}")
            except (ValueError, KeyError, OSError):
                pass
    return out


def cmd_cassettes(args):
    """A sheet of generic cassettes with the label, ISO, brand and process line as they would be drawn, to choose from."""
    run = Run(args.run, workspace(args.workspace))
    ids = [k for k, _ in cani.GENERICS]
    if args.ids == "all":
        pick = list(range(len(ids)))
    elif args.ids:
        pick = [cassette_index(s) for s in args.ids.split(",")]
    else:
        pick = list(dict.fromkeys([cani.default_generic(args.label)] + list(range(6))))
    iso = cani.guess_iso(args.label) if args.iso is None else args.iso
    items = [(cani.generic(i, args.label, iso, args.brand, args.proc), f"{i} {ids[i]}") for i in pick]
    out = run.dir / "cassettes.png"
    out.write_bytes(canister.sheet_png(items, cols=min(4, len(items)), title=f"{args.label}  ISO {iso}", scale=2))
    shown = canister.fit_text(args.label, canister.NAME_W)[1]
    if shown != args.label:
        print(f"the label does not fit the cassette: drawn as {shown!r}")
    for i in pick:
        print(f"  {i:>2} {ids[i]:<14} {cani.GENERICS[i][1]}")
    print(f"sheet: {out}")


def cmd_preset(args):
    """The names and the cassette of the run's preset (preset.json): fit writes every .isp with them."""
    run = Run(args.run, workspace(args.workspace))
    ps = run.read("preset.json") or {}
    for k in ("title", "file", "cassette", "label", "iso", "brand", "proc"):
        v = getattr(args, k)
        if v is not None:
            ps[k] = v
    if not ps.get("title"):
        raise Failure("preset: --title (the profile's name the camera shows)")
    if len(ps["title"].encode("utf-8")) > 31:
        raise Failure("--title: longer than 31 bytes of UTF-8")
    ps["file"] = short_of(ps["title"], ps.get("file"))
    taken = taken_names(run)
    if ps["file"] in taken and not args.force:
        raise Failure(f"--file {ps['file']}: the name of {taken[ps['file']]}; choose another (or --force)")
    same = [n for _, n in bundled_presets() if n and n.lower() == ps["title"].lower()]
    if same:
        print(f"note: the title {ps['title']!r} is the name of a bundled FILMLOOK preset - a distinct one reads better")
    if ps.get("cassette") not in (None, ""):
        ps["cassette"] = cani.GENERICS[cassette_index(ps["cassette"])][0]
        ps.setdefault("label", "".join(c if 32 <= ord(c) < 127 else "?" for c in ps["title"]).upper())
        ps.setdefault("iso", cani.guess_iso(ps["label"]))
        shown = canister.fit_text(ps["label"], canister.NAME_W)[1]
        if shown != ps["label"]:
            print(f"the label does not fit the cassette: drawn as {shown!r}")
        (run.dir / "cassette.png").write_bytes(canister.render_png(cassette_of(ps)))
    run.write("preset.json", ps)
    print(f"preset of run {run.dir.name}: title {ps['title']!r}, file {ps['file']}.ISP, cassette "
          + (f"{ps['cassette']} (label {ps['label']!r}, ISO {ps['iso']!r}): {run.dir / 'cassette.png'}"
             if ps.get("cassette") else "not chosen (a generic one by the name)"))


# ------------------------------------------------------------------ fit
def cmd_fit(args):
    run = Run(args.run, workspace(args.workspace), create=False)
    sel = run.selection()
    p = run.look()
    ps = run.read("preset.json") or {}
    title = args.title or ps.get("title")
    if not title:
        raise Failure("fit: --title, or set the run's preset first (preset --title ...)")
    if len(title.encode("utf-8")) > 31:
        raise Failure("--title: longer than 31 bytes of UTF-8")
    short = short_of(title, args.file or (ps.get("file") if not args.title else None))
    look = ScanLook(p, name=short)
    cassette = cassette_of(ps)
    hist = run.read("history.json", [])
    n = len(hist) + 1
    d = run.dir / f"iter_{n:02d}"
    d.mkdir(exist_ok=True)
    (d / "look.json").write_text(json.dumps(p, indent=1), encoding="utf-8")
    recipe = args.recipe
    fit_info = {}
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        hw = train.fit_look(look, recipe, "lin", args.quick, log=lambda *_: None, info=fit_info)
    use = train.use_of(recipe)
    meta = {"fitter": train.version(), "variant": recipe, "matrix": train.MATRIX, "domain": "lin",
            "quick": int(bool(args.quick)), "neurofit": look.key()}
    if cassette:
        meta["art"] = f"generic:{cassette_index(ps['cassette'])}"
    data = ispfile.write(title, "OM5M2", ispfile.ispb_om5m2(hw), meta=meta,
                         extra=[(b"CANI", cassette)] if cassette else ())
    ispfile.read(data)
    (d / f"{short}.ISP").write_bytes(data)
    (d / "hardware.json").write_text(json.dumps({**meta, **train.hardware_dict(hw)}), encoding="utf-8")

    raws, scans = run.raws(sel), run.scans(sel)
    isp = st.tables_srgb(hw, use)
    isp_lab = lambda raw: color.lab(isp(raw))      # noqa: E731
    s = st.set_stats(*st.pixels(scans, st.scan_lab))
    median = None if args.exposure == "free" else s["Lq"][8]
    r, ev = st.raw_stats(raws, isp_lab, median)
    dist, _ = st.distance(r, s)
    dlook, _ = st.distance(st.raw_stats(raws, st.look_lab(look), median)[0], s)
    # what the tables lose of the look, on the RAW pixels themselves
    de = np.concatenate([color.de2000(look.lab(camera.scene_of_raw(a.reshape(-1, 3))), isp_lab(a.reshape(-1, 3)))
                         for _, a in raws])
    anchors = st.anchor_report(st.anchor_means(sel, raws, scans, isp_lab))
    both = [v["dE_ab"] for v in anchors.values() if "dE_ab" in v]
    an = run.read("analysis.json") or {}
    floor = (an.get("sets") or {}).get("noise_floor")
    m = {"iter": n, "look": look.key(), "recipe": recipe, "quick": bool(args.quick), "file": f"{d.name}/{short}.ISP",
         "compared_at_ev": round(ev, 2), "isp_vs_scans": st.plain(dist), "look_vs_scans_score": dlook["score"],
         "tables_vs_look_dE2000": {"mean": round(float(de.mean()), 2), "p95": round(float(np.percentile(de, 95)), 2)},
         "anchors": anchors, "anchors_dE_ab_mean": round(float(np.mean(both)), 2) if both else None,
         "noise_floor": floor, "mono": fit_info.get("mono", ""), "note": args.note or ""}
    (d / "metrics.json").write_text(json.dumps(m, indent=1), encoding="utf-8")

    # boards for the eye: the RAW set through the tables above, the scans below, both ordered by the first tag. The RAW
    # pictures are shown at the exposure the sets are compared at (compared_at_ev): otherwise the eye judges the sets'
    # difference in key, which is not the look. stages.jpg shows the pictures as shot
    from PIL import Image
    gain = 2.0 ** ev
    isp_at = lambda raw: isp(np.minimum(raw * gain, 1.0))      # noqa: E731
    nat = ScanLook(natural_look())
    ro, so = pic.order(raws), pic.order(scans)
    cells_r = [pic.thumb(pic.render(raws[i][1], isp_at), f"R{ev:+.1f}EV {(raws[i][0].get('tags') or [''])[0]} {raws[i][0]['file']}")
               for i in ro]
    cells_s = [pic.thumb(scans[i][1], f"S {(scans[i][0].get('tags') or [''])[0]} {scans[i][0]['file']}") for i in so]
    boards = []
    half = pic.PER_ROW * 2
    for k in range(max(1, -(-max(len(cells_r), len(cells_s)) // half))):
        if k >= args.boards:
            break
        a, b = cells_r[k * half:(k + 1) * half], cells_s[k * half:(k + 1) * half]
        # a set that ran out is started again, so that every board holds both
        a = a or cells_r[:half]
        b = b or cells_s[:half]
        a = a + [Image.new("RGB", a[0].size, (32, 32, 32))] * ((-len(a)) % pic.PER_ROW)
        name = f"board_{k + 1:02d}.jpg"
        pic.sheet(a + b, d / name)
        boards.append(name)
    trio = []
    for i in ro[:: max(1, len(ro) // 6)][:6]:
        e, a = raws[i]
        trio += [pic.thumb(pic.render(a, lambda r: nat(camera.scene_of_raw(r))), f"neutral {e['file']}"),
                 pic.thumb(pic.render(a, lambda r: look(camera.scene_of_raw(r))), "look (target)"),
                 pic.thumb(pic.render(a, isp), "ISP tables (.isp)")]
    pic.sheet(trio, d / "stages.jpg", per_row=3)
    hist.append({"iter": n, "look": look.key(), "score": dist["score"], "tone_dL": round(dist["tone_dL"], 2),
                 "cast_dab": round(dist["cast_dab"], 2), "chroma_log2": round(dist["chroma_log2"], 3),
                 "anchors_dE_ab": m["anchors_dE_ab_mean"], "tables_dE": m["tables_vs_look_dE2000"]["mean"],
                 "quick": bool(args.quick), "file": m["file"], "note": args.note or ""})
    run.write("history.json", hist)
    if args.out:
        out = Path(args.out).expanduser().resolve()
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(d / f"{short}.ISP", out)
    print(f"iter {n}: {d / (short + '.ISP')} ({len(data)} bytes, recipe {recipe}{', quick' if args.quick else ''}, "
          f"title {title!r}, cassette {ps.get('cassette') if cassette else 'none'}"
          f"{', black-and-white ' + fit_info['mono'] if fit_info.get('mono') else ''})")
    print(f"  ISP against the scans (RAW set exposed {ev:+.2f} EV to the scans' median): tone dL {dist['tone_dL']:.2f}, "
          f"cast {dist['cast_dab']:.2f}, chroma log2 {dist['chroma_log2']:.3f}, score {dist['score']} (the look itself "
          f"{dlook['score']}; noise floor {floor['score'] if floor else 'n/a'})")
    print(f"  tables against the look, dE2000: mean {m['tables_vs_look_dE2000']['mean']}, p95 {m['tables_vs_look_dE2000']['p95']}")
    for c, v in anchors.items():
        if "dE_ab" in v:
            print(f"  anchor {c}: dE_ab {v['dE_ab']}, dL {v['dL']}, chroma x{v['chroma_ratio']}, hue {v['dhue_deg']:+} deg")
    print(f"  look at: {', '.join(str(d / b) for b in boards)} (RAW at {ev:+.2f} EV), {d / 'stages.jpg'} (as shot); "
          f"numbers: {d / 'metrics.json'}")
    if args.out:
        print(f"  copied to {out}")


# ------------------------------------------------------------------ the rest
def cmd_check_raw(args):
    """The camera model of every RAW file under the folders: is it the camera the colour model is of?"""
    try:
        files = pic.collect(args.dir, pic.RAW_EXT, recursive=True)
    except pic.SetError as e:
        raise Failure(str(e)) from None
    bad = 0
    for name, p in files:
        m = raw_model(p)
        ok = m == CAMERA_MODEL
        bad += not ok
        if not ok or args.all:
            print(f"{'ok ' if ok else 'NOT'} {m or '?':<12} {p}")
    print(f"{len(files)} RAW files, {len(files) - bad} of {CAMERA_MODEL}, {bad} of another camera or unreadable")


def cmd_doctor(args):
    """What the tool runs with: Python, the packages, the workspace."""
    import importlib
    import platform
    todo = []
    print(f"Python {platform.python_version()} ({sys.executable})")
    print(f"OMKit apps: {APPS}")
    for mod, need in (("numpy", True), ("PIL", True), ("rawpy", True), ("torch", False)):
        try:
            m = importlib.import_module(mod)
            extra = ""
            if mod == "torch":
                extra = ", CUDA" if m.cuda.is_available() else ", no CUDA: the fit runs on the CPU"
            print(f"  {mod}: {getattr(m, '__version__', '?')}{extra}")
        except ImportError:
            if need and not todo:
                todo.append(f"python -m pip install -r \"{APPS / 'requirements.txt'}\"")
            print(f"  {mod}: missing" + (" (needed)" if need else " (optional: a GPU makes the full fit faster)"))
    print(f"fitter version: {train.version()}")
    try:
        ws = workspace(args.workspace)
        runs = sorted(p.parent.name for p in ws.glob("*/selection.json")) if ws.is_dir() else []
        print(f"workspace: {ws}{'' if ws.is_dir() else ' (created by the first run)'}; runs: {', '.join(runs) or 'none'}")
    except Failure as e:
        todo.append("choose a workspace outside the checkout")
        print(f"workspace: {e}")
    if todo:
        raise Failure("not ready: " + "; ".join(todo))
    print("ready")


def cmd_status(args):
    run = Run(args.run, workspace(args.workspace), create=False)
    hist = run.read("history.json", [])
    an = run.read("analysis.json") or {}
    floor = (an.get("sets") or {}).get("noise_floor")
    print(f"run {run.dir}")
    print("iter  look          score  tone  cast  chroma  anchors  tables  note")
    for h in hist:
        print(f"{h['iter']:>4}  {h['look']}  {h['score']:>5}  {h['tone_dL']:>4}  {h['cast_dab']:>4}  {h['chroma_log2']:>6}  "
              f"{str(h['anchors_dE_ab']):>7}  {h['tables_dE']:>6}  {'quick ' if h['quick'] else ''}{h['note']}")
    print(f"noise floor (scans against scans): {floor}")


def cmd_restore(args):
    run = Run(args.run, workspace(args.workspace), create=False)
    src = run.dir / f"iter_{args.iter:02d}" / "look.json"
    if not src.exists():
        raise Failure(f"no {src}")
    shutil.copyfile(src, run.dir / "look.json")
    print(f"look.json restored from iter {args.iter}")


def parser():
    ap = argparse.ArgumentParser(prog="isp_neurofit", description=__doc__.split("\n\n")[0],
                                 epilog="Documentation: OMKit/docs/apps/isp_neurofit.md")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def cmd(name, fn, run=True, **kw):
        sp = sub.add_parser(name, **kw)
        if run:
            sp.add_argument("--run", required=True, help="the run's name: a folder of the workspace")
            sp.add_argument("--workspace", help=f"the folder that holds the runs (default: ${ENV_WORKSPACE}, else "
                                                f"./{DEFAULT_WORKSPACE}); must be outside the OMKit checkout")
        sp.set_defaults(fn=fn)
        return sp
    a = cmd("analyze", cmd_analyze, help="read both sets: analysis.json, contact sheets, a first selection.json")
    a.add_argument("--raw", nargs="+", help="the camera's RAW files (ORF): folders and single files; left out - the "
                                            "run's own of its last analysis")
    a.add_argument("--raw-from", help="take the RAW set of another run of the workspace (or a run folder) instead: its "
                                      "pictures with their tags, use, weights, RAW anchors and cache")
    a.add_argument("--scans", nargs="+", help="film scans (JPEG, PNG, TIFF, WebP): folders and single files; left out - "
                                              "the run's own of its last analysis")
    a.add_argument("--recursive", action="store_true", help="read the subfolders of the named folders as well")
    a.add_argument("--raw-every", type=int, help="take every N-th RAW file only (large folders: about 1 s per RAW)")
    c = cmd("cassettes", cmd_cassettes, help="cassettes.png: generic cassettes with the preset's label, to choose from")
    c.add_argument("--label", required=True, help="the name drawn on the cassette (ASCII, upper case reads best)")
    c.add_argument("--iso", help="the ISO window (default: the number in the label)")
    c.add_argument("--brand", help="row A instead of the generic's own line (e.g. a film maker)")
    c.add_argument("--proc", help="row C instead of the generic's own line (e.g. C-41)")
    c.add_argument("--ids", help="which cassettes: ids or numbers by comma, or all (default: the one guessed from the "
                                 "label and the six plain ones)")
    ps = cmd("preset", cmd_preset, help="preset.json: the profile's name, file name and cassette every fit writes")
    ps.add_argument("--title", help="the profile's name shown by the camera (up to 31 bytes of UTF-8)")
    ps.add_argument("--file", help="8.3 file name without the extension (default: from the title)")
    ps.add_argument("--cassette", help="a generic cassette: id or number (see cassettes)")
    ps.add_argument("--label", help="the name on the cassette (default: the title, ASCII upper case)")
    ps.add_argument("--iso", help="the ISO window (default: the number in the label)")
    ps.add_argument("--brand", help="row A of the cassette")
    ps.add_argument("--proc", help="row C of the cassette")
    ps.add_argument("--force", action="store_true", help="take a file name another preset has")
    cmd("anchors", cmd_anchors, help="anchors.jpg: the marked anchors, and their colours in both sets")
    i = cmd("init", cmd_init, help="look.json = the neutral start")
    i.add_argument("--force", action="store_true", help="overwrite an existing look.json")
    e = cmd("estimate", cmd_estimate, help="move look.json towards the scans' statistics")
    e.add_argument("--parts", default="tone,cast,sat,anchors", help="what to estimate, by comma: tone, cast, sat, anchors")
    e.add_argument("--strength", type=float, default=0.7, help="the share of each step taken (0..1)")
    e.add_argument("--rounds", type=int, default=3, help="how many steps")
    e.add_argument("--no-pin", action="store_true", help="leave the tone curve where the estimate put it on the "
                                                         "exposure axis (default: the scene's grey is pinned to "
                                                         "the neutral start's)")
    e.add_argument("--exposure", choices=("keep", "free"), default="keep",
                   help="keep: the RAW set is exposed to the scans' median lightness before it is compared, so only the "
                        "curve's shape follows the scans (default); free: the sets are trusted to be exposed alike, and "
                        "the difference of their lightness goes into the look")
    f = cmd("fit", cmd_fit, help="fit look.json to the camera tables: a new iteration with an .isp, boards and metrics")
    f.add_argument("--title", help="the profile's name shown by the camera (default: the run's preset.json)")
    f.add_argument("--file", help="8.3 file name without the extension (default: preset.json, else from the title)")
    f.add_argument("--recipe", default=train.DEFAULT, choices=list(train.RECIPES), help="the fitter's recipe")
    f.add_argument("--quick", action="store_true", help="a short fit for the iterations in between")
    f.add_argument("--boards", type=int, default=2, help="how many boards to draw at most")
    f.add_argument("--note", help="what this iteration changes (kept in history.json)")
    f.add_argument("--out", help="copy the .isp here as well")
    f.add_argument("--exposure", choices=("keep", "free"), default="keep", help="as in estimate: how the sets are compared")
    cmd("pin-grey", cmd_pin_grey, help="slide look.json's tone curve so that the scene's grey stays the neutral grey")
    k = cmd("check-raw", cmd_check_raw, run=False, help=f"which RAW files under the folders are of the {CAMERA_MODEL}")
    k.add_argument("--dir", nargs="+", required=True, help="folders (read with their subfolders) and files")
    k.add_argument("--all", action="store_true", help="list the matching files as well")
    dr = cmd("doctor", cmd_doctor, run=False, help="check Python, the packages and the workspace")
    dr.add_argument("--workspace", help="the workspace to check (default as for the other commands)")
    cmd("status", cmd_status, help="the iterations so far")
    r = cmd("restore", cmd_restore, help="look.json back from an iteration")
    r.add_argument("--iter", type=int, required=True)
    return ap


def main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="backslashreplace")
        except (AttributeError, ValueError):
            pass
    args = parser().parse_args(argv)
    try:
        args.fn(args)
    except Failure as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    except ImportError as e:
        if "rawpy" in str(e):
            print("error: reading camera RAW files needs the rawpy package: python -m pip install rawpy", file=sys.stderr)
            return 2
        raise
    return 0
