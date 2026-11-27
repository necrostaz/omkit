# OMKit architecture

OMKit adds features to the camera without replacing its firmware. A small loader patch is written into the stock firmware
once; everything else - `core`, the driver for this firmware, the modules, their settings, logs and data - lives in the
`OMKIT` folder on the memory card and runs from RAM. Take the folder off the card (or hold the bypass key at power-on - OK
on the OM-5 Mark II) and the camera behaves as stock.

This document describes the parts and how they work together, independently of a particular camera. Firmware addresses,
hook sites and other details of one camera and firmware version are in [`platform/om5m2/`](platform/om5m2/). The module
rules are in [modules.md](modules.md), the API in [sdk.md](sdk.md), the settings in [config.md](config.md), the bundled
modules in [modules/](modules/README.md).

Confidence marks used below: **measured on the camera**, **observed in the emulator**, **from the firmware code** (static
analysis), **assumption** (not verified).

## 1. Design principles

| Principle | Consequence |
|---|---|
| Nothing persistent is changed in the camera except the loader patch | `core` and modules change RAM only. They never write flash or ROM and never call the firmware's own settings save |
| The camera's stored settings stay stock | Module state is kept in files on the card. A feature such as a picture profile is applied from memory over a stock setting; the value the firmware saves is always one the stock firmware understands. Settings are written to flash by the firmware itself (for example at power-off), so a non-stock value in a working setting would survive a return to stock firmware |
| The firmware is the only owner of its memory | `core` writes firmware code and data only through the driver, at a few checked places (hooks, tables). Modules never touch firmware memory at all |
| One layer knows the camera | Firmware addresses, calling conventions and data layouts exist only in the driver (`src/platform/<platform>/<firmware>/`) and the loader (`src/stub/...`). `core`, the SDK and modules are the same for every camera |
| Loading never blocks the camera | The loader only starts a low-priority task. The camera is usable while `core` and the modules load; features appear when they are ready |
| Every failure has a safe path | A missing card, file or folder, a wrong firmware, a broken module: `core` logs the reason and leaves that part out; the camera keeps working as stock |
| Everything is bounded and logged | Buffers, queues, waits and loops have limits; what is lost when a limit is reached is counted in the log |

## 2. Layers

```
+-----------------------------------------------------------------------------------------+
| modules (.OMM)        CREATPRF, FILMLOOK, HELLO, ...      only omk_api.h: events,       |
|                       src/modules/<name>/                 services, store, log, files   |
+------------------------------ public API: one function table, grows only --------------+
| SDK                   src/sdk/include: omk_api.h, omk_types.h, omk_events.h, omk_ispb.h |
|                       src/sdk/lib/omk_libc.c (memcpy, memset, ...)                      |
+-----------------------------------------------------------------------------------------+
| core services         config, log, store, memory, module loader, events and deferred    |
| (src/core)            calls, power-off and sleep, picture profiles, film look, ISP       |
|                       colour tables, grain, task dump                                   |
+------------------------------ driver interface: src/core/include/omk_drv.h -------------+
| driver                firmware check, file and RTOS thunks, memory regions, hooks and   |
| (src/platform/<p>/<f>) trampolines, meaning of firmware data, UI and ISP integration     |
+-----------------------------------------------------------------------------------------+
| CORE.BIN              core + driver linked into one image, packed by tools/pack.py       |
+-----------------------------------------------------------------------------------------+
| loader patch          a hook at the end of the firmware's start: creates a task that     |
| (src/stub/<p>/<f>)    reads, checks and starts CORE.BIN                                  |
+-----------------------------------------------------------------------------------------+
| stock firmware        RTOS, tasks on three CPUs, file system, UI, image pipeline         |
+-----------------------------------------------------------------------------------------+
```

The driver is linked together with `core` into `CORE.BIN`. A new event or service is therefore a new `CORE.BIN` on the
card, not a new firmware write: only the loader lives in the firmware.

## 3. Files on the card

