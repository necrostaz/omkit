"""python -m isp_player [--port N] [--open], run from OMKit/apps (or with OMKit/apps on PYTHONPATH)."""
import sys

from isp_player.server import main

sys.exit(main())
