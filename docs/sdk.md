# OMKit SDK and module API

Everything a module can use: the headers in `src/sdk/include`, the small C library in `src/sdk/lib`, and the build tools.
The code is the reference: `src/sdk/include/omk_api.h` (the table), `src/core/api.c` (what each slot is bound to) and the
`core` sources behind them. This page describes API level **18** (`core` 1.24). Rules for modules: [modules.md](modules.md).

## 1. Headers

| File | Contents |
|---|---|
| `omk_api.h` | the API table `omk_api`, its constants, `module_init`. The only header a module needs; it includes the two below |
| `omk_types.h` | `u8`, `u16`, `u32`, `s16`, `s32`, `uptr` (an address as a number) |
| `omk_events.h` | event ids, their data structures, handler phases `OMK_PH_*` |
| `omk_ispb.h` | offsets of the platform's ISP colour block (`ISPB`) and `omk_ispb_mono()`; for modules that build or read blocks |
| `src/sdk/lib/omk_libc.c` | `memcpy`, `memmove`, `memset`, `memcmp`, `strlen`; built into every module (GCC calls them for structure copies and zeroing) |
| `res_strings.h` | generated at build time from the module's `res/strings.txt`: `STR_<NAME>` ids and `STR_COUNT` |

A module includes nothing from `src/core` or `src/platform`; the build only gives it `src/sdk/include` and its generated
folder.

Handles are opaque: `omk_mod` (the module, given to `module_init`), `omk_store`, `omk_file`, `omk_ev` (one event call).

## 2. Calling contexts

| Context | What runs there |
|---|---|
| **init** | `module_init`, in the `core` task, while the module is loading |
| **core task** | deferred calls, periodic calls, power handlers, profile and look callbacks - all in the `core` task on CPU 0 |
| **handler** | an event handler: the firmware task that hit the hook, on CPU 0 |
| **any CPU 0** | any of the above |

Nothing of a module runs on CPUs 1 and 2: `core` calls no module code there. Many functions take the firmware lock or
assume CPU 0; none may be called from an interrupt.

**A module's own variables belong to CPU 0.** The module is linked to run at the cached address of its image, so its
static and global variables (`data`, `bss`) and its constants are read and written through the data cache of CPU 0,
and the caches of the three CPUs are not coherent: what the module wrote a moment ago may not be in memory yet for
another CPU, and may never be seen there in order. So **never hand a pointer to a static or global variable (or to
the stack) to anything that CPUs 1 and 2 read later** - the camera's shooting and live view tasks run there, and so do
the driver's hooks in them. Today every service copies what it is given (ISP blocks, matrices, pictures, names, the
EXIF text, grain tables) into its own memory before a hook can see it, so passing a static buffer to an API call is
fine and is what the bundled modules do; a later change of that buffer reaches the camera only through another call.
Memory from `alloc` is the opposite: it is used through uncached addresses and is the same for every CPU (and slower -
keep working data that only the module touches in static variables).

| Function | init | core task | handler |
|---|---|---|---|
| `log`, `cfg_*`, `str`, `res`, `ms`, `mem_info`, `store_get/set/del`, `prof_get`, `look_get`, `prof_count`, `*_picture_size`, `prof_mono_matrix_get`, `card_busy` | yes | yes | yes |
| `alloc`, `free` | yes | yes | allowed (under the firmware lock, a first-fit search); avoid |
| `defer` | yes (runs after loading) | yes | yes |
| `next` | - | - | REPLACE phase only |
| `file_*`, `file_list*`, `res_read`, `sleep` | yes | yes | **no** (the card is slow; `sleep` would block a firmware task) |
| `on`, `on_power`, `store_open`, `prof_enable`, `look_enable`, first `isp_*`, first `grain_list` | **only here** | no | no |
| `every`, `prof_set`, `prof_picture`, `prof_mono_matrix`, `isp_*` (owner), `look_*` (owner), `exif_comment` | yes | yes | not intended |

`store_*` calls are short but wait 1 ms at a time if another task is using the same store at that moment.

## 3. Versions and compatibility

- `api->version` is the API level of the running `core`; `api->size` is the size of its table in bytes;
  `api->core_version` (level 4) is the `core` version as text, for display only.
- The table **only grows**: new functions are added at the end, existing slots keep their place and meaning. A function
  that is removed keeps its slot and returns a failure (`overlay_removed`).
- The build stores the SDK's `OMK_API_VERSION` in the `.OMM` header. `core` refuses to load a module whose level is
  newer than its own (`needs a newer core API`). A module built with level 19 therefore runs only on a `core` with level
  19 or later.
- A module checks in `module_init` that the table is big enough for what it uses, and checks `api->version` before
  calling a function of a later level. The modules in this tree start with
  `if (a->version < OMK_API_VERSION || a->size < sizeof(*a)) return 1;`.