```
OMKIT/
  CORE.BIN              core and driver (read by the loader)
  OMKIT.INI             settings of core and modules; in a release with the bundled modules on; if missing, core
                        writes its sample (no module on)
  MODULES/NAME.OMM      module files; loaded only when listed in [modules] of OMKIT.INI
  LOG/CORE.LOG          the log: the power-ons one after another, each from a header line; never removed by core
  STORE/NAME.STA|.STB   the stores of core (CORE), core services (GRAIN) and modules: two copies each, of a fixed
                        length, rewritten in place
  GRAIN/*.GRN           grain tile containers for the film look (formats/grn.md)
  NAME/                 a module's own folder: its files (presets, data); made by core at the module's first file call
```

All names are 8.3 (`A-Z 0-9 _`, a three-letter extension). Long file names can be listed and read by modules from API 15,
but nothing creates them. `core` makes `LOG` and `STORE` itself, at start and again before writing (the card may have
been formatted meanwhile), and a module's folder at the module's first file call.

**A reset during a card write.** The camera's file system has no journal: making or removing a file, or giving it a new
cluster, changes the allocation bitmap, the FAT and the file's directory entries in several separate writes, and a reset
between two of them leaves the volume inconsistent (an entry set half written: Windows hides the folder and `chkdsk`
reports it; or a cluster marked free that a file still uses). The camera neither notices nor repairs this. So `core`
keeps such changes out of its regular writes: a store's two copies are made once, with a fixed length, and from then on
rewritten in place - only the data and the modification time in the file's first directory entry change, one sector,
so a reset at any point leaves either the old or the new state (checked by cutting every write of a save in an emulator,
on exFAT and FAT32). The log is appended to, which changes its length in its directory entry at every write and the FAT
and the bitmap once a cluster; it is never removed. Old OMKit versions removed and made the log and the store files
again at every start and every save. If the card was reset during writes and Windows does not show a folder of `OMKIT`,
run `chkdsk X: /f` (or `fsck.exfat` / `fsck.vfat`) on it.

`OMKIT.INI` sections:

| Section | Keys |
|---|---|
| `[core]` | `log` (on/off), `log_level` (error, warn, info; debug - debug build only), `fw_heap`, `fw_heap_limit`; debug build only: `core_events`, `fw_cache_inv`, `power_rescue`, `taskdump`, `taskdump_every`, `taskdump_base`, `taskdump_stuck`. Unknown keys are reported in the log |
| `[modules]` | `enabled` (off: load nothing), then `NAME = on/off` per module; line order is load order |
| `[grain]` | the grain of the film look's stills: `mask`, `chroma`; test keys in a debug build only |
| `[optics]` | Bloom and Halation on the film look's stills: `layer` (on by default) and the settings of the two effects; test keys in a debug build only |
| `[debug]`, `[shoot]` | debug build only: the debug service over USB, the test scenario (a release build does not read them and says so in one line) |
| `[NAME]` | a module's own settings, plus two keys `core` reads for every module: `memory` (a size limit, e.g. `512K`) and `event_<event> = off | priority` |

The file is read once at power-on (at most 8 KB and 128 entries); a change takes effect at the next power-on. On a
repeated key the last value wins; names are case-insensitive. Every key, its default and range:
[config.md](config.md); the bundled modules' settings: [modules/](modules/README.md).

## 4. Boot sequence

```mermaid
sequenceDiagram
    participant FW as Firmware (start-up)
    participant L as Loader task (low priority)
    participant C as core (same task)
    participant M as Modules
    FW->>L: end of start-up: the hook creates the task, returns at once
    L->>L: wait until the firmware has unpacked its code (bounded)
    L->>L: bypass key held? -> exit (stock)
    L->>L: open OMKIT/CORE.BIN (retries while the card mounts)
    L->>L: check magic, ABI, length, checksum -> on error exit (stock)
    L->>C: jump to the core entry
    C->>C: driver check of the firmware -> mismatch: return, no log (stock)
    C->>C: memory, run id, OMKIT.INI, LOG and STORE folders, log
    C->>C: store CORE: boot count, "not ended" saved at once
    C->>C: power-off and sleep hooks; is a power-off already under way?
    C->>M: load modules in [modules] order: check, relocate, module_init
    M-->>C: register events, take services (their hooks go in now)
    C->>C: install the event hooks that have handlers
    C->>C: "ready", then the service loop (every 250 ms or when woken)
```

