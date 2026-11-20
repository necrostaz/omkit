"""Start the ISP fitter from anywhere:

    python OMKit/apps/isp_fitter/run.py [--port N] [--open] [--prior FILE.npz] [--cache DIR | --no-cache]

It puts OMKit/apps (the folder of ispkit and isp_fitter) first on the import path and runs isp_fitter.server.main()."""
import sys
from pathlib import Path

APPS = Path(__file__).resolve().parent.parent
HERE = str(Path(__file__).resolve().parent)
# this script's own folder is not a package root: drop it, so its modules cannot shadow others
sys.path[:] = [p for p in sys.path if Path(p or ".").resolve() != Path(HERE)]
sys.path.insert(0, str(APPS))

from isp_fitter.server import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
