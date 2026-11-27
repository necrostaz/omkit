# Changelog

All notable changes to OMKit are listed here, newest first. Each release has its own section; the section of a release
is also the text of its GitHub release. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and OMKit uses [semantic versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.2.0] - 2026-11-27

### Added

- **Bloom and Halation on the film look's stills.** Two optical effects, set per preset:
  - **Bloom** - the soft glow of a diffusion filter: bright parts of the picture glow into the darker ones around them,
    with a long faint tail, in the colour of their light;
  - **Halation** - the halo of colour film without an anti-halation layer: lamps, flames and street lights get a red
    edge that turns orange where it is strong, the lamp's own core stays white.

  The look's column has two new rows after the grain item (G): **B - Bloom**, the last item of its first page, and
  **H - Halation**, alone on a second page (step down from Bloom). Each is a slider from 0 (off) to 5, kept with the
  preset's other edits; a long OK sets it back to 0. **Every preset starts with both at 0: the pictures do not change
  until you turn a row up.** Black-and-white presets get both without colour. With `exif = on` in `[FILMLOOK]` the
  JPEG's UserComment carries ` o=B,H`. Everything about them:
  [docs/modules/filmlook/optics.md](https://github.com/necrostaz/omkit/blob/main/docs/modules/filmlook/optics.md).
- **What to know about Bloom and Halation** ([limitations](https://github.com/necrostaz/omkit/blob/main/docs/modules/filmlook/optics.md#4-limitations)):
  - only on the still: live view and the shooting screen never show them - look at the picture in playback;
  - a still with them reaches the card later, about 2.3-2.9 s after the shutter release instead of about 1.6-1.9 s;
    bursts are taken as usual, only written more slowly;
  - RAW+JPEG: only the JPEG gets them. The RAW never does, and the preview inside the ORF (what a computer shows for
    the RAW file) is without them; the camera's playback shows the JPEG, with them;
  - only the film look's stills: not CREATPRF profiles, other picture modes, multi-frame shots or video;
  - in a JPEG a lamp and a white surface in the sun look the same, so sunlit white surfaces get a faint Halation edge
    too, and an underexposed lamp gets none.
- **Settings for Bloom and Halation** in the new `[optics]` section of `OMKIT.INI`. The layer is on by default, with
  the tuning OMKit is released with (the release's `OMKIT.INI` spells it out); `layer = off` removes the effects and
  their rows, and the other keys tune both effects for all presets ([docs/config.md](https://github.com/necrostaz/omkit/blob/main/docs/config.md#6-optics)). Each still
  with a strength above 0 gets one line in the log: `optics: job N: Bloom B, Halation H, ...`.

#### For developers (debug build)

The published release is a release build (see Changed); the items below are in a debug build of `core` only, built
from the source with `make BUILD=debug` ([docs/build.md](https://github.com/necrostaz/omkit/blob/main/docs/build.md#35-release-and-debug-builds)), and their settings
are read only there:

- **The debug service**, off by default: with `[debug] enabled = on` the log and snapshots of the camera's tasks also go
  to RAM that survives a reset, and `tools/omkdebug.py` reads them, and the card's files, over USB in the MTP mode, also
  after a crash; with `write = on` it updates files of the `OMKIT` folder, each checked by a CRC-32 read back from the
  card ([docs/debug.md](https://github.com/necrostaz/omkit/blob/main/docs/debug.md), with its security notes).
- **The test scenario** `[shoot]`: a series of pictures without the user, `core` pressing the shutter button and
  choosing a preset or a profile before each step
  ([docs/config.md](https://github.com/necrostaz/omkit/blob/main/docs/config.md#12-shoot-the-test-scenario-debug-build-only)).
- **Test sessions over USB**: the computer restarts the camera by the firmware's own reboot request, gives it a one-shot
  scenario `OMKIT\SHOOT.RUN` ([docs/config.md](https://github.com/necrostaz/omkit/blob/main/docs/config.md#121-one-scenario-at-the-next-power-on-omkitshootrun)) and
  reads the build that runs; `omkdebug.py session` does a whole session, `--hands-free` with the USB cable kept as
  charge only and a restart at the end ([docs/config.md](https://github.com/necrostaz/omkit/blob/main/docs/config.md#122-without-hands-hands_free--on-in-shootrun),
  [docs/debug.md](https://github.com/necrostaz/omkit/blob/main/docs/debug.md#6-a-test-session)).
- The log's `debug` level, the task dumps (`[core] taskdump*`), the probes of the camera's state, the rescue of a camera
  stuck with the lever off (`[core] power_rescue`) and the test keys of `[grain]` and `[optics]`.
- Two build kinds of `core` and the modules: `make BUILD=release` (the default) and `BUILD=debug`;
  `tools/buildcheck.py` checks that a release image has nothing of the debug build. A module writes its debug lines
  with `OMK_LOG_D`, compiled into its debug build only. The image of `core` with its stack may now end at `0x634b0000`
  (was `0x63490000`).
- `card_busy` (API 19) also answers 1 while the camera is in a half-press, as `core` holds its own writes then.

### Changed

- **Updating copies `OMKIT.INI` too.** A new release's `OMKIT.INI` may have new settings (this one: the `[optics]`
  section) and may no longer have old ones, so the card's own file is no longer kept: if you changed settings, save
  your `OMKIT.INI` first, copy the release's one with the other files, then make your changes again in it, with care,
  at the matching places ([install guide](https://github.com/necrostaz/omkit/blob/main/docs/platform/om5m2/fw1100/install.md#6-updating-omkit)).
- **Versions: core 1.46 (module API 20), CREATPRF 1.6, FILMLOOK 1.13.** Both modules need the `core` of this release:
  update `CORE.BIN` and the files of `MODULES` together (on the core of 0.1.0 they are not loaded - `needs a newer core
  API` in the log - and the camera works without them).
- **A release is a release build.** Its `CORE.BIN` and modules have only what the user needs; the development code is
  not in them at all. The log's first line names the build: `======== OMKit core 1.46 release (...)`. The debug build's
  settings in an `OMKIT.INI` of 0.1.0 only give warnings: `log_level = debug` is taken as `info`; `[core]
  core_events`, `fw_cache_inv` and `taskdump*` get `config: [core] KEY unknown`. The file needs no edit.
- **No card writes of OMKit in a half-press.** `core` keeps its log and store writes out of a half-press or a change of
  the camera's state (up to 6 s, and 0.5 s after it), as it keeps them out of a picture being written. It reads the
  camera's state from memory, once every quarter of a second, and no hardware register.
- **A reset during a card write no longer breaks the card's volume.** The stores' copies have a fixed length
  and are rewritten in place, so a save changes neither the FAT nor the allocation bitmap nor more than one directory
  entry, and the log is no longer removed and made again at every start. Before, a reset in the middle of a save could
  leave a half-written directory entry set in `OMKIT\STORE` (Windows hid the folder until `chkdsk /f`). The copies of an
  older core are made again once, in the new length, at their first save; the settings in them are kept.
- **One log for all power-ons.** `OMKIT\LOG\CORE.LOG` is appended to from one start to the next; each power-on begins
  with a header line - the build, the run id, the camera clock's date and time - and a line with the versions of the
  loaded modules. The log is written through an open file, so an append costs the same however long the file is. OMKit
  never cuts it: delete it on the computer when it gets long. `run_logs` is gone (ignored if still in `OMKIT.INI`); old
  `XXXXXXXX.LOG` files are left on the card.
- **FILMLOOK stores each preset's Bloom and Halation strengths with its edits** (a new format of the stored edits).
- **Going back to 0.1.0**
  ([docs/platform/om5m2/fw1100/install.md](https://github.com/necrostaz/omkit/blob/main/docs/platform/om5m2/fw1100/install.md#6-updating-omkit)): copy its `CORE.BIN`
  and modules as for an update. The `core` of 0.1.0 reads the stores of this one as they are and does not read
  `[optics]`; its FILMLOOK (1.12) does not know the new format of the edits and drops them - tone curve and grain too -
  for every preset edited since the update.

### Fixed

- **A camera that did not switch off with the power lever**
  ([issue #2](https://github.com/necrostaz/omkit/issues/2)): in the viewfinder the camera could stay on, live view
  running, until the battery was taken out. On the camera this no longer happens with the release build of this
  version, which reads none of the camera's display registers (0.1.0 read one at every pass for a diagnostic line); the
  exact mechanism is not proven. Also, while the lever is off and the camera still runs, `core` now writes nothing to
  the card (before, it wrote the log again 5 s after the lever, just when the battery may be taken out).
- **A store whose file was removed while the camera was on** (another card put in, files deleted over USB) is made
  again at its next save; before, its saves failed until the next power-on.

## [0.1.0] - 2026-11-20

First public release, for the OM SYSTEM OM-5 Mark II on firmware 1.1.

### Added

- **OMKit core.** The camera is flashed once with a small loader patch; from then on the OMKit core and its modules
  load from the `OMKIT` folder on the SD card. Settings live in `OMKIT/OMKIT.INI` (the release's file has the bundled
  modules on), and every start writes a log to `OMKIT/LOG`. Taking the card out, or holding OK at power-on, starts the
  camera without OMKit.
- **Creative profiles (CREATPRF).** Three colour and three monochrome profiles in the picture mode strip, with their
  own icons. They are edited on the camera in their own editor column: Color/Vivid (the saturation of 12 hue sectors),
  the tone curve (highlights, mid tones, shadows), sharpness and contrast; for monochrome also the colour filter and its
  level, shading, grain and toning. The profiles apply to stills, live view and video recorded from the still modes.
  Ready-made community profiles (JPEG files) load into the camera over USB with OM Workspace.
- **Film look (FILMLOOK).** A film look in the place of one stock picture mode (i-Enhance by default). The dials step
  through presets (`.ISP` files on the card) and every preset shows its own cassette picture. 32 bundled presets, from
  Kodak Portra, Ektar, Kodachrome and Tri-X to Fujifilm and Ilford stocks, cinema films and a few looks of their own;
  add your own by copying `.ISP` files to the card. A tone curve editor per preset, kept across power-offs. Film grain
  on the stills: 11 grain tiles in four strengths (Off, Low, Med, High), weaker in the shadows and highlights.
- **Firmware tool** for Windows, macOS and Linux (Python 3.9+). It downloads the official firmware 1.1 from the
  manufacturer, checks it, applies the loader patch on your computer, checks the result and keeps the official file
  for going back to the stock firmware. No firmware is part of OMKit.
- **Preset programs** for the computer: ISP player (see a picture through a preset as the camera renders it), ISP
  fitter (turn a `.cube` look into a preset) and ISP neurofit (make a preset from the camera's RAW files and scans of
  a film stock, driven by an AI agent).
- **Module SDK** with the example module HELLO, for writing your own modules.
