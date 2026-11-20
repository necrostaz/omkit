# OMKit apps

Programs for a computer that work with the kit's picture profiles. They live in `OMKit/apps/`, run locally and share one
Python library, `OMKit/apps/ispkit/` (the model of the camera's colour path, the `.isp` file, the cassette pictures,
picture loading and the render, and the fit: the `.cube` look, the training set with its photo colour prior, the
recipes and the optimiser).

| Start | Address |
|---|---|
| `python OMKit/apps/isp_player/run.py` | `http://127.0.0.1:8790/` |
| `python OMKit/apps/isp_fitter/run.py` | `http://127.0.0.1:8791/` |
| `python OMKit/apps/isp_neurofit/run.py <command>` | a command line, no page ([install the agent skill](isp_neurofit.md#31-install-for-an-agent)) |

| App | What it does | Status |
|---|---|---|
| [ISP player](isp_player.md) | shows a picture (a camera RAW or a JPEG, PNG, TIFF) through an `.isp` profile as the OM-5 Mark II would render it, and what the file holds | available |
| [ISP fitter](isp_fitter.md) | fits the camera's colour tables to a look (a 3D LUT in a `.cube` file), shows the look and its copy side by side, sets up the cassette and saves an `.isp` profile; `build_prior.py` builds a photo colour prior from your own pictures | available |
| [ISP neurofit](isp_neurofit.md) | makes an `.isp` profile from two unpaired sets - the camera's RAW files and scans of one film - with an AI agent (a bundled agent skill) or you judging contact sheets by eye; a command line tool | available |

## Install once

Python 3.10 or newer. Install the packages into a virtual environment, from the folder that holds `OMKit/`:

```sh
# Windows (Command Prompt or PowerShell)
py -m venv %USERPROFILE%\venv\omkit-apps           # PowerShell: $HOME\venv\omkit-apps
%USERPROFILE%\venv\omkit-apps\Scripts\activate      # PowerShell: & $HOME\venv\omkit-apps\Scripts\Activate.ps1

# Linux, macOS (Debian/Ubuntu: sudo apt install python3-venv first)
python3 -m venv ~/venv/omkit-apps
. ~/venv/omkit-apps/bin/activate

# then, on every system
python -m pip install -r OMKit/apps/requirements.txt
```

With the environment active, `python` is that environment's Python on every system, so the commands on these pages
work as written; activate it again in every new terminal. Without an environment, Linux and macOS call Python
`python3`, and Debian and Ubuntu refuse `pip install` into the system Python.

This installs numpy, Pillow and rawpy (for the camera's `.ORF` files). PyTorch with CUDA is optional: with it the
render runs on an NVIDIA GPU, with the same result ([player](isp_player.md#2-install)), and fits run several times
faster ([fitter](isp_fitter.md#1-install)).

## Tests

```sh
python -m unittest discover -s OMKit/apps -t OMKit/apps
```
