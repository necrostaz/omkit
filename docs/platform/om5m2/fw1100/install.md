# Installing OMKit on the OM-5 Mark II, firmware 1.1

This guide installs OMKit on an OM-5 Mark II that runs the official firmware 1.1, and tells how to remove it again. It
is the full reference for an OMKit release for `om5m2/fw1100`; the release folder itself holds a short step-by-step
guide `INSTALL.txt`, the card folder `OMKIT`, the firmware tool `firmware_tool` and the release notes `RELEASE.md`.

Read the whole guide before you start, in particular [the warnings](#9-warnings) and [the disclaimer](#11-disclaimer).

- **First installation** (sections 2-5): make the update files with the firmware tool, install the loader patch once,
  then copy the `OMKIT` folder to the card. The bundled modules are already on.
- **Updating OMKit** on a camera that already has the loader patch ([section 6](#6-updating-omkit)): no firmware tool and
  no firmware update - only the files in the card's `OMKIT` folder are replaced, and your settings are kept.

## 1. How it works

OMKit lives on the memory card. To start it, the camera's firmware needs one small change, the **loader patch**: once per
power-on it reads `OMKIT\CORE.BIN` from the card and starts it; without that file the camera works as stock. What the
patch changes and why: [patch.md](patch.md).

The OMKit release does not contain any firmware. The firmware tool makes the update files on your own computer:

1. it gets the **official** firmware 1.1 file of the manufacturer (it downloads it from the manufacturer's update server,
   or uses a copy you already have) and accepts it only if its SHA-256 is exactly that of the official file;
2. it applies the patch, checks every checksum of the firmware file and the SHA-256 of the result, and writes two update
   files: `patched` (to install the loader) and `original` (the official file unchanged, to go back).

You then install the patched file from a memory card, the same way the camera installs any firmware update from a card,
and copy the `OMKIT` folder to the card.

The patch is installed once. It stays in the camera until an official firmware replaces it (section 8), and it does not
change between OMKit releases for this firmware: a newer release only brings new files for the card (section 6).

## 2. What you need

| What | Why, and how to check |
|---|---|
| An OM-5 Mark II on the **official firmware 1.1** | The patch and OMKit are made for exactly this firmware. Check the firmware version the camera shows in its menu (the camera's instruction manual says where) or in the manufacturer's desktop application (OM Workspace) with the camera connected. On an older version: first update to 1.1 the official way, with OM Workspace. On a newer version: this release does not support it; do not install it. **Never flash the update files over any other firmware version: this can brick the camera** |
| A **fully charged** battery | A firmware update must never be interrupted |
| A memory card (SD) formatted in the camera | For the update, and then for OMKit. A second card is handy: one for updates, one for normal use with OMKit |
| A computer with **Python 3.9 or newer** and an internet connection | For the firmware tool. Installing Python: [section 3.1](#31-python). About 250 MB of free disk space |
| A backup of the camera's settings, made with OM Workspace | Recommended before any firmware change |

To update OMKit on a camera that already has the patch (section 6) you need only the new release, the camera's card and
a computer that can write to it: no Python, no firmware file and no firmware update.

## 3. Make the update files

### 3.1. Python

The tool needs Python 3.9 or newer, nothing else. The launchers find it themselves and say how to install it if it is
missing:

- **Windows**: from <https://www.python.org/downloads/> (in the installer tick "Add python.exe to PATH"), or in a
  terminal `winget install Python.Python.3.12`.
- **macOS**: from <https://www.python.org/downloads/macos/> (afterwards run "Install Certificates.command" in the Python
  folder of Applications once, or the download fails with a certificate error), or with Homebrew `brew install python`.
- **Linux**: the distribution's package, for example `sudo apt install python3` (Debian, Ubuntu) or
  `sudo dnf install python3` (Fedora).

### 3.2. Run the tool

Extract the release ZIP to a folder on your computer, then start the launcher in `firmware_tool`:

| System | How |
|---|---|
| Windows | Double-click `get_firmware_windows.bat` |
| macOS | Double-click `get_firmware_macos.command`. If macOS refuses to open it (a file from the internet), open Terminal, type `bash ` (with a space), drag the file into the window and press Return |
| Linux | In a terminal, in the `firmware_tool` folder: `sh get_firmware_linux.sh` |

The tool shows the disclaimer and waits for Enter. Then it gets the official file, checks it, patches it and prints
where the results are. It takes the official file from the first of these places:

1. a file given with `--firmware FILE` (run the launcher or `omkit_firmware.py` in a terminal with that option);
2. the `output/original` folder of an earlier run;
3. a file named `OLY_E_148_1100_0000_0000.BIN` (any letter case, also `... (1).BIN` and the like) next to the tool,
   in the release folder or in your `Downloads` folder;
4. otherwise a download from the manufacturer's update server (about 90 MB).

Whatever the source, the file is used only if its size and SHA-256 equal those of the official file; the tool says which
source it used. What the tool sends to the server: the same request the manufacturer's updater sends for this camera
model, with a zero MAC address and serial number, and the header `User-Agent: Olympus`; nothing about you or your
computer (apart from the IP address any connection shows).

The result, in `firmware_tool/output/`:

| File | What | Use |
|---|---|---|
| `patched/DCOLYMP/E1489999.BIN` | Firmware 1.1 main code with the OMKit loader (24 903 680 bytes) | To install OMKit (section 4) |
| `original/DCOLYMP/E1489999.BIN` | The official firmware 1.1 file, unchanged, only renamed | To go back to stock (section 8) |

Each folder has a `README.txt` that repeats what to copy where. The tool never writes to a memory card and never talks to
the camera.

**Keep the `original` folder.** The manufacturer's server gives only the latest firmware: once a newer version is
published, 1.1 can no longer be downloaded.

### 3.3. If the download fails or the server offers another version

The tool stops and writes nothing if the server cannot be reached, if the download is not the official file, or if the
server offers another firmware version than 1.1 (it says which). The server always gives the latest version and cannot be
asked for an older one, so after a newer firmware is published this release cannot download 1.1 any more.

In that case get the official 1.1 file `OLY_E_148_1100_0000_0000.BIN` yourself, for example from an earlier download, a
copy kept by someone you trust or the manufacturer's desktop application, and either put it next to the tool (or in your
`Downloads` folder) and run the launcher again, or give it explicitly:

```
python3 omkit_firmware.py --firmware /path/to/OLY_E_148_1100_0000_0000.BIN
```

(on Windows `py -3 omkit_firmware.py --firmware C:\path\to\OLY_E_148_1100_0000_0000.BIN`). The tool checks the file
exactly as it checks a download; any other file is refused.

## 4. Install the loader patch

This is done once. A camera that already runs OMKit has the patch: to update OMKit, go to section 6.

1. **Card without OMKit.** Use a card that has **no `OMKIT` folder** (formatting it in the camera is simplest). Copy the
   folder `DCOLYMP` from `firmware_tool/output/patched/` to the root of the card, so that the card holds
   `DCOLYMP\E1489999.BIN`. Put the card in the camera.
2. **Battery** fully charged, in the camera.
3. **Start the update.** Switch the camera off. Hold the **OK** button, and switch the camera on while holding OK.
4. **Wait.** The screen stays empty; the front lamp lights steadily while the camera writes, then blinks: done. Do not
   switch off, open a cover or take out the battery or the card while the lamp is lit steadily.
5. Switch the camera off and on, without holding OK. **Check that it starts normally**, as before, with your settings
   (an update from the card keeps the camera's settings). If it does not start normally, go back to the stock firmware
   (section 8) with the same card.
6. **Remove the update file.** Delete `DCOLYMP\E1489999.BIN` from the card on the computer, or format the card in the
   camera. As long as it is on the card, holding OK at power-on starts the update again.

If in step 3 the camera simply starts as usual, it did not find the file: check the folder and file name
(`DCOLYMP\E1489999.BIN` at the root of the card).

## 5. Put OMKit on the card

1. Copy the folder `OMKIT` of the release to the root of the card (the card from section 4 after step 6, or another card
   formatted in the camera). The card then holds `OMKIT\CORE.BIN`, `OMKIT\OMKIT.INI`, `OMKIT\MODULES\`, `OMKIT\GRAIN\`
   and `OMKIT\FILMLOOK\`.
2. Put the card in the camera, switch it on, wait a few seconds, switch it off. OMKit starts about 1.5 s after power-on.
   Let the camera power off completely: switching it on again within about 2 s cancels the power-off and is not a new
   start.
3. **Check the first start** on the computer: `OMKIT\LOG\CORE.LOG` begins with
   `======== OMKit core <version> release (<branch>@<commit>), om5m2 fw1100, run <id>, ...` and `config OMKIT.INI`, has
   a line `module NAME <version>: ...` for each bundled module and `modules: 2 running, 0 failed`.

   No `LOG` folder: the loader did not find `OMKIT\CORE.BIN` (check the folder name and that it is at the root of the
   card), the patch is not installed, or the camera does not run firmware 1.1: `core` then stops at once and writes
   nothing, and the camera works as stock.

**The modules are on from the first start.** The release's settings file `OMKIT\OMKIT.INI` turns the bundled modules on
with their default settings; nothing needs to be edited. It is the sample that `core` itself writes, with these lines
under `[modules]`:

```ini
[modules]
enabled = on
CREATPRF = on
FILMLOOK = on
```

To change a setting, or to turn a module off (`NAME = off`), edit `OMKIT\OMKIT.INI` in a text editor; a change takes
effect at the next power-on. If the file is deleted, `core` writes its own sample at the next start, which loads **no**
module: copy the release's `OMKIT.INI` back, or add the lines above.

What the modules do and their settings: [the bundled modules](../../../modules/README.md) (FILMLOOK: film looks in a
picture mode slot, by default i-Enhance; CREATPRF: colour and monochrome profiles in the picture mode screen). Every
setting of `OMKIT.INI`: [config.md](../../../config.md).

## 6. Updating OMKit

A newer OMKit release for the OM-5 Mark II with firmware 1.1 is installed **without the firmware tool and without a
firmware update**: the loader patch already in the camera stays as it is, and only the files in the card's `OMKIT`
folder change. This holds as long as the camera still has the patch: OMKit started with the earlier release, and no
official firmware has been installed since (that removes the patch; then follow sections 3-5 with a release made for the
camera's firmware).

1. Read the new release's section in `CHANGELOG.md`. It says when an update needs more than the steps below, and
   which settings of `OMKIT.INI` are new, changed or gone.
2. Switch the camera off, take the card out and put it in the computer. If you changed settings in the card's
   `OMKIT\OMKIT.INI`, save a copy of it on the computer.
3. Copy the new release's files into the card's `OMKIT` folder, replacing the files of the same name - **`OMKIT.INI`
   too**:

   | From the release's `OMKIT` folder | To the card |
   |---|---|
   | `CORE.BIN` | `OMKIT\CORE.BIN` |
   | the files in `MODULES\` | `OMKIT\MODULES\` |
   | the files in `FILMLOOK\` | `OMKIT\FILMLOOK\` |
   | the files in `GRAIN\` | `OMKIT\GRAIN\` |
   | `OMKIT.INI` | `OMKIT\OMKIT.INI` (the release's settings; your changes go back in at step 4) |

   Copy the files inside the folders rather than the folders themselves: on macOS, Finder replaces a whole folder of the
   same name (with your own presets in it) unless you hold Option and choose Merge.
4. If you had changed settings, make your changes again in the new `OMKIT.INI`, with care, at the matching places:
   a new `OMKIT.INI` may have new settings (and modules turned on) and may no longer have old ones, so copying your old
   file over the new one could turn off what the release adds. Compare the two files if in doubt.
5. Put the card back and switch the camera on. `OMKIT\LOG\CORE.LOG` now begins with the new `core` version, and the
   module lines show the new module versions.

What stays on the card:

- **Your settings** do not stay: the release's `OMKIT.INI` replaces the card's one, and your changes go back in from the
  copy you saved (steps 2-4).
- **Your presets**: your own `.ISP` files in `OMKIT\FILMLOOK\` stay. The bundled presets are replaced by the new
  release's: a preset whose file did not change keeps the edits made in the camera, a changed one starts without them. A
  bundled preset that the new release no longer has (or has under another name) stays until you delete it. Deleting
  bundled presets you do not want is fine: FILMLOOK lists the `.ISP` files it finds there, at most 64.
- **What the modules remember**: `OMKIT\STORE\` (the chosen look, preset and profile, the preset edits) and the logs in
  `OMKIT\LOG\`. The profiles' own settings are kept by the camera itself.
- **The log** `OMKIT\LOG\CORE.LOG` gets a few lines at every power-on and is never cut by OMKit: delete it on the
  computer now and then, when it gets long; the next start makes a new one.

**Going back to an earlier OMKit release** is the same copy, with the earlier release's files (`CORE.BIN`, `MODULES\`).
The settings `core` keeps in `OMKIT\STORE\` are read by the earlier `core` as they are. What a release adds may be lost
on the way back: its `CHANGELOG.md` section says so - for example, going back from 0.2.0 to 0.1.0 drops the edits of
the presets edited since the update (FILMLOOK 1.12 does not know their new format), and the `[optics]` section of
`OMKIT.INI` is simply not read.

## 7. What to expect

| Situation | Behaviour | Basis |
|---|---|---|
| Every full power-on with the `OMKIT` folder | OMKit starts by itself, about 1.4-1.7 s after power-on, with the modules turned on in `OMKIT.INI` | camera |
| New files in `OMKIT` (an update, section 6) | The next power-on runs them; the loader patch is not involved | camera |
| Card without `OMKIT\CORE.BIN`, or no card | The camera works as stock | camera |
| Switching off and on quickly | Not a new start (the power-off is cancelled); OMKit keeps running | camera |
| Wake-up from light sleep | OMKit keeps running, the look and its preset come back | camera |
| Wake-up from deep sleep (auto power off) | `core` starts again, as after a normal power-on | camera |
| Starting through Wi-Fi or USB | **Not verified** | |
| A firmware update from the card while the patch is installed | Works (installing a firmware from the card with OK held) | camera |

"camera": seen on the developers' OM-5 Mark II, which has run this patch since October 2026.

## 8. Going back to the stock firmware

1. Use a card with **no `OMKIT` folder**. Copy the folder `DCOLYMP` from `firmware_tool/output/original/` to the root of
   the card.
2. Fully charged battery; switch off; hold OK and switch on while holding it; wait until the lamp blinks (as in
   section 4). Switch off and on.
3. Delete `DCOLYMP\E1489999.BIN` from the card.
4. Optional, for a clean state: a full reset of the camera and a restore of the settings backup made on the stock
   firmware.

Any later official firmware, installed with OM Workspace or from the card, also replaces the main code and removes the
loader patch. OMKit keeps its own data on the card only (`OMKIT\`); delete that folder to remove it completely.

## 9. Warnings

- **Never interrupt a firmware update**: no power-off, no opening of the battery or card cover, no removing of the card,
  while the lamp is lit. Start only with a fully charged battery. A power loss while the main code is written leaves the
  camera without a working firmware; no independent recovery path is known, so it would need service.
- **A card used for a firmware update must not hold an `OMKIT` folder**, and a card with an update file in `DCOLYMP`
  must not be used in normal use: holding OK at power-on would start the update.
- **Only for the OM-5 Mark II with firmware 1.1.** The update files are for this model and version only. Do not install
  them on another camera or over another firmware version.
- **Official firmware updates remove the patch.** OMKit then no longer starts. A newer firmware needs a new OMKit
  release made for it; until then stay on 1.1 if you want OMKit.
- **Do not share the update files.** They are made from the manufacturer's firmware on your computer, for your camera.
- The patch has been in use on the developers' camera since October 2026, but OMKit is young software: expect bugs.

## 10. Troubleshooting

| Problem | What to do |
|---|---|
| A module misbehaves | Turn it off in `OMKIT.INI` (`NAME = off` under `[modules]`), or delete its file in `OMKIT\MODULES\` |
| A module does not start: the log has no line `module NAME <version>: ...` for it | `OMKIT.INI` must have `NAME = on` under `[modules]` (and no `enabled = off`). A file `core` wrote by itself (the log said `template written`, after `OMKIT.INI` was deleted or missing) turns no module on: copy the release's `OMKIT.INI` or add the lines (section 5) |
| OMKit misbehaves, or the camera restarts while OMKit loads | Simplest: take the card out, or delete `OMKIT\CORE.BIN` on the computer (or rename it to a name that does **not** start with `CORE.BIN`, such as `NOCORE.BIN`). Without that file the patched camera behaves like a stock one |
| Same, without a computer | Hold OK while switching on and keep holding it for a few seconds: the loader skips OMKit for this power-on. **First make sure the card has no update file in `DCOLYMP`**, or OK starts the firmware update |
| The camera restarts again and again | Take the battery out, take the card out, put the battery back: without the card nothing of OMKit runs |
| The camera does not respond | Take the battery out. The log of that power-on is kept and continued at the next start |
| The update does not start (the camera starts normally) | Check `DCOLYMP\E1489999.BIN` at the root of the card, and hold OK before switching on |
| The firmware tool stops | Read its message: it says what failed. Network problems: try again later, or see [section 3.3](#33-if-the-download-fails-or-the-server-offers-another-version) |

## 11. Disclaimer

OMKit is provided "as is", **without warranty of any kind**, express or implied. Modifying a camera's firmware can
damage the camera, void its warranty and lose your data or pictures. You install and use OMKit **entirely at your own
risk**; the authors and contributors accept **no liability** for any damage or loss arising from it.

OMKit is an independent project. It is **not affiliated with, authorised, endorsed or supported by** OM Digital
Solutions or Olympus. Product names are used only to identify the camera and firmware OMKit works with. To return the
camera to its original state, follow section 8.
