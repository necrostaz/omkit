# OMKIT.INI: settings reference

`OMKIT\OMKIT.INI` on the memory card holds the settings of `core` and of the modules. This page lists every key `core`
reads (core 1.27), what it does, its default and its range, and the rules of the file itself. The settings of a module
are on that module's page (section 7).

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

The first lines of `OMKIT\LOG\CORE.LOG` say which case it was:

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
| `log` | switch | `on` | | `off` - nothing is logged and no log file is written |
| `log_level` | text | `info` | `error`, `warn`, `info`, `debug`, `off` | how much detail goes to the log. `info`: start, modules, what the user chose, power states, warnings and errors - a few lines a session; `debug` adds the lines that repeat with use (one per picture, per dial step, per second in an editor; counters) and also writes a few boot lines to the card at once, so a crash during start leaves a trace. An unknown value: `info`, with the warning `config: log_level '...' unknown, using info` |
| `run_logs` | switch | `off` | | `on` - every power-on also gets its own log `OMKIT\LOG\XXXXXXXX.LOG` (XXXXXXXX: the run id from the first log line) |
| `fw_heap` | switch | `off` | | `on` - when the module memory (the arena) is full, small module buffers may come from the camera's firmware heap, up to `fw_heap_limit` in total. The heap is shared with shooting and the menus: turn it on only if a module reports that it is out of memory. Needs `fw_heap_limit`, otherwise it stays off with a warning. Code and resources never go there, and `core` always leaves at least a quarter of each heap class to the camera |
| `fw_heap_limit` | size | `256K` in the sample, none without it | | the most the modules may take from the firmware heap. Not a size: a warning, and `fw_heap` stays off |
| `core_events` | switch | `on` | | how the `core` task waits between its passes: `on` - on an event flag that the power-off and sleep hooks set, so it reacts at once; `off` - a plain 250 ms sleep. A diagnostic switch |

The `[core]` keys below are **diagnostics**, not in the sample:

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

The keys below are **diagnostics**, not in the sample:

| Key | Type | Default | Range | Effect |
|---|---|---|---|---|
| `mask` values `step`, `list` | | | | `step` - no grain below the middle grey, full grain above it (a test of the mask); `list` - the mask route without the grain blend: stills have no grain |
| `mask_test` | text | empty | a list of up to 8 of `off`, `list`, `flat`, `step`, `film`, `chroma`, separated by commas or spaces | a test over power-ons: one level a power-on, each must pass `mask_jobs` grain stills; a power-on that ends without a power-off after a level was armed (a reset of the camera) is recorded against that level. The state is kept on the card (store `GRAIN`) and reported in the log; after the list the camera uses `mask` |
| `mask_jobs` | number | `4` | 1..64 | grain stills a level of `mask_test` must pass |
| `mask_run` | number | `1` | | a run number of `mask_test`: another number starts the list again |
| `chroma_test` | number | `100` | 0..1024 | the colour grain of the `chroma` level of `mask_test` |

A number out of range is replaced by the default with the warning `grain: KEY = N out of LO..HI, DEF taken`. How the mask
and the colour grain work in the camera: [platform/om5m2/grain.md](platform/om5m2/grain.md).

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
| `[FILMLOOK]` | film looks in a picture mode slot | `slot`, `name`, `presets`, `scope`, `art`, `enable_edits`, `save_preset_edits`, `grain`, `grain_tile`, `exif`: [modules/filmlook/README.md](modules/filmlook/README.md#5-settings) |
| `[CREATPRF]` | colour and monochrome profiles | `mono_filters`: [modules/creatprf/README.md](modules/creatprf/README.md#4-settings) |
| `[HELLO]` | the example module for developers | `greeting`, `hide`, `fail`: [modules/hello/README.md](modules/hello/README.md) |

## 8. The sample `core` writes

```ini
; OMKit settings. Lines starting with ; or # are comments.
; The file is read when the camera is switched on. Delete it, and core will create this sample again.

[core]
; on | off - whether to write the log OMKIT\LOG\CORE.LOG on the card
log = on
; error | warn | info | debug - how much detail
log_level = info
; on - also write a separate log for every power-on, OMKIT\LOG\XXXXXXXX.LOG
run_logs = off
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

[modules]
; off - do not load any modules
enabled = on
; NAME = on | off: module from the file OMKIT\MODULES\NAME.OMM, its files are in the folder OMKIT\NAME.
; Line order is load order. Module settings are in its own section [NAME]
; for example: CREATPRF = on
```

In the `OMKIT.INI` of a release and of the card bundle, the `[modules]` section goes on with the bundled modules, in
the order of `RELEASE_MODULES` in `OMKit/Makefile` (the load order):

```ini
; ON: the bundled modules of this OMKit card (the example above is only a comment; without this file, core writes its sample, which loads none)
CREATPRF = on
FILMLOOK = on
```

## 9. A complete example

Both bundled modules, the film look in the place of i-Enhance, a little colour grain, a log with more detail:

```ini
[core]
log = on
log_level = debug

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

## 10. `[shoot]`: the test scenario

A series of pictures without the user, for testing presets: `core` presses the shutter button itself - by the button's
own path in the camera, half-press first, then the full press - and before each step chooses a preset the way the user
chooses it in the picture mode screen, so the module that owns it (FILMLOOK, CREATPRF) applies it as usual. The camera's
own settings (exposure mode, focus, quality, ISO) stay as set on the camera. Not in the sample: a test tool, off unless
asked for. The camera does not take pictures while it is connected to a computer for data (Storage, MTP): use the USB
choice that only charges, or no cable.

| Key | Type | Default | Range | Effect |
|---|---|---|---|---|
| `run` | text | `off` | `off`, `boot` | `boot` - the scenario starts once after the modules have loaded |
| `start_delay` | number | `5000` | 0..600000 | milliseconds from the start of the modules to the first step |
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
has a line per step and per picture and the result: `shoot: done: N of M pictures made`.