- Error codes are small negative numbers whose meaning is per function (below). 0 is success unless a function returns a
  count or a pointer.

## 4. API reference

The column **L** is the API level in which the function appeared. Level 3 is the base table (the oldest level a module
can be built for).

### 4.1. Log

| Function | L | Meaning |
|---|---|---|
| `void log(omk_mod *m, int level, const char *fmt, ...)` | 3 | One log line `ms L NAME: text`. `level`: `OMK_LOG_ERROR` 1, `OMK_LOG_WARN` 2, `OMK_LOG_INFO` 3, `OMK_LOG_DEBUG` 4; lines above `log_level` of `[core]` are dropped at once (cheap) |

- Format: `%s %c %d %u %x %X`, a width with an optional zero (`%08x`, `%5u`). No `%l`, `%p`, floating point or precision;
  any other character after `%` is printed as is.
- A line is at most 192 bytes with its prefix; a longer one is cut and ends with `~`. A line that does not fit the 32 KB
  ring is dropped whole and counted.
- Any task on CPU 0. The line is formatted on the caller's stack (about 200 bytes) and copied under the firmware lock.

### 4.2. Config (`[NAME]` in `OMKIT.INI`)

| Function | L | Returns |
|---|---|---|
| `const char *cfg_str(omk_mod *m, const char *key, const char *def)` | 3 | the value (trimmed, read only, valid until power-off) or `def` |
| `int cfg_int(omk_mod *m, const char *key, int def)` | 3 | decimal or `0x` hex, optional `-`; anything else, or no key, gives `def` |
| `int cfg_bool(omk_mod *m, const char *key, int def)` | 3 | 1 for `on yes true 1`, 0 for `off no false 0`, otherwise `def` |
| `int cfg_size(omk_mod *m, const char *key, u32 *out)` | 3 | 1 - present, `*out` in bytes (suffix `K` or `M`, any case); 0 - no key; -1 - not a size |

Keys are case-insensitive; on a repeated key the last value wins. The file is read once at power-on.

### 4.3. Memory

| Function | L | Meaning |
|---|---|---|
| `void *alloc(omk_mod *m, u32 size)` | 3 | a block of at least `size` bytes, 16-byte aligned, from the arena (or, with `fw_heap = on`, from the firmware heap when the arena is full). 0 - out of memory, over the module's `memory` limit, `size` 0 or over 256 MB, or called on another CPU; the first failure is logged, later ones counted |
| `void free(omk_mod *m, void *p)` | 3 | frees a block of this module; `p` 0 is ignored. A pointer that is not the start of a live block of this module is refused and logged, the block is kept |
| `void mem_info(omk_mod *m, omk_mem_info *out)` | 3 | the module's counters: `used`, `peak`, `allocs`, `fails`, `limit`, `fw_used` (bytes, including 16-byte block headers and alignment) |

Memory is never zeroed by `alloc`. The module's image, `bss`, in-memory resources and store also count against its
`memory` limit; the image with its `bss` is rounded up to whole 64-byte cache lines (it is the one part of the arena
used through the cache, see section 2).

### 4.4. Store

| Function | L | Meaning |
|---|---|---|
| `omk_store *store_open(omk_mod *m, u32 size)` | 3 | **init only.** The module's store with `size` bytes for records (rounded up to 4), loaded from `STORE\NAME.STA/.STB`. A second call returns the same store (size ignored). 0 - called after `module_init`, no store slot (8 in all, `core` uses one or two), no memory |
| `int store_get(omk_store *s, u16 key, void *buf, u32 max)` | 3 | the value's length (it may be larger than `max`; at most `max` bytes are copied), or -1 - no such key or `s` 0 |
| `int store_set(omk_store *s, u16 key, const void *data, u32 len, u32 flags)` | 3 | 0, or -1 - no room, `len` over 65535 or `s` 0. `flags`: `OMK_STORE_PERSIST` - saved to the card; 0 - kept in memory only (lost at power-off) |
| `int store_del(omk_store *s, u16 key)` | 3 | 0, or -1 - no such key |

- A record takes 8 bytes plus the value rounded up to 4. Changing a value's length moves it to the end of the store.
- Persistent changes are saved by `core` 1.5 s after the last change - and no later than 12 s after the first change
  not saved yet, when a value keeps changing (a save every 12 s then) -, to the older of the two files, with a sequence
  number and CRC-32; a cut write loses at most that change. Saves wait while the camera writes the card and stop during
  sleep; the last save happens at the card-off point of a power-off, after the modules' power handlers.
- An unchanged `store_set` (same bytes and flags) does not cause a save.
- The memory used is twice `size` plus 20 bytes (records and a file image), counted to the module.

### 4.5. Files in the module folder