1. **Loader.** A hook at the end of the firmware's start-up only creates a task and returns: nothing is written to memory
   in the firmware's own start path. The task waits for the firmware to finish unpacking its code, runs once per
   power-on, gives up if the bypass key is held, and tries to open `OMKIT\CORE.BIN` several times while the card
   mounts. It reads the file, checks the header `{u32 'OMKC', u32 length, u32 sum of the body words, u32 ABI}` and the
   sum, and calls the entry. Any failure ends the task: the camera stays stock. The packed format is made by
   `tools/pack.py`.
2. **core start.** `core` runs in the loader's task with its own stack. It zeroes its `bss` and first asks the driver
   to check that the firmware is the one it was built for (fixed bytes at known places); on a mismatch it returns at
   once and the task ends. It calls no firmware function then - not even for a log line: every address the driver
   knows belongs to the other firmware (since core 1.30; before it one line `firmware mismatch` was logged).
3. **Configuration, log, store.** The run id (8 hex digits, also the name of the per-power-on log), `OMKIT.INI` (a
   template is written if there is none), the log, the `CORE` store. The boot record "this run has not ended" is saved
   to the card before anything risky happens, so that a crash while modules load is recognised at the next start.
4. **Power hooks** go in before the modules; if the firmware has already begun a power-off, modules are not loaded
   unless the power-off is cancelled.
5. **Modules** load one by one (section 7 of [modules.md](modules.md)). A module registers its event handlers and takes
   the services it needs only inside `module_init`; service hooks go in at that moment. The module file is read once:
   its on-demand data passes through `core` for the CRC and stays in memory until `module_init` returns, so the
   pictures a module sets at start cost no further card access.
6. **Event hooks** are installed after all modules have loaded, only for events that have handlers. From then on the
   handler lists never change.
7. **Service loop.** `core` keeps the task as its low-priority service loop.

**Measured on the camera:** the end of start-up is reached about 1.2 s after power-on and `core` runs about 1.4 s after
power-on. A user who switches the camera off earlier is handled by the "power-off already under way" check.

## 5. Tasks and CPUs

The camera runs an RTOS (uITRON-style tasks, event flags, mailboxes) on three CPUs. CPU 0 runs the UI, the application
and the file system; CPUs 1 and 2 run parts of the image pipeline and drawing (**from the firmware code**, confirmed by
hooks seen on CPUs 1 and 2 on the camera).

| Code | Where it runs |
|---|---|
| Loader and `core` | one task on CPU 0, lower priority than the firmware's file and UI tasks |
| `core` service loop | the same task: wakes every 250 ms, or at once when a power hook, an editor hook or a deferred call (`defer`) signals it (an RTOS event flag; `core_events = off` falls back to a plain sleep, and so does a flag whose wait fails at once 50 times in a row or returns at once more than 500 times a second) |
| `module_init`, deferred calls (`defer`), periodic calls (`every`), power handlers, profile and look callbacks | the `core` task |
| Module event handlers | the firmware task that hit the hook, on CPU 0 only |
| Driver hooks | any firmware task, on any CPU; on CPUs 1 and 2 a hook does its driver work but calls no module handler |

What the service loop does on every pass, in order: power state; while the camera runs - saving changed stores and
writing the log (both postponed while the camera itself is writing the card), memory and stack checks, deferred calls,
the services (profiles, look, ISP, grain), the modules' periodic calls, the task dump.

**Caches.** The data caches of the three CPUs are not coherent with each other (**from the firmware code**, and a
camera crash loop when the rule was broken). Therefore:

