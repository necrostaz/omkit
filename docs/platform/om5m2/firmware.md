# OM-5 Mark II: firmware files, startup, updates and recovery

What the camera's firmware consists of, how it starts, how it is updated, and how to get back to the stock firmware if
something goes wrong. OMKit never distributes firmware images: a release's firmware tool makes the patched update file on
the user's computer from the official file ([fw1100/install.md](fw1100/install.md), [../../release.md](../../release.md)).
Version-specific facts: [fw1100/README.md](fw1100/README.md).

Basis labels: [README.md](README.md#basis-labels).

## 1. Files and versions

| Item | Value | Basis |
|---|---|---|
| Model code | `E148`: hardware kind `E` (camera bodies), model number `148`. The firmware's own check uses `0x1480` | code |
| Official file name | `OLY_E_148_<version>_0000_0000.BIN`, for example `OLY_E_148_1100_0000_0000.BIN` for version 1.1. The version is four digits, `1100` = 1.1 | observed on the manufacturer's files |
| Distribution | Through the manufacturer's desktop application (OM Workspace), which downloads the file and installs it over USB | observed |
| Structure | A chain of components, each with its own header, version and checksum: main code (`0x0100`), resources such as icons, strings and fonts (`0x0101`), pictures (`0x0102`), large data such as the coprocessor's Linux image and the setting name tables (`0x0106`), and a small component for another device (`0x010e`) | code, observed |
| Boot loader | A separate stage that runs before the main code. It is not part of the official update file, and an update does not change it | code |

OMKit supports exactly one firmware build per driver: `platform/om5m2/fw1100` is version 1.1. Each driver checks at start
that the firmware is the one it was built for ([fw1100/README.md](fw1100/README.md#2-identity-check)).

## 2. Structure of the main code in memory

- The boot stage loads the first part of the main code into DRAM and jumps to it; the main code's startup then copies its
  initial area (the kernel, drivers, interrupt and NMI handlers, flash driver) from flash (code).
- The rest of the main code is in **ten sections**, loaded in the order 1, 8, 2, 3, 4, 5, 6, 7, 9, 10 by a startup task. Two
  of them (6 and 7 in version 1.1) are compressed and are unpacked by another processor straight into DRAM (code).
- When section `n` is in place, bit `n - 1` of a "sections loaded" word is set; all ten loaded = `0x3ff` (code, camera).
  Code that depends on a section must check its bit; OMKit's loader waits for all ten.
- The camera settings are applied right after section 2, before the others (code).
- Each power-on loads everything again from flash: changes made in RAM (hooks, table edits) last until the camera powers
  off. A quick off-and-on of the lever that cancels the power-off is not a new start (camera).
- The firmware keeps a timeline of its startup in memory (named events with microsecond times). On the camera: picture on
  the screen at about 0.6 s, end of fast startup (`FastEndCb`) at about 1.2 s; `core` starts about 1.4 s after power-on
  (camera).

## 3. What the firmware loads from the card

Nothing executable, apart from the update file (section 4). The firmware has debug shells with memory commands and a loader
for the ARM coprocessor, but they are reachable only through internal service ports; the USB/PTP vendor operations offer
no memory or execution access; no "autoexec" file is read (code). This is why OMKit needs a small patch in the firmware
to start its code from the card ([fw1100/patch.md](fw1100/patch.md)).

## 4. Updating the firmware

### 4.1. From the card

| Item | Behaviour | Basis |
|---|---|---|
| File | In folder `DCOLYMP` at the root of the card, named `E148????.BIN` (the `????` part is free, `E1489999.BIN` is the usual choice) | code, camera |
| Start | Switch off; hold OK; switch on while holding OK. With no valid file the camera starts normally | camera |
| Progress | The screen stays empty, the front lamp is lit steadily while writing, then blinks: done. Switch off and on | camera |
| Checks before writing | Every component is checked (model, kind, checksum) before anything is written; a damaged file is refused as a whole | code |
| What is written | Only the components present in the file; the main code is written last. A file with only the main component is accepted | code, camera |
| Version check | None found in the writer: the same version installs again (camera); an older one probably too (inference) | code, camera |
| Settings | Kept across the update | camera |
| Rollback | None: a component is erased and written whole. Power loss while writing the main code leaves a broken main code | code (inference) |

### 4.2. Over USB

The desktop application talks to the running firmware over PTP; the main code performs the update with the same checks and
the same writer (code). It needs a working main code even more than the card path: USB, PTP and the application must run.

### 4.3. Where the updater lives, and what that means

The whole updater (finding the file, checking it, writing flash) is **inside the main code**; it runs as tasks of the
running system (code). No independent recovery loader was found: if a main code crashes at startup before the updater
task starts, neither the card nor USB can repair the camera (inference); it would need service. Where exactly the
"OK held" decision is made was not found in the code (open question). Hence:

- A patch to the main code must not touch anything on the startup path, the updater, the RTOS, the file system or the
  flash driver, and must not change compressed sections.
- New code must not be able to break startup. The OMKit loader only creates a task at the end of fast startup and
  reads a file later in that task; everything else runs from the card and can be removed with the card.
- **The card used for a firmware update must not contain an `OMKIT` folder.** Whether the OMKit loader runs during an update
  is not known, and loading `core` while the main code is being written is not acceptable.

## 5. Recovery

| Situation | What to do | Basis |
|---|---|---|
| A module misbehaves | Turn it off in `OMKIT.INI` (`NAME = off` under `[modules]`), or remove its file | design |
| `core` misbehaves or the camera restarts while OMKit loads | Hold OK while switching on and keep holding it for a few seconds: the loader skips `core`. Make sure the card has no update file in `DCOLYMP`, or OK starts the updater | emulator, camera |
| Same, simplest | Take the card out, or remove `OMKIT\CORE.BIN` on a computer (or rename it to a name that does **not** start with `core.bin`, see [storage.md](storage.md#2-names-83-and-long-names)). Without the file the patched firmware behaves like the stock one | camera |
| Reboot loop | Take the battery out, take the card out, put the battery back: without the card nothing of OMKit runs | camera (a fault in `core` gives one restart into a camera without OMKit until the next load) |
| Camera does not respond (UI frozen) | Take the battery out. The previous log is kept: `CORE.LOG` is continued, not replaced, after a power-on that did not end with a power-off | camera |
| Back to the stock firmware | Install the official firmware file for the camera from the card (section 4.1) or with the desktop application. This replaces the main code including the loader patch | camera |
| Clean settings after experiments | Choose a stock picture mode before leaving a modified firmware; after returning to stock do a full reset and restore a settings backup made on the stock firmware | camera |

Before trying anything that changes the firmware: a full battery, a settings backup made with the desktop application,
and a second card holding only the official firmware file.