| Function | L | Meaning |
|---|---|---|
| `omk_file *file_open(omk_mod *m, const char *name, const char *mode)` | 3 (long names 15) | opens `OMKIT\NAME\name`. `name`: an 8.3 name of `A-Z 0-9 _` (lower case is converted), no directories; from level 15 also a long name as `file_list_long` gives it, `"r"` only. `mode`: `"r"`, or `"w"` - the file starts empty. 0 - a bad name or mode, the card not available (power-off or sleep), the path longer than 63 characters, or the file or the folder missing |
| `u32 file_read(omk_file *f, void *buf, u32 len)` | 3 | bytes read; 0 at the end, on error, or while the card is not available |
| `u32 file_write(omk_file *f, const void *buf, u32 len)` | 3 | bytes written; fewer than `len` means the card is full or failing |
| `void file_close(omk_file *f)` | 3 | closes; the data reaches the card here. `f` 0 is ignored |
| `u32 file_list(omk_mod *m, const char *pattern, char (*names)[13], u32 max)` | 14 | names of the module folder matching `pattern` (8.3 characters with `*` and `?`, e.g. `"*.ISP"`), upper case, at most `max`. Skipped: folders, empty files, names starting with a dot, names that are not 8.3. Returns how many were stored; 0 also when the folder is missing |
| `u32 file_list_long(omk_mod *m, const char *pattern, char (*names)[OMK_NAME_MAX], u32 max)` | 15 | as `file_list`, long names too: an 8.3 name in upper case, a long one as on the card (up to 39 printable ASCII characters, none of `\ / : * ? " < > |`); longer or other names are skipped and counted in the log |
| `int card_busy(void)` | 19 | whether the card may be written now: 0 - free; 1 - the camera is writing a picture or has just made one (`core` postpones its own writes then too); -1 - power-off or sleep, `file_open` refuses. Cheap; any context |

- init and core task only. `core` creates the module folder at the module's first `file_open` or `file_list*` (API 19); there is no seek, append, delete or rename.
- `file_list*` look at most 256 folder entries; each step of the firmware's folder walk reads the folder from its start.

### 4.6. Resources and strings

| Function | L | Meaning |
|---|---|---|
| `const void *res(omk_mod *m, u32 type, u32 id, u32 *size)` | 3 | a pointer to an in-memory resource (read only, 4-byte aligned, valid while the module runs). 0 - no such resource, or it is on demand; `*size` (if not 0) gets its size in both cases, 0 when there is none |
| `u32 res_read(omk_mod *m, u32 type, u32 id, u32 offset, void *buf, u32 len)` | 3 | reads part of an on-demand resource from the module file on the card; `len` is clamped to the end. Bytes read; 0 - not an on-demand resource, `offset` past the end, `len` 0, or a card error (logged). init and core task only. Within `module_init` nothing is read from the card for it: `core` keeps the on-demand data it has just checked in memory until `module_init` returns (when it is at most 256 KB and the arena has room; otherwise the file stays open between the reads) |
| `const char *str(omk_mod *m, u32 id)` | 3 | the string `STR_<NAME>` from `res/strings.txt`; `""` when there is none |

Types 1..`OMK_RES_USER_MAX` (0x7fff) are the module's; the rest are reserved by `core` (0x8000 is the string table).

### 4.7. Time

| Function | L | Meaning |
|---|---|---|
| `u32 ms(void)` | 3 | milliseconds since the camera started; corrected after a long sleep; wraps after about 49 days |
| `void sleep(u32 ms)` | 3 | init and core task only. It stops the whole `core` loop for that time: use `every` or `defer` instead where possible |

### 4.8. Events and deferred calls

| Function | L | Meaning |
|---|---|---|
| `int on(omk_mod *m, u32 event, u32 phase, int priority, omk_handler fn, void *ctx)` | 3 | registers `fn(ev, data, ctx)` for one phase of an event (section 5). Higher `priority` runs earlier; ties in load order. 0 - accepted; -1 - no such event on this firmware; -2 - phase not allowed for it (or several phase bits, or `fn` 0); -3 - not from `module_init`; -4 - no free handler slots (64 in all) |
| `void next(omk_ev *ev)` | 3 | REPLACE phase only: runs the next REPLACE handler, after the last one the original firmware function. A REPLACE handler that does not call it suppresses the original |
| `int defer(omk_mod *m, void (*fn)(void *arg), void *arg)` | 3 | queues `fn(arg)` for the `core` task. 0, or -1 - the queue (32 calls, shared by all modules) is full, `fn` 0, or not on CPU 0. Calls run in order on the next pass of the `core` loop - which the call wakes at once (core 1.30; up to 250 ms later before) -, only while the camera runs: during sleep they wait, after a power-off they never run. A pass runs the calls queued when it began: a call that defers itself again (polling) runs at the next pass, once a pass, with the rest of the `core` loop in between |

`OMKIT.INI` can change a registration: `[NAME] event_<event name> = off` disables the module's handlers of that event,
`= <number>` sets their priority. Hooks are installed once all modules are loaded; handlers registered for an event whose
hook fails to install are never called (logged).

