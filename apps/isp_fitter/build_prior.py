"""Build a photo colour prior for the ISP fitter from a folder of one's own pictures:

    python OMKit/apps/isp_fitter/build_prior.py PHOTOS_DIR OUT.npz [--recursive] [--description TEXT]

then fit with it:

    python OMKit/apps/isp_fitter/run.py --prior OUT.npz

Every JPEG, PNG and TIFF of the folder is read, brought to sRGB, turned by its EXIF orientation and shrunk to 240 pixels
on the long side; all the pixels are binned as the bundled prior was (ispkit/prior.py: 24^3 and 16^3 cells in encoded
sRGB, a cell's mean linear colour, the weight count^0.5 normalised to 1), every picture with the weight 1. The file holds
only these cell colours and weights, no pictures and no file names. Pick pictures of the kinds of scenes the profile
will be used on, a few dozen of them, varied: people, foliage, sky, city, indoor light, night."""
import argparse
import sys
from pathlib import Path

APPS = Path(__file__).resolve().parent.parent
HERE = str(Path(__file__).resolve().parent)
sys.path[:] = [p for p in sys.path if Path(p or ".").resolve() != Path(HERE)]
sys.path.insert(0, str(APPS))

import ispkit  # noqa: E402,F401 - first: it sets the BLAS threads before numpy loads
from ispkit import prior  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(prog="build_prior", description="Build a photo colour prior for the ISP fitter from a "
                                                                 "folder of pictures (JPEG, PNG, TIFF)")
    ap.add_argument("photos", help="the folder of pictures")
    ap.add_argument("out", help="the prior file to write (.npz)")
    ap.add_argument("--recursive", action="store_true", help="also the pictures in sub-folders")
    ap.add_argument("--description", default="", help="a free text kept in the file's info")
    a = ap.parse_args(argv)
    folder = Path(a.photos)
    if not folder.is_dir():
        print(f"build_prior: {folder} is not a folder", file=sys.stderr)
        return 1
    paths = prior.pictures_in(folder, a.recursive)
    images = []
    for i, p in enumerate(paths, 1):
        try:
            images.append(prior.load_picture(p))
        except Exception as e:        # noqa: BLE001 - a file Pillow cannot read is skipped, with a message
            print(f"  skipped {p.name}: {type(e).__name__}: {e}", file=sys.stderr)
            continue
        print(f"\r  read {i} of {len(paths)}", end="", flush=True)
    print()
    if not images:
        print(f"build_prior: no pictures read in {folder}", file=sys.stderr)
        return 1
    if len(images) < 10:
        print(f"build_prior: only {len(images)} pictures - the prior describes them only; a few dozen varied ones are "
              "better", file=sys.stderr)
    pr = prior.build(images, description=a.description)
    out = prior.save(pr, a.out)
    print(f"build_prior: {out} - {pr.info['pictures']} pictures, {pr.info['pixels']} pixels, "
          + ", ".join(f"{b}^3: {len(pr.cells(b)[0])} cells" for b in prior.BINS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