- `core`'s image, `bss` and stack are used through cached addresses and only by the `core` task and CPU 0 hooks;
- the **arena** (the memory `core` gives to modules, section 7.5) is used through uncached addresses and is the same
  for every CPU; everything a hook on another CPU reads or writes lives there. The one exception inside it is a
  module's image;
- a module's image - its code, constants and its own variables (`data`, `bss`) - is written through the uncached
  address and then used through the cached alias of the same memory: the code executes there, and the module's
  variables go through the data cache of CPU 0, the only CPU module code runs on. They are therefore CPU 0's, like
  `core`'s own: another CPU does not see them reliably ([sdk.md](sdk.md), section 2). `core` gives every image whole
  cache lines - its block starts and ends on a 64-byte boundary (the first-level line is 16 bytes; 64 covers a second
  level with longer lines) - so a line written back from the cache never carries the bytes of a neighbouring arena
  block, which is used past the cache. Memory that once held running module code is never handed out again;
- the image with its stack and the arena must not overlap: the linker script refuses a `core` image that would reach the
  arena.

## 6. Hooks

A hook redirects one place of the firmware code, in RAM, to a trampoline in `core`. The flash is not changed: hooks
disappear at power-off and are installed again at the next start.

| Step | How |
|---|---|
| Choice of the place | the driver keeps a list of hook sites; each is a 5-byte jump that does not cross a 16-byte instruction cache line and displaces only instructions without relative addresses |
| Check | before writing, the bytes at the site are compared with the expected ones; a mismatch is a refusal with a code in the log, never a patch over unknown code |
| Write | the firmware code range is guarded by the memory protection unit: a write through a cached address resets the camera. The driver writes through the uncached alias, byte by byte, the first byte last |
| Trampoline | saves the registers and flags, calls a C function of the driver, replays the displaced instructions, jumps back |
| Instruction cache | not invalidated (the firmware has no run-time function for it); a hook never crosses a cache line, so a CPU sees either the old or the new bytes. **Measured on the camera:** hooks work on CPU 0, 1 and 2 with the protection on |

Who installs hooks:

- `core` at start: the power-off and sleep hooks;
- the services, when a module takes them in `module_init` (profiles, film look, ISP colour tables, grain);
- the event manager, after all modules are loaded, for events that have handlers.

Modules never install hooks. A module that needs to react to a firmware function uses an **event** (section 7.1); a
module that needs to change what the camera does uses a **service** of `core`. A new need means a new event or service in
`core` and the driver.

## 7. Core services

### 7.1. Events

An event is a meaning (`OMK_EV_PICTMODE_CHECK`: "the firmware checks whether a picture mode is allowed"), with a data
structure in `omk_events.h`. The driver maps it to a hook and to the firmware's registers; a module sees only the
structure.

```
firmware -> jmp -> driver trampoline -> core dispatcher -> NOTIFY_BEFORE handlers (a copy of the data)
                                                        -> FILTER_BEFORE handlers (may change the input)
                                                        -> REPLACE chain: each calls next(), the last next()
                                                           runs the original firmware function
                                                        -> FILTER_AFTER handlers (may change the result)
                                                        -> NOTIFY_AFTER handlers (a copy)
                                     <- back to the firmware with the result
```

- The driver declares which events exist on this firmware and which phases each allows; registering anything else
  fails, and the module decides whether to work without it.
- Order: higher priority first, ties in module load order. `OMKIT.INI` `[NAME] event_<event> = off | number` disables a
  handler or overrides its priority. The final order is written to the log at start.
- Handlers run only on CPU 0, one invocation of an event at a time. A call on CPU 1 or 2, or a nested call of the same
  event while its handlers run (the same task reaching the hook again), goes straight to the original function and is
  counted (debug log every 10 s). So does a call from another task of CPU 0 that preempted the first inside its
  handlers: it is not made to wait - the hook sits in a firmware task that may hold firmware locks - and gets what the
  firmware does without OMKit; such calls are counted apart and reported as a warning (`event NAME: N calls came from
  another task ...`). The claim of an event is one step under the firmware lock, with the claiming task recorded.
