# Firmware 1.1: system catalogue

The firmware 1.1 functions, data, structures and registers that OMKit uses on the system side: memory, protection, the
kernel, power, files. Imaging-side entries (picture modes, UI, colour, stills, grain) are in [imaging.md](imaging.md).
Background for each topic is on the platform pages ([../README.md](../README.md)).

Columns: **Basis** as in [../README.md](../README.md#basis-labels); **Used by** names the OMKit file (driver files are in
`src/platform/om5m2/fw1100/`, the loader in `src/stub/platform/om5m2/fw1100/`, core in `src/core/`). All addresses are
execution addresses in the cached view unless they start with `0xa`/`0xb` (uncached) or are registers.

## Calling firmware functions

| Topic | Rule | Basis |
|---|---|---|
| Arguments | Pointers in `a0`, `a1`, integers in `d0`, `d1`, more on the stack from `(4,sp)` of the caller's frame. GCC passes the first two words in `d0`, `d1` whatever their type, so every call goes through a thunk (`thunks.s`) | code, camera |
| Results | Integer in `d0`, pointer in `a0` | code |
| `call f,[regs],frame` | On MN10300 the call instruction names the registers to save and the frame size, and they must match the callee's `ret`. The thunks copy the form from the firmware's own call sites | code |
| Return slot | `call` writes the return address to `(sp)`, so a thunk reserves its own slot (`add -4,sp`) | code |
| Saved registers | `d2`, `d3`, `a2`, `a3` | code |
| Firmware addresses in `call` | Use symbols (`fw.ld`); the assembler encodes a numeric `call` target as an offset | build |

## 1. Memory layout

### 1.1. Main image and sections

| Item | Address | What | Basis |
|---|---|---|---|
| Main code entry | `0x60021020` | Execution base; flash byte = `0x40780020 + offset`, RAM byte = `0x60021020 + offset` | code |
| Pre-main area | `0x60021020..0x60030000` | Loaded by the boot stage before main runs | code |
| Initial area | `0x60030000..0x600e8800` | Kernel, drivers, interrupt/NMI handlers; copied from flash `0x4078f000` by main's startup | code |
| Section table | `0x600284fc` (label `BCS` at `0x600284f8`) | 10 records `{start, source, flag}`; end of each section from the selector `0x600463af`; loader `0x60046418` | code |
| Section 1 | `0x600eb000..0x60241000` | `FastEndCb` (loader hook) | code |
| Section 2 | `0x6029f800..0x605af800` | Loader trampoline and body in its tail | code |
| Section 3 | `0x605ce800..0x60622000` | | code |
| Section 4 | `0x60627000..0x60829800` | File system | code |
| Section 5 | `0x60843000..0x60846000` | Cache library | code |
| Section 6 | `0x60847000..0x60ca8000` | Compressed | code |
| Section 7 | `0x60cbe800..0x612a5000` | Compressed; the DbgSup range ends where it starts | code |
| Section 8 | `0x61343800..0x613f3000` | | code |
| Section 9 | `0x6140e000..0x615ee800` | Loader flag `omk_done` in its tail | code |
| Section 10 | `0x61609800..0x6169d000` | | code |
| Load order | 1, 8, 2, 3, 4, 5, 6, 7, 9, 10 | Settings applied after section 2 | code |
| Sections loaded | `0xa26e2698` | Bit `n - 1` set when section `n` is in place; `0x3ff` = all. Read uncached | code, camera; used by `stub.s` |
| SRAM copy | `0x616a3000..0x616a4800` -> `0x47000800` | Code that runs from local memory (sleep loop); SRAM address = main address - `0x1a6a2800` | code |
| Firmware working data | from about `0x619e0000` | The firmware's absolute stores all land at `0x619e0000` and above (cached) | code |
| Code range for return-address checks | `0x60000200..0x617e0000` | Main image code and data | code; used by `kernel.c` |

### 1.2. Heaps and pools

