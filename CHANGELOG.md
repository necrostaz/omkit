# Changelog

All notable changes to OMKit are listed here, newest first. Each release has its own section; the section of a release
is also the text of its GitHub release. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and OMKit uses [semantic versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Test scenario (`[shoot]` in `OMKIT.INI`).** A series of pictures without the user: `core` presses the shutter button
  by its own path in the camera and chooses a preset of the film look or a profile before each step, as the user would.
  For testing presets; off unless asked for ([docs/config.md](docs/config.md#10-shoot-the-test-scenario)).

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
