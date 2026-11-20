"""Loading a picture for the colour path model: the camera's own RAW (.ORF) or a developed picture (JPEG, PNG, TIFF).

A developed picture's light is taken as the scene: its colour space (from its ICC profile, else EXIF, else sRGB) gives
linear BT.709. A RAW gives the white-balanced sensor values the camera's tables would get (camera.py; needs rawpy).

Pictures are kept at most MAX_SIDE pixels on the long side (an integer step, no resampling: every kept pixel is a pixel
of the picture, a RAW pixel a 2x2 Bayer block). The record holds the picture per distinct colour - the model, CIELAB and
CIEDE2000 are per-colour functions, so working on the distinct colours gives the same picture as working per pixel, and a
developed picture of 1239 x 826 has only 40-270 thousand of them (a RAW about 60 % of its pixels)."""
from __future__ import annotations

import hashlib
import io
import re

import numpy as np
from PIL import Image, ImageCms, ImageOps

from . import camera, color

MAX_SIDE = 1400                                  # pictures are kept at most this size


def distinct(a):
    """The distinct rows of a (N, 3) (equal to the last bit), the index of every row among them and a row of each."""
    a = np.ascontiguousarray(a, dtype=np.float64)
    _, first, inv = np.unique(a.view(np.dtype((np.void, a.itemsize * 3))).ravel(), return_index=True, return_inverse=True)
    return a[first], inv.ravel(), first


def _prophoto_decode(v):
    v = np.clip(v, 0, 1)
    return np.where(v < 16 / 512, v / 16, v ** 1.8)


def _gamma(g):
    return lambda v: np.clip(v, 0, 1) ** g


# Picture colour spaces: label, transfer (code -> linear), linear RGB -> XYZ D65
SPACES = {
    "srgb": ("sRGB", color.srgb_decode, color.M709),
    "p3": ("Display P3", color.srgb_decode, color.M_P3),
    "adobe": ("Adobe RGB (1998)", _gamma(563 / 256), color.M_ADOBE),
    "prophoto": ("ProPhoto RGB", _prophoto_decode, color.BRAD_D50_D65 @ color.MPRO_D50),
    "rec2020": ("Rec.2020 (gamma 2.4)", _gamma(2.4), color.M2020),
    "linear": ("Linear sRGB", lambda v: np.clip(v, 0, 1), color.M709),
}


def detect_space(im):
    """The picture's colour space from its ICC profile, else from EXIF (ColorSpace 0xFFFF + interop R03 = Adobe RGB, as
    cameras write it), else sRGB. Returns (key, how found); key None - an ICC profile not in SPACES: converted to sRGB by
    the profile itself (load_image)."""
    icc = im.info.get("icc_profile")
    if icc:
        try:
            desc = ImageCms.getProfileDescription(ImageCms.ImageCmsProfile(io.BytesIO(icc))).strip()
        except Exception:
            desc = "?"
        low = desc.lower()
        for key, words in (("p3", ("p3",)), ("adobe", ("adobe rgb", "compatible with adobe")), ("prophoto", ("prophoto", "romm")),
                           ("rec2020", ("2020",)), ("linear", ("linear",)), ("srgb", ("srgb", "iec61966"))):
            if any(w in low for w in words):
                return key, f"ICC profile '{desc}'"
        return None, f"ICC profile '{desc}'"
    try:
        exif = im.getexif()
        ifd = exif.get_ifd(0x8769)
        if ifd.get(0xA001) == 0xFFFF:
            interop = exif.get_ifd(0xA005)
            if str(interop.get(0x0001, "")).startswith("R03"):
                return "adobe", "EXIF (Adobe RGB)"
            return "srgb", "EXIF: uncalibrated, taken as sRGB"
        if ifd.get(0xA001) == 1:
            return "srgb", "EXIF (sRGB)"
    except Exception:
        pass
    if "gamma" in im.info and abs(float(im.info["gamma"]) - 1.0) < 0.05:
        return "linear", "PNG gAMA 1.0"
    return "srgb", "no profile, taken as sRGB"


def is_orf(data: bytes, name: str) -> bool:
    return name.lower().endswith(".orf") or data[:4] in (b"IIRO", b"IIRS", b"MMOR")


# The camera model string an ORF's EXIF holds (Model), as the kit's camera and its relatives write it
_MODEL = re.compile(rb"(OM-\d[0-9A-Za-z]*|E-M\d+[0-9A-Za-z]*|E-P[0-9A-Za-z]+|PEN-[0-9A-Za-z]+|TG-\d+)\s*\x00")
CAMERA_MODEL = "OM-5MarkII"                      # the camera the colour path model (camera.py) is of


