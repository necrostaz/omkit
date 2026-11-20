# The files of a run

A run is the folder `<workspace>/<name>`. Every command prints the paths it wrote.

| File | Written by | What it is |
|---|---|---|
| `selection.json` | `analyze` (keeps earlier choices), edited by hand | the pictures, their use, weights, tags and crops, and the anchors |
| `analysis.json` | `analyze` | per-picture statistics, the sets compared, the noise floor, hints |
| `contact_raw_NN.jpg`, `contact_scan_NN.jpg` | `analyze` | contact sheets: RAW files through the neutral start (`R1`, `R2`...), scans as they are (`S1`...) |
| `cassettes.png` | `cassettes` | candidate cassette pictures with the label |
| `preset.json`, `cassette.png` | `preset` | the profile's name, 8.3 file name and cassette; the chosen cassette as the camera draws it |
| `anchors.jpg` | `anchors` | the anchor boxes drawn on their pictures |
| `look.json` | `init`, `estimate`, `pin-grey`, `restore`, edited by hand | the look being fitted ([look-parameters.md](look-parameters.md)) |
| `iter_NN/` | `fit` | one iteration: `look.json`, `hardware.json` (the fitted tables), `<FILE>.ISP`, `board_NN.jpg`, `stages.jpg`, `metrics.json` |
| `history.json` | `fit` | one line per iteration: the measures and the note (`status` prints it) |
| `cache/` | `analyze` and the rest | small copies of the pictures as arrays (safe to delete) |

## selection.json

```json
{
 "raw_sources": ["/path/to/raw"], "scan_sources": ["/path/to/scans"], "recursive": false,
 "raw": [{"file": "P1010001.ORF", "path": "/path/to/raw/P1010001.ORF", "use": true, "weight": 1.0, "tags": ["street"]}],
 "scans": [{"file": "scan01.jpg", "path": "/path/to/scans/scan01.jpg", "use": true, "weight": 1.0, "tags": ["street"],
            "crop_lrtb": [0, 0, 0, 0]}],
 "anchors": [{"set": "raw", "file": "P1010001.ORF", "class": "neutral", "box": [0.40, 0.70, 0.55, 0.85]},
             {"set": "scan", "file": "scan01.jpg", "class": "neutral", "box": [0.10, 0.80, 0.30, 0.95]}]
}
```

- `file` is the picture's key (its file name, or with folders in front when two folders hold the same name); anchors
  name pictures by it. `path` is where it is read from. Do not edit the sources and paths by hand: run `analyze` with
  other `--raw` / `--scans` instead.
- `use` (true / false), `weight` (0..1), `tags` (the first is the kind of scene), `crop_lrtb` (scans: fractions of the
  frame cut left, right, top, bottom) - edit freely, then run `analyze` again so that the hints and the noise floor
  follow.
- `batch` (scans of several folders): the folder's name, for comparing batches (`scan_batches` in `analysis.json`).
- `box`: x0, y0, x1, y1 as fractions of the frame from the top left corner, after rotation and (scans) after the crop.

## metrics.json (one iteration)

| Key | Meaning |
|---|---|
| `isp_vs_scans` | the RAW set through the quantised tables against the scans: `tone_dL` (mean lightness quantile difference 2-98 % after aligning the medians, L*), `key_dL` (the medians' difference: the sets, not the film), `cast_dab` (near-neutral a*b* difference over the common lightness bands), `chroma_log2` (mean abs log2 chroma ratio over the common hue bins), `chroma_ratio_by_hue`, `score` = tone + cast + 10 x chroma (only to compare rounds of one run) |
| `look_vs_scans_score` | the same score for the look itself, before the tables |
| `compared_at_ev` | the exposure the RAW set was given to reach the scans' median: a value of the comparison, not of the look |
| `tables_vs_look_dE2000` | what the tables lose of the look on the RAW pixels: mean and 95th percentile |
| `anchors`, `anchors_dE_ab_mean` | per class: Lab of both sets, `dL`, `chroma_ratio`, `dhue_deg`, `dE_ab`, `dE2000` |
| `noise_floor` | the same measures between two random halves of the scans (median of 8 splits; needs 6 scans or more) |
| `mono` | `neutral` or `toned` when the fitter took the profile as black and white |