### 4.9. Periodic call

| Function | L | Meaning |
|---|---|---|
| `int every(omk_mod *m, u32 ms, void (*fn)(void *ctx), void *ctx)` | 14 | `fn(ctx)` in the `core` task about every `ms` milliseconds from the next loop pass on, at a granularity of the loop's 250 ms step. One per module: a new call replaces it; `ms` 0 removes it. 0, or -1 - `fn` 0 with `ms` not 0. Not called while the camera is off or asleep. A call over 100 ms is logged as a warning |

### 4.10. Power

| Function | L | Meaning |
|---|---|---|
| `int on_power(omk_mod *m, omk_power_fn fn, void *ctx)` | 5 | **init only.** `fn(event, ctx)` gets the power states; a new call replaces the handler, `fn` 0 removes it. 0, or -3 - not from `module_init` |

| Event | When | What to do |
|---|---|---|
| `OMK_POWER_OFF` 1 | a power-off began (lever, auto power-off, battery, ...) | stop; put what must survive into the store (in memory: `core` saves it at the card-off point) |
| `OMK_POWER_RESUME` 2 | the power-off did not happen | carry on; the card may be missing for a while |
| `OMK_POWER_SLEEP` 3 | the camera goes to sleep | stop loops and timers; every task freezes soon |
| `OMK_POWER_WAKE` 4 | back from sleep (or sleep did not happen) | carry on; time passed and the card may have changed |

Order: power-off and sleep - the last loaded module first; resume and wake - load order. The handler runs in the `core`
task; it must be short and must not sleep (over 50 ms is a warning). While the state is not "running", file functions
refuse and deferred and periodic calls do not run.

### 4.11. Picture profiles (one owner)

The camera's colour and monochrome profiles: `core` shows them in the picture mode screen over a stock picture mode and
applies them to shooting from memory. A profile id: `OMK_PROF_COLOR(n)` or `OMK_PROF_MONO(n)`, `n` from 1;
`OMK_PROF_NONE` (0) is a stock picture mode. Helpers: `OMK_PROF_IS_MONO(p)`, `OMK_PROF_NUM(p)`.

