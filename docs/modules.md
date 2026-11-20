# OMKit modules

A module is a piece of C code that `core` loads from the card at power-on and that works only through the public API
([sdk.md](sdk.md)). It never sees the firmware: no addresses, no firmware functions, no hooks. Everything that depends on
the camera is an event or a service of `core` ([architecture.md](architecture.md)).

This page says what a module is, the rules every module must follow, and how to write a first one. The modules that
come with OMKit (FILMLOOK, CREATPRF, HELLO) are described in [modules/](modules/README.md); the settings file in
[config.md](config.md).

## 1. What a module is

| Property | Value |
|---|---|
| File | `OMKIT\MODULES\NAME.OMM` on the card |
| Name | 1-8 characters `A-Z 0-9 _`, not `CORE`. The name in the file must match the file name. It is also the module's config section `[NAME]`, its store `STORE\NAME.STA/.STB`, its folder `OMKIT\NAME\` and the prefix of its log lines |
| Enabled by | a line `NAME = on` in `[modules]` of `OMKIT.INI`; line order is load order; `enabled = off` in `[modules]` loads none |
| Version | `major.minor` (16 bits each), given at build time (`VERSION=1.2`), shown in the log at load |
| API level | the `OMK_API_VERSION` of the SDK it was built with, stored in the file; `core` refuses a module that needs a newer API than it has |
| Contents | the image (code, constants, data; linked at address 0), relocations, a resource directory, resources kept in memory, resources read from the card on demand; CRC-32 of the whole file |
| Entry | `int module_init(const omk_api *api, omk_mod *self)` |
| At most | 16 modules; image 2 MB, `bss` 2 MB, 1024 resources, 2 MB of in-memory resources, 64 MB of on-demand resources, 64 KB of strings |

