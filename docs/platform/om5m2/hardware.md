# OM-5 Mark II: hardware as the firmware sees it

What OMKit relies on about the processors, the memory, the caches, the memory protection unit and the interrupts. Nobody
has published documentation of this hardware; everything here is read from the firmware code or measured on a camera.
Register addresses are properties of the chip and are given here. Addresses of firmware functions and data change with
the firmware version: they are in [fw1100/system.md](fw1100/system.md) for firmware 1.1.

Basis labels are explained in [README.md](README.md#basis-labels).

## 1. Summary

| Topic | What to know | Basis |
|---|---|---|
| Main CPUs | Three MN10300-family CPUs (base instruction set, no AM33 extensions), each runs its own copy of the RTOS kernel | code |
| Other processors | An ARM Cortex-A9 coprocessor (it runs Linux), a power/clock "subprocessor", a unit that unpacks compressed firmware sections | code, inference |
| Address views | Every DRAM byte has a cached address `0x6...`/`0x7...` and an uncached alias `+0x40000000` (`0xa...`/`0xb...`) | code, camera |
| Data caches | 8 KiB per CPU, 16-byte lines, write-back, **not coherent between CPUs** | code, camera |
| Instruction cache | Never invalidated by the firmware at run time | code |
| Memory protection | No MMU. A debug unit (DbgSup) raises an NMI on writes to a configured range; the range covers most of the main image (code **and** data) through the cached addresses only | code, camera |
| Faults | Any instruction or memory fault on any CPU ends in an NMI and a restart within a short time | code, camera |

## 2. Processors

| Processor | What runs there | Basis |
|---|---|---|
| CPU 0 | Startup, the camera application and UI (`GUI_TASK`, `CAM_APP`), the file system server, USB/PTP, shooting control, power. About 160 tasks. OMKit `core` and its modules run here | code (task tables), camera |
| CPU 1 | Wi-Fi, Bluetooth LE, AF helpers, the 3D/vector drawing task of the graphic editors. About 70 tasks | code, camera (a hook in the editors' renderer ran on CPU 1) |
| CPU 2 | Video recording (`MVR_*`), part of the still recording (`REC_*`), on-screen drawing for the second CPU (`OSD_DRAW_CPU2`), motion detection; live view processing calls | code, camera (live view hooks ran on CPU 2) |
| ARM Cortex-A9 | Linux, loaded by the main firmware from the large data component; the firmware has commands to load and start it | code (strings and loader) |
| Subprocessor | Power sequencing, keys while asleep, the real-time clock, the hardware watchdog. The main CPUs talk to it through a register window at `0x98e00000` | code |
| Section unpacker | Unpacks the compressed firmware sections at startup; it is driven through a mailbox in the shared block and writes DRAM past the CPUs' data caches. Whether it is a fourth MN103 core or a separate block is not known | code (inference) |

Public specifications name the image processor TruePic IX and speak of four MN103 cores; the firmware runs a kernel on
three of them (CPU numbers 0..2).

The CPU number is the 16-bit word at system register `0x8050`. MN10300 code for these CPUs must use the base instruction
set only: AM33 encodings are unknown instructions here (a system error, i.e. an NMI).

## 3. Memory map (general)

| Range | What | Basis |
|---|---|---|
| `0x40000000..` / `0x80000000..` | Flash memory, cached view / uncached view. The firmware writes it only through its flash driver | code |
| `0x47000000..` | Small local memory (SRAM): the early startup stack and code that must run while DRAM may be unavailable (the sleep loop) | code |
| `0x60000000..0x7fffffff` | DRAM, cached view. The main firmware image executes here from `0x60021020` | code, camera |
| `0xa0000000..0xafffffff` | DRAM, uncached alias of `0x60000000..0x6fffffff`: same byte, the address `+0x40000000` | code, camera |
| `0xb0000000..0xbfffffff` | Two 128 MiB windows into further DRAM banks, chosen per CPU by registers `0x9810004c` and `0x98100058` (`+0x400` per CPU). Large image buffers live here | code, camera (registers read `0x00007654`) |
| `0x8000..` (16-bit addresses) | Per-CPU system registers: interrupt vectors `0x8000..0x801c`, system error cause `0x8044`, CPU number `0x8050`, cache control `0x8070`, NMI cause `0x8900` | code |
| `0x8fe00000..`, `0x8ff00000..` | Memory shared by the CPUs: pointers to each kernel's control block (`0x8ff00004 + 4 * cpu`), inter-CPU flags and mailboxes, a spin bit used as a hardware semaphore (`0x8fe00018`, bit 7) | code; "hardware semaphore" is an assumption |
| `0x98100000 + 0x400 * cpu` | Per-CPU memory/cache controller (second-level cache operations, bank windows) | code |
| `0x9890014c` | Free-running microsecond timer, counts down, 30 bits | code, camera |
| `0x98e00000..` | Subprocessor window: watchdog command, clock (`+0x98` seconds, `+0x9c` fraction), power commands | code |
| `0x99200408`, `0x99300408`, `0x99b00408` | Inter-processor interrupt to CPU 0, 1, 2 | code |
| `0x99700000..` | DbgSup, the memory protection unit (section 5) | code, camera |
| `0x9c000000..` | Video compositor (planes of the screen and viewfinder) | code, camera |

**DRAM size.** The bank code maps `0xa0000000..0xdfffffff` as eight 128 MiB banks, so the DRAM is very likely 1 GiB
(inference). Code on the main CPUs sees the first 256 MiB directly; the rest only through the windows. The ARM
coprocessor uses part of this memory too.

**Speed** (camera): an uncached word read takes about 0.65 us; filling 3 MiB through the uncached alias takes about
0.4 s (about 8 MB/s).

The layout of the main image in DRAM (sections, the firmware's working data, the free gap OMKit uses) depends on the
firmware version: [fw1100/system.md](fw1100/system.md#1-memory-layout).

## 4. Caches and coherency

| Item | Value | Basis |
|---|---|---|
| L1 data cache | 8 KiB per CPU: 2 ways x 256 lines x 16 bytes | code (cache maintenance loops) |
| Write policy | Write-back: a dirty line reaches DRAM only when evicted or cleaned | assumption from the cache control bits; confirmed by a failure on the camera (below) |
| L1 instruction cache | Present; size unknown. OMKit assumes a 16-byte line | code (control bits); line size is an assumption |
| Second level cache | Present, operated through the per-CPU controller; its line is probably 64 bytes | code; line size is an assumption |
| Coherency between CPUs | **None** for the data caches. Each CPU's cache operations act on its own cache only | code; consistent with camera behaviour |
| Cache control register | `0x8070`, per CPU: at startup `0x30` (flush) then `3` (instructions and data on); bit `0x10` invalidates the instruction cache, `0x20` the data cache | code |
| Instruction cache invalidation at run time | **Never** done by the firmware; only at startup | code (every use of `0x8070`) |

What follows from this (all of it learned the hard way on a camera, where the emulator showed nothing):

- **Cached and uncached writes to the same memory corrupt each other.** In one `core` build its stack (written through the
  cache) shared lines with the start of the module arena (written uncached). Evicted dirty stack lines overwrote module
  code; the camera went into a reboot loop (camera). OMKit therefore keeps the `core` image and stack (cached) and the
  arena (uncached) in separate 4 KiB-aligned regions. Inside the arena the same rule holds for module images, the only
  blocks used through the cache (a module's variables, on CPU 0): each starts and ends on a 64-byte boundary - the
  first-level line and the second level's probable one - so it shares no line with its uncached neighbours.
- **Data shared between CPUs goes through the uncached alias** (or is only ever touched from one CPU). A hook can be called
  by a task of any CPU; a word another CPU wrote a moment ago through its cache may still be old in DRAM.
- **After writing through the uncached alias, the writing CPU may still hold an old line** of that address in its own
  cache. Either never read that address through the cache afterwards, or invalidate the line on that CPU.
- **New code loaded through the uncached alias can be executed through the cached address** without any cache work, as long
  as that memory was never executed or read through the cache before (camera: `core` itself is loaded this way). Memory
  that held code which already ran must not be reused for other code in the same power-on: the firmware has no run-time
  instruction cache invalidation.
- **Code patched in place** (hooks) becomes visible on a CPU when the old line leaves its instruction cache. A patch must
  not cross a 16-byte line, so that a CPU sees either the old or the new instruction, never half of each.

## 5. Memory protection: DbgSup

There are no pages and no MMU: the CPUs implement the base MN10300 set, and the firmware never touches MMU registers
(code). All tasks of all CPUs share one address space.

The firmware programs a hardware debug-support unit, **DbgSup** (`0x99700000`), at startup. It compares bus addresses with
two groups of four ranges and raises an **NMI** on a match. As configured by the firmware, the ranges cover addresses where
no memory exists, plus one range over the **main image from its start to the end of the last section that has a fixed
place in it** (for firmware 1.1: `0x60000200..0x60cbe7ff`, see [fw1100/system.md](fw1100/system.md#2-memory-protection)).
The unit is set up twice during startup (plus a temporary channel that narrows as the sections load) and stays on.

| Register | Meaning | Basis |
|---|---|---|
| `+0x00` | Control: 0 off; observed `0x00001103` (on) | code, camera |
| `+0x04` | Channel enable, `0xff` | code, camera |
| `+0x10..+0x1c` | Bus sources per group | code, camera |
| `+0x20`, `+0x24` | Addresses A and B of the latest access the unit looked at (not only a fault) | camera; meaning of A/B is an assumption |
| `+0x28` | Comparison result; the NMI handler reads it | code |
| `+0x40..+0x5c` | Group 1: four pairs {end, start} | code, camera (values match the firmware tables exactly) |
| `+0x60..+0x7c` | Group 2: four pairs | code, camera |

Behaviour measured on the camera:

| Access | Result |
|---|---|
| Write through the cached address inside the range | NMI, restart (seen with code and with data words) |
| Write of the same word through the uncached alias | Works; the unit stays on and silent; the cached address then reads the new value |
| Read through the cached address inside the range | Works |
| Write through the cached address outside the range (e.g. later sections) | Works |

So the unit is a trap for stray writes, not a security barrier. Consequences for code:

- Writes into firmware code **and data** inside the protected range go through the uncached alias only (OMKit: only
  through driver functions, see [memory.md](memory.md#6-rules-for-core-and-modules)). Each new place OMKit writes in
  firmware memory is checked against the range.
- OMKit leaves the unit on: a stray write by a module into firmware code becomes an immediate, visible restart instead of
  silent corruption. The firmware itself contains functions to turn it off; OMKit does not use them.
- The NMI handler writes 0 to the control register on all three CPUs; the unit is programmed again on every power-on.

## 6. Interrupts, NMI and the watchdog

- Each CPU has its own vector table in the main image (addresses per version in
  [fw1100/system.md](fw1100/system.md#3-kernel-interrupts-and-faults)).
- The regular interrupt entry of CPU 0 writes the interrupted `sp`, `psw` and `pc` into CPU 0's crash record on **every**
  interrupt. These three words therefore show the last interrupted place, not a fault (code, camera).
- **NMI sources** (register `0x8900`): bit 1 external NMI, bit 2 watchdog, bit 4 system error. For a system error,
  register `0x8044` holds the cause: `4` misaligned access, `8` unknown instruction, `0x20` memory error (including the
  DbgSup trap), `0x200` unknown extension (code).
- **NMI handler** (the same on all CPUs): turns DbgSup off, saves the task number, registers and the DbgSup result into the
  CPU's crash record (`0x40` bytes per CPU), prints a message on the debug port, sets the subprocessor watchdog to its
  minimum and spins. The camera restarts shortly after (code, camera).
- **The crash record does not survive the restart**: the handler writes it through the cache, and the dirty lines are lost
  at the reset (camera: the record was always empty after a restart). After a real power-off DRAM reads as nearly all
  ones with random flipped bits (camera).
- **Watchdog**: a firmware task feeds the subprocessor watchdog every 3 s with a 10 s limit (code). A CPU stuck with
  interrupts off, or starved by a busy high-priority loop, ends in a restart. A task that only waits forever (for example
  the UI waiting for a message that never comes) is **not** caught: the camera then stops responding until the battery is
  taken out (camera).
- **No boot failure counter** exists in the firmware: a fault that happens on every start is a reboot loop (code).
- **Running a function on another CPU**: the firmware has a call that runs a function in the interrupt context of another
  CPU (code). OMKit does not use it.

## 7. Time

| Source | Unit | Notes | Basis |
|---|---|---|---|
| Microsecond timer `0x9890014c` | 1 us, 30 bits, counts down | Wraps every ~1073 s; OMKit's `drv_ticks` returns `0x3fffffff - value`. Whether it runs during sleep is unknown | code, camera |
| Kernel time (`get_tim`) | 1 ms, 48 bits | Per CPU; CPU 0's tick is not served while asleep, so it probably stops during sleep | code; sleep behaviour is an assumption |
| Subprocessor clock `0x98e00098` / `0x98e0009c` | seconds / 1/1024 s in bits 5..14 | Keeps counting during sleep. The firmware checks `((0x98e00000) >> 2) & 63 == 42` before reading; the two words are read separately, so read the seconds twice to catch a carry | code |

## 8. Physical facts that matter to software

- The memory card sits in the battery compartment: taking the card out means taking the power away (camera).
- After the power lever goes to off, the screen goes black at once but the camera keeps running for up to a few seconds;
  turning it on again in that window cancels the power-off, and nothing restarts (camera). See [power.md](power.md).
- There is one card slot; the firmware calls it drive `C:`.
