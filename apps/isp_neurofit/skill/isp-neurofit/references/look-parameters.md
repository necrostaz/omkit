# look.json: the look's parameters

`look.json` in the run's folder is the look being fitted. `init` writes the neutral start, `estimate` moves it towards
the scans' statistics, and it can be edited by hand. `fit` copies it into `iter_NN/look.json`; `restore --iter N`
brings an iteration's back. The tool refuses a `look.json` with missing keys, wrong list lengths or a decreasing tone
curve.

The look works on the scene (linear light, middle grey 0.18, the camera's headroom above the scene's white kept):

1. the **tone curve**, per channel: a neutral lands exactly on it; colours lose chroma towards black and white, as on
   film;
2. in CIELAB of the result: the chroma times `sat`, `sat_L(L)` and `hue_sat(h)`; the hue plus `hue_shift(h)`; the
   lightness plus `hue_lum(h)` - the three hue operations fade out towards the neutral axis (below chroma 10); then the
   cast `cast_a(L)`, `cast_b(L)` is added.

| Key | Size | Meaning |
|---|---|---|
| `tone_ev` | 25 | the exposure nodes, stops from middle grey: -8 ... +4 (do not change) |
| `tone_L` | 25 | the display L* of a neutral at those nodes. Contrast: the slope around 0 EV; black: the first nodes; the shoulder: the nodes above +1.5. Must not decrease |
| `cast_L` | 5 | the lightness nodes 0, 25, 50, 75, 100 (do not change) |
| `cast_a`, `cast_b` | 5 | a* and b* added at those lightness nodes: +a magenta, -a green, +b yellow, -b blue. Usually within +-6 |
| `sat` | 1 | overall saturation (0 - black and white) |
| `sat_L` | 5 | a saturation factor at the lightness nodes |
| `hue_deg` | 12 | the hue nodes, CIELAB hue every 30 degrees (do not change) |
| `hue_sat` | 12 | a saturation factor per hue node |
| `hue_shift` | 12 | a hue turn per node, degrees |
| `hue_lum` | 12 | a lightness offset per node, L* |
| `notes` | - | what was done (not part of the look) |

The hue nodes, CIELAB hue angle: 0 red-magenta, 30 red, 60 orange (skin 40-70), 90 yellow, 120-150 foliage green,
180-210 cyan, 240-270 sky and blue, 300-330 violet and magenta. A colour is addressed by its hue **before** the look's
own hue changes. Change neighbouring nodes smoothly.

## Sensible limits

To avoid fitting noise and breaking the camera's tables:

- the tone slope in the middle at most about 2x the start's;
- black not above L* 12, white not below 88, unless the scans clearly are so;
- `hue_shift` within +-25 degrees; `hue_sat` 0.5-1.6; neighbouring nodes differ by at most 0.3 and 15 degrees;
- no change of a single hue without an anchor and without that colour in both sets: nothing can confirm it.

After a hand edit of `tone_L`, run `pin-grey` when the middle moved: it slides the curve so that the scene's grey stays
where the neutral start puts it (the curve's shape is kept).

## What the estimate does

- **tone**: the lightness quantiles of the RAW set through the current look are mapped onto the scans' quantiles; the
  correction is read at every node of the curve, smoothed and made non-decreasing. The RAW set is first exposed to the
  scans' median (unless `--exposure free`), so only the shape follows the scans; then the grey is pinned (unless
  `--no-pin`);
- **cast**: the mean a*, b* of the near-neutral pixels (chroma below 12) in ten lightness bands, scans minus RAW;
- **sat**: the chroma ratio (median and 80th percentile) in the 12 hue bins that both sets fill with at least 1.5 % of
  their coloured pixels; the weighted mean goes into `sat`, the rest, smoothed over neighbours, into `hue_sat`. Scans
  with a mean chroma below 3 are black and white: `sat` 0;
- **anchors**: per class the median of each box, averaged over the boxes. `neutral` goes into the cast near its own
  lightness; the others give a hue turn and a chroma ratio to the hue nodes near the object's colour. The anchor's
  lightness is not carried over (it belongs to the tone and the exposure). A pair more than 35 degrees apart in hue or
  2x in chroma is not applied: those are different things or different light.
