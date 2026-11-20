"""The photo colour prior of the fitter's training set: which colours real photographs hold, and how often.

A look is fitted on the colours the camera will meet. Besides a synthetic set (the neutral axis, ColorChecker, the RGB
cube), the training set holds colours taken from photographs: their pixels binned in encoded sRGB into bins^3 cells; a
cell gives one training point - the mean LINEAR colour of its pixels - with the weight count^0.5 (gamma below), the
weights normalised to sum to 1. A pixel counts with its picture's weight (1 unless given). The fitter scales the prior's
weights by train.PRIOR_WEIGHT.

The prior holds colour statistics only - cell mean colours and weights - not pictures. It is kept in an .npz file
(numpy's zip of arrays, read with allow_pickle=False):

    x24, w24    float64 (M, 3) and (M,): the prior of 24^3 cells (the normal fit)
    x16, w16    the same for 16^3 cells (the quick fit)
    info        a JSON text: format, bins, gamma, how many pictures and pixels, a free description

The bundled prior (data/prior.npz) is the one the fitter was developed with; build() makes one from a folder of the
user's own pictures (isp_fitter/build_prior.py). Pictures are first brought to sRGB (an ICC profile converts them),
turned by their EXIF orientation and shrunk to LONG_SIDE pixels on the long side (box filter)."""
from __future__ import annotations

import hashlib
import io
import json
import math
from pathlib import Path

import numpy as np

from . import color

FORMAT = 1
DEFAULT = Path(__file__).resolve().parent / "data" / "prior.npz"
BINS = (24, 16)                   # the normal fit, the quick fit
GAMMA = 0.5                       # a cell's weight: (its pixel count) ** GAMMA
LONG_SIDE = 240                   # pictures are shrunk to this many pixels on the long side
IMAGE_EXT = (".jpg", ".jpeg", ".png", ".tif", ".tiff")


class Prior:
    """A loaded prior: cells(bins) -> (x linear BT.709 (M, 3), w (M,) summing to 1); info; digest (a hash of the arrays,
    part of the fit cache's key)."""

    def __init__(self, arrays: dict, info: dict, source: str):
        self.arrays, self.info, self.source = arrays, info, source
        h = hashlib.sha256()
        for k in sorted(arrays):
            h.update(k.encode() + np.ascontiguousarray(arrays[k], np.float64).tobytes())
        self.digest = h.hexdigest()[:12]

    def cells(self, bins: int):
        if f"x{bins}" not in self.arrays:
            raise KeyError(f"the prior has no {bins}^3 cells (it has {', '.join(sorted(self.arrays))})")
        return self.arrays[f"x{bins}"], self.arrays[f"w{bins}"]


_LOADED: dict = {}


def load(path=None) -> Prior:
    """The prior in an .npz file (default: the bundled one), checked; kept once read."""
    path = Path(path or DEFAULT).resolve()
    if path in _LOADED:
        return _LOADED[path]
    try:
        with np.load(path, allow_pickle=False) as z:
            files = dict(z)
    except (OSError, ValueError) as e:
        raise ValueError(f"cannot read the prior {path.name}: {e}") from None
    try:
        info = json.loads(str(files.pop("info")))
    except (KeyError, ValueError):
        raise ValueError(f"{path.name}: not a prior file (no info)") from None
    if info.get("format") != FORMAT:
        raise ValueError(f"{path.name}: prior format {info.get('format')}, this fitter reads {FORMAT}")
    arrays = {}
    for b in BINS:
        x, w = files.get(f"x{b}"), files.get(f"w{b}")
        if x is None or w is None:
            raise ValueError(f"{path.name}: no x{b} / w{b} arrays")
        if x.dtype != np.float64 or w.dtype != np.float64 or x.ndim != 2 or x.shape[1:] != (3,) or w.shape != x.shape[:1]:
            raise ValueError(f"{path.name}: x{b} / w{b} are not float64 (M, 3) and (M,)")
        if not (np.all(np.isfinite(x)) and np.all(np.isfinite(w)) and np.all(w >= 0)):
            raise ValueError(f"{path.name}: x{b} / w{b} hold values that are not finite or negative weights")
        arrays[f"x{b}"], arrays[f"w{b}"] = x, w
    out = Prior(arrays, info, str(path))
    _LOADED[path] = out
    return out