| Function | L | Meaning |
|---|---|---|
| `int prof_count(u32 *colors, u32 *monos)` | 6 | the number of colour and monochrome profiles of this camera (either pointer may be 0); returns their sum, 0 - no profile support in this firmware |
| `int prof_enable(omk_mod *m, omk_prof_fn fn, void *ctx)` | 6 | **init only.** Takes the service and installs its hooks; `fn(prof, ctx)` is called in the `core` task when the user picks a profile or leaves it for a stock mode. 0; -1 - not supported or hooks failed; -2 - another module owns it; -3 - not from `module_init` |
| `int prof_set(omk_mod *m, u32 prof)` | 6 | sets the active profile. 0; -1 - no such profile or not the owner; -4 - the stored picture mode is not the base one (the user changed it without OMKit): not applied |
| `u32 prof_get(void)` | 6 | the active profile |
| `int prof_picture_size(u32 pic, u32 *w, u32 *h)` | 8 | the height and the largest width this camera takes for picture `pic`. 0; -1 - no such picture here |
| `int prof_picture(omk_mod *m, u32 pic, u32 w, u32 h, const u8 *pixels)` | 8 | owner only: sets a picture (copied; `pixels` may be freed after the call). A mode icon must have the full width, a label may be narrower. 0; -1 - not the owner, bad id or size; -2 - no memory; -3 - the firmware's table is not as expected |
| `int prof_mono_matrix(omk_mod *m, u32 filter, u32 level, const s16 *matrix)` | 10 | owner only: the 3x3 colour filter matrix (nine `s16` in the camera's fixed point) of a monochrome filter 1..8 at level 1..3; `matrix` 0 - back to the camera's own. 0; -1 - not the owner, bad arguments or no room; -3 - the firmware's table is not as expected |
| `int prof_mono_matrix_get(u32 filter, u32 level, s16 *matrix)` | 18 | any module: the camera's own matrix, also while it is replaced. 0; -1 - bad arguments; -3 - the table is not as expected |

Picture ids (`pic`):

| Id | L | Picture |
|---|---|---|
| `OMK_PIC_MODE(prof, variant)` | 8 | mode icon in the picture mode strip; `variant`: `OMK_PROF_ICON_NORMAL`, `_LARGE`, `_LARGE_EDITED` (a profile changed from its defaults) |
| `OMK_PIC_LABEL(prof, edited)` | 8 | the "Picture Mode" menu line |
| `OMK_PIC_PANEL(prof, edited)` | 9 | the picture mode cell of the control panel |
| `OMK_PIC_SHOOT(prof, edited)` | 9 | the picture mode sign of the shooting screen |
| `OMK_PIC_EDITOR(what)` | 11 | pictures of the profile editor the camera does not have: `OMK_EDITOR_COLOR_LEVEL`, `OMK_EDITOR_GRAIN(level)`, `OMK_EDITOR_GRAIN_BAR(level)`; level 12: `OMK_EDITOR_LABEL_FILTER` (not shown since core 1.29: Color/Level shows the camera's "Color"), `OMK_EDITOR_LABEL_LEVEL` (dial labels: the rule in [platform/om5m2/ui.md](platform/om5m2/ui.md#451-rule-dial-labels-and-the-editing-area)) |

An edited variant that is not set shows the plain one. Level 7 had a `prof_icon` function, replaced in level 8 by these.

**Pixels.** 4 bits per pixel, the high nibble first, every row a whole number of bytes (`(w + 1) / 2`), rows top down.
The values are meanings, not colours; `core` draws them in the firmware's own style: `OMK_ICON_CLEAR` 0, `OMK_ICON_SIGN`
1, `OMK_ICON_OUTLINE` 2, `OMK_ICON_EDGE_IN` 3 (between sign and outline, nearer the sign), `OMK_ICON_EDGE_OUT` 4,
`OMK_ICON_OUTLINE_EDGE` 5 (outside the outline), `OMK_ICON_HALO` 6.

### 4.12. Film look (one owner)

A stock picture mode of the picture mode screen - the slot - shows the module's look instead: its pictures, its name,
and an editor column whose first item steps through the module's entries with the dials. While the look is active the
stored picture mode stays Natural and shooting gets Natural; the colour comes from the ISP service (4.14).

| Function | L | Meaning |
|---|---|---|
| `int look_enable(omk_mod *m, const char *slot, omk_look_fn fn, void *ctx)` | 14 | **init only.** Takes the service in `slot`: `ienhance`, `vivid`, `muted`, `portrait`, `monotone` or `underwater`. `fn(event, arg, ctx)` gets the events below in the `core` task. 0; -1 - no such slot (the log lists them) or no service here; -2 - another owner; -3 - not from `module_init` |
| `int look_list(omk_mod *m, const char *const *names, u32 count, u32 current)` | 14 | the entries (names up to 31 printable characters, at most `OMK_LOOK_MAX` 64) and the chosen one; copied. 0; -1 - not the owner or bad; -2 - no memory |
| `int look_set(omk_mod *m, int on)` | 14 | turns the look on or off without the UI (e.g. restoring at power-on). 0; -1 - not the owner; -4 - the stored picture mode is not Natural: not turned on |
| `int look_get(void)` | 14 | 1 - the look is active |
| `int look_picture_size(u32 pic, u32 *w, u32 *h)`, `int look_picture(omk_mod *m, u32 pic, u32 w, u32 h, const u8 *pixels)` | 14 | as `prof_picture_size` / `prof_picture`, for `OMK_LOOK_PIC_*` |
| `int look_name(omk_mod *m, const char *text)` | 14 | the look's name where the slot's mode name was, in every language: up to 23 printable ASCII characters. 0; -1 - not the owner or bad; -2 - no memory; -3 - the firmware's string table is not as expected |
| `int look_art(omk_mod *m, u32 index, const void *data, u32 size)` | 14 | the cassette picture of entry `index`: the `CANI` section of an `.isp` file ([formats/isp.md](formats/isp.md)); checked (version 1, at most 64 records and 6144 bytes, every record's operation, segments and points) and copied. 0; -1 - not the owner, bad index, or a section that fails the checks (the log says why); -2 - no memory |
| `int look_edits(omk_mod *m, const omk_look_edits *e)` | 16 | the editors shown after the look's item for the chosen entry and their values (copied). 0; -1 - not the owner or bad; -2 - editors not available on this firmware |

Look events (`fn(event, arg, ctx)`):

| Event | L | `arg` |
|---|---|---|
| `OMK_LOOK_OFF` 0 | 14 | 0: the user left the look for another picture mode |
| `OMK_LOOK_ON` 1 | 14 | the chosen entry: the user chose the look |
| `OMK_LOOK_ENTRY` 2 | 14 | the entry index a dial chose (only while the look is active) |
| `OMK_LOOK_EDIT` 3 | 16 | `OMK_EDIT_EVENT(group, index, value)`: an editor changed a value (only while the look is active). Decode with `OMK_EDIT_GROUP_OF`, `OMK_EDIT_INDEX_OF`, `OMK_EDIT_VALUE_OF`, `OMK_EDIT_IS_RESET` (a long OK: the group back to the entry's own values) |

Editor groups (`omk_look_edits.groups` and `.edited` are bit masks `1 << OMK_EDIT_*`):

| Group | L | Index and value |
|---|---|---|
| `OMK_EDIT_TONE` 0 | 16 | index 0 shadows, 1 middle, 2 highlights; value -7..7 |
| `OMK_EDIT_VIVID` 1 | 16 | index 0..11, a hue sector; value -5..5, a shift of 0.05 per step from the entry's own saturation |
| `OMK_EDIT_LEVEL` 2 | 16 | index 0 the filter 0..8 (0 - the entry's own), index 1 the level 1..3 |
| `OMK_EDIT_GRAIN` 3 | 17 | index 0 the grain tile (its number in `grain_list`), index 1 the strength 0..3 (Off, Low, Med, High) |

`omk_look_edits` also carries what is drawn: `entry` (the entry these values belong to; if the UI has moved on they are
not shown), `tone[3]`, `vivid[12]`, `filter`, `level`, `curve[64]` (the curve drawn, output 0..255 at inputs k/63),
`vivid_mask` (editable sectors), `vivid_hue[12]` (degrees), `vivid_rgb[12][3]` (sRGB), `vivid_base[12]` (each
sector's own saturation in 0.05 steps). The UI shows a dial's change at once and sends `OMK_LOOK_EDIT`; the module applies
it and calls `look_edits` again with what is to be shown.

Look pictures (`OMK_LOOK_PIC_*`, same pixel format as 4.11): `MODE` 0 (strip, 66 x 50), `LARGE` 1 (80 x 60), `LABEL` 2
(menu line, up to 124 x 39), `PANEL` 3 (control panel cell, up to 128 x 39), `SHOOT` 4 (shooting screen sign, up to 80 x
36), `ITEM` 5 (editor column item, 66 x 50), `PREV` 6 and `NEXT` 7 (dial labels, up to 56 x 24); level 16: `LEVEL` 8
(Color/Level item of a black-and-white entry, 66 x 50); level 17: `GRAIN` 9 (grain item, 66 x 50), `TYPE` 10 (its tile
dial label, up to 56 x 24), `STRENGTH` 11 (its strength dial label, up to 56 x 24; core 1.29 - 116 and 104 before). The
sizes are those of the first platform; ask `look_picture_size`. Dial labels follow one rule - the camera's face, size and
place, drawn with `tools/dial_label.py`, a shorter word where one does not fit:
[platform/om5m2/ui.md](platform/om5m2/ui.md#451-rule-dial-labels-and-the-editing-area).

### 4.13. Grain (the look's owner)

`core` owns the grain tile containers `OMKIT\GRAIN\*.GRN` ([formats/grn.md](formats/grn.md)), at most `OMK_GRAIN_TILES`
32, ordered by file name and checked at the first call. It loads the chosen tile and puts the grain on the look's stills
(not on live view or movies).

| Function | L | Meaning |
|---|---|---|
| `u32 grain_list(omk_mod *m, char (*files)[13], u32 max)` | 17 | the containers' 8.3 names in order (a tile's number is its index here); returns how many. The first call, **init only**, takes the grain service and installs its hooks; it needs room in the arena for one tile plus a reserve. 0 - no containers, no room, or no service here (the log says why) |
| `int look_grain(omk_mod *m, u32 entry, s32 tile, u32 level)` | 17 | the tile (-1 none) and strength 0..3 of entry `entry`, the one the UI shows: the grain item shows them and the stills of the look get them while the strength is not Off. `core` loads a new tile from the card after a short pause; a still taken before it is ready has no grain. 0; -1 - not the owner or bad; -2 - no grain service |

`[grain]` in `OMKIT.INI` (`mask`, `chroma`) sets how the grain is applied; it is the user's setting, not the module's.

### 4.14. ISP colour tables (one owner)

| Function | L | Meaning |
|---|---|---|
| `int isp_scope(omk_mod *m, u32 scope)` | 13 | which jobs get the tables: `OMK_ISP_LIVE` (live view, also what the camera meters and focuses on) and/or `OMK_ISP_STILLS` (the default). Call before `isp_override` |
| `int isp_override(omk_mod *m, const void *block, u32 size)` | 13 | publishes the platform's ISP block (the `ISPB` section of an `.isp` file, [formats/isp.md](formats/isp.md), layout in `omk_ispb.h`); copied, may be freed after the call. `block` 0 - back to the camera's own tables |
| `int isp_census(omk_mod *m, int on)` | 13 | 1 - log the jobs of the colour path at debug level; 0 - stop |

- The first module to call any of the three owns the service; that first call must come from `module_init` (the hooks
  go in then).
- Codes: 0; -1 - a bad block; -2 - too soon; -3 - no service (another firmware, or the hooks failed); -4 - the first
  call not from `module_init`; -5 - another module owns it.
- Rates: a change of the gamma or the sector weight tables (another preset) at most once a second; a change of only the
  matrix, the Cb/Cr gains, the sector records or the tone (an edit) every 40 ms, applied at once in live view.

### 4.15. Other

| Function | L | Meaning |
|---|---|---|
| `int exif_comment(omk_mod *m, const char *text)` | 14 | any module: the EXIF UserComment of the pictures taken from now on (JPEG and RAW, not the in-camera RAW development), up to 117 printable ASCII characters; 0 - the camera's own. Needs the profiles or the look service in place. 0; -1 - not available |
| `const char *core_version` | 4 | the `core` version, e.g. `"1.24"`; for display and the log only |
| `int overlay_removed(omk_mod *m, const char *text, u32 ms)` | 3, removed | the on-screen text service was removed; the slot keeps the table layout and always returns -1 |

## 5. Events (`omk_events.h`)

Handler: `void fn(omk_ev *ev, void *data, void *ctx)`. `data` points to the event's structure; `ctx` is what the module
passed to `on`.

| Phase | Call order | What the handler may do |
|---|---|---|
| `OMK_PH_NOTIFY_BEFORE` 0x01 | 1 | read the input (a copy: changing it has no effect) |
| `OMK_PH_FILTER_BEFORE` 0x02 | 2 | change the input |
| `OMK_PH_REPLACE` 0x04 | 3 | do the work itself, or call `api->next(ev)`: the next REPLACE handler, the original function after the last one. Without REPLACE handlers the original runs here |
| `OMK_PH_FILTER_AFTER` 0x08 | 4 | change the result |
| `OMK_PH_NOTIFY_AFTER` 0x10 | 5 | read the outcome (a copy) |

The driver decides which events exist on a firmware and which phases each allows. Handlers are not called - only the
original runs - when the event fires on CPU 1 or 2, or again while its handlers are still running: a nested call from
the same task, or a call from another task of CPU 0 that preempted it (it is not made to wait). All are counted in the
log - the first two at debug level, the calls from another task as a warning, since the handlers missed them.

| Event | L | Phases | Data |
|---|---|---|---|
| `OMK_EV_PICTMODE_CHECK` 1 | 3 | NOTIFY_BEFORE, REPLACE, FILTER_AFTER, NOTIFY_AFTER | `omk_ev_pictmode_check { u32 mode; u32 allowed; }`: the firmware checks whether picture mode `mode` is allowed - once per mode when it builds the list of the direct selection screen, and when the dial steps over the picture mode cell of the control panel. `allowed` 1 - allowed as is, 0 - the firmware replaces it (hidden or restricted); a FILTER_AFTER handler may change it |
| `OMK_EV_S1_API` 2 | 3 | NOTIFY_BEFORE | none. **Preliminary:** a firmware call that accompanies a half-press of the shutter button (two calls per press were measured on the camera). For experiments, not for module logic |

Ids run from 1 to `OMK_EV_COUNT - 1`.

## 6. The ISP block helpers (`omk_ispb.h`)

Offsets into the `OM5M2` layout 1 block (5064 bytes, little-endian): `OMK_ISPB_LAYOUT` (u16, 1), `OMK_ISPB_PRESENT`
(u16, the parts the block replaces: `OMK_ISPB_P_MATRIX`, `_GAMMA`, `_TONE`, `_GAINS`, `_SECTORS`), `OMK_ISPB_MATRIX`
(s16[9], Q7, rows sum to 128), `OMK_ISPB_CB`, `OMK_ISPB_CR` (u16, Q8), `OMK_ISPB_GAMMA` (u16[3][256]), `OMK_ISPB_TONE`
(u16[256]), `OMK_ISPB_SECTOR_MASK`, `OMK_ISPB_SECTORS` (12 records of 24 bytes), `OMK_ISPB_PROFILES` (s16[12][7][16]).
The meaning of every field is in [formats/isp.md](formats/isp.md), section 5.

`omk_ispb_u16(b, at)` reads a little-endian `u16`. `omk_ispb_mono(b)` returns 1 for a black-and-white block: one that
sets the Cb/Cr gains with one of them 0, or sets the matrix with rows that differ by at most `OMK_ISPB_MONO_ROWS` (2) in
every column; parts the block does not set are not looked at. `core` uses the same function (for example, colour grain
is never put on a black-and-white look).

## 7. The C library

Modules are built freestanding (`-ffreestanding -fno-builtin -nostdlib`). `omk_libc.c` gives `memcpy`, `memmove`
(overlap allowed; word-wise when source and destination have the same alignment), `memset`, `memcmp` and `strlen`. There
is no `printf` (use `api->log`), no `malloc` (use `api->alloc`), no file I/O beyond the API. No `libgcc` is linked, so
code that GCC turns into library calls (floating point, 64-bit division) leaves undefined symbols and the module does
not build (**inferred from the build flags**; the modules in this tree use integer arithmetic only).

## 8. Limits at a glance

| What | Limit |
|---|---|
| Modules | 16 |
| Module image / `bss` / in-memory resources | 2 MB each |
| Resources / on-demand data / string table | 1024 / 64 MB / 64 KB |
| Event handlers | 64 in all, 16 per event and phase |
| Defer queue | 32 calls, shared |
| Stores | 8 in all, including `core`'s |
| Config | 8 KB, 128 entries |
| Log line / log ring | 192 bytes / 32 KB |
| Path on the card | 63 characters (`OMKIT\NAME\file`) |
| Look entries / grain tiles | 64 / 32 |

## 9. Build tools

### 9.1. `src/modules/Makefile`

```sh
make -C OMKit/src/modules MOD=<name> [MODDIR=<folder>] [VERSION=1.0] [OUT=<dir>] [EXTRA=...] [RES_OVER=<folder>]
```

| Variable | Default | Meaning |
|---|---|---|
| `MOD` | `hello` | the module's folder name; upper-cased it is the module name and the file name `<MOD>.OMM` |
| `MODDIR` | `<MOD>` | the module's source folder: every `*.c` there is compiled; `res/` and `res/disk/` hold its resources. Set it for a module kept outside this tree |
| `VERSION` | `1.0` | `major.minor`, stored in the file |
| `OUT` | `OMKit/build/modules` | the output folder (relative to `src/modules`, or absolute) |
| `EXTRA` | - | more compiler options, e.g. `-DDEBUG` |
| `RES_OVER` | - | a folder laid out as `res/` whose resources replace the module's own of the same type and id; for private builds only |
| `TC` | `~/opt/mn10300/bin/mn10300-elf-` | the toolchain prefix (`toolchain/build-toolchain.sh` installs it there) |
| `PYTHON` | `python3` | the Python that runs `mkmod.py` |

Steps: `mkmod.py header` makes `<OUT>/<MOD>.gen/res_strings.h` from `res/strings.txt`; GCC compiles and links the module
with `omk_libc.c` (`-Os -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -nostdlib -nostartfiles -Wall
-Wextra -Werror`, include path `src/sdk/include` only) by `module.ld` with `-q` (keep relocations); `mkmod.py build` makes
the `.OMM`. For modules in this tree, the pictures in `resources/modules/<MOD>/icons/` are added as on-demand resources.
A change of `VERSION` or the resource options rebuilds the module.

### 9.2. `src/modules/module.ld`

The image starts at address 0: `.text`, `.rodata`, `.data`, then `.bss` (not in the file). The entry is `module_init`
(`_module_init` in the object files). `core` adds the load address to every relocated word.

### 9.3. `tools/mkmod.py`

```sh
python3 OMKit/tools/mkmod.py build <module.elf> <NAME.OMM> --name NAME [--version 1.0] [--res <dir>] [--res-disk <dir>]
                                   [--res-over <dir>] [--api N]
python3 OMKit/tools/mkmod.py header <res/strings.txt> <res_strings.h>
```

| Check or step | Rule |
|---|---|
| ELF | 32-bit little-endian, MN10300 |
| Name | 1-8 of `A-Z 0-9 _`, not `CORE` |
| Image | the loadable sections start at 0; `bss` follows the image |
| Symbols | no undefined symbols (a module cannot link against the firmware); `_module_init` must exist |
| Relocations | only absolute 32-bit words (`R_MN10300_32`) are kept, except address differences and references to absolute symbols; relative ones need nothing. Any other absolute relocation is an error |
| Resources | `<type>_<id>[.anything]` files; type 1..32767, id 0..65535; a type and id defined twice is an error; `--res-disk` adds a folder of on-demand resources; `--res-over` replaces by type and id and lists what it replaced |
| Strings | `NAME = text`, names `[A-Z][A-Z0-9_]*`, printable ASCII only, at most 64 KB; resource 0x8000/0 |
| API level | `--api N` or, by default, `OMK_API_VERSION` of `src/sdk/include/omk_api.h`. The Makefile does not pass `--api` |
| Output | header (64 bytes), image, relocations, resource directory, in-memory data, on-demand data; CRC-32 of the file with the CRC field zero |

The file format, field by field, is in `src/core/include/omk_mod.h` (`omm_head`, `omm_res`).

### 9.4. `core` and the card bundle

| Command | Result |
|---|---|
| `make -C OMKit [OUT=<dir>] [EXTRA=...]` | `CORE.BIN` (in `OMKit/build/` by default) for the platform set by `PLATFORM` in `OMKit/Makefile` (`om5m2/fw1100`) |
| `make -C OMKit card` | a card folder `OMKit/build/card/OMKIT/` with `CORE.BIN`, the bundled modules in `MODULES/`, the grain tiles in `GRAIN/`, the FILMLOOK presets in `FILMLOOK/` and `OMKIT.INI` with the bundled modules on ([config.md](config.md#1-where-the-file-comes-from)) |
| `make -C OMKit test` | the host tests, the data and picture checks and the apps' tests ([build.md](build.md#5-tests)) |
| `python3 OMKit/tools/pack.py <core.body> <CORE.BIN>` | packs the linked `core` for the loader: `{u32 'OMKC', u32 body length, u32 sum of the body's 32-bit words, u32 ABI 1}` + body (at most 3 MB) |