| Item | Address | What | Basis | Used by |
|---|---|---|---|---|
| `scheap` pools, CPU 0 | `0x619e2fc0`, `0x61a96900`, `0x61d29200` | 128 / 2048 / 16384-byte classes, 3013 / 430 / 57 blocks; counters `0x61ec9e98 + 4 * class` (never issued), `0x61ec9ea4 + 4 * class` (freed); bitmaps `0x61ecba08`, `0x61ecbb84`, `0x61ecbbbc` (1 = in use) | code, camera | not used |
| `scheap_phyalloc` tables | `0x60028288..0x600282d4` | 178 / 21 / 43 blocks, stride with header 192 / 2112 / 16448 | code | `drv.c` (`drv_fwheap_class`) |
| `scheap_phyalloc` counters | `0xa26dbc84` (issued), `0xa26dbc94` (freed), `+4 * class` | Free = total - issued + freed | code | `drv.c` |
| Shared 128-byte blocks | `0xa27a0a80..0xa27a5580` | Third small allocator | code | not used |
| Image pool | object `0xa263ce08`, on the `0xb0000000` window, `0x1f47600` bytes | Created at startup | code | not used |
| Image allocator mode | `0xa263fbd4` (u16) | 1 = large layout (9 blocks reserved: ART, monochrome looks), 2 = normal; set at every picture mode change | code, emulator, camera | imaging driver |

### 1.3. The OMKit area

| Item | Address | What | Basis |
|---|---|---|---|
| Resource record 0 | `0xa3091000..0xa3456d74` | First record of the resources component (length `0x3c5d74`); the next records start at `0xa3791000` | code |
| Free gap | `0xa3456d74..0xa3791000` | 3 383 948 bytes, reserved for record 0 and unused | code, emulator, camera |
| Tested part | `0xa3470000..0xa3780000` | Pattern-tested word by word on the camera | camera |
| Loader buffer | `0xa345a000` | `CORE.BIN` is read here (at most 3 MiB) | `gen.py` |
| `core` entry | `0x6345a010` | Cached view of buffer + 16 | `gen.py`, `core.ld` |
| `core` ceiling | `0x63490000` | Image with stack must end below it (linker assertion) | `core.ld` |
| Arena | from the end of the `core` image rounded up to 4 KiB (uncached, at least `0xa3470000`) to `0xa3780000` | Modules and data | `drv.c` (`drv_mem_region`) |
| Second gap | `0xa3cb88f8..0xa3f25540` | Also silent in the emulator; its role is not understood; not used | emulator |

## 2. Memory protection