def bin_pixels(images, weights, bins: int, gamma: float = GAMMA):
    """(x, w) from pictures: images - a list of (n_i, 3) uint8 sRGB pixel arrays, weights - one weight per picture.
    Every pixel's encoded sRGB picks its cell; a cell's point is the weighted mean linear colour of its pixels, its weight
    the summed pixel weight ** gamma, the weights normalised to sum to 1. Empty arrays without pictures."""
    if not len(images):
        return np.zeros((0, 3)), np.zeros(0)
    px = np.concatenate(images).astype(float) / 255.0
    pw = np.concatenate([np.full(len(a), wt) for a, wt in zip(images, weights)])
    b = np.minimum((px * bins).astype(int), bins - 1)
    _, inv = np.unique((b[:, 0] * bins + b[:, 1]) * bins + b[:, 2], return_inverse=True)
    cnt = np.bincount(inv, weights=pw)
    lin = color.srgb_decode(px)
    mean = np.stack([np.bincount(inv, weights=pw * lin[:, c]) for c in range(3)], axis=1) / cnt[:, None]
    w = cnt ** gamma
    return mean, w / w.sum()


def load_picture(path) -> np.ndarray:
    """A picture as the prior takes it: sRGB uint8 (h, w, 3), its EXIF orientation applied, an ICC profile other than
    sRGB converted to sRGB, shrunk to LONG_SIDE on the long side (box filter)."""
    from PIL import Image, ImageCms, ImageOps
    im = Image.open(path)
    # a JPEG is decoded at 1/2, 1/4 or 1/8 of its size when that still leaves at least LONG_SIDE on the long side
    k = LONG_SIDE / max(im.size)
    im.draft("RGB", (max(1, im.width // 8, math.ceil(im.width * k)), max(1, im.height // 8, math.ceil(im.height * k))))
    im = ImageOps.exif_transpose(im)
    icc = im.info.get("icc_profile")
    if icc:
        try:
            prof = ImageCms.ImageCmsProfile(io.BytesIO(icc))
            if "srgb" not in ImageCms.getProfileDescription(prof).lower():
                im = ImageCms.profileToProfile(im.convert("RGB"), prof, ImageCms.createProfile("sRGB"), outputMode="RGB")
        except Exception:     # noqa: BLE001 - an unreadable profile: the pixels are taken as sRGB
            pass
    im = im.convert("RGB")
    s = LONG_SIDE / max(im.size)
    if s < 1:
        im = im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))), Image.BOX)
    return np.asarray(im, np.uint8)


def pictures_in(folder, recursive=False) -> list:
    """The pictures of a folder (IMAGE_EXT), sorted by path - the order is part of the result (sums in float)."""
    folder = Path(folder)
    it = folder.rglob("*") if recursive else folder.iterdir()
    return sorted(p for p in it if p.is_file() and p.suffix.lower() in IMAGE_EXT)


def build(images, weights=None, description="", bins=BINS, gamma=GAMMA) -> Prior:
    """A prior from pictures already loaded (load_picture): images - (h, w, 3) or (n, 3) uint8 arrays."""
    flat = [np.asarray(a, np.uint8).reshape(-1, 3) for a in images]
    weights = [1.0] * len(flat) if weights is None else list(weights)
    if not flat:
        raise ValueError("no pictures")
    arrays = {}
    for b in bins:
        arrays[f"x{b}"], arrays[f"w{b}"] = bin_pixels(flat, weights, b, gamma)
    info = {"format": FORMAT, "bins": list(bins), "gamma": gamma, "long_side": LONG_SIDE, "pictures": len(flat),
            "pixels": int(sum(len(a) for a in flat)), "description": description}
    return Prior(arrays, info, "built")


def save(prior: Prior, path) -> Path:
    """Write a prior to an .npz file (uncompressed: the values are stored exactly, as float64)."""
    path = Path(path)
    if path.suffix.lower() != ".npz":
        path = path.with_suffix(".npz")
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        np.savez(f, info=np.array(json.dumps(prior.info, sort_keys=True)),
                 **{k: np.ascontiguousarray(v, np.float64) for k, v in sorted(prior.arrays.items())})
    return path