def orf_model(head: bytes) -> str:
    """The camera model an ORF names in its first bytes (the EXIF Model string, e.g. "OM-5MarkII"); "" when none is
    found. The first 256 KB of the file are enough."""
    m = _MODEL.search(head[:262144])
    return m.group(1).decode("ascii") if m else ""


def load_orf(data: bytes, name: str):
    """The camera's own RAW: the shot's white balance, the camera's matrix S+80 and the metered grey (camera.py) give the
    scene it stands for, the headroom above the scene's 1 kept - the tables get exactly what the camera would give them.
    Returns (record, info)."""
    raw = camera.raw_of_orf(data)
    step = max(1, int(np.ceil(max(raw.shape[:2]) / MAX_SIDE)))
    raw = raw[::step, ::step]
    lin = camera.scene_of_raw(raw)
    key = hashlib.sha256(data).hexdigest()[:12]
    y = lin @ color.M709[1]
    raw_u, inv, first = distinct(raw.reshape(-1, 3))      # the scene of a distinct RAW colour as computed above
    rec = {"lin": lin.reshape(-1, 3)[first], "raw": raw_u, "inv": inv, "shape": lin.shape, "name": name}
    info = {"id": key, "w": lin.shape[1], "h": lin.shape[0], "name": name, "space": "raw",
            "space_label": "Camera RAW (ORF)",
            "how": "the shot's white balance, the camera's matrix S+80, metered grey = 0.18 of the scene; highlights "
                   "above 1 kept",
            "median_y": round(float(np.median(y)), 4), "spaces": {k: v[0] for k, v in SPACES.items()}}
    return rec, info


def load_image(data: bytes, name: str, space: str = "auto"):
    """A picture -> (record, info). record: lin - the linear scene (BT.709, grey 0.18) per distinct colour, raw - the
    white-balanced RAW per distinct colour (a RAW only), inv - every pixel's index among them, shape (h, w, 3), name.
    info: what a page shows (id, size, colour space and how it was found, the median luma). space: a key of SPACES, or
    "auto" - from the picture."""
    if is_orf(data, name):
        try:
            return load_orf(data, name)
        except ImportError:
            raise ValueError("reading camera RAW files needs the rawpy package (pip install rawpy)") from None
        except (MemoryError, ValueError):
            raise
        except Exception as e:      # the RAW reader's own errors: a damaged or unsupported file
            raise ValueError(f"cannot read the RAW file: {type(e).__name__}: {e}") from None
    try:
        im = Image.open(io.BytesIO(data))
        im = ImageOps.exif_transpose(im)
        im.load()
    except (OSError, Image.DecompressionBombError) as e:      # not a picture Pillow reads, damaged, or far too large
        raise ValueError(f"cannot read the picture: {e}") from None
    found, how = detect_space(im)
    if space != "auto":
        if space not in SPACES:
            raise ValueError(f"unknown colour space {space!r}")
        found, how = space, "chosen by hand"
    if found is None:            # an ICC profile not in SPACES: converted to sRGB by the profile
        icc = ImageCms.ImageCmsProfile(io.BytesIO(im.info["icc_profile"]))
        im = ImageCms.profileToProfile(im.convert("RGB"), icc, ImageCms.createProfile("sRGB"), outputMode="RGB")
        found, how = "srgb", how + " -> sRGB (converted by the profile)"
    if im.mode in ("I;16", "I;16B", "I;16L", "I"):
        a = np.asarray(im, dtype=np.float64) / 65535.0
        a = np.repeat(a[..., None], 3, axis=2)
    else:
        a = np.asarray(im.convert("RGB"), dtype=np.float64) / 255.0
    h, w = a.shape[:2]
    step = max(1, int(np.ceil(max(h, w) / MAX_SIDE)))
    a = a[::step, ::step]
    label, decode, to_xyz = SPACES[found]
    # Linear light of the picture, as BT.709 linear (the model's input): a developed picture's light is taken as the scene
    lin = np.clip(np.einsum("ij,hwj->hwi", np.linalg.solve(color.M709, to_xyz), decode(a)), 0, None)
    key = hashlib.sha256(data + found.encode()).hexdigest()[:12]
    y = lin @ color.M709[1]
    lin_u, inv, _ = distinct(lin.reshape(-1, 3))
    rec = {"lin": lin_u, "inv": inv, "shape": lin.shape, "name": name}
    info = {"id": key, "w": lin.shape[1], "h": lin.shape[0], "name": name, "space": found, "space_label": label,
            "how": how, "median_y": round(float(np.median(y)), 4), "spaces": {k: v[0] for k, v in SPACES.items()}}
    return rec, info