| Item | Address | What | Basis | Used by |
|---|---|---|---|---|
| DbgSup unit | `0x99700000` | Registers in [../hardware.md](../hardware.md#5-memory-protection-dbgsup) | code, camera | |
| Range tables | `0x6007c33c` (group 1), `0x6007c35c` (group 2) | Group 1: `0x00000000..0x00007fff`, `0x0000a000..0x1fffffff`, `0x38000000..0x3fffffff`, `0x47002000..0x5fffffff`. Group 2: `0x00000000..0x00007fff`, `0x28b81000..0x46ffffff`, `0x47002000..0x5fffffff`, **`0x60000200..0x60cbe7ff`** | code, camera (registers equal the tables) | |
| Protected range | `0x60000200..0x60cbe7ff` | Main image up to the end of section 6; writes through the cached view trap | code, camera | `drv.c` (`GUARD_LO`, `GUARD_HI`) |
| Driver | `0x60058ca6..0x60059b8a` | Setup, shell commands, status report | code | |
| Enabled | `0x600455fc` (start), `0x60046519` (after the last section); a temporary channel `0x60045610` narrows as sections load | | code | |
| Disable helper | `0x60059391` | Turns the unit off around a write, under a semaphore | code | not used (by design) |

## 3. Kernel, interrupts and faults

| Item | Address | What | Basis | Used by |
|---|---|---|---|---|
| Vector tables | CPU 0 `0x60030000`, CPU 1 `0x60050000`, CPU 2 `0x600e8000` | | code | |
| Regular interrupt entry, CPU 0 | `0x60030020` | Writes `sp`, `psw`, `pc` of the interrupted context into CPU 0's crash record every time | code, camera | |
| NMI entry | vector `+8` -> `0x60055290` / `0x60055300` / `0x60055370` | Saves registers, clears DbgSup control on all CPUs | code | |
| NMI handler | `0x60059bb1` | Prints `[SYS WANA] NMI_INT Hassei!`, cause registers, `:-) Rebooooooooot Soon!`; arms the watchdog; spins at `0x60059e00` | code | |
| Crash record | `0x62ad5580 + 0x40 * cpu` | `+0` task (written only by the NMI handler), `+4` sp, `+8` psw, `+0xc` pc, `+0x10` DbgSup result, `+0x20..+0x3c` registers. Written through the cache: empty after a restart | code, camera | `drv.c` (`drv_crash_read`) |
| Subprocessor watchdog | `0x60022a47(n)` | Writes `n` to `0x98e0008c` and command `0x20041001` to `0x98e000a4`; 1 = restart soon | code | |
| `SYS_WDT` task | entry `0x6005b44a`, table entry `0x60038e18` | Feeds every 3 s, limit 10 s (`0x6005b3fd`) | code | |
| Run on another CPU | `0x60056380(d0 cpu, a0 function, a1 argument)` | In the target's interrupt context | code | not used |
| Startup timeline | records `0xa26e1400`, count u16 `0xa26e13fc` | 24-byte records `{char name[20]; u32 time}`, up to 196, time in microseconds (`0x9890014c` scale); written by `0x6004559d` | code, camera | `drv.c` (`drv_boot_event`) |
| Task descriptor table | `0x60038b94..0x60039ca4` | 28-byte records `{id, 0, 0, entry, priority, stack, 0}` | code | |
| Object id constants | `0x6004132c..0x60041c1a` | Task and queue numbers read with `movhu (abs)`; file server queue `(0x60041830)` = 34, its task `(0x60041364)` = 16, power task queue `(0x60041800)` = 10 | code, emulator | |

## 4. RTOS services

| Function | Address | Call | Notes | Basis | Used by |
|---|---|---|---|---|---|
| `dly_tsk` wrapper (`OlyTaSS`) | `0x60051a2b` (kernel `0x600313f7`) | `[d2],8`; `d0` ms | Not interruptible | code, camera | `thunks.s` (`drv_sleep`), `stub.s` |
| `ext_tsk` | `0x6003167e` | `[a2],8` | | code, emulator | `stub.s` |
| `acre_Task` | `0x60051d5c` | `[d2,d3,a2,a3],56`; `d0` 2 start now, `d1` parameter, `a0` name, `a1` entry, `(4,sp)` priority, `(8,sp)` stack size, `(12,sp)` stack or 0 | Fills the stack with `0xdeadbeef` (`0x60051f15`); names in `0xa26acd5e + 1152 * cpu` (`0x60051e6b`); 64 per CPU | code, camera | `stub.s` |
| `acre_Event1` | `0x60051ace` | `[d2,d3,a2,a3],36`; `d0` attribute, `d1` initial pattern, `a0` name -> `d0` id | Flag on the caller's CPU | code, camera | `thunks.s` (`drv_ev_create`) |
| `OlyEvWt` | `0x60051143` | `[d2,d3,a2,a3],52`; `d0` id, `d1` pattern, `a0` -> result, `a1` mode, `(4,sp)` timeout ms | 0, -2 timeout, -1 | code, camera | `drv_ev_wait` |
| `OlyEvSt` | `0x60051078` | `[d2,d3,a2],40`; `d0` id, `d1` bits | | code, camera | `drv_ev_set` |
| `OlyEvCl` | `0x6005123a` | `[d2,d3,a2],28`; `d0` id, `d1` mask kept | Pattern &= `d1`, as uITRON `clr_flg` | code, camera | `drv_ev_clear` |
| Firmware lock / unlock | `0x6006e645` / `0x6006e68e` | `[d2],8`; lock takes `d0` = 0 and returns the state | Interrupts and dispatch off on this CPU | code, camera | `drv_lock`, `drv_unlock` |
| `get_tid` | `0x600317b4` | `[a2],4` | | code | not used |
| `get_tim` | `0x600317f8` | `[a2,a3],12`; `a0` -> 8 bytes | 1 ms, per CPU | code | not used |
| `ref_tsk`, task list | `0x60034bab`, `0x6005b5e5` | | Take the kernel lock, own CPU only; why the task dump reads structures itself | code | not used |
| Synchronous server request | `0x60054b45` | | Temporary queue, `OlyQeWr` `0x600514ac`, `OlyQeRd` `0x600513dc` with timeout -1 | code, emulator | (indirectly, by file calls) |
| Kernel code | `0x6002f000..0x6003a000` | | Return addresses inside are skipped by the task dump | code | `kernel.c` |
| KCB pointers | `0x8ff00004 + 4 * cpu` | | Expected values `0x60041cf4`, `0x60041e80`, `0x6004200c` | code, camera | `kernel.c` |
| TCB pools | CPU 0 `0x62028f40 + 120 * i`; CPU 1 `0x625b4ec0...`; CPU 2 `0x625d39c0...` | | | code | (through the KCB) |
| Dispatcher | `0x60035a07`; switch `0x600386cb` | | A switched-out task has `0x60035ae3` at saved `sp + 16` | code, emulator, camera | `kernel.c` |
| Static task names | config `0x6002f064` -> per-CPU tables `0x61659740`, `0x6165a9e2`, `0x6165b216` | 26-byte records | | code, camera | `kernel.c` |
| Wrapper return addresses | `0x600511eb` in `OlyEvWt`, `0x60051464` in `OlyQeRd`, `0x60053b93` `OlyEvWtCpu`, `0x60053fc4` waiting for another CPU | For reading task dumps | | code, emulator | |
| Cross-CPU locks | `0x603eac01` try, `0x603eac3b` take, `0x603eac5d` release; words `0x8ff001cc + (n >> 8)`, spin bit `0x8fe00018` bit 7 | | Used by the image pipeline | code | not used |

## 5. Power and sleep

### 5.1. Hooks `core` installs

All in the application task on CPU 0; each replaces 5 bytes after checking them (`drv.c`, `power_hooks`; trampolines in
`thunks.s`). Verified on the camera since `core` 0.58-0.60.

| Hook | Site | Stock bytes | Displaced | Back to | What it reports |
|---|---|---|---|---|---|
| power-off | `0x6089e774` `CameraCommon::shutDown` | `cf f0 f8 fe e4` | `movm [d2,d3,a2,a3],(sp)`; `add -28,sp` | `0x6089e779` | Reason in `d1` |
| card off | `0x601b3267` | `fc dc c9 0a 21` (`60`) | `mov 0x60210ac9,a0` (6 bytes) | `0x601b326d` | Last moment with the card; waits up to 1 s for `core` |
| sleep | `0x6089e863` `CameraCommon::startSleep` | `cf a0 f8 fe f8` | `movm [d2,a2],(sp)`; `add -8,sp` | `0x6089e868` | Going to sleep |
| sleep gate | `0x60325f80` API `0xC0001` | `82 fc cc 01 00` (`0c 00`) | `mov d0,d2`; `mov 0xc0001,d0` (7 bytes) | `0x60325f87` | Waits up to 1 s for `core` before the card is powered down |
| wake | `0x60898419` in `sleepForSystem` | `00 42 14 8f 23` | 5 bytes, re-executed as they are | `0x6089841e` | Sleep result in `d2` |

### 5.2. Data read by `core`

| Item | Address | What | Basis | Used by |
|---|---|---|---|---|
| Lever off accepted | `0x61e41990` (cached, CPU 0) | 1 from the lever event (before `shutDown`) until a cancel; set by `0x6091a7eb` and `0x6091a089`, cleared only by `0x6089821b` | code, emulator, camera | `drv_power_pending` |
| Power begun or sleep | `0x61e42010` | Set by `OlyHfwPower` at power-off and sleep; only for the log | code, camera | `drv_power_probe` |
| UI state | `0x6234f838`, read at its uncached alias `0xa234f838` as everywhere in the driver | Bytes: group, state; `0x0104` black screen | emulator, camera | `drv_power_probe` |
| Raw key input, group 0 | `0xa26dea7c` (uncached) | Bit 0 = lever (1 off) | emulator, camera | `drv_power_probe`, task dump |
| Card powered | `0x62151ee8` | 0 after card off and in light sleep | code, camera | `drv_power_probe` |
| Application state | `(0x61e497d8)` -> `+0xd8` -> `+0x70` -> `+172` | `0x5100` shooting, `0x5101` half-press, `0x3000` playback, `0x2` transition, `0x200c` preparing power-off/sleep, `0x2001` power-off, `0x2002` sleep, `0x1003` waking; menus `0x5911`, `0x5704`, `0x5800` (assumption) | emulator, camera | `drv_power_probe`, `drv_ui_probe` |
| Media busy | `0x61e418f0` = 1 or `0x61e418f4` != 0 | The camera's own card work; its sleep code waits for them | code | `drv_card_busy` |
| Wake reason / key | `0x61e43cd4` / `0x61e43cd8` | Written after sleep | code | not used |
| Light sleep limit | `0x61e43ccc` | 300000 ms | code, emulator | not used |

### 5.3. Firmware functions on the path (for reference)

| Function | Address | Basis |
|---|---|---|
| `startPowerOffSubAppStateMisc` (table entry; body `0x60d13016`) | `0x60d13011` | code |
| `executePowerOffSubAppStateMisc` | `0x60d1327e` | code |
| `OlyHfwPower::startShutDown` | `0x60897fba` (work part `0x608977d6`; settings saved at `0x60897c97`) | code, emulator |
| API `0xC0002` power-off | `0x60326055` -> last stage `0x60062952` | code |
| Card off request | `0x601b2fc7` -> `0x601b3267` (request 53 to the file server) | code, emulator |
| BLE wait | `0x6151bd83` | code |
| Power task loop | `0x60062b34` (queue 10) | code |
| `OlyHfwPower::startSleep` | settings saved at `0x6089883d`, media busy wait `0x60898267` | code |
| Sleep service task | `0x600630a7` (queue 72), sleep loop in SRAM `0x47000903` | code |
| `sleepForSystem` | `0x608983be` | code |
| `endSleepSubAppStateMisc` | `0x60d13a75` | code |
| Deep sleep power-off | `0x60d1313b` | code |
| Last instruction loop of power-off | SRAM `0x47000b15` -> `0x47000b6a` | code |

## 6. Files

| Function | Address | Call | Notes | Basis | Used by |
|---|---|---|---|---|---|
| `fopen` | `0x606aa3bd` | `[],4`; `a0` path, `a1` mode -> `a0` | = `fopen` with flags and `d0` 0 | code, camera | `drv_fopen`, `stub.s` |
| `fopen` with flags | `0x606ab771` | `[d2,d3,a2,a3],44`; `d0` flags | Flag `0x10`: long-name path walk | code, emulator | `drv_fopen_long` |
| `fread` | `0x606aa54e` | `[d2,d3,a2,a3],24`; `a0` buffer, `d0` size, `d1` count, `a1` file | | code, camera | `drv_fread`, `stub.s` |
| `fwrite` | `0x606aa444` | as `fread` | | code, camera | `drv_fwrite` |
| `fclose` | `0x606aa3c6` | `[d2,a2,a3],16` | | code, camera | `drv_fclose`, `stub.s` |
| `fflush` | `0x606aa4d2` | `[d2,a2,a3],16` | Not used by `core` (see [../power.md](../power.md)) | code, camera | `drv_fflush` (thunk only) |
| `fseek` | `0x606aa646` | `[d2,d3,a2,a3],20`; `a0`, `d0` offset, `d1` origin (2 = start) | | code | `drv_fseek` |
| `ftell` | `0x606aa5dc` | `[d2,a2,a3],16` | | code, camera | `drv_ftell` |
| `remove` | `0x606aa784` | `[d2,a2,a3],24`; `a0` path | | code, camera | `drv_remove` |
| `mkdir` | `0x606aa81f` | `[d2,a2,a3],28`; `a0` path, `d0` 0 | | code, camera | `drv_mkdir` |
| folder exists | `0x606ab659` | `[],8`; `a0` path -> `d0` | Not 0 for a folder (attribute `0x10`); the firmware calls it before `mkdir` | code, camera | `drv_exists` |
| `findfirst` | `0x606ab917` | `[d2,a2,a3],32`; `a0` mask, `a1` entry (280 bytes) -> `d0` handle or -1 | Takes two 2 KiB blocks of the firmware heap | code, emulator, camera | `drv_findfirst` |
| `findnext` | `0x606ab9b6` | `[d2,a2,a3],16`; `d0` handle, `a0` entry -> 0 / -1 | | code, camera | `drv_findnext` |
| `findclose` | `0x606aba1c` | `[d2,a2],12`; `d0` handle | | code, camera | `drv_findclose` |
| `stat` | `0x606ab04b` | `[d2,d3,a2,a3],44` | | code | not used |
| Call path chooser | `0x606b9064` | | Direct call in the server, synchronous request from other CPU 0 tasks, queued copy from CPUs 1-2 | code, emulator | |
| Mount check | `0x606ba50b` | | `fopen` fails without a mounted card | code | |
| Mode table | `0x607cef08` | | `r w a + b c d e h` | code | |

## 7. Other

| Item | Address | What | Basis | Used by |
|---|---|---|---|---|
| Setting reader | `0x602f4c5b` | `[],4`; `d0` setting id -> `d0` working value. Read only | code, camera | `drv_setting_read` |
| Key state | `0x60130fb8` | `[d2,d3,a2,a3],24`; `d0` key code -> 0 if held. Key codes: OK `4`, MENU `0x89`, UP 5, DOWN 6, LEFT 7, RIGHT 8, PLAY `0x87`, INFO `0x1205`, DEL `0x84`, S1 `0x100`, S2 `0x101`, lever `0x000c` | code, emulator | `stub.s` |
| Data cache invalidate range | `0x608442e6` | `[d2,d3,a2,a3],28`; `d0` address, `d1` length; L1 and second level of this CPU, **without** writing back | code, camera | `drv_dcache_inv` |
| Data cache clean and invalidate range | `0x60844232` | `[d2,d3,a2,a3],32`; over 8 KiB the whole L1 | code, camera | not used now |
| Whole data cache flush | `0x60069fbf` | With interrupts off | code | not used |
| S1 notification hook | `0x60323a64`, API `0x80001` entry | Stock `fc cc 01 00 08` (`00`), displaced `mov 0x80001,d0` (6 bytes), back `0x60323a6a`; module event | code, camera | `drv.c` (`OMK_EV_S1_API`) |
| Picture mode availability hook | `0x602d7964` | Module event; see [imaging.md](imaging.md) | code, camera | `drv.c` |
| Video compositor plane 2 scale | `0x9c000178` | Not 0 in the viewfinder; for the log | camera | `drv_power_probe` |

## 8. Loader patch symbols

The addresses the loader patch uses (hook site, caves, flag, buffer, entry) are listed with the patch:
[patch.md](patch.md#5-the-source-srcstubplatformom5m2fw1100).
