# OMKIT.INI: settings reference

`OMKIT\OMKIT.INI` on the memory card holds the settings of `core` and of the modules. This page lists every key `core`
reads (core 1.27), what it does, its default and its range, and the rules of the file itself. The settings of a module
are on that module's page (section 7).

**Build kinds.** `core` is built as a release or as a debug build ([build.md](build.md#35-release-and-debug-builds));
releases and their cards are release builds. The keys, values and sections marked **debug build only** below exist only
in a debug build. A release build does not read them: a key of `[core]` gets the usual warning `config: [core] KEY
unknown`, `log_level = debug` gets `config: log_level = debug is only in a debug build, using info`, and the sections
`[debug]` and `[shoot]` get one line for all their keys - `config: N keys are only in a debug build and are not read (the
first: [debug] enabled)`; `[optics.A]`..`[optics.F]` count as such sections. Nothing else happens.

## 1. Where the file comes from

- `core` reads `OMKIT\OMKIT.INI` once, at power-on. A change takes effect at the next power-on.
- **A release and the card bundle (`make -C OMKit card`) carry an `OMKIT.INI`**: the sample of section 8 with the
  bundled modules on, so they run from the first start with their default settings. The file is made from the sample
  in `core`'s source by `tools/cardini.py`; its `[modules]` section ends with a comment and the lines
  `CREATPRF = on`, `FILMLOOK = on` (section 4). Every other line is the sample's.
- If there is no such file, `core` writes the sample (section 8) and uses it. If the card cannot be written, the defaults
  are used and the log says so.
- **The sample `core` writes loads no modules.** Its `[modules]` section only says `enabled = on`; a module runs only
  after a line `NAME = on` is added for it (section 4). So deleting the file goes back to the defaults of `core` but
  turns the modules off; copy the release's `OMKIT.INI` back instead to return to the release's defaults.
- Updating OMKit with a newer release keeps your own `OMKIT.INI` (the install guide of the platform, for example
  [platform/om5m2/fw1100/install.md](platform/om5m2/fw1100/install.md#6-updating-omkit)).

The first lines of each power-on in `OMKIT\LOG\CORE.LOG` (after its header line `======== OMKit core ...`) say which case it
was:

| Log line | Meaning |
|---|---|
| `config OMKIT.INI` | the file was read |
| `config: no OMKIT.INI, template written, defaults used` | there was none; the sample is now on the card |
| `config: no OMKIT.INI and cannot write template, defaults used` | there was none and the card could not be written |

## 2. File rules

| Rule | Detail |
|---|---|
| Encoding | plain ASCII; a UTF-8 byte order mark at the start is skipped. Lines end in CR LF or LF |
| Size | at most 8192 bytes are read; the rest is ignored with the warning `config: OMKIT.INI longer than 8192 bytes, the rest is ignored` |
| Entries | at most 128 `key = value` lines in the whole file; further ones are counted as lines not understood |
| Sections | `[name]` on a line of its own; spaces inside the brackets around the name are ignored. A key belongs to the last section above it; a key above every section belongs to none and is never read. A section may appear more than once |
| Keys | `key = value`; spaces around the key and the value are trimmed. Keys and section names are case-insensitive |
| Comments | a line whose first non-space character is `;` or `#`. There are **no comments at the end of a line**: `log = on ; text` gives the value `on ; text`, which is not understood (the default is taken) |
| Repeats | the last value of a key wins |
| Empty value | `key =` is present with an empty value: a number or switch takes its default; a text key gets the empty text |
| Bad lines | a line that is neither a comment, a section nor `key = value` is skipped; the log says `config: N lines not understood, first at line L` |

### 2.1. Value types

| Type | Accepted | Anything else |
|---|---|---|
| switch | `on`, `yes`, `true`, `1` and `off`, `no`, `false`, `0`, any case | the key's default |
| number | decimal, or hexadecimal with `0x`; an optional `-` | the key's default |
| size | a decimal number of bytes, optionally followed by `K` (x 1024) or `M` (x 1048576), any case: `256K`, `1M`, `65536` | a warning in the log; see the key |
| text | as written, trimmed | - |

## 3. `[core]`

| Key | Type | Default | Range | Effect |
|---|---|---|---|---|
| `log` | switch | `on` | | `off` - nothing is logged and no log file is written. The log `OMKIT\LOG\CORE.LOG` holds the power-ons one after another, each from a header line; `core` never removes it - delete it on a computer when it gets too long. (`run_logs`, a file per power-on, is gone; the key is ignored) |
| `log_level` | text | `info` | `error`, `warn`, `info`, `off`; `debug` - debug build only | how much detail goes to the log. `info`: start, modules, what the user chose, power states, warnings and errors - a few lines a session; `debug` adds the lines that repeat with use (one per picture, per dial step, per second in an editor; counters) and also writes a few boot lines to the card at once, so a crash during start leaves a trace. An unknown value: `info`, with the warning `config: log_level '...' unknown, using info` |
| `fw_heap` | switch | `off` | | `on` - when the module memory (the arena) is full, small module buffers may come from the camera's firmware heap, up to `fw_heap_limit` in total. The heap is shared with shooting and the menus: turn it on only if a module reports that it is out of memory. Needs `fw_heap_limit`, otherwise it stays off with a warning. Code and resources never go there, and `core` always leaves at least a quarter of each heap class to the camera |
| `fw_heap_limit` | size | `256K` in the sample, none without it | | the most the modules may take from the firmware heap. Not a size: a warning, and `fw_heap` stays off |
| `core_events` | switch | `on` | | **debug build only** (a release build always waits on the event flag). How the `core` task waits between its passes: `on` - on an event flag that the power-off and sleep hooks set, so it reacts at once; `off` - a plain 250 ms sleep. A diagnostic switch |
| `power_rescue` | switch | `on` | | **debug build only** (a release build keeps card writes off while the lever is off and does nothing else). Not in the sample. When the power lever is off but the camera does not switch off because it is stuck in a half-press or a transition (the shutter released, nothing being written, 8 s), `core` saves the log and the stores once and asks the firmware for its own power-off with a restart; the camera then starts with the lever off and switches off. If that power-off does not begin, the watchdog resets the camera. `off` - the camera stays on until the battery is taken out. Whatever this key, `core` writes nothing to the card while the lever is off and the camera still runs |

The `[core]` keys below are **diagnostics, debug build only**, not in the sample:

| Key | Type | Default | Range | Effect |
|---|---|---|---|---|
| `taskdump` | switch | `on` | | the task dump: when a power-off or sleep does not happen, or the camera stays in a state that normally lasts a moment, `core` writes the state of every firmware task to the log (at most 8 dumps a power-on). It writes nothing without such a trigger |
| `taskdump_every` | number, seconds | `0` (off) | 0..3600 | also a dump every N seconds (at least every 10 s) |
| `taskdump_base` | number, seconds | `0` (off) | 0..3600 | also one dump N seconds after power-on, for comparison |
| `taskdump_stuck` | number, seconds | `6` | 0..600 (0 off) | how long the camera may stay in a passing state before it counts as stuck |
| `fw_cache_inv` | switch | `off` | | for the camera's hooks of the picture mode screen: use the firmware's own cache invalidation instead of the default way. Not tried on the camera; leave it off |

A number out of range is replaced by the default, with the warning `config: [core] KEY = N out of 0..MAX, using DEF`. A
key in `[core]` that `core` does not know gets the warning `config: [core] KEY unknown` (a typo shows up this way).

## 4. `[modules]`

| Key | Type | Default | Effect |
|---|---|---|---|
| `enabled` | switch | `on` | `off` - no module is loaded at all |
| `NAME` | switch | (none) | `NAME = on` loads the module `OMKIT\MODULES\NAME.OMM`; `off` or no line - it is not loaded |

- The order of the `NAME` lines is the load order.
- A module name is 1-8 characters `A-Z 0-9 _`, not `CORE`. At most 16 modules.
- A value other than a switch value gets the warning `config: [modules] NAME = value, expected on or off`, and the module
  is not loaded.
- A module that is listed but has no file: `module NAME: no file MODULES\NAME.OMM`.

Both bundled modules, as in the `OMKIT.INI` of a release and of the card bundle:

```ini
[modules]
enabled = on
CREATPRF = on
FILMLOOK = on
```

## 5. `[grain]`

The grain of the film look's stills. `core` owns the grain; a look module (FILMLOOK) lets the user choose a tile and a
strength per preset ([modules/filmlook/grain.md](modules/filmlook/grain.md)). These keys are read only when a look module
takes the grain service at power-on - for FILMLOOK, when its grain is on and `OMKIT\GRAIN\` holds at least one valid tile.

| Key | Type | Default | Range | Effect |
|---|---|---|---|---|
| `mask` | text | `film` | `film`, `flat`, `off` | how the grain's strength follows the tone of the picture: `film` - weaker in the shadows and highlights, as on film (about half of the mid-tone grain in the deep shadows, a little more in the highlights); `flat` - the same in every tone; `off` - the same in every tone by an older route of the camera, the fallback if `film` or `flat` give trouble. An unknown value: `film`, with a warning |
| `chroma` | number | `0` | 0..1024 | colour grain: noise in the colour of colour presets, weighted like the grain. 0 - none. Never on a black-and-white preset (the rule: [formats/isp.md](formats/isp.md#51-black-and-white-blocks)). At 100 the colour deviation is about 4 levels. There is no menu item for it |

The keys below are **diagnostics, debug build only**, not in the sample (a release build takes `film` for `mask =
step` or `list`, with a warning, and does not read the others):

| Key | Type | Default | Range | Effect |
|---|---|---|---|---|
| `mask` values `step`, `list` | | | | `step` - no grain below the middle grey, full grain above it (a test of the mask); `list` - the mask route without the grain blend: stills have no grain |
| `mask_test` | text | empty | a list of up to 8 of `off`, `list`, `flat`, `step`, `film`, `chroma`, separated by commas or spaces | a test over power-ons: one level a power-on, each must pass `mask_jobs` grain stills; a power-on that ends without a power-off after a level was armed (a reset of the camera) is recorded against that level. The state is kept on the card (store `GRAIN`) and reported in the log; after the list the camera uses `mask` |
| `mask_jobs` | number | `4` | 1..64 | grain stills a level of `mask_test` must pass |
| `mask_run` | number | `1` | | a run number of `mask_test`: another number starts the list again |
| `chroma_test` | number | `100` | 0..1024 | the colour grain of the `chroma` level of `mask_test` |
| `level_test` | number | `0` | 0..3 | a test of the grain stills without choosing a strength in the camera: a preset with a tile and the strength Off gets this strength (1 Low, 2 Med, 3 High) on its stills; the grain item still shows Off. 0 - none |

A number out of range is replaced by the default with the warning `grain: KEY = N out of LO..HI, DEF taken`. How the mask
and the colour grain work in the camera: [platform/om5m2/grain.md](platform/om5m2/grain.md).

### 5.1. `[debug]` (debug build only)

The debug service of `core` 1.40 for developers: a log in RAM that survives a reset, read over USB in the MTP mode,
optionally no card writes by `core` and file updates over USB. **Off by default**, not in the sample and not in a
release's `OMKIT.INI`. With `enabled = on`, anyone who connects the camera by USB can read its RAM and the files of the
`OMKIT` and `DCIM` folders; with `write = on` also replace the files of `OMKIT`. The keys (`enabled`, `card_writes`,
`write`, `ring_level`, `tasks`, `window_ms`, `window_every`, `stuck_ms`), the security notes and the computer's tool:
[debug.md](debug.md). An unknown key in `[debug]` gets the warning `config: [debug] KEY unknown`.

### 5.2. `[optics]`

Bloom and Halation on the stills of the film look: a glow around bright places (a diffusion filter) and the red glow of
film around lamps. `core` makes them; a look module (FILMLOOK) lets the user choose the strength of each per preset - the
rows Bloom and Halation in the look's column, 0 (off) to 5 ([modules/filmlook/optics.md](modules/filmlook/optics.md)). A
still whose preset has both at 0 is made as without the layer. The section is read at power-on. **On by default**:
without an `[optics]` section the values below are taken - the field-tested set, the one the sample spells out (section
8). What each key does to the picture and when to change it: [modules/filmlook/optics.md section
6](modules/filmlook/optics.md#6-settings-optics).

| Key | Type | Default | Range | Effect |
|---|---|---|---|---|
| `layer` | switch | `on` | | `off` - no Bloom and Halation at all: nothing is installed, and the look's column has no Bloom and Halation rows |
| `bloom_threshold`, `halation_threshold` | number | `180`, `240` | 0..254 | the code of the picture's brightness where the glow starts |
| `bloom_sigma`, `halation_sigma` | number | `90`, `45` | 0..1000 | the glow's size: the Gaussian's sigma in pixels of the picture (a sigma below the layer's smallest, about 35, comes out as that, one above about 510 as about 510, each with a warning) |
| `bloom_sigma2`, `bloom_mix`, `halation_sigma2`, `halation_mix` | number | `0` | 0..1000, 0..100 | a second Gaussian and its weight in percent |
| `bloom_gain`, `halation_gain` | number | `110`, `220` | 0..5000 | the light added at strength 5 at 100 % of the steps, percent of the source |
| `bloom_steps`, `halation_steps` | five numbers | `20, 32, 50, 71, 100` and `10, 19, 34, 80, 140` | 0..200 each | the strengths 1..5 in percent of the gain. Not five numbers: a warning, the default |
| `bloom_dark`, `halation_dark` | number | `80`, `100` | 0..100 | percent less of the glow on places that are bright themselves |
| `bloom_tail`, `bloom_tail_sigma` | number | `50`, `400` | 0..90, 0..1000 | Bloom's long tail: the percent of its light in a wide Gaussian of that sigma |
| `bloom_haze` | number | `0` | 0..25 | percent of all the light (not only above the threshold) in the tail: shadows lift, contrast falls. With a tail only |
| `bloom_toe`, `bloom_max`, `halation_toe`, `halation_max` | number | `0`, `38`, `3`, `60` | 0..50 (toe), 0..200 (max) | the strength's curve after the blur, percent of white: the toe takes a weak glow away, the ceiling holds a strong one (0 - none) |
| `bloom_colour` | number | `300` | 0..400 | Bloom's colour: percent of the colour of its sources (0 - neutral) |
| `halation_cb`, `halation_cr` | number | `-24`, `42` | -200..200 | the halation's colour where it is strong (orange), percent |
| `halation_edge_cb`, `halation_edge_cr`, `halation_edge_code` | number | `-40`, `75`, `70` | -200..200, 0..255 | its colour at the weak edge (red) and the code of the halation from which the strong colour holds (0 - one colour) |
| `halation_core` | switch as 0/1 | `1` | 0..1 | 1 - a lamp's core stays white |
| `emission`, `bloom_emission`, `halation_emission` | number | `16`, `4`, `32` | 0..64 | the source: a blown pixel stands for this many whites (0 - only the excess over the threshold). Bloom with few whites glows from bright surfaces too, the halation keeps to lamps. `emission`, when given, is the default of the two |
| `halation_first` | 0/1 | `1` | 0..1 | 1 - the halation takes its colour from the place's own light, Bloom is added after it |
| `day_mean`, `day_share` | two numbers each | `95, 125` and `6, 14` | 0..255, 0..100 | a day scene: the picture's mean code from..to, or the percent of it at code 200 and above from..to |
| `day_bloom`, `day_halation` | number | `100`, `100` | 0..100 | percent of the gains on a day scene (100 - the same by day and by night) |
| `white` | text | `auto` | `auto`, `table`, `layer`, `off`, 160..255 | the look's white, the scale of the codes: `auto` - the code the look's tables give a blown pixel, else found in the picture; `off` - the codes as they are |
| `jobs` | text | `first` | `first`, `both` | of a RAW+JPEG press: `first` - only the JPEG gets the layer; `both` - the preview in the ORF too |

A number out of range is taken at the nearest end, with the warning `optics: KEY = N out of LO..HI, D taken`. Each
still of the look with a strength above 0 gets one line in the log: `optics: job N: Bloom B, Halation H, white W, day
D%; the worker T ms, the step waited T ms`, or a warning when the still went without the layer.

The keys below are **debug build only** (development of the layer; a release build takes the field-tested values and
reports them once: `config: [optics]: N keys are not a release build's and are not read (the first: KEY)`):

| Key | Default | Effect |
|---|---|---|
| `route`, `layer_width`, `layer_bytes`, `gamma`, `read`, `read_cached`, `layer_cached` | `pair`, `324`, `012`, `220`, `1`, `1`, `1` | how the layer is carried through the camera's image pipeline and read |
| `bloom`, `halation` | - | 0..5: a test's strengths in place of the preset's rows |
| `schedule`, sections `[optics.A]`..`[optics.F]` | - | a list of test stages and sets of the keys above, one a press |
| `probe`, `bisect`, `bisect_from`, `run`, `worker_test`, `bench` | - | the probe of the pipeline's step, its bisection over power-ons, a test of the worker task, a CPU bench in every job. With `probe` above 0 and no `layer` key the layer is off |

## 6. Keys `core` reads in every module's section

A module's section is named after the module: `[CREATPRF]`, `[FILMLOOK]`. Besides the module's own keys, `core` reads two
kinds of keys there for every module:

| Key | Type | Default | Effect |
|---|---|---|---|
| `memory` | size | no limit | the most memory the module may hold (its image, data, resources, store and allocations). Not a size: `config: [NAME] memory is not a size, no limit` |
| `event_<event>` | `off` or a number | the module's own | `off` disables the module's handlers of that event; a number replaces their priority (higher runs first). Event names: [sdk.md](sdk.md). Anything else: a warning, the module's own priority stays |

These are for developers and troubleshooting; the bundled modules need neither.

## 7. Module settings

| Section | Module | Keys |
|---|---|---|
| `[FILMLOOK]` | film looks in a picture mode slot | `slot`, `name`, `presets`, `scope`, `art`, `enable_edits`, `save_preset_edits`, `grain`, `grain_tile`, `optics`, `exif`: [modules/filmlook/README.md](modules/filmlook/README.md#5-settings) |
| `[CREATPRF]` | colour and monochrome profiles | `mono_filters`: [modules/creatprf/README.md](modules/creatprf/README.md#4-settings) |
| `[HELLO]` | the example module for developers | `greeting`, `hide`, `fail`: [modules/hello/README.md](modules/hello/README.md) |

## 8. The sample `core` writes

```ini
; OMKit settings. Lines starting with ; or # are comments.
; The file is read when the camera is switched on. Delete it, and core will create this sample again.

[core]
; on | off - whether to write the log OMKIT\LOG\CORE.LOG on the card
log = on
; error | warn | info - how much detail (debug - in a debug build of core only)
log_level = info
; on - if the arena runs out of space, take small module buffers (up to 16K) from the firmware heap.
; Requires fw_heap_limit. The heap is shared with shooting: enable only if the arena is too small for modules
fw_heap = off
fw_heap_limit = 256K

[grain]
; the grain of the film look's stills (a look module such as FILMLOOK picks the tile and the strength).
; film - weaker in the shadows and highlights, as on film; flat - the same in every tone;
; off - the same in every tone by the older route (the fallback if film or flat fail)
mask = film
; colour grain on colour presets, 0..1024 (0 - none; never on black and white presets)
chroma = 0

[optics]
; Bloom and Halation on the film look's stills. The strength of each is the preset's Bloom and Halation row
; in the look's column: 0 - off, 1..5. off here - no Bloom and Halation at all
layer = on
; The values below are the defaults. The glow starts at the code *_threshold of the picture (0..254);
; *_sigma - its size in pixels of the picture; *_gain - the light added at strength 5, percent
; Bloom: a diffusion filter. bloom_tail percent of its light in a long tail of bloom_tail_sigma pixels;
; bloom_dark - percent less on bright places; bloom_max - at most this percent of white;
; bloom_colour - percent of the colour of its sources (0 - neutral)
bloom_threshold = 180
bloom_sigma = 90
bloom_gain = 110
bloom_dark = 80
bloom_tail = 50
bloom_tail_sigma = 400
bloom_haze = 0
bloom_toe = 0
bloom_max = 38
bloom_colour = 300
; Halation: the red glow around lamps. halation_cb, halation_cr - its colour where it is strong,
; halation_edge_cb, halation_edge_cr - at its weak edge, the strong colour from the code halation_edge_code;
; halation_core = 1 - a lamp's core stays white; halation_toe, halation_max - percent of white
halation_threshold = 240
halation_sigma = 45
halation_gain = 220
halation_dark = 100
halation_cb = -24
halation_cr = 42
halation_edge_cb = -40
halation_edge_cr = 75
halation_edge_code = 70
halation_core = 1
halation_toe = 3
halation_max = 60
; The source: a blown pixel stands for this many whites (0 - only the excess over the threshold);
; emission - for both when given, else each its own
emission = 16
bloom_emission = 4
halation_emission = 32
; 1 - the halation takes its colour before Bloom is added
halation_first = 1
; The strengths 1..5 in percent of the gains
bloom_steps = 20, 32, 50, 71, 100
halation_steps = 10, 19, 34, 80, 140
; A day scene (the picture's mean code from..to, or the percent of it at code 200 from..to) may take
; day_bloom and day_halation percent of the gains; 100 - the same by day and by night
day_mean = 95, 125
day_share = 6, 14
day_bloom = 100
day_halation = 100
; auto - the codes on the scale of the look's white; off - as they are; 160..255 - a fixed white
white = auto
; first - of a RAW+JPEG press only the JPEG gets the layer; both - the preview in the ORF too
jobs = first

[modules]
; off - do not load any modules
enabled = on
; NAME = on | off: module from the file OMKIT\MODULES\NAME.OMM, its files are in the folder OMKIT\NAME.
; Line order is load order. Module settings are in its own section [NAME]
; for example: CREATPRF = on
```

(The sample is the same in both build kinds.)

In the `OMKIT.INI` of a release and of the card bundle, the `[modules]` section goes on with the bundled modules, in
the order of `RELEASE_MODULES` in `OMKit/Makefile` (the load order):

```ini
; ON: the bundled modules of this OMKit card (the example above is only a comment; without this file, core writes its sample, which loads none)
CREATPRF = on
FILMLOOK = on
```

## 9. A complete example

Both bundled modules, the film look in the place of i-Enhance, a little colour grain, a log with warnings and errors
only:

```ini
[core]
log = on
log_level = warn

[grain]
mask = film
chroma = 60

[modules]
enabled = on
CREATPRF = on
FILMLOOK = on

[CREATPRF]
mono_filters = alternative

[FILMLOOK]
slot = ienhance
name = Film Look
grain_tile = CLASS400
```

## 10. `[shoot]`: the test scenario (debug build only)

A series of pictures without the user, for testing presets: `core` presses the shutter button itself - by the button's
own path in the camera, half-press first, then the full press - and before each step chooses a preset the way the user
chooses it in the picture mode screen, so the module that owns it (FILMLOOK, CREATPRF) applies it as usual. The camera's
own settings (exposure mode, focus, quality, ISO) stay as set on the camera. Not in the sample: a test tool, off unless
asked for. The camera does not take pictures while it is connected to a computer for data (Storage, MTP): every start
first waits until the camera is in its shooting state (the cable out, or the USB choice that only charges), up to
`arm_wait`, and the start's delay counts from then.

| Key | Type | Default | Range | Effect |
|---|---|---|---|---|
| `run` | text | `off` | `off`, `boot` | `boot` - the scenario starts once after the modules have loaded (at every power-on) |
| `start_delay` | number | `5000` | 0..600000 | milliseconds from the moment the camera shoots (2 s in its shooting state) to the first step |
| `arm_wait` | number | `600000` | 1000..86400000 | the longest wait for the camera to shoot after the start (a cable in the MTP mode, a menu open); then the scenario ends without a picture |
| `steps` | text | empty | up to 16 steps separated by commas | the presets in order: `look:N` - entry N (from 1) of the film look, `look:NAME` - the entry of that name (as the picture mode screen shows it), `prof:cN` / `prof:mN` - colour or monochrome profile N, `stock` - the camera's own picture mode (the look and the profiles off), `keep` - no change. Empty: one step without a change. An unknown step stops the scenario before the first picture, with an error in the log |
| `shots` | number | `1` | 1..20 | pictures per step (200 in a scenario at most) |
| `interval` | number | `3000` | 500..3600000 | milliseconds between two pictures, from the end of the one before |
| `settle` | number | `1500` | 0..60000 | milliseconds after a change of preset before the next picture |
| `trigger` | text | `keys` | `keys`, `wifi` | `keys` - the shutter button's own path; `wifi` - the events the camera's Wi-Fi remote control sends (for comparison) |
| `half` | number | `1500` | 100..10000 | the longest wait for the half-press (focus); without it the full press follows anyway |
| `hold` | number | `150` | 50..2000 | milliseconds the full press is held |
| `write` | number | `20000` | 2000..120000 | the longest wait for a picture to be made and the camera idle again; three pictures in a row not seen made stop the scenario |

A number out of range is taken at the nearest end. Every press is released on every way out - the end, a time limit,
power-off or sleep, a stop - and a half-press the camera still shows 3 s after the release is released once more. The log
has a line per step and per picture and the result: `shoot: done: N of M pictures made`. A power-off or sleep does not
end the scenario: the camera begins a power-off also when the USB cable is taken out in the MTP mode, and cancels it
because the power lever is on. The scenario releases the keys, waits until the camera shoots again and goes on; a
picture that was under way is taken again.

### 10.1. One scenario at the next power-on: `OMKIT\SHOOT.RUN`

A file `SHOOT.RUN` in the `OMKIT` folder holds `[shoot]` keys for one start: at the next power-on `core` reads it after
`OMKIT.INI` (its keys win over those of `[shoot]`), **removes it from the card** and then starts the scenario as `run =
boot` would. Removed first, the file runs at most once, even if the scenario brings the camera down. Lines of another
section are not used (`shoot: one-shot scenario from SHOOT.RUN (removed from the card), lines not understood in it`);
a file over 1 KB is not used at all. If the file cannot be removed, the scenario does not run (error in the log). The
debug tool writes this file for a test session (`omkdebug.py session --scenario ... --restart`, [debug.md](debug.md)):

```ini
[shoot]
steps = look:Kodak Portra 400, stock
shots = 1
```

### 10.2. Without hands: `hands_free = on` in `SHOOT.RUN`

Two keys are taken **only from `SHOOT.RUN`** (in `OMKIT.INI` they are ignored, with a warning in the log):

| Key | Type | Default | Effect |
|---|---|---|---|
| `hands_free` | on/off | `off` | the scenario runs with the USB cable left in, as if "charge only" had been chosen; a restart at its end |
| `session` | text | empty | a name the computer gives the run (up to 16 characters), written back in the result |

With `hands_free = on`, for that one start:

- **USB is held as charge only.** Early in the start, before the modules, `core` places a hook on the camera's use of
  its USB choice: whatever the choice - the USB dialog's item, or the camera's USB mode setting - the camera takes
  "charge only": it shoots and does not connect to the computer. If the camera shows its USB dialog, `core` presses OK
  and the hook turns the item into "charge only". The camera's settings are not changed.
- **At the end** - the scenario done, failed, stopped or out of time, or the camera's application stuck (30 s after its
  task dump, `[core] taskdump`) - `core` writes the result to `OMKIT\SHOOT.END`, lets USB go and restarts
  the camera: an ordinary power-off for a reboot (the settings saved, the card unmounted) with a wake-up alarm of one
  second, started by a task of the firmware other than the camera's application, so that it works also while the
  application hangs. If that power-off does not begin or stops half way, the watchdog resets the camera (nothing more
  is saved).
- **The next start** connects to the computer in the MTP mode: with the camera's USB mode set to MTP by itself; with
  the USB dialog, `core` answers it once with MTP (`SHOOT.END` asks for it, and `core` takes the request off before it
  answers).
- If the camera has connected to the computer before `core` could hold USB (its USB mode set to MTP or Storage: the
  camera chooses at the card's mount, about when `core` starts), the scenario ends at once, the result says so and the
  camera stays connected. **For a session without hands, leave the camera's USB mode at asking (the dialog).**

`OMKIT\SHOOT.END` has a fixed length of 256 bytes (it is rewritten in place) and `key=value` lines: `session`, `run` (the
power-on that ran the scenario), `build`, `result` (`done`, `failed`, `stopped`), `why`, `taken`, `total`, `failed`,
`usb_hook` (how many USB choices the hook changed), `connect` (`mtp` - the next start answers the dialog with MTP; `none`).
It is written also with `[debug] card_writes = off`: like the removal of `SHOOT.RUN`, it is the computer's request.
`omkdebug.py session --hands-free` writes `SHOOT.RUN` with these keys and reads the result ([debug.md](debug.md#6-a-test-session)).
