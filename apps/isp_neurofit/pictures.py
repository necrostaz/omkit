"""The two picture sets: finding the files, reading them small (with a cache in the run's folder), and the sheets the eye
judges - contact sheets, boards and the anchors sheet (JPEG).

The pictures themselves are only read: a run keeps small copies as arrays in its cache folder, never the files."""
from __future__ import annotations

import io
from pathlib import Path

import numpy as np

from ispkit import camera

RAW_EXT = (".orf", ".ori")
SCAN_EXT = (".jpg", ".jpeg", ".png", ".tif", ".tiff", ".webp")
LONG = 320                      # cached pictures: long side, pixels
THUMB = 250                     # a contact sheet / board cell
PER_ROW = 6
PER_SHEET = 30


class SetError(ValueError):
    """A set names a file or folder that is not there."""


def _box(a, long_side):
    """Block average to about long_side."""
    f = max(1, int(np.ceil(max(a.shape[:2]) / long_side)))
    h, w = a.shape[0] // f * f, a.shape[1] // f * f
    return a[:h, :w].reshape(h // f, f, w // f, f, 3).mean(axis=(1, 3))


def cached(run_dir: Path, kind: str, src: Path, make):
    """make() once per file version (name, size, time): kept as cache/<kind>/<name>.<size>.<mtime>.npy in the run."""
    st = src.stat()
    cp = run_dir / "cache" / kind / f"{src.name}.{st.st_size}.{int(st.st_mtime)}.npy"
    if cp.exists():
        try:
            return np.load(cp)
        except (OSError, ValueError, EOFError):
            pass
    a = make()
    cp.parent.mkdir(parents=True, exist_ok=True)
    np.save(cp, a)
    return a


def load_raw(run_dir: Path, src: Path):
    """The white-balanced RAW of the camera (h, w, 3), 1 = the sensor's white, long side LONG. Needs rawpy."""
    return cached(run_dir, "raw", src,
                  lambda: _box(camera.raw_of_orf(src.read_bytes()), LONG).astype(np.float32)).astype(float)


def scan_info(src: Path) -> dict:
    """A scan's size, mode and the description of its ICC profile ("" without one)."""
    from PIL import Image, ImageCms
    with Image.open(src) as im:
        icc = im.info.get("icc_profile")
        desc = ""
        if icc:
            try:
                desc = ImageCms.getProfileDescription(ImageCms.ImageCmsProfile(io.BytesIO(icc))).strip()
            except Exception:     # noqa: BLE001 - an unreadable profile
                desc = "?"
        return {"width": im.width, "height": im.height, "mode": im.mode, "icc": desc}


def load_scan(run_dir: Path, src: Path):
    """A scan as sRGB uint8 (h, w, 3), long side LONG, not cropped; another ICC profile is converted to sRGB."""
    def make():
        from PIL import Image, ImageCms, ImageOps
        im = Image.open(src)
        im.draft("RGB", (max(1, im.width // 4), max(1, im.height // 4)))
        im = ImageOps.exif_transpose(im)
        icc = im.info.get("icc_profile")
        if icc:
            try:
                prof = ImageCms.ImageCmsProfile(io.BytesIO(icc))
                if "srgb" not in ImageCms.getProfileDescription(prof).lower():
                    im = ImageCms.profileToProfile(im.convert("RGB"), prof, ImageCms.createProfile("sRGB"),
                                                   outputMode="RGB")
            except Exception:     # noqa: BLE001 - an unreadable profile: the pixels are taken as sRGB
                pass
        if im.mode.startswith("I"):                       # 16-bit grey
            a = np.asarray(im, np.float64)
            im = Image.fromarray((a / max(a.max(), 1) * 255).astype(np.uint8))
        im = im.convert("RGB")
        s = LONG / max(im.size)
        if s < 1:
            im = im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))), Image.BOX)
        return np.asarray(im, np.uint8)
    return cached(run_dir, "scan", src, make)


