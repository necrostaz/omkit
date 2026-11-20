---
name: isp-neurofit
license: Apache-2.0 (part of OMKit; see its LICENSE and NOTICE)
description: Makes an .isp colour profile (an OMKit FILMLOOK preset) for the OM System OM-5 Mark II from two unpaired picture sets - the camera's RAW files (.ORF) and scans of one film stock - so that the RAW files rendered through the profile look like the scans. The agent analyses both sets, picks comparable pictures, marks anchor objects, estimates and fits the look with a command line tool, judges contact sheets by eye and iterates. Use when the user asks to make, fit or match an .isp profile, FILMLOOK preset or film look from film scans and OM-5 Mark II RAW files. Needs an agent that can view image files.
---

# isp-neurofit: an `.isp` profile from film scans

Input: a set of the camera's RAW files (ORF of the OM-5 Mark II) and a set of scans of **one** film stock. The scenes
differ; there are no pairs. Output: an `.isp` file (colour and tone only: matrix, R/G/B gamma, tone curve, saturation,
12 hue sectors; the preset's name and cassette picture) with which the RAW files, through the camera's colour model,
look as much as possible like the scans, and a short report. Grain, sharpness and in-camera edits are not fitted.

Always start from the neutral look. Never start from, copy or peek at an existing preset or profile of that film - not
as a start and not as a hint.

## Before you start

- **Running the tool.** Every command below is written `python scripts/neurofit.py ...`: `scripts/neurofit.py` is in
  this skill's folder - use its absolute path (the folder that holds this SKILL.md). It runs the OMKit checkout the
  skill was installed from. First run `python scripts/neurofit.py doctor`: it checks Python, the packages (numpy,
  Pillow, rawpy) and the workspace, and says what to install when something is missing.
- **The workspace.** Runs live in a workspace folder: `--workspace DIR` on every command that takes `--run`, else the
  environment variable `ISP_NEUROFIT_WORKSPACE`, else `isp_neurofit_runs` in the current folder. It must be outside the
  OMKit checkout (the tool refuses otherwise). If the user named none and the variable is not set, ask where to keep
  the runs in the first question (step 0) and pass `--workspace` every time. A run is the folder `<workspace>/<name>`.
- **Viewing images.** This skill needs you to open and look at image files (JPEG and PNG contact sheets, boards). If
  you cannot view images, say so to the user at once: you can still run the commands, but selection, anchors and the
  judgement of the result must then be done by the user, who opens the sheets the commands name and tells you what
  they see. Do not pretend to have judged a sheet you could not see.
- **Privacy.** The pictures stay on the user's computer. Never upload them, never copy them into the OMKit checkout or
  any repository, never fetch pictures from anywhere else: only the user's own RAW files and scans are used.
- File formats of a run (`selection.json`, `look.json`, `metrics.json`): [references/run-files.md](references/run-files.md).
  The look's parameters and how to edit them by hand: [references/look-parameters.md](references/look-parameters.md).

## Step 0. The request: sets, name, cassette

The request is free text. Work out where the RAW files are, where the scans are, which film it is, and what the user
has already decided (name, cassette, "no questions"). Ask about what is missing and cannot be decided by you. The run's
name: short, ASCII, after the film (`portra400`); do not overwrite an earlier run of that name - take another.

**RAW files.**

- a folder, several folders, single files: `--raw PATH...` (subfolders with `--recursive`);
- "the same RAW files as last time", "as in run X": `--raw-from RUN` takes that run's whole RAW set with its tags,
  weights, RAW anchors and read cache. Runs are the folders of the workspace (list them; "the last one" is the one
  whose `history.json` is newest; ask when unclear). **The choice is not carried over as it is:** review `use` and
  `weight` for the new scans (step 1) - other scans have other light and subjects; check the RAW anchors still fit;
- hundreds of files: do not read them all (about 1 s per RAW) - `--raw-every N` takes every N-th file.

**Scans.** A folder, several folders (`--scans PATH...`, `--recursive`), or a description of what to look for ("all
subfolders starting with ...", "folders with portra 400 in the name"). Search yourself: list the folders, count the
image files in each, and open two or three pictures of a folder whose name does not make clear that it is that film.
Then **show the user what you found and ask once**: the folders with their number of files, year, and anything
suspicious (another film in the name, edited copies, black and white, another scanner). Use only what the user
confirms. One folder the user named explicitly needs no confirmation. After the analysis compare the batches
(`scan_batches` in the output and in `analysis.json`: mean hue, chroma, black level per folder): exclude a batch that
stands out, or give it a lower weight, and say so in the report.

**Name and cassette** - before fitting, so that the rest runs without stops until the report. Take what the request
already says without asking; ask only the rest.

1. Make 2-3 name proposals. Each has three parts: the name the camera shows (`--title`, up to 31 bytes of UTF-8), the
   8.3 file name (`--file`, up to 8 ASCII letters and digits), the label on the cassette (`--label`, ASCII, upper case;
   a long one is narrowed or cut - the command says how) and the ISO in the window. The file name must not be one of the
   kit's bundled FILMLOOK presets or a preset of another run in the workspace (`preset` refuses those); tell the name
   apart by what is special about these scans (place, year, laboratory, push): "Portra 800 Harbour" / `PORTR8HB`.
2. Draw candidate cassettes with that label and look at the sheet yourself:
   ```
   python scripts/neurofit.py cassettes --workspace WS --run NAME --label "PORTRA 800" [--brand KODAK] [--proc C-41] [--ids p_portra,yellow,filmlook]
   ```
   Without `--ids`: the cassette guessed from the label and six plain ones; `--ids all`: all 30. Choose 3-4 that suit
   (a picture of that film if there is one, a plain one by brand, a neutral one) and draw the sheet again with them.
3. Ask: the name (the proposals) and the cassette (the proposals, described in words - colour, stripe; give the path
   of `cassettes.png` in the question so that the user can open it).
4. Record the choice - every later `fit` writes the `.isp` with this name and cassette section:
   ```
   python scripts/neurofit.py preset --workspace WS --run NAME --title "Portra 800 Harbour" --file PORTR8HB --cassette p_portra --label "PORTRA 800" [--iso 800] [--brand KODAK] [--proc C-41]
   ```
   Look at `cassette.png`. If the user does not want to choose a cassette: `preset` without `--cassette` - the camera
   then draws a generic one by the name.

Put all questions of step 0 (the scans found, the workspace if needed, the name and the cassette) into **one combined
question** to the user once the scan search is done.

**Black-and-white film.** The `.isp` has no black-and-white flag: the camera treats a profile as black and white by a
rule (chroma gains 0 or equal matrix rows), and the fitter makes such a profile by itself when the look has no colour.
Your part: `estimate` sets `sat` to 0; ask the user whether the result should be neutral or toned (toning is `cast_a`,
`cast_b`; for neutral set them to 0 - leftovers of a few tenths make it look toned); no anchors; a black-and-white
cassette. `fit` prints `black-and-white neutral` or `toned` when the fitter took the profile as such - report it.
How colours turn into grey cannot be told from unpaired sets - say so.

## What is your work here, and what is the numbers'

The numbers (set statistics, the fitter) are reproducible but blind: they do not know what a picture shows and take a
difference of subjects for a property of the film. Eyes are needed for three things, and this skill exists for them:

1. **selection** - what in the two sets is comparable and what is not;
2. **anchors** - the same kind of object in both sets (skin, sky, foliage, a grey thing);
3. **judgement** - does the result look like the film, and what exactly is off.

Do not replace a look by a number or a number by a look: change something when the sheet and the measure agree. When
they disagree, find out why (usually the composition of the sets) and write it into `--note`.

## Step 1. Analyse the sets

```
python scripts/neurofit.py analyze --workspace WS --run NAME --raw "RAW_FOLDER" --scans "SCANS_FOLDER"
```

Sets as decided in step 0: several paths separated by spaces, `--recursive`, `--raw-from RUN` instead of `--raw`,
`--raw-every N`. Running `analyze --run NAME` again without paths takes the same sets and keeps the choices made. A
picture is named in `selection.json` and in the anchors by its key `file`: the file name, or, when two folders hold the
same name, with the folder in front (`folder/name.jpg`).

Read the output (`hints`), `analysis.json`, and **open every contact sheet** `contact_raw_NN.jpg`, `contact_scan_NN.jpg`
(the RAW files on them are rendered through the neutral start). For every picture decide and write into
`selection.json`:

- `use: false` - rejects and strangers: scans of another film or with a clearly different processing (stands out by
  its hue from the rest), an uninverted negative, cross-processing, a badly faded slide, a scan with dust and a frame
  over half of it, black and white among colour; RAW files with half the frame blown out, test charts, shots of a
  screen, mixed light with a strong cast, and RAW files of another camera (`analyze` names them: `NOT OM-5MarkII`).
  Duplicates and series of one subject - keep one or two or lower their `weight`, or the subject outweighs the set;
- `tags` - the first tag is the kind of scene, the same words in both sets: `portrait`, `landscape`, `street`,
  `interior`, `night`, `snow`, `foliage`... (the boards are ordered by the first tag). Replace the automatic tags
  (`low_key` and the like);
- `crop_lrtb` - fractions of the frame to cut left, right, top, bottom, when a scan has a border, sprocket holes or a
  caption;
- `weight` - 0.3-0.5 for doubtful pictures that are a pity to drop.

Then **balance the composition**: the two sets compared must hold the same mix of kinds of scenes and light. If the
scans are all portraits in shade and the RAW files landscapes in sun, there is nothing to compare: keep the overlap and
tell the user what is missing. After the edits run `analyze` again (the choices are kept; hints and the noise floor are
computed again).

## Step 2. Suggest to the user what to add

Once, before fitting, in one question: briefly what is wrong and what exactly to shoot or add. Sources: `hints` and what
you saw yourself. By falling value:

1. a kind of scene or a colour that is in one set and missing in the other, by name ("the scans have no foliage");
2. objects for anchors in both sets: skin, clear sky, foliage, something grey (asphalt, concrete, a white wall, a grey
   card);
3. too few pictures: under 8 RAW files or 12 scans the statistics follow single scenes; 20-40 is comfortable;
4. a different key of the sets (the hint about more than 1 EV) - RAW files of subjects as light or dark as the scans';
5. scans of mixed origin (laboratories, scanners, years) - one batch is better;
6. the ideal, if the user is willing: the same scene on film and with the camera, with a grey card or a colour chart.

Answers to offer: "I will add files" (wait, then `analyze` again) or "work with what there is". If the sets are good,
do not ask - just say so. A refusal is no obstacle: work with what there is and name in the report what is unreliable
because of it.

## Step 3. Anchors

An anchor is a box on a picture with a class: `{"set": "raw" | "scan", "file": "...", "class": "skin", "box": [x0, y0,
x1, y1]}` in `selection.json` under `anchors`; coordinates are fractions of the frame (after rotation; for scans after
`crop_lrtb`). Classes: `skin`, `sky`, `foliage`, `neutral` and your own (`red_brick`, `sand`), the same names in both
sets. Rules:

- only even, evenly lit areas of one material, without highlights and deep shadows; keep the box well inside the
  object (the box's median is taken);
- a class works only when marked in **both** sets; 2-4 boxes per class from different pictures beat one;
- comparable light: skin in sun against skin in sun; sky only clear blue, not haze at the horizon or a sunset;
- skin is the most important anchor and must not be mixed: pale skin in even light (overcast, shade, a window) and
  tanned skin in sun are **different classes** (`skin`, `skin_sun`); the best anchor is the same person in both sets.
  Apply an anchor of pale skin in comparable light fully (hue, chroma, a little lightness through `hue_lum`), smoothly
  over three hue nodes: half strength tends to leave skin a cold pink where the film gives a warm cream;
- `neutral` - what is surely grey or white in life. Not snow in shade, not a "white" shirt under a lamp;
- when in doubt, do not mark. A false anchor is worse than none.

To place a box precisely, open the picture itself (the scan file; for a RAW file its cell on `contact_raw`). Check:

```
python scripts/neurofit.py anchors --workspace WS --run NAME
```

Open `anchors.jpg`: every box lies on its object. The output gives the colour difference per class.

## Step 4. First estimate and fit

```
python scripts/neurofit.py init --workspace WS --run NAME
python scripts/neurofit.py estimate --workspace WS --run NAME
python scripts/neurofit.py fit --workspace WS --run NAME --quick --note "first estimate"
```

`estimate` moves `look.json` towards the scans' statistics: tone (the curve's shape), the cast of the neutrals by
lightness, saturation overall and by hue, then the anchors. `fit` fits the look to the camera's tables and makes
`iter_NN/`: the `.ISP`, boards `board_NN.jpg` (RAW files through the tables **at the exposure of the comparison** on
top, scans below, both by first tag; how many boards: `--boards`), `stages.jpg` (neutral start | the look (target) |
the ISP tables, **as shot**), `metrics.json`. The name, file name and cassette come from step 0's `preset.json`.
`--quick` for the rounds in between (seconds); the full fit takes longer (seconds with a GPU, up to a minute or so
on a CPU).

By default the sets are compared after the RAW set is exposed to the scans' median lightness (`--exposure keep`): the
difference in exposure and key does not go into the look; the curve's shape is fitted. `--exposure free` only when the
sets were shot in the same conditions and the scans' brightness is an intended property of the look; then give the same
flag to both `estimate` and `fit`.

**Grey is pinned.** After estimating the tone, `estimate` slides the curve along the exposure axis so that the scene's
grey (0 EV) stays at the neutral start's L* (`pin-grey` does the same for a look edited by hand): otherwise the estimate
drifts to a dark curve, and the camera applies it to pictures as shot. **People judge a profile at 0 EV, as shot** -
judge the look as shot too (`stages.jpg`), and use the boards at the comparison exposure only to compare the shape of
the tone and the colours. In the report point to the as-shot sheets.

## Step 5. The round: look -> name -> correct

After every `fit` **open** `board_NN.jpg` and `stages.jpg` and note, comparing the same kinds of scenes rather than
picture with picture:

1. contrast and density: the middle, the depth of the shadows, the density of the highlights;
2. black and white: is the black lifted, does white reach white;
3. the cast of the neutrals: shadows, middle and highlights separately (warm highlights, cold shadows);
4. saturation overall and in shadows and highlights;
5. single colours: skin, sky, foliage, reds - hue, saturation, lightness;
6. artefacts: steps in gradients, coloured black, turned colours, posterised saturated areas.

Then the numbers (`metrics.json`, the output of `fit`, `status`):

- `isp_vs_scans`: `tone_dL` (tone shape, L* units), `cast_dab`, `chroma_log2` and `chroma_ratio_by_hue` (above 1 - the
  scans are more saturated), anchors (`dE_ab`, `dhue_deg`, `chroma_ratio`);
- `noise_floor` - the same measures between two random halves of the scans. **Do not fit closer to the scans than
  this**: below it you fit the choice of pictures, not the film;
- `tables_vs_look_dE2000` - how much of the look the camera's tables lose. A mean up to about 1.5 is normal. If it is
  large while `look_vs_scans_score` is small, the look is not to blame but the limits of the camera's colour path:
  soften the look or accept the loss; a change of the target does not cure it.

One change per round - one or two related things, or you cannot tell what helped. Ways:

- **estimate a part again**: `estimate --parts tone` (or `cast`, `sat`, `anchors`), `--strength 0.3...1`, `--rounds`;
- **edit `look.json` by hand** - when the eye sees what the statistics miss. The parameters and their sensible ranges:
  [references/look-parameters.md](references/look-parameters.md);
- **the composition of the sets**: exclude a picture that pulls the statistics (then `analyze` again), add or remove
  an anchor.

After a change - `fit --quick --note "what changed and why"` again. Worse by eye and by the numbers - go back:
`restore --workspace WS --run NAME --iter N`. The history: `status --workspace WS --run NAME`.

Do not fit an isolated hue without an anchor and without that colour in both sets: nothing can confirm it.

**When to stop:** the measures are at the noise floor and the eye finds no systematic difference; or two rounds in a
row without improvement; or 8 rounds. Do not chase the last tenths.

## Step 6. The result

1. The full fit of the best look (without `--quick`), with a copy where the user wants it:
   ```
   python scripts/neurofit.py fit --workspace WS --run NAME --note "final" --out "DEST_FOLDER/NAME.ISP"
   ```
   Look at its sheets: the full fit can differ slightly from the quick one.
2. Open the final `board` and `stages` sheets again and judge honestly: what came across, what did not, and why (no
   data in the sets - the limits of the camera's tables - the camera model).
3. Report to the user, briefly: the path of the `.ISP` and of the sheets; the name, file name and cassette; which sets
   went in (scan folders, where the RAW set came from, how many pictures); what came out (tone, cast, saturation, key
   colours); the numbers against the noise floor; what is unreliable and why; how to improve the sets next time. Say
   that the result is checked on the camera's colour model, not on the camera: the check is the ISP player
   (`OMKit/apps/isp_player/run.py`) and shooting with the preset in the card's `OMKIT/FILMLOOK` folder.

## Do not

- take an existing profile or preset of the film, or peek into one;
- copy the user's files anywhere (a repository, the OMKit checkout) or send them anywhere;
- write to the camera's card or touch the camera - only files in the workspace and where the user asked;
- change the fitter (`ispkit`) for one run: it is the version of all presets. If the fitter needs a change, tell the
  user separately;
- pass off a fit to the composition of the sets as a property of the film: if the sets are not comparable, say so;
- trust the hue and overall saturation statistics when the subjects of the sets differ in colour (autumn leaves in
  the RAW set against city and snow in the scans make the look cold and pale): check against the anchors and the look
  as shot.
