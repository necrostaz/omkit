"""python -m isp_fitter [--port N] [--open] [--prior FILE] [--cache DIR | --no-cache], run from OMKit/apps (or with
OMKit/apps on PYTHONPATH)."""
import sys

from isp_fitter.server import main

sys.exit(main())
