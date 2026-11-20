"""Start the ISP player from anywhere:

    python OMKit/apps/isp_player/run.py [--port N] [--open]

It puts OMKit/apps (the folder of ispkit and isp_player) first on the import path and runs isp_player.server.main()."""
import sys
from pathlib import Path

APPS = Path(__file__).resolve().parent.parent
HERE = str(Path(__file__).resolve().parent)
# this script's own folder is not a package root: drop it, so its modules cannot shadow others
sys.path[:] = [p for p in sys.path if Path(p or ".").resolve() != Path(HERE)]
sys.path.insert(0, str(APPS))

from isp_player.server import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