- Long work is passed to the `core` task with `defer` (a queue of 32 calls shared by all modules); the call wakes the
  `core` task, so it runs at once rather than at the end of the 250 ms wait (core 1.30; a `defer` made in the `core`
  task itself runs at its next pass). A pass runs only the calls queued when it began: a call that defers itself again
  runs once a pass, and the rest of the loop - power, log, stores - runs in between.

### 7.2. Services

A service is a feature where the module describes data and `core` with the driver builds it into the firmware: a
module owns it, `core` keeps the hooks alive and delivers the user's actions to the owner in the `core` task.

| Service | What the owner gives | Owners |
|---|---|---|
| Picture profiles | profile choices, pictures, monochrome filter matrices | one module |
| Film look | a stock picture mode slot shows the module's look: entries, names, pictures, cassette art, editors | one module |
| ISP colour tables | the platform's colour block for live view and/or stills | one module |
| Grain | tile and strength per look entry; `core` owns the tile files | the film look's owner |
| EXIF comment | a text for the next pictures | any module (needs profiles or the look in place) |

The "one owner" services are taken by the first module that asks, inside its `module_init`; if that module then fails,
the service is released: the profile or the look is turned off, the camera's own colour tables come back and the grain
is off (the ISP tables and the grain since core 1.30), and a module loaded later may take the service. The stored picture mode is never changed to a value the stock firmware does not know: a
profile or a look is shown over a stock mode and applied to shooting from memory.

### 7.3. Log

A ring buffer in RAM between the writers (any task on CPU 0) and one consumer, the `core` task, which appends what is new
to the file. A line is formatted on the caller's stack; under the firmware lock it is only copied into the ring.

