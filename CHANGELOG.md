# Changelog

All notable changes to OMKit are listed here, newest first. Each release has its own section; the section of a release
is also the text of its GitHub release. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and OMKit uses [semantic versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Release and debug builds.** `core` and the modules are built in two kinds, chosen by `make BUILD=release` (the
  default) or `BUILD=debug`. A release build has only what the user needs: the debug service over USB, the task dump,
  the test scenario, the probes of the camera's state, the log's `debug` level and the rescue of a stuck camera are
  compiled into the debug build alone, and their settings are read only there. The log's first line names the kind;
  `tools/buildcheck.py` checks that a release image has nothing of the debug build
  ([docs/build.md](docs/build.md#35-release-and-debug-builds)). The items below marked "debug build" are in that kind
  only.
- **Debug service (core 1.40; debug build), off by default.** With `[debug] enabled = on` in `OMKIT.INI`, the log and snapshots of
  the camera's tasks also go to RAM that survives a reset, and `tools/omkdebug.py` reads them over USB in the MTP mode,
  also after a crash; it reads files of the card, and with `write = on` updates files of the `OMKIT` folder, each
  checked by a CRC-32 read back from the card. `card_writes = off` keeps the log and the stores in RAM only.
  See [docs/debug.md](docs/debug.md), including its security notes.
- **Test scenario (core 1.41; debug build), off by default.** `[shoot]` in `OMKIT.INI` takes a series of pictures without the user:
  `core` presses the shutter button by its own path in the camera and chooses a preset of the film look or a profile
  before each step, as the user would. For testing presets
  ([docs/config.md](docs/config.md#10-shoot-the-test-scenario)).
- **A test session over USB (debug build).** With `[debug] write = on`, the computer restarts the camera over USB by the firmware's own
  reboot request (a normal power-off that keeps the settings and unmounts the card, then a wake-up alarm), starts or
  stops the test scenario, and reads the build that runs (`INFO`); `omkdebug.py session` does a whole session: files,
  scenario, restart, the pictures and the logs ([docs/debug.md](docs/debug.md#6-a-test-session)). A scenario waits until
  the camera shoots (`[shoot] arm_wait`), and `OMKIT\SHOOT.RUN` starts one at the next power-on only, removed before it
  runs ([docs/config.md](docs/config.md#101-one-scenario-at-the-next-power-on-omkitshootrun)).
- **A test session without hands (debug build).** `hands_free = on` in `OMKIT\SHOOT.RUN` keeps the USB cable as charge only for that
  one start, whatever the camera's USB choice (the camera shoots and stays off the computer; its settings are not
  changed), and at the scenario's end `core` writes the result to `OMKIT\SHOOT.END` and restarts the camera, which comes
  back in the MTP mode; a stuck camera application ends it too
  ([docs/config.md](docs/config.md#102-without-hands-hands_free--on-in-shootrun)). `omkdebug.py session --hands-free`
  runs such a session and `--pull-log` also takes the RAM log of the power-on before
  ([docs/debug.md](docs/debug.md#6-a-test-session)).
- The log names the build at each power-on: `OMKit core <version> <release|debug> (branch@commit)`.

### Changed

- **A reset during a card write no longer breaks the card's volume.** The stores' copies have a fixed length
  and are rewritten in place, so a save changes neither the FAT nor the allocation bitmap nor more than one directory
  entry, and the log is no longer removed and made again at every start. Before, a reset in the middle of a save could
  leave a half-written directory entry set in `OMKIT\STORE` (Windows hid the folder until `chkdsk /f`). The copies of an
  older core are made again once, in the new length, at their first save; the settings in them are kept.
- **One log for all power-ons.** `OMKIT\LOG\CORE.LOG` is appended to from one start to the next; each
  power-on begins with a header line - the build (branch and commit), the run id, the camera clock's date and time - and
  a line with the versions of the loaded modules. The log is written through an open file, so an append costs the
  same however long the file is; a file the camera closed itself (it mounts the card again in its PC mode over USB) is
  opened again. `run_logs` is gone (ignored if still in `OMKIT.INI`); old
  `XXXXXXXX.LOG` files are left on the card.
- **core 1.42** has both the debug service and the test scenario. Its image with the stack may now end at `0x634a0000`
  (was `0x63490000`); the module arena still starts at the image's real end.
- **core 1.43** adds the test session over USB (restart over USB, the one-shot scenario `OMKIT\SHOOT.RUN`, the build
  in the log's first line and in `INFO`) and fixes writing files over USB, which failed since core 1.40.
- **core 1.44** keeps the card's volume whole across resets during a save (the stores rewritten in place) and keeps one
  `CORE.LOG` for all power-ons.
- **core 1.45** adds test sessions over USB without hands: during a one-shot scenario the camera stays in charge only
  with the cable in, and it restarts itself into the MTP mode at the end.

### Fixed

- **A camera that does not switch off with the power lever** (issue #2: in the viewfinder the camera could stay on, live
  view running, until the battery was taken out). The camera was already stuck before the lever: its half-press
  exposure sequence waits for ever, so the firmware never begins its power-off. While the lever is off and the camera
  still runs, `core` now writes nothing to the card (before, it wrote the log again 5 s after the lever, just when the
  battery may be taken out). If the camera stays stuck in that state for 8 s with the shutter released, `core` saves
  once and asks the firmware for a power-off with a restart, so the camera starts with the lever off and switches off;
  the watchdog is the last resort (`[core] power_rescue`, on by default). `core` also keeps its card writes out of a
  half-press, and reads no display register on its regular passes.

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
