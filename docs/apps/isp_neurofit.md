# ISP neurofit

ISP neurofit makes an **`.isp` profile** for the OM-5 Mark II from two **unpaired** picture sets: the camera's RAW
files (`.ORF`) and scans of one film stock. The scenes of the two sets differ - nothing is compared picture to picture.
A small parametric film look is moved until the RAW set rendered through it has the statistics of the scans (lightness,
the cast of the neutrals, saturation by hue, and the colour of objects marked in both sets); the look is then fitted to
the camera's colour tables with the [ISP fitter](isp_fitter.md)'s recipe and saved as an `.isp` file for the
[FILMLOOK module](../modules/filmlook/README.md).

The numbers are blind to what a picture shows, so the method needs eyes: someone picks the comparable pictures, marks
the same kind of object in both sets (skin, sky, foliage, a grey thing) and judges the result on contact sheets. That
someone is an **AI agent** following the bundled agent skill, or you, by hand.

Code: `OMKit/apps/isp_neurofit/` (the command line `cli.py` with `look.py`, `stats.py`, `pictures.py`; the agent skill
`skill/isp-neurofit/`; the skill's installer `install_skill.py`) on top of the shared library `OMKit/apps/ispkit/`.

Contents: [1. What it does, and what not](#1-what-it-does-and-what-not) · [2. Install](#2-install) ·
[3. The agent skill](#3-the-agent-skill) · [4. The workflow](#4-the-workflow) · [5. The look model](#5-the-look-model) ·
[6. The measures, and when to stop](#6-the-measures-and-when-to-stop) · [7. Limits and caveats](#7-limits-and-caveats) ·
[8. Privacy](#8-privacy) · [9. Troubleshooting](#9-troubleshooting)

## 1. What it does, and what not

- **Fits colour and tone only**: the tone curve, the cast of the neutrals over lightness, saturation, and per-hue
  saturation, hue and lightness - what the camera's colour tables can carry (matrix, R/G/B gamma, saturation, tone
  curve, 12 hue sectors). Grain, sharpness, halation, vignetting and the camera's other picture settings are not fitted.
- **Starts from a neutral look** every time, never from an existing preset or profile of the film.
- **Writes** the preset's name (as the camera shows it), its 8.3 file name and a cassette picture into the file.
- **Is checked on the camera's colour model, not on the camera**: look at the result in the [ISP player](isp_player.md),
  then shoot with it.
- Does not download anything, does not touch the camera or its card.

## 2. Install

1. Python 3.10 or newer, then from the folder that holds `OMKit/`:

   ```sh
   python -m pip install -r OMKit/apps/requirements.txt
   ```

   Best in a virtual environment: [apps/README.md, Install once](README.md#install-once) shows it for Windows,
   Linux and macOS.

   numpy, Pillow and rawpy (rawpy reads the camera's `.ORF` files). PyTorch with CUDA is optional and makes the full
   fit several times faster ([fitter, section 1](isp_fitter.md#1-install)).
2. Check:

   ```sh
   python OMKit/apps/isp_neurofit/run.py doctor
   ```

   It lists the Python, the packages, the fitter's version and the workspace, and says what is missing.

**The workspace.** Every run is a folder of the workspace. The workspace is, in this order: `--workspace DIR` on the
command, the environment variable `ISP_NEUROFIT_WORKSPACE`, or `isp_neurofit_runs` in the current folder. It must be
outside the OMKit checkout - the tool refuses a workspace inside it, because runs hold small copies of your pictures
and must not end up in a repository. Setting the variable once saves typing:

```sh
export ISP_NEUROFIT_WORKSPACE="$HOME/isp_neurofit_runs"           # macOS, Linux
setx ISP_NEUROFIT_WORKSPACE "%USERPROFILE%\isp_neurofit_runs"     # Windows (new terminals)
```

**What it needs for the full workflow**: the commands run anywhere; judging the sheets needs eyes - an AI agent that
can open and view image files (JPEG, PNG), or you. An agent without image viewing can run the commands, but you must
then do the selection, the anchors and the judgement yourself.

## 3. The agent skill

`OMKit/apps/isp_neurofit/skill/isp-neurofit/` is an agent skill in the open Agent Skills format: a folder named after
the skill with `SKILL.md` (a YAML header with `name` and `description`, then the instructions), `references/` (the run's
files, the look's parameters) and `scripts/neurofit.py` (a small wrapper that runs the tool of the OMKit checkout). The
instructions are written for any agent: they ask the user, open image files and list folders, and use no tool of a
particular agent.

The wrapper finds the tool by, in this order: the environment variable `OMKIT_APPS` (the checkout's `OMKit/apps`
folder), `scripts/config.json` written by the installer, or its own place when the skill is used inside the checkout.

### 3.1. Install for an agent

The installer copies the skill folder and records the absolute paths of this checkout's `OMKit/apps` and of the Python
that ran it (`scripts/config.json` in the copy; the wrapper uses that Python when the one the agent calls lacks the
packages):

| Agent | Command | Where the skill goes |
|---|---|---|
| Claude Code, every project | `python OMKit/apps/isp_neurofit/install_skill.py --claude-user` | `~/.claude/skills/isp-neurofit/` |
| Claude Code, one project | `python OMKit/apps/isp_neurofit/install_skill.py --claude-project PROJECT_DIR` | `PROJECT_DIR/.claude/skills/isp-neurofit/` |
| any agent that reads `SKILL.md` folders | `python OMKit/apps/isp_neurofit/install_skill.py --dest SKILLS_DIR` | `SKILLS_DIR/isp-neurofit/` - use the agent's own skills folder |

`--force` replaces an installed copy (install again after moving or updating the checkout). Then check the copy:
`python SKILLS_DIR/isp-neurofit/scripts/neurofit.py doctor`.

An agent without a skills folder: point it at the installed (or the in-checkout) `SKILL.md` and tell it to follow it -
for example "read `.../isp-neurofit/SKILL.md` and follow it to make a profile from the RAW files in X and the scans in
Y". Agents that load skills by their description pick it up from a request such as "make a FILMLOOK preset from my
Portra 400 scans and these OM-5 Mark II RAW files".

### 3.2. What the agent does

The skill takes the agent through the steps of section 4: it works out the two sets from your request (and asks once
to confirm scan folders it found, the preset's name and the cassette, and the workspace if none is set), analyses the
sets and opens every contact sheet, drops pictures that do not compare, tags the kinds of scenes, suggests what to add
to the sets, marks anchors, estimates and fits, then goes round looking at the boards, naming what is off and
correcting one thing per round, and finally makes the full fit and reports what came across and what is unreliable.

## 4. The workflow

The command line, from any folder (`python OMKit/apps/isp_neurofit/run.py <command>`; from inside `OMKit/apps` also
`python -m isp_neurofit <command>`; through an installed skill `python .../isp-neurofit/scripts/neurofit.py <command>`).
Every command with `--run NAME` also takes `--workspace DIR`; `--help` on any command lists its options.

| Step | Command | Writes into the run's folder |
|---|---|---|
| check the setup | `doctor` | - |
| 1. read both sets | `analyze --run NAME --raw PATH... --scans PATH... [--recursive] [--raw-every N]` | `selection.json`, `analysis.json`, `contact_raw_NN.jpg`, `contact_scan_NN.jpg` |
| 2. choose a cassette | `cassettes --run NAME --label "FILM 400" [--ids ...] [--brand ...] [--proc ...]` | `cassettes.png` |
| 3. name the preset | `preset --run NAME --title "Film 400" --file FILM400 [--cassette ID] [--label ...] [--iso ...]` | `preset.json`, `cassette.png` |
| 4. check the anchors | `anchors --run NAME` | `anchors.jpg` |
| 5. the neutral start | `init --run NAME` | `look.json` |
| 6. estimate the look | `estimate --run NAME [--parts tone,cast,sat,anchors] [--strength 0.7] [--rounds 3]` | `look.json` |
| 7. fit it to the tables | `fit --run NAME [--quick] [--note "..."] [--out FILE.ISP]` | `iter_NN/`, `history.json` |
| the history; go back | `status --run NAME`; `restore --run NAME --iter N` | `look.json` |
| keep the grey after a hand edit | `pin-grey --run NAME` | `look.json` |
| are these RAW files of the OM-5 Mark II? | `check-raw --dir PATH...` | - |

By hand, the steps are:

1. **Analyse.** `analyze` reads every RAW file (about 1 s each; `--raw-every N` takes every N-th of a big folder) and
   scan (JPEG, PNG, TIFF, WebP; a scan with another ICC profile is converted to sRGB), and writes contact sheets: the
   RAW files through the neutral start, the scans as they are. Its hints name what differs between the sets (missing
   hues, a different key, too few pictures, black and white scans, framed scans) and RAW files of another camera.
   Scans from several folders are compared batch by batch (`scan_batches` in `analysis.json`).
2. **Choose.** In `selection.json` set `use: false` for what does not compare (another film, another processing, test
   charts, half-blown frames, another camera), lower the `weight` of near duplicates, give every picture a first tag
   with the kind of scene (`portrait`, `street`, `foliage`...; the boards are ordered by it), set `crop_lrtb` for scans
   with a border. Aim for the same mix of scenes and light in both sets. Run `analyze` again: the choices are kept.
   The file format: [skill reference](../../apps/isp_neurofit/skill/isp-neurofit/references/run-files.md).
3. **Anchors.** Add boxes on the same kind of object in both sets (`skin`, `sky`, `foliage`, `neutral`, or your own
   classes) and check them with `anchors`. Only even, evenly lit areas in comparable light; a class counts only when
   marked in both sets.
4. **Name** the preset with `cassettes` and `preset` (the 8.3 name must not be one of the bundled FILMLOOK presets or
   another run's).
5. **Estimate and fit**: `init`, `estimate`, `fit --quick`. Open `iter_01/board_01.jpg` (RAW files through the fitted
   tables on top, at the exposure of the comparison; scans below) and `stages.jpg` (neutral start, the look, the
   tables - as shot).
6. **Go round**: name one thing that is off (contrast, black, the cast of shadows or highlights, saturation, a colour),
   change it - `estimate --parts ...` again, or edit `look.json` by hand
   ([the parameters](../../apps/isp_neurofit/skill/isp-neurofit/references/look-parameters.md)) - and `fit --quick
   --note "..."` again. `restore` goes back.
7. **Finish** with a full fit (`fit` without `--quick`, `--out` to copy the file), look at its sheets, and copy the
   `.ISP` to the card's `OMKIT\FILMLOOK\` folder ([fitter, section 5](isp_fitter.md#5-saving-and-using-the-profile)).

An iteration `iter_NN/` holds `look.json` (the look fitted), `hardware.json` (the fitted tables), `<FILE>.ISP` (with
`ISPB`, `META` - the fitter's version, the recipe, `neurofit` = a hash of the look - and `CANI` when a cassette is
chosen), `board_NN.jpg`, `stages.jpg` and `metrics.json`. `cache/` holds the pictures read, small; deleting it is safe.

Times on a desktop (a 12-core CPU, a current NVIDIA GPU; 8 RAW files and 14 scans): `analyze` 7 s - about 1 s per
RAW file and a fraction of that per scan; `fit --quick` 7 s and the full fit 8 s with the GPU, boards included. The fit
itself takes as long as in the ISP fitter: about half a minute for "Best" on the CPU alone
([fitter, section 1](isp_fitter.md#1-install)).

## 5. The look model

### 5.1. The look

The film's look is a small function of the scene (linear light, middle grey at 0.18, the camera's headroom above the
scene's white kept) to display sRGB:

1. **a tone curve over exposure**: the display L* of a neutral at 25 nodes from -8 to +4 stops around middle grey,
   applied per channel - a neutral lands exactly on the curve, colours lose chroma towards black and white, as on film;
2. in CIELAB of that: **saturation** (overall, and over five lightness nodes), and **12 hue nodes** every 30 degrees of
   hue, each with a saturation factor, a hue turn and a lightness offset - all three fading out towards the neutral
   axis; then **a cast** a*, b* over five lightness nodes.

**The neutral start** is plain sRGB of the scene up to 0.5, then a shoulder that brings the camera's headroom to the
display white. It is the model's stand-in for the camera's Natural mode, not its curve.

A small look rather than a free 3D table is on purpose: with unpaired sets, statistics cannot tell the film from the
subjects, and a free table would "paint" the subjects. The few parameters have nothing to paint with.

### 5.2. What is compared

A picture enters the statistics with its weight, whatever its size (up to 8000 pixels from a copy 320 pixels on the
long side). The RAW set is rendered through the look, the scans are taken as they are, both in CIELAB:

| Statistic | How |
|---|---|
| tone | 17 lightness quantiles (0.5 ... 99.5 %); compared from 2 to 98 % with the medians aligned |
| cast | mean a*, b* of the near-neutral pixels (chroma below 12) in ten lightness bands |
| chroma by hue | in 12 hue bins of the coloured pixels (chroma above 8): the median and the 80th percentile, where both sets hold at least 1.5 % of their coloured pixels |
| anchors | per class the median of each box, averaged over the boxes |

**The exposure of the comparison.** Unpaired sets say nothing about their relative exposure, so before comparing,
the RAW set is exposed (a gain on the RAW, clipped at the sensor's white, as the camera would) until its median
lightness is the scans' - searched within +-3 stops. That exposure is a value of the comparison, never part of the
look (`--exposure free` turns it off when the sets were shot alike). Because the comparison exposes the RAW set
through the look, a dark curve with more exposure explains the scans as well as a normal one: `estimate` therefore
**pins the grey** - it slides the curve along the exposure axis so that the scene's grey stays at the neutral start's
L* (49.5). The curve's shape is the film's; its place is a convention, so the camera's metering keeps working.

**The estimate** maps the RAW set's lightness quantiles onto the scans' at every node of the curve (smoothed,
non-decreasing), adds the cast difference, splits the chroma ratio into an overall `sat` and smoothed per-hue factors,
and turns the hue nodes near an anchor's colour by the anchor's hue difference (a `neutral` anchor goes into the cast
instead). An anchor pair more than 35 degrees apart in hue or 2x in chroma is not applied: those are different things
or different light. Details: [look-parameters.md](../../apps/isp_neurofit/skill/isp-neurofit/references/look-parameters.md).

### 5.3. From the look to the camera's tables

The look is a target like a `.cube` look in the ISP fitter: the same training set and photo colour prior, the same
recipes (default "Best", with the look's own matrix), the same CIEDE2000 cost and optimiser, and the same rounding to
the camera's formats - see [fitter section 7](isp_fitter.md#7-the-fitting-model) for the model, the recipes and
black-and-white looks, and [section 8](isp_fitter.md#8-the-photo-colour-prior-how-the-weights-were-obtained) for the
prior. A look without colour (scans of black-and-white film: `sat` 0) becomes a black-and-white profile
([fitter, 7.4](isp_fitter.md#74-black-and-white-looks)); `fit` says so. The boards and the measures are computed from
the rounded tables, as the camera would use them.

## 6. The measures, and when to stop

`fit` prints and `metrics.json` keeps (field by field:
[run-files.md](../../apps/isp_neurofit/skill/isp-neurofit/references/run-files.md)):

| Measure | What it is |
|---|---|
| `tone_dL` | the mean lightness quantile difference, 2-98 %, medians aligned (L*) |
| `cast_dab` | the mean a*b* difference of the near-neutral pixels over the lightness bands both sets fill |
| `chroma_log2` | the mean absolute log2 chroma ratio over the hue bins both sets fill; `chroma_ratio_by_hue` per bin (above 1: the scans are more saturated) |
| `score` | `tone_dL + cast_dab + 10 x chroma_log2` - only to compare the rounds of one run |
| anchors | per class `dE_ab`, `dhue_deg`, `chroma_ratio`, `dL` |
| `tables_vs_look_dE2000` | what the camera's tables lose of the look on the RAW pixels (the fitter's error); a mean up to about 1.5 is normal |
| `noise_floor` | the same measures between two random halves of the scans (the median of 8 splits; needs 6 scans or more) |

The **noise floor** is what the measures show for two sets of the same film: fitting closer than that fits the choice
of pictures, not the film. Stop when the measures are near the noise floor and the eye finds no systematic difference
on the boards, or after two rounds without improvement, or after about 8 rounds. A large `tables_vs_look_dE2000` with a
small `look_vs_scans_score` means the look asks for more than the camera's colour path can do: soften the look or
accept the loss.

## 7. Limits and caveats

- **Unpaired sets.** Without pairs, the difference between the sets is the film plus the difference of their subjects,
  light and exposure. The composition of the sets dominates the result: autumn leaves in the RAW set against city and
  snow in the scans make the look cold and pale. The same kinds of scenes and light in both sets, and anchors, are what
  make the difference the film's.
- **Hue turns need anchors.** Histograms of unpaired sets do not show a turn of hue; without anchors only saturation by
  hue is estimated, and that is unreliable when the subjects' colours differ (red paint in the scans against brown
  leaves in the RAW set).
- **The scanner and the laboratory are part of the look.** Scans are taken as finished positives in their ICC profile
  (or sRGB); one batch from one laboratory is better than a mix.
- **Black-and-white film**: how colours turn into grey cannot be told from unpaired sets; the profile is the camera's
  luma with the film's tone (and toning).
- **The camera model**: RAW files are developed by the colour model of the OM-5 Mark II (its white balance, matrix and
  metering, [fitter, 7.1](isp_fitter.md#71-what-the-cameras-colour-path-offers)). RAW files of other cameras are read
  but rendered wrongly: `analyze` and `check-raw` name them. The model is not the camera: it differs from the camera's
  JPEGs by a median CIEDE2000 of about 1.3
  ([color-pipeline.md, section 6](../platform/om5m2/color-pipeline.md#6-camera-against-the-model)).
- **The neutral start is not Natural**: a stand-in with a similar tone.
- **Sizes**: fewer than 8 RAW files or 12 scans make the statistics follow single scenes; 20-40 each is comfortable.

## 8. Privacy

Everything runs on your computer and nothing is sent anywhere. The pictures are only read; a run keeps small copies
(320 pixels on the long side) in its `cache/` folder and contact sheets and boards made from them, inside the
workspace you chose - outside the OMKit checkout, so that they cannot be committed by accident. The `.isp` file holds
no pictures: tables, the name, the cassette and the `META` lines. Delete a run's folder when you no longer need it.

## 9. Troubleshooting

| Symptom | What to do |
|---|---|
| `error: the workspace ... is inside the OMKit checkout` | choose a folder outside it: `--workspace DIR` or `ISP_NEUROFIT_WORKSPACE` (section 2) |
| `reading camera RAW files needs the rawpy package` | `python -m pip install rawpy` |
| `NOT OM-5MarkII` in the output of `analyze` | those RAW files are of another camera: set `use: false` for them |
| a RAW file in `unreadable` | not an ORF the reader understands, or a damaged file |
| `no look.json in the run` / `no selection.json` | run `init` / `analyze` first |
| `look.json: tone_L: the tone curve must not decrease` | a hand edit broke the look: fix it, or `restore --iter N` |
| `--file NAME: the name of the bundled FILMLOOK preset ...` | take another 8.3 name (`--force` only if you mean to replace it) |
| `the label does not fit the cassette: drawn as ...` | a shorter label, or accept the narrowed text |
| the RAW pictures on a board look much brighter or darker than shot | normal: boards show them at the exposure of the comparison (`compared_at_ev`); `stages.jpg` shows them as shot |
| the look gets dark as shot | the grey was not pinned (an edit, or `--no-pin`): `pin-grey` |
| the result follows the subjects (cold, pale, odd hues) | the sets differ in composition: drop pictures that do not compare, add anchors, add pictures of the missing kind |
| the skill's wrapper says the checkout is not found | install the skill again from the checkout (section 3.1), or set `OMKIT_APPS` to the checkout's `OMKit/apps` |
| the full fit takes many minutes | normal on the CPU; use `--quick` for the rounds, a GPU for the final fit |