| Property | Value |
|---|---|
| Line | `  ms L who: text`, at most 192 bytes (longer lines are cut, `~` at the end) |
| Levels | E, W, I, D (D in a debug build only); `log_level` in `[core]` (default info); `log = off` disables it. Info holds what changes state: start, modules, what the user chose, power, failures put right. Lines that repeat with use - one per picture, per dial step or per second in an editor - are debug (core 1.30) |
| Ring | 32 KB; a line that does not fit is dropped whole and counted (`log: N lines lost`) |
| Write | when the log has been quiet for 0.5 s, at least every 3 s; not while the camera writes the card, half-presses (up to 6 s, and 0.5 s after it) or changes state (both build kinds: the application's state is read from memory once a pass, no register), never during power-off or sleep, and not while the power lever is off but the camera did not switch off (the battery may be taken out at any moment; the lines wait in memory) |
| No card or folder | the file does not open: the next try after 1 s, then 2, 4 ... up to 60 s apart, not at every pass; a write that works, or the camera running again after a sleep or a power-off that did not happen, ends the pauses (`log: the card takes the log again, after N tries without it`). The lines wait in the ring meanwhile |
| File | `LOG\CORE.LOG`, one for all power-ons: each start appends to it, `core` never removes or cuts it (delete it on a computer when it gets too long). Each power-on begins with an empty line and a header: `======== OMKit core <version> <release|debug> (<branch>@<commit>), <platform>, run <run id>, started <date and time> ========` (the build's kind, branch and commit; the camera clock's date), then, when the modules are loaded, `modules: NAME 1.5, ...` with their versions. Older versions removed the file at each power-on, and `run_logs = on` wrote a file per power-on; that key is gone (ignored), old `XXXXXXXX.LOG` files are left as they are |
| Open file | the file stays open between writes (each portion is written and flushed at once), because the first write after an open walks the file's cluster chain from its start - a cost that grows with the file; it is closed after 30 s without lines, at sleep and power-off, and before a file job of the debug service. When the camera mounts the card again (its PC mode over USB), it frees every open file without a write: `core` sees that by the file's identity and opens the log again (`log: ... opened again`), never closing a freed file - its slot may already be another file's. While the card is not mounted nothing is written |
| After a crash | the run that went down is simply before the next one in the file; the `CORE` store tells whether it ended with a power-off |
| Power-off | the last lines are written at the card-off point of the power-off |
| Boot checkpoints | a debug build at `debug` level: a few lines (module loading, hook installation) are written to the card at once, so a crash leaves a trace |

Modules log only through `api->log`; there are no other log files.

### 7.4. Store

A small key-value dictionary per owner (`u16` key, bytes value), the usual embedded pattern of two copies with a sequence
number and CRC-32.

- Reads and writes go to memory only; they are short and can be made from any task on CPU 0.
- Values marked persistent are saved by the `core` task 1.5 s after the last change - but no later than 12 s after the
  first change not saved yet, so a value that keeps changing still reaches the card -, alternately to `NAME.STA` and
  `NAME.STB`; at start the intact copy with the higher sequence number wins. A failed or cut write leaves the other copy
  intact. Only the persistent records are written.
- Both copies have one fixed length (the header and the store's whole record space, rounded up to 512 bytes, zeros
  after the records) and a save rewrites the copy in place: no file is made, removed or grown, so a reset during a save
  cannot break the card's volume (above). A copy that is not there or has another length - a new card, a store of
  another size, the files of an older core - is made once, at its first save.
- A save that fails (no card, no folder, a short write) is tried again after 3 s, then 6, 12 ... up to 60 s apart
  (`store NAME: cannot open ...` once, `store NAME: card back, saved` when it works); the camera running again after a
  sleep or a power-off that did not happen ends the pauses. The save at the card-off point is always tried.
- Saving waits while the camera writes the card, stops during sleep and power-off, and the last save happens at the
  card-off point of a power-off (after the modules' power-off handlers).
- `core`'s own store `CORE` keeps the boot count and whether the last run ended with a power-off.

### 7.5. Memory

| Source | Use | Access |
|---|---|---|
| Arena: memory regions the driver reports as unused by the firmware (a few MB on the first platform) | module images, resources, `core` buffers, module allocations | uncached addresses, all CPUs; a module's image through the cached alias, CPU 0 (section 5) |
| Firmware heap, only with `fw_heap = on` and `fw_heap_limit` | small module data blocks, when the arena is full | CPU 0 only; never code or resources; `core` leaves at least a quarter of each heap class to the firmware |

- Modules never allocate memory themselves: `api->alloc` and `api->free`. Each module is an owner with counters (used,
  peak, allocations, failures) and an optional limit (`memory` in `[NAME]`).
- The allocator is first-fit with merging of neighbours, 16-byte aligned blocks with a 16-byte header that names the
  owner. It runs under the firmware lock and only on CPU 0. A module's image block is the exception in alignment: its
  data start and end on 64-byte boundaries (whole cache lines, section 5), which costs up to 112 bytes left free in
  front of it and up to 63 at its end.
- `core` takes its own buffers first, so modules cannot starve the log and the stores.
- The log shows the first failure of an owner and then a count every 10 s, a warning when the arena is more than 90 %
  used, and at `debug` level a summary every 30 s.

The firmware heap is shared with shooting and the UI: it stays off by default.

### 7.6. Power-off and sleep

`core` distinguishes three states: running, power-off, sleep. Driver hooks in firmware tasks report the changes; the
`core` task follows in its loop and acknowledges each change.

```
running --(power-off begins)--> power-off --(card-off point: wait for core, at most 1 s;
   ^                                |         last log write and store save)--> camera off
   |                                +--(still alive after 5 s: cancelled)--> running
   +--(wake, or no sleep within 15 s)-- sleep --(sleep entry point: wait for core, at most 1 s)--> frozen
```

| State | What `core` does |
|---|---|
| power-off | logs the reason; calls the modules' power handlers (`OMK_POWER_OFF`, the last loaded module first); stops card writes and deferred work. At the card-off point the firmware task waits (at most 1 s) for the acknowledgement, then `core` marks the run "ended", saves the stores and writes the log from that task |
| sleep | calls `OMK_POWER_SLEEP`; no card writes (the card loses power in sleep); every task, `core` included, is frozen until wake |
| wake / resume | `OMK_POWER_WAKE` or `OMK_POWER_RESUME` in load order; card writes start again; the millisecond clock is corrected after a long sleep from the subprocessor clock |

While the state is not "running", the API file functions refuse, deferred and periodic calls do not run, and the log
waits in memory. Long card work of `core` itself gives way too: a power-off or a sleep that begins while the modules
are loading stops the loading between two modules (the rest loads when the camera runs again; the event hooks go in
once all are loaded), and a grain tile being read is given up between two 4 KB pieces and read again later. The gates never hold the firmware longer than 1 s, whatever `core` does (**measured on the camera**:
the gate usually waits 0 ms, because `core` has acknowledged long before).

A deep sleep (auto power-off) is a real power-off for `core`: the next wake is a new boot (**from the firmware code**).
The freezing of all tasks in a light sleep is also **from the firmware code**, observed in the emulator.

### 7.7. Diagnostics

`core` is built in two kinds ([build.md](build.md#35-release-and-debug-builds)). A release build keeps the first item
below and the stack warning; the crash records, the used stack depth, the task dump and the debug service are compiled
into a **debug build only**, like every probe of the camera's state that the user's functions do not need.

- **Boot count and unclean end.** The `CORE` store tells whether the last run ended with a power-off; if not, the log
  says so; the lines of the run that went down are just before, in the same `CORE.LOG`.
- **Crash records.** At start, `core` logs the reset records the firmware's NMI handler left (task, cause, registers),
  when there are any.
- **Stack.** The `core` stack (16 KB) is filled with a pattern; the used depth is logged at debug level, a warning when
  less than 2 KB was never used.
- **Task dump.** When a power-off or sleep does not happen, or the application stays in a state that normally lasts a
  moment, `core` reads the RTOS task tables of the three CPUs (read only, no locks, no firmware calls) and writes every
  task's state, wait object and return addresses to the log (`taskdump`, `taskdump_every`, `taskdump_base`,
  `taskdump_stuck` in `[core]`; at most 8 dumps per power-on).

## 8. Failure handling

| What fails | What happens | Where it is seen |
|---|---|---|
| No card, no `OMKIT\CORE.BIN`, bad header or checksum | the loader task ends | camera is stock; no log |
| Bypass key held at power-on | the loader does nothing | camera is stock |
| `CORE.BIN` built for another firmware | `core` returns at once, without calling the firmware | camera is stock; no log of that power-on |
| A hook site does not hold the expected bytes | that hook is not installed; the feature that needs it is off | `hook ... not installed, code N` |
| Module file missing, wrong name, newer API, bad sizes, relocation outside the image, CRC mismatch, out of memory | the module is not loaded, its memory is freed; the others load | `module NAME: <reason>, not loaded` |
| `module_init` returns non-zero | the module is unloaded: its handlers, power handler, periodic call and services are dropped, its store is closed without saving, its memory is freed except the image (its cache lines may still exist) | `module_init returned N, unloaded` |
| `alloc` fails at run time | the module gets 0 and must carry on | `memory: NAME wants N bytes ...` |
| Card not available (sleep, power-off) | `core` keeps the log and stores in memory and writes later; API file functions refuse | `file_open(...): the card is not available now` |
| No card or no `OMKIT` folders while the camera runs | log and store writes are tried again with growing pauses (up to 60 s), sections 7.3 and 7.4 | `store NAME: cannot open ...`, later `card back, saved` |
| Card busy (the camera is writing a picture) | `core` postpones its own log and store writes; modules ask `card_busy` | `card: core writes waited N ms` (debug) |
| Defer queue or log ring full | the call or line is dropped and counted | `defer queue full`, `lines lost` |
| A module crashes (bad pointer, write to protected memory) or hangs a firmware task | there is no isolation between modules and the firmware: the camera resets (or must be switched off). The next run logs the unclean end and the crash records | `the previous run ... did not end with a power-off`, `NMI record` |

There is no automatic disabling of a module that crashes the camera. To recover: set `NAME = off` in `[modules]`,
rename or remove the module file, hold the bypass key at power-on, or take `CORE.BIN` off the card. Note that the
firmware may match a long file name by its start: rename `CORE.BIN` to a name that does not start with `core.bin`
(**measured on the camera**).

## 9. Source layout

| Path | Contents | Depends on the platform |
|---|---|---|
| `src/core/` | `core` in C: `main.c` (start, service loop), `cfg.c`, `log.c`, `store.c`, `mem.c`, `module.c` (loader), `api.c` (the API table), `event.c`, `power.c`, `prof.c`, `look.c`, `isp.c`, `grain.c`, `taskdump.c`, `clock.c`, `util.c`; `core.ld` | no (the link address in `core.ld` is the loader's) |
| `src/core/include/` | internal headers; `omk_drv.h` is the whole driver interface | no |
| `src/platform/<platform>/<firmware>/` | the driver: `drv.c` (check, files, memory, hooks, power), service drivers, `thunks.s` (trampolines and calls into the firmware), `fw.ld` (firmware symbols) | yes |
| `src/sdk/include/`, `src/sdk/lib/` | everything a module sees ([sdk.md](sdk.md)) | no |
| `src/modules/` | modules (`<name>/*.c`, `<name>/res/`), the shared `Makefile` and `module.ld` | no |
| `src/stub/platform/<platform>/<firmware>/` | the loader: `stub.s` (the hook body) and `gen.py` (its description for the patch builder) | yes |
| `resources/modules/<name>/` | files modules need, kept in git: pictures, grain tiles | no |
| `tools/` | `pack.py` (`CORE.BIN`), `mkmod.py` (`.OMM`), picture generators, grain tile set check | no |
| `tests/host/` | host tests of `core` parts with a stub driver | no |
| `toolchain/` | the toolchain build (binutils and GCC) for the CPU | per CPU |

The compile flag `OMK_EMU` (`make -C OMKit EXTRA=-DOMK_EMU`) makes a `core` for running in an emulator: the parts
under `#ifdef OMK_EMU` let an emulator test check the firmware heap thunks at boot and set the film look's UI entry
(`src/core/main.c`, `pictprof.c`). Such a build is never for a camera; `make`, `make card` and the release build do not
set it.

## 10. Adding a platform or a firmware version

`core`, the SDK and the modules do not change. A new camera or firmware version needs:

1. `src/stub/platform/<platform>/<firmware>/`: a loader patch - a hook at the end of the firmware's start that creates a
   low-priority task; the task waits for the firmware's code, honours a bypass key, reads `CORE.BIN` with the
   firmware's file functions, checks it as `tools/pack.py` packs it, and calls the entry. The `CORE.BIN` load address
   must be memory the firmware does not use.
2. `src/platform/<platform>/<firmware>/`: an implementation of every function of `omk_drv.h`. The minimum for `core` and
   modules to run: `drv_name`, `drv_root`, `drv_sep`, `drv_check`, the file functions, `drv_sleep`, `drv_ticks`,
   `drv_cpu`, `drv_lock`/`drv_unlock`, `drv_mem_region`, `drv_exec_addr`, `drv_code_sync`, `drv_code_jump`. Features
   the firmware does not support return "not supported" (no events, `drv_prof_count` 0, a failing `drv_isp_install`, ...):
   the API then refuses them and modules carry on without them.
3. A linker script `fw.ld` with the firmware symbols, and `PLATFORM` in the top `Makefile`.
4. A page under `docs/platform/<platform>/` with what was found: memory map, cache and protection rules, hook sites,
   what was measured on the camera.

A camera with another CPU also needs its own toolchain and `module.ld`; the C sources stay the same.
