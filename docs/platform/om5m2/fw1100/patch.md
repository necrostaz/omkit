# The OMKit loader patch for firmware 1.1

OMKit itself lives on the memory card. To start it, the stock firmware 1.1 gets one small patch: a hook that, once per
power-on, starts a task which reads `C:\OMKIT\CORE.BIN`, checks it and jumps into it. This page says what the patch changes
and why. How users install it: [install.md](install.md).

Basis labels: [../README.md](../README.md#basis-labels).

## 1. Why a patch

The stock firmware never executes code from the card: the only file it acts on is a firmware update, and its debug
shells are reachable only through internal service ports ([../firmware.md](../firmware.md#3-what-the-firmware-loads-from-the-card)).
Code in RAM cannot be changed from outside either. So the smallest possible change goes into the main code: a hook that
loads the rest from the card. Everything else (the driver, `core`, the modules) can then change without touching the
firmware again.

Design rules for the patch (from what the updater needs, [../firmware.md](../firmware.md#43-where-the-updater-lives-and-what-that-means)):

| Rule | How the patch follows it |
|---|---|
| Must not be able to break startup or the updater | One 5-byte hook outside the startup call graph and the updater; the hook body only creates a task; all file work happens later in that task |
| No change to compressed sections | The hook site and the code are in uncompressed sections 1 and 2 |
| No writes into the protected range | The hook body writes no memory at all; its flag lives in section 9, outside the range |
| Without the card it behaves like the stock firmware | No file: the task ends; the camera is unchanged |
| A broken `core` must not cause a reboot loop that cannot be stopped | Holding OK skips `core`; removing the card removes OMKit |

## 2. Where it hooks

### 2.1. Hook site: `FastEndCb`, `0x601b1362`

`FastEndCb` is a callback of the fast startup in section 1. On the camera it runs once per power-on, about 1.2 s after the
lever, in the context of a task, after sections 1, 8, 2, 3, 4 and 5 are loaded (it is registered through code in
section 5) (code, camera: the firmware's startup timeline records it, and `core` started after it in every
logged start, 0.15-0.35 s later). It is part of startup, but late: the camera already shows the live picture. Whether it
also runs when the camera starts its updater (OK held with an update file on the card) is not known, but the loader
skips `core` whenever OK is held at power-on (camera), so `core` never runs alongside the updater. Even so the hook body
does nothing but create a task, and an update card should not hold an `OMKIT` folder.

| | Bytes | Instructions |
|---|---|---|
| Stock | `cf 20 f8 fe fc` | `movm [a2],(sp)`; `add -4,sp` |
| Patched | `dc 5e e1 3f 00` | `jmp 0x605af4c0` (the trampoline) |

The two displaced instructions do not depend on their address, no branch leads into them, and they lie within one 16-byte
line (code; checked by the patch builder's rules).

### 2.2. Code caves

| Place | Range | Use | Why it is safe |
|---|---|---|---|
| Tail of section 2 | `0x605af4c0..0x605af800` (832 bytes of `0xff` filler) | Trampoline at `0x605af4c0` (42 bytes), hook body and strings at `0x605af510` (312 bytes) | Loaded by the section loader; no literal, relative branch or constructor table points into it; outside the updater's code; the word before it (`0x605af4b0`) is left alone. Inside the DbgSup range: executed and read only, never written |
| Tail of section 9 | word `0x615ee7c0` | `omk_done`, the "already loaded" flag, stored as 0 | Writable (outside the DbgSup range; written on the camera); section 9 is loaded again from flash at every power-on, so the flag is 0 at every real start |

### 2.3. The trampoline

The standard trampoline of the patch builder: save all registers and `psw`, reserve the call slot, `call omk_hook,[],16`,
restore, execute the two displaced instructions, `jmp 0x601b1367` (right after the hook site). The hook body is an ordinary
function with a 16-byte frame.

## 3. What exactly changes

Four places in the main code, plus the checksum of the main component. Nothing else; the other components of the update
file are untouched.

| Name | Address | Length | Before | After |
|---|---|---|---|---|
| `omk_site` (hook) | `0x601b1362` | 5 | `cf 20 f8 fe fc` | `jmp` to the trampoline |
| `omk_site` (trampoline) | `0x605af4c0` | 42 | `0xff` filler | trampoline |
| `omk_hook` | `0x605af510` | 312 | `0xff` filler | hook body, task, names (`stub.s`) |
| `omk_data` | `0x615ee7c0` | 4 | `ff ff ff ff` | `00 00 00 00` |

The build is deterministic: the same `spec.json` and `stub.s` give the same bytes.

## 4. What it does at boot

### 4.1. In the hook (the startup task, about 1.2 s after power-on)

Creates a task with the firmware's `acre_Task`: name `OMK_LOAD`, priority 66 (below every firmware service task), a 2 KiB
stack from the kernel pool, started at once. Then it returns and the stock `FastEndCb` continues. It writes no memory
and calls nothing else.

### 4.2. In the task `OMK_LOAD`

1. **Wait for all sections**: up to 300 times, check the "sections loaded" word `0xa26e2698` for `0x3ff`, sleeping 100 ms
   between checks (30 s at most). Not all loaded: end.
2. **Once per power-on**: if `omk_done` is not 0, end; otherwise set it to 1. A cancelled power-off is not a restart, so
   `core` is never loaded twice into a running system.
3. **Bypass**: ask the firmware whether OK (key code 4) is held (`0x60130fb8`, 0 = held). Held: end.
4. **Open the file**: `fopen("C:\OMKIT\CORE.BIN", "r")`; if it fails (no card, card not mounted yet, no file), sleep 1 s and
   try again, at most 10 times.
5. **Read**: `fread` up to 3 MiB into `0xa345a000` (uncached view of the free gap), then `fclose`.
6. **Check** the 16-byte header and the body:

   | Offset | Field | Check |
   |---|---|---|
   | 0 | magic `OMKC` (`0x434b4d4f`) | equal |
   | 4 | body length | not 0, a multiple of 4, and 16 + length = bytes read (so a file over 3 MiB is refused too) |
   | 8 | sum of the body's 32-bit words | equal to the sum computed |
   | 12 | loader ABI | 1 |

7. **Jump**: `calls 0x6345a010`, the first byte of the body through the cached view. `core` is linked for that address and
   never returns: it switches to its own stack and runs its service loop in this task.
8. On any failure in steps 1-6: `ext_tsk`. The camera goes on as stock.

### 4.3. Failure paths

| Situation | Result | Basis |
|---|---|---|
| No card, no `OMKIT\CORE.BIN` | 10 tries over about 10 s, task ends, stock camera | emulator, camera |
| Card mounted late | Loaded on a later try | emulator |
| Bad magic, sum, ABI or length | File read, control not passed | emulator |
| Sections not loaded within 30 s | No attempt | emulator |
| OK held | File not read; the camera works as stock | emulator, camera |
| `core` finds another firmware | `core` returns at once without calling a firmware function (no log); the task ends | code |
| `core` faults while starting | NMI, one restart; the next start loads `core` again, so a fault on every start is a reboot loop: hold OK or remove the card ([../firmware.md](../firmware.md#5-recovery)) | code, camera |

### 4.4. What `core` gets

Entry at `0x6345a010` in the `OMK_LOAD` task on CPU 0, with the loader's 2 KiB stack (`core` moves to its own at once), the
file image at `0xa345a000` (its body at `0x6345a010` in the cached view), and nothing else: no arguments. Everything else
`core` finds through its driver.

## 5. The source: `src/stub/platform/om5m2/fw1100/`

| File | What |
|---|---|
| `gen.py` | Writes `spec.json`. Holds the addresses: hook site, trampoline and body places, the flag word, the load buffer and entry, task priority and stack, the try counts and delays, and the firmware functions the body calls. `gen()` can produce a variant with another hook site and name (the same body elsewhere) |
| `spec.json` | Generated description for a patch builder: model `E148`, the symbols, two blobs (`omk_hook` assembled from `stub.s` at the body address; `omk_data`, four zero bytes at the flag word) and one hook (`omk_site`: site, trampoline place, body, frame 16) |
| `stub.s` | MN10300 assembly of the hook body (`omk_hook`), the task (`omk_task`) and its strings (`OMK_LOAD`, `C:\OMKIT\CORE.BIN`, `r`). Uses symbols from `spec.json` |

Firmware functions the body calls:

| Symbol | Address | What | Call |
|---|---|---|---|
| `f_acre` | `0x60051d5c` | `acre_Task`: `d0` 2 = start now, `d1` parameter, `a0` name, `a1` entry, `(4,sp)` priority, `(8,sp)` stack size, `(12,sp)` stack (0 = kernel pool) | `call ...,[d2,d3,a2,a3],56` |
| `f_sleep` | `0x60051a2b` | `dly_tsk` wrapper, `d0` milliseconds | `call ...,[d2],8` |
| `f_ext` | `0x6003167e` | `ext_tsk` | `call ...,[a2],8` |
| `f_keyup` | `0x60130fb8` | key state, `d0` key code; 0 = held | `call ...,[d2,d3,a2,a3],24` |
| `f_fopen` | `0x606aa3bd` | `a0` path, `a1` mode -> `a0` file or 0 | `call ...,[],4` |
| `f_fread` | `0x606aa54e` | `a0` buffer, `d0` size, `d1` count, `a1` file -> `d0` | `call ...,[d2,d3,a2,a3],24` |
| `f_fclose` | `0x606aa3c6` | `a0` file | `call ...,[d2,a2,a3],16` |
| `omk_sections` | `0xa26e2698` | "sections loaded" bits (uncached) | read |

`CORE.BIN` itself is produced by OMKit's build (`tools/pack.py` adds the header).

## 6. Status

| What | Result | Basis |
|---|---|---|
| Emulator: good file, no file, bad magic/sum/ABI, wrong lengths, OK held, sections late, card late, tries exhausted; camera model start, shooting, playback | All as expected. The emulator does not run `FastEndCb`, so there the same body is hooked on another site | emulator |
| Camera, since 2026-10-30 | `core` starts by itself on every full power-on, 1.4-1.7 s after it; without `OMKIT` the camera works as stock | camera |
| Updater from the patched firmware | Re-installing a firmware from the card with OK works | camera |
| Quick lever off-on | No new start (the power-off was cancelled); `core` simply keeps running | camera |
| Wake-up from light sleep | `core` keeps running, the look and its preset come back | camera |
| Wake-up from deep sleep (auto power off) | A new boot: the loader runs and `core` starts as after a lever power-on | camera |
| Wi-Fi/USB start paths | Not verified | |

## 7. Installing the patch

The step-by-step guide for users: [install.md](install.md). In short: an OMKit release carries no firmware. Its firmware
tool (`release/user/omkit_firmware.py`, Python standard library only) gets the official 1.1 file (a download from the
manufacturer's update server, the way the manufacturer's updater asks for it, or a copy the user already has), accepts it
only by its SHA-256, writes the edits of section 3 from the release's `patch.json` into the decoded main component,
recomputes the component's checksum and keeps only that component, as the update file
`DCOLYMP\E1489999.BIN` (24 903 680 bytes). It checks every checksum and the SHA-256 of the result
(`548098cd3d62a534360bb9a5385e20e0c79f6d2f477f97ad20c92a3f5c8f990c`, the file installed on the camera since 2026-10-30)
and writes nothing if any check fails. The camera installs it from the card like any update (OK held at power-on).

`patch.json` holds, for each changed component, the offsets in its decoded payload and the new bytes (the loader code and
data of section 3, no bytes of the official firmware), with the SHA-256 of the official file, of the component before
and after, and of the result. The release's maintainers generate it from the same build of `spec.json` and `stub.s`
([../../../release.md](../../../release.md)).

## 8. Returning to stock

Install the official firmware 1.1 file (or any later official version) from the card with OK held at power-on, or with the
manufacturer's desktop application ([../firmware.md](../firmware.md#4-updating-the-firmware)). The update rewrites the
whole main code, so the hook, the trampoline, the body and the flag are gone. The card used for that must not contain an
`OMKIT` folder.

OMKit keeps its own state on the card, so nothing of it stays in the camera. What may stay is in the camera's settings:
before going back, choose a stock picture mode; after it, a full reset and a settings backup restore give a clean state
([../storage.md](../storage.md#5-camera-settings-in-flash)).