def crop(a, lrtb):
    """Cut fractions of the frame off: left, right, top, bottom."""
    l, r, t, b = lrtb or (0, 0, 0, 0)
    h, w = a.shape[:2]
    return a[round(h * t):max(round(h * t) + 1, round(h * (1 - b))), round(w * l):max(round(w * l) + 1, round(w * (1 - r)))]


def path_of(sel, kind, e) -> Path:
    """Where a picture of selection.json is: its own path (or the set's first folder and the file name)."""
    return Path(e["path"]) if e.get("path") else Path(sel["raw_dir" if kind == "raw" else "scan_dir"]) / e["file"]


def collect(sources, exts, recursive=False):
    """The pictures of a set: [(key, path)]. sources - folders (with their subfolders when recursive) and single files.
    The key is what selection.json, the anchors and the sheets call a picture: its file name, or - when two folders
    hold the same name - as many parent folders in front of it as tell the two apart."""
    paths = []
    for s in sources:
        p = Path(s).resolve()
        if p.is_file():
            found = [p] if p.suffix.lower() in exts else []
        elif p.is_dir():
            found = sorted(q for q in (p.rglob("*") if recursive else p.iterdir())
                           if q.is_file() and q.suffix.lower() in exts)
        else:
            raise SetError(f"no such file or folder: {s}")
        paths += [q for q in found if q not in paths]
    depth = {p: 1 for p in paths}
    key = lambda p: "/".join(p.parts[-depth[p]:])      # noqa: E731
    while True:
        seen = {}
        for p in paths:
            seen.setdefault(key(p).lower(), []).append(p)
        clash = [p for ps in seen.values() if len(ps) > 1 for p in ps if depth[p] < len(p.parts)]
        if not clash:
            return [(key(p), p) for p in paths]
        for p in clash:
            depth[p] += 1


# ------------------------------------------------------------------ sheets
def thumb(rgb, label):
    """A sheet cell: the picture (sRGB 0..1) fitted in THUMB x THUMB under a caption line."""
    from PIL import Image, ImageDraw
    im = Image.fromarray((np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8))
    im.thumbnail((THUMB, THUMB), Image.BILINEAR)
    cell = Image.new("RGB", (THUMB + 8, THUMB + 22), (32, 32, 32))
    cell.paste(im, ((THUMB + 8 - im.width) // 2, 18 + (THUMB - im.height) // 2))
    ImageDraw.Draw(cell).text((4, 3), label.encode("ascii", "replace").decode()[:40], fill=(235, 235, 235))
    return cell


def sheet(cells, path, per_row=PER_ROW):
    """Cells in rows of per_row, saved as a JPEG."""
    from PIL import Image
    if not cells:
        return
    cw, ch = cells[0].size
    rows = -(-len(cells) // per_row)
    sh = Image.new("RGB", (cw * min(per_row, len(cells)), ch * rows), (32, 32, 32))
    for i, c in enumerate(cells):
        sh.paste(c, (i % per_row * cw, i // per_row * ch))
    sh.save(path, quality=88)


def sheets(cells, stem: Path):
    """Cells on as many sheets <stem>_NN.jpg as needed (PER_SHEET each); returns the file names."""
    out = []
    for n, a in enumerate(range(0, len(cells), PER_SHEET)):
        p = stem.with_name(f"{stem.name}_{n + 1:02d}.jpg")
        sheet(cells[a:a + PER_SHEET], p)
        out.append(p.name)
    return out


def render(raw, fn):
    """A RAW picture (h, w, 3) through a per-pixel function."""
    return fn(raw.reshape(-1, 3)).reshape(raw.shape)


def order(items):
    """The order of a set on the sheets: by the first tag, then the key."""
    return sorted(range(len(items)), key=lambda i: ((items[i][0].get("tags") or ["~"])[0], items[i][0]["file"]))