The file format is in `src/core/include/omk_mod.h`; `tools/mkmod.py` makes it ([sdk.md](sdk.md#9-build-tools)).

### 1.1. Resources

| Kind | Source in the module folder | In the module | Access |
|---|---|---|---|
| In memory | `res/<type>_<id>.<any>` | loaded with the module | `api->res(self, type, id, &size)` - a pointer, valid while the module runs |
| On demand | `res/disk/<type>_<id>.<any>`, and `resources/modules/<name>/icons/` for modules in this tree | stays in the file on the card (its CRC is checked at load) | `api->res_read(self, type, id, offset, buf, len)` |
| Strings | `res/strings.txt`: `NAME = text` lines, `#` and `;` comments | a string table in memory; `res_strings.h` with `STR_NAME` ids is generated at build time | `api->str(self, STR_NAME)` |

Types 1..32767 are the module's own; ids 0..65535 (decimal in the file name). Strings are English only, printable ASCII
(0x20-0x7E); identical texts are stored once.

### 1.2. Its own folder and config section

- `OMKIT\NAME\` holds the module's files (presets, data). `core` creates it at the module's first file call
  (`file_open`, `file_list*`; API 19), so only modules that use files get one; the user puts the files there.
- `[NAME]` in `OMKIT.INI` holds the module's settings (`api->cfg_*`). Two keys are read by `core` for every module:
  `memory = 512K` (the module's memory limit; none by default) and `event_<event> = off | priority` (per event handler).

## 2. Lifecycle

```
power-on
  core checks the file (name, format, API level, sizes, relocations, CRC)   -> failure: "module NAME: <reason>, not loaded"
  core allocates image + bss in the arena, relocates, zeroes bss
  module_init(api, self)                    core task; may take long only at the cost of the camera's features
     register events, power handler, periodic call, take services, open the store, read files and resources
     returns 0  -> running
     returns !0 -> unloaded: handlers, services, periodic call dropped; store closed without saving; memory freed
  (all modules loaded) core installs the event hooks; handler lists are fixed from now on
running
  event handlers              firmware task, CPU 0, while the firmware runs that code
  deferred calls (defer)      core task, in order
  periodic call (every)       core task, about every N ms (250 ms steps)
  service callbacks           core task: profile chosen, look turned on, an entry or an edit chosen
  power handler               core task: OMK_POWER_OFF / RESUME / SLEEP / WAKE
power-off
  OMK_POWER_OFF (last loaded module first): put what must survive into the store; core saves it at the card-off point
```

There is no unload and no `module_exit`: a module runs until the camera is switched off. The power-off handler is its
shutdown. After `OMK_POWER_OFF` the module may still get `OMK_POWER_RESUME` if the power-off is cancelled.

`module_init` is the only place where a module may:

- register event handlers (`on`), set the power handler (`on_power`);
- open its store (`store_open`);
- take a service for the first time (`prof_enable`, `look_enable`, the first `isp_*` call, the first `grain_list`).

## 3. Rules

Each rule says what `core` checks itself and what is up to the module. A module that breaks a safety rule can reset the
camera or, worse, leave something in the camera that survives a power-off: there is no memory isolation between a module
and the firmware.

### 3.1. Safety

| # | Rule | Why |
|---|---|---|
| S1 | **Never write to flash or ROM, never write the camera's settings storage, never call the firmware's settings save.** There is no API for it, and a module must not reach it by other means | Settings are written to flash by the firmware itself; a non-stock value would survive a return to stock firmware and can make the camera fail at start |
| S2 | **Do not touch the camera's memory outside the API.** No firmware addresses, no reads or writes of firmware data, no calls of firmware functions, no hooks, no peripheral registers. Build only against `src/sdk/include` | The firmware's memory is guarded and shared with three CPUs whose caches are not coherent; only the driver knows the safe ways. The build refuses undefined symbols and absolute short references, but C pointers can still go anywhere |
| S3 | **Never write firmware code or data, least of all through cached addresses.** Everything the module wants changed in the camera goes through a `core` service | A write to the guarded firmware range through a cached address resets the camera (**measured on the camera**); `core` writes such places only through the driver, through uncached addresses |
| S4 | **Keep the camera's stored settings stock.** Keep the module's state in its store or its files; apply effects from memory | A profile or a look is shown over a stock picture mode and applied to shooting from memory: removing the card leaves a stock camera |
| S5 | **No long blocking in event handlers.** No card, no `sleep`, no waiting, no loops over large data, no allocation in a loop. Count, copy a few words, and `defer` the rest | A handler runs inside a firmware task (UI, application, shooting) with the firmware's own locks and deadlines |
| S6 | **Never delay a power-off or a sleep.** The power handler is short: stop timers, update the store in memory, return. No card (the API refuses it then), no `sleep` | The firmware waits for `core` at most 1 s and then goes on regardless; a slow handler loses the last log and store save. Handlers over 50 ms get a warning |
| S7 | **The card may be busy with a picture.** Read and write files in `module_init` or in response to a user action, not periodically; keep state in the store, not in files | The camera writes pictures to the same card; `core` postpones its own writes while it does, but module file calls are not postponed: check `api->card_busy()` (API 19) before writing and try again later |
| S8 | **Survive the absence of anything.** No card, no folder, no file, an event or service not supported on this firmware, a service owned by another module, `alloc` returning 0 | Every one of these happens in normal use; the camera must keep working |
| S9 | **Free only your own memory, and only once.** Never keep a pointer to memory after freeing it | `core` refuses to free a block that is not the module's (logged), but a stale pointer is a silent corruption |
| S10 | **Your static variables are CPU 0's: never give their address to anything other CPUs read.** Hand data to the camera only through API calls (the services copy it); memory that must be visible to every CPU comes from `alloc` | The module's `data` and `bss` go through the data cache of CPU 0, and the caches of the three CPUs are not coherent ([sdk.md](sdk.md), section 2). `core` keeps the image in cache lines of its own, but cannot make another CPU see them |

### 3.2. Correctness

| # | Rule | Details |
|---|---|---|
| C1 | **Log only through `api->log`, and log your steps.** No own log files. Start-up stages at `debug`, decisions at `info`, refusals and failures at `warn` / `error` | From the log alone it must be clear where the module stopped and why. The line carries the module name |
| C2 | **Check every API return code** and take a safe path on failure, with a log line | Codes per function: [sdk.md](sdk.md). Typical: `on` -1 (not on this firmware), `prof_enable` -2 (another owner), `file_open` 0 (no folder), `alloc` 0 |
| C3 | **Check the API level in `module_init`** before using the table: `api->version` and `api->size`. Use a function of a later level only after checking `api->version` | The table only grows. A function's level is in [sdk.md](sdk.md) |
| C4 | **Call each function only from the context it allows.** | Table in [sdk.md](sdk.md#2-calling-contexts) |
| C5 | **Strings are printable ASCII, English.** Log text, names shown in the UI, EXIF text, `strings.txt` | The UI gets ASCII; `mkmod` refuses other characters in `strings.txt` |
| C6 | **Names are 8.3**: the module name (1-8 of `A-Z 0-9 _`), file names (`NAME.EXT`, three-letter extension), no directories | The firmware's FAT layer cuts or mangles other names; `file_open` refuses them (long names: read only, from API 15) |
| C7 | **Stay within the limits**: image and resource sizes, the `memory` limit, the store size given to `store_open`, look and grain entry counts, string lengths | Over a limit a call fails with a code; nothing is cut silently except log lines (192 bytes) |
| C8 | **Time arithmetic wraps.** Compare times as `now - then >= interval` with `u32`, never `now >= then + interval` | `api->ms()` is a 32-bit millisecond count |
| C9 | **Data shared between an event handler and the `core` task** is changed by tasks that can preempt each other on CPU 0: use single words, counters and flags, or a sequence number; do not share structures being rebuilt | Handlers and the `core` task both run on CPU 0, but in different tasks |
| C10 | **Return non-zero from `module_init` only when the module cannot work at all.** A missing optional feature is a log line, not a failure | A failed module is unloaded, and its image memory stays used until power-off |
| C11 | **Do not use stack-heavy code in handlers.** No large local arrays in event handlers | They run on the stack of a firmware task |
| C12 | **Pictures follow the platform's picture rules; a dial label is the camera's own word or one drawn by `tools/dial_label.py`.** Up to 56 x 24, no descenders, a shorter word rather than a smaller one; an editor's drawing starts where the curve's window does | The labels and editors of every module then look and stand alike: [platform/om5m2/ui.md](platform/om5m2/ui.md#451-rule-dial-labels-and-the-editing-area), sections 4.5.1 and 5.5 |

### 3.3. Performance

| # | Rule | Details |
|---|---|---|
| P1 | **Keep periodic calls short.** A periodic call (`every`) over 100 ms is logged as `NAME: the periodic call took N ms` | It runs in the `core` loop: while it runs, the log, the stores, the other modules and the services' delivery of user actions wait. Split long work over several calls |
| P2 | **No heavy work in hooks on the shooting path.** Event handlers and anything the camera calls while it takes a picture or runs live view must cost microseconds | Shooting and live view have deadlines; handlers also hold up the task that called them |
| P3 | **Do not log from handlers per call.** Count in the handler and log a summary from a deferred call, at most one deferral pending (see `src/modules/hello/hello.c`) | A log line takes the firmware lock to copy into the ring; the ring is 32 KB and drops whole lines when full |
| P4 | **Keep `module_init` fast.** Its time is logged at debug level (`module_init returned 0 in N ms`). Read many on-demand resources in `module_init`: `core` has the module's on-demand data in memory there (up to 256 KB of it; beyond that the module file stays open between the reads), later every `res_read` opens the file | Later modules and the camera's features wait for it; the camera is already in use |
| P5 | **Respect the arena budget.** It is shared by all modules and `core` services (the grain service keeps 768 KB free for the modules that load after it). Keep large data on demand and read it when needed; free buffers you no longer need; check `mem_info` | An arena more than 90 % used is logged; out of memory the module gets 0 |
| P6 | **Read only what you need from the card.** `res_read` a part, not the whole resource; avoid `file_list` loops (every step of the firmware's folder walk reads the folder from the start; at most 256 entries are looked at) | Card access is slow and shared with the camera |
| P7 | **Do not flood the defer queue.** 32 entries for all modules; keep at most one call of a kind pending | A full queue drops the call (`events: defer queue full, N calls lost`) |
| P8 | **Respect service rates**: a new ISP gamma or sector table at most once a second, an edit every 40 ms (see `isp_override`) | Faster calls return -2 (too soon) |

## 4. Your first module

### 4.1. Prepare

1. Build the toolchain once (Linux or WSL): `bash OMKit/toolchain/build-toolchain.sh` (installs into `~/opt/mn10300`;
   [build.md](build.md)).
2. Build `core`: `make -C OMKit` gives `OMKit/build/CORE.BIN` (`OUT=<dir>` for another folder).
3. A card with the loader in the camera's firmware: put `CORE.BIN` into `OMKIT\` on the card and switch the camera on
   once. `core` writes `OMKIT\OMKIT.INI` with the defaults and the log `OMKIT\LOG\CORE.LOG`.

### 4.2. Start from `hello`

`src/modules/hello` exercises the whole basic API (log, config, memory, store, files, resources, strings, events, power;
what each part shows: [modules/hello/README.md](modules/hello/README.md)).
Copy it to a new folder, for example `src/modules/mymod`, or to a folder outside the tree. The folder name, upper-cased,
is the module name: `mymod` gives `MYMOD.OMM`.

A minimal module:

```c
#include "omk_api.h"

static const omk_api *api;
static omk_mod *self;
static u32 ticks;

static void tick(void *ctx)
{
    (void)ctx;
    ticks++;
    api->log(self, OMK_LOG_DEBUG, "tick %u at %u ms", ticks, api->ms());
}

int module_init(const omk_api *a, omk_mod *m)
{
    int period;

    if (a->version < OMK_API_VERSION || a->size < sizeof(*a))
        return 1;                           /* a core older than this SDK */
    api = a;
    self = m;
    api->log(self, OMK_LOG_INFO, "starting on core %s, api %u", api->core_version, api->version);
    period = api->cfg_int(self, "period_ms", 5000);
    if (period < 250 || period > 3600000) {
        api->log(self, OMK_LOG_WARN, "period_ms = %d out of 250..3600000, 5000 taken", period);
        period = 5000;
    }
    if (api->every(self, (u32)period, tick, 0))
        api->log(self, OMK_LOG_WARN, "no periodic call");
    return 0;
}
```

Notes: the build uses `-Wall -Wextra -Werror`, so unused parameters need `(void)`; there is no C library beyond
`memcpy`, `memmove`, `memset`, `memcmp` and `strlen` (`src/sdk/lib/omk_libc.c`) - no `printf` (use `api->log`), no
`malloc` (use `api->alloc`).

### 4.3. Build

```sh
make -C OMKit/src/modules MOD=mymod VERSION=1.0 OUT=<dir>                     # a module in src/modules/mymod
make -C OMKit/src/modules MOD=mymod MODDIR=/path/to/mymod VERSION=1.0 OUT=<dir>   # a module kept elsewhere
```

The result is `<dir>/MYMOD.OMM`; `mkmod` prints its sizes. Without `OUT` it goes to `OMKit/build/modules/`; a relative
`OUT` is relative to `OMKit/src/modules`, so pass an absolute one.

### 4.4. Install and enable

1. Copy `MYMOD.OMM` to `OMKIT\MODULES\` on the card.
2. In `OMKIT.INI` add the module to `[modules]` and give it settings:

   ```ini
   [core]
   log_level = debug

   [modules]
   enabled = on
   MYMOD = on

   [MYMOD]
   period_ms = 2000
   ```

3. If the module uses files, put them in `OMKIT\MYMOD\` (`core` creates the folder at the module's first file call).

### 4.5. Run and read the log

Switch the camera on, use it, switch it off with the power lever (the last lines are written at power-off), and read
`OMKIT\LOG\CORE.LOG`:

```
    1452 I OMKit core 1.27, om5m2 fw1100, run 3F09A2C1
    ...
    1630 I module MYMOD 1.0: image 412, bss 8, resources 0, at <address>
    1631 I MYMOD: starting on core 1.27, api 19
    1640 I modules: 1 running, 0 failed
    1641 I ready, api 19
    3700 D MYMOD: tick 1 at 3700 ms
```

What to look for:

| Line | Meaning |
|---|---|
| `module MYMOD: no file MODULES\MYMOD.OMM` | the file is not on the card or has another name |
| `module MYMOD: needs a newer core API` | the module was built with a newer SDK than this `CORE.BIN` |
| `module MYMOD: checksum mismatch` | the file is damaged (copy it again) |
| `module MYMOD: module_init returned N, unloaded` | the module refused to start; its own lines before this say why |
| `MYMOD: the periodic call took N ms` | performance rule P1 |
| `the previous run ... did not end with a power-off` | the last run crashed or the battery was taken out; `CORE.LOG` keeps that run's lines above the new ones |

### 4.6. Iterate

Change, build, copy the `.OMM`, switch the camera off and on. If the module resets the camera at start, the camera keeps
restarting while it is enabled: hold the bypass key (OK) at power-on or take the card out, then set `MYMOD = off` or
remove the file. With `run_logs = on` in `[core]`, every power-on also gets its own log `OMKIT\LOG\<run id>.LOG`.
