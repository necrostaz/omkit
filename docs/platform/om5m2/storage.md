# OM-5 Mark II: card storage and settings

The memory card as the firmware exposes it to code running in the camera, how OMKit uses it, and the camera's settings
storage in flash, which OMKit never writes. Function addresses and calling conventions for firmware 1.1 are in
[fw1100/system.md](fw1100/system.md#6-files).

Basis labels: [README.md](README.md#basis-labels).

## 1. The file API

The firmware has a C-style file interface over its FAT/exFAT code: `fopen`, `fread`, `fwrite`, `fclose`, `fseek`, `ftell`,
`fflush`, `remove`, `mkdir`, a "folder exists" test, `stat`, `findfirst`/`findnext`/`findclose` (code; all of them used
by OMKit on the camera except `stat`).

| Topic | Behaviour | Basis |
|---|---|---|
| Who executes | The file system server task (`FILECLBK`, CPU 0). A call from another task on CPU 0 becomes a synchronous request to it; from CPUs 1 and 2 it goes through a separate queue with a copy of the path. Requests are served one at a time, in order | code |
| Where it may be called | From any task. **Not** from an interrupt handler, not under the firmware lock: the call waits for the server | code |
| Drive and paths | `C:` is the card slot. Paths use the backslash: `C:\OMKIT\CORE.BIN` | code, camera |
| No card / not mounted | `fopen` returns 0. Without extra code "no file" and "card not ready" look the same | code, camera |
| Mount time | The card is mounted by the camera application some time after startup; the loader therefore retries `fopen` | code |
| Modes | `r`, `w`, `a`, `+`, `b`, plus firmware-specific `c`, `d`, `e`, `h`. `r` reads through an internal 16 KiB buffer into any destination; `d` reads sectors straight into the destination (it must be prepared for that) | code |
| `w` does **not truncate** | Opening an existing file with `w`/`wb` keeps its old length: writing a shorter content leaves the old tail. Remove the file first | camera |
| `a` / `ab` | Opens or creates the file and puts the position at its end: appending without tracking offsets | code, camera |
| `fseek` origins | The firmware numbers them its own way: 2 = from the start, 0 = from the current position, 1 = from the end (inference). A position past the end is clamped to the end | code |
| `rename` | Not found in the firmware | code (searched) |
| `fflush` | Exists; OMKit does not use it (see [power.md](power.md#22-what-must-happen-before-power-goes-away)). Data reach the card on `fclose` | camera |
| Listing a folder | `findfirst` with a mask `C:\DIR\*.EXT` and a 280-byte entry (attributes `+0`, size `+16`, name `+24`, the long name if there is one). The root folder cannot be listed this way, only a subfolder. Clear the entry before every call; after a failed `findfirst` do not call `findclose`. Each `findnext` walks the folder from its start, so long folders are slow | code, emulator, camera |

## 2. Names: 8.3 and long names

- Plain `fopen` parses the path as 8.3 blocks; a component longer than 12 characters is an error (code). There is a
  variant of `fopen` with a flag that walks the path with long names (the firmware opens some of its own files this way);
  OMKit uses it only to read files the user named on a computer (code, emulator).
- **Use 8.3 names with a three-letter extension** for files OMKit creates. A two-letter extension was cut wrongly by the
  firmware's FAT code: `CORE.SA` came out as `COR.ESA` (emulator).
- **Long names that start with an 8.3 name may be matched by it.** A file renamed on a computer to `core.bin.bak` was still
  opened by the loader as `CORE.BIN` (camera). Assumption: the firmware compares a long name only over the length of the
  requested name. To disable a file, give it a name that does not start with the original one (`NOCORE.BIN`, `OFF.BIN`),
  or remove it.
- The firmware is not known to create long names; OMKit only reads them.

## 3. "Card busy": the camera's own writes come first

- While the camera writes a picture or otherwise works with the card, two "media busy" words in its memory are set; its
  own power code waits for them before sleeping (code). OMKit reads the same words (address in
  [fw1100/system.md](fw1100/system.md#6-files)) and postpones its log and store writes while they are set, and for 3 s after
  the shooting parameters of a picture were built. Assumption: the words cover the camera's own card work; `core`'s writes
  do not set them.
- Measured on the camera: after a picture `core` waited about 3 s before its next write (`card: core writes waited 3012 ms`).
- Not covered yet: video recording (whether the words are set), the file API of modules (it is not held back), and the
  window between the check and the write.
- Card access by OMKit is short and infrequent anyway: reads at start, appends to the log every 0.5-3 s while there is
  something new, store saves 1.5 s after the last change.

## 4. What OMKit keeps on the card

| Path | What | Written how |
|---|---|---|
| `C:\OMKIT\CORE.BIN` | `core`, read by the loader | Never written by the camera |
| `C:\OMKIT\OMKIT.INI` | Configuration | Created with defaults if missing; otherwise only read |
| `C:\OMKIT\MODULES\NAME.OMM` | Module files | Only read |
| `C:\OMKIT\NAME\` | Files of module `NAME` (for example the grain tiles in `GRAIN\`) | Through the module file API |
| `C:\OMKIT\LOG\CORE.LOG` | Log of all power-ons, each from its header line | Appended (`ab`); never removed by `core` |
| `C:\OMKIT\STORE\NAME.STA`, `NAME.STB` | Stores of `core` and modules | Two copies written in turn, each with a version number and a CRC-32; the newest intact copy is used, so a power loss during a write damages at most one copy. Both have a fixed length and are rewritten in place (`r+b`): only the data and the modification time in the file's first directory entry change, so a reset during a save leaves the volume consistent. A copy that is missing or has another length is made once (`remove`, then `wb`) |

`core` creates the `LOG` and `STORE` folders at start and again before writing, in case the card was formatted meanwhile
(check that the folder exists, then `mkdir`, as the firmware itself does). The layout of the card is part of OMKit's
architecture: [../../architecture.md](../../architecture.md).

Rule for the card used to update the firmware: it must not hold an `OMKIT` folder (see [firmware.md](firmware.md#4-updating-the-firmware)).

## 5. Camera settings in flash

| Item | What | Basis |
|---|---|---|
| Where | Four settings stores; the large one ("M", 256 KiB of flash) holds every user setting and every layer of them: working values, the user layer, the Myset buffer and all nine slots (Current, Myset 1-4, C1-C4) | code |
| When it is written | The whole image of the store, only if it differs from the flash copy: when a menu is closed after a change, at every power-off, before sleep, before a firmware update from a running camera, at a factory reset, in the crash handler | code, emulator |
| What a firmware update does | Installing a firmware from the card or from a computer **keeps** the settings (camera). Values written by a modified firmware therefore survive a return to stock | camera |
| What the stock firmware does with stored values | Accepts them as they are at start (no range check on that path); what it does with an unexpected value depends on where it is used | code (medium confidence) |
| Full reset | Resets almost everything; a few internal values are not on its list | code, camera |

**Why OMKit never writes it.**

- OMKit has no code that writes flash. The firmware's flash writers are not called, and the settings image is never
  written by OMKit.
- But the camera saves its settings by itself, at moments OMKit cannot always see in advance (menu closing, sleep, deep
  sleep, battery; [power.md](power.md#4-settings-in-flash-during-power-off-and-sleep)). Any value a module puts into the
  camera's settings in RAM can therefore reach flash and outlive OMKit.
- The rule for modules follows: **the camera's settings only ever hold values the stock firmware itself can produce.** A
  module's own state (an active profile, a look) lives in the module and in its store on the card. With the card removed,
  or on the stock firmware, the camera must behave as a normal camera.
- A user backup of the settings (the `*.set` file of the desktop application) does not contain every value; a full reset
  plus restoring the backup is the way back to a clean state.
