# OM-5 Mark II: the RTOS

The firmware runs a uITRON 4.0 kernel, one instance per CPU, all three built from the same code. On top of it the firmware
has its own wrappers, named `Oly*` in the firmware's strings (`OlyTaSt`, `OlyEvWt`, `OlyQeRd`, ...). This page describes
the kernel as OMKit uses it and how the task dump works. Function addresses for firmware 1.1 are in
[fw1100/system.md](fw1100/system.md#4-rtos-services); the structure layouts below were read from firmware 1.1.

Basis labels: [README.md](README.md#basis-labels).

## 1. Basics

| Item | Value | Basis |
|---|---|---|
| Kernel | uITRON 4.0 services (`dly_tsk`, `slp_tsk`, `wup_tsk`, `set_flg`, `wai_flg`, `sig_sem`, `loc_mtx`, `get_tim`, ...), one kernel control block per CPU, shared code | code |
| Tick | 1 ms; timeouts are in milliseconds | inference (the firmware prints kernel time as milliseconds) |
| Priorities | Smaller number = higher priority. Most firmware service tasks run at 55 (93 of the 158 static tasks of CPU 0); the three still recording tasks run at 54, one task at 56; `core` runs at 66, below them | code |
| Object ids | `cpu << 12 | n`; bit 15 marks an "L class" object; the block is found in the kernel of that CPU | code |
| Error codes | As in the specification: -17 `E_PAR`, -18 `E_ID`, -25 `E_CTX`, -41 `E_OBJ`, -42 `E_NOEXS`, -43 `E_QOVR`, -50 `E_TMOUT`. The `Oly*` wrappers return -1 on failure (-2 for a timeout) and record the failure in the firmware's error log | code |
| Service table | None: services are called directly. Every wrapper is labelled with its name string and calls one kernel function | code |

## 2. Tasks

- **Static tasks** come from a configuration table per CPU: 158 / 75 / 32 entries on CPUs 0 / 1 / 2 (code). A live count on
  the camera: 166 / 75 / 32 tasks (camera, task dump).
- **Dynamic tasks** are created by the firmware's `acre_Task` wrapper: name (up to 16 characters, kept in a per-CPU table of
  64 names), entry, priority, stack size, stack (0: from the kernel pool), start now or later (code). It fills the new
  stack with `0xdeadbeef` except its top 32 bytes, which makes the stack high-water mark readable later. Priorities 8..11
  are replaced by the wrapper with 45, 46, 54 and 55 (code; why is not known). When the name table is full, the wrapper
  returns -1 although the task was created (code).
- Tasks created by the plain kernel call have no name in that table.

Known tasks that matter to OMKit (firmware 1.1; ids may differ in other versions):

| Task | Id | Priority | Stack | CPU | Role | Basis |
|---|---|---|---|---|---|---|
| `FILECLBK` | 16 | 55 | 4 KiB | 0 | File system server: executes all card requests in order | code |
| `FILEFGND` | `0x11` | 55 | 2 KiB | 0 | File system foreground work | camera (task dump) |
| `GUI_TASK` | `0xa7` | 55 | 16 KiB | 0 | User interface | camera (task dump) |
| `CAM_APP` | | | | 0 | The camera application state machine; runs power-off and sleep | camera (task dump), code |
| `BCHK_MAIN` (power task) | `0x0b` | 55 (54 while doing periodic work) | 1.25 KiB | 0 | Serves power messages (queue 10): switch parts off and on | code |
| Sleep server | | 55 | 2 KiB, in SRAM | 0 | Runs the light-sleep loop (queue 72) | code |
| `SYS_WDT` | `0x19` | 55 | 1 KiB | 0 | Feeds the watchdog every 3 s; the limit is 10 s. It runs at 55 like most service tasks, so a task above 55 that computes without waiting holds it up | code |
| `FIRMUP_CLI`, `FIRMUP_SRV`, `FIRMUP_LED` | 89, 18, 118 | 45, 55, 25 | 2, 2, 1 KiB | 0 | Firmware update from the card: client, service, lamp | code |
| `USRSTART_LOAD`, `USRSTART2..5` | 47, 43..46 | 55 | 1-2 KiB | 0 | Startup: section loading and system start | code |
| `REC_IMAGE`, `REC_FILE`, `REC_MAIN` | `0x20`, `0x1f`, `0x21` | 54 | 13.75, 4.5, 4 KiB | 0 | Still recording; `REC_IMAGE` takes each picture's processing job from its queue and runs it synchronously | code, camera |
| `REC_MAIN_CPU2`, `REC_PRE_V1_CPU2`, `REC_VIEW_CPU2` | `0x2012..0x2014` | 55, 26, 25 | 8 KiB each | 2 | Still recording, second half | code |
| `C3D_MAIN` | | | | 1 | Vector/3D drawing for the graphic editors | camera |
| `OMK_LOAD` | dynamic | 66 | 2 KiB from the kernel pool | 0 | Created by the OMKit loader; becomes the `core` task | camera |

## 3. Kernel objects

| Object | How the firmware uses it | Notes | Basis |
|---|---|---|---|
| Event flags | The main way tasks wake each other: about 135 places set a flag and 180 wait on one through `OlyEvSt` / `OlyEvWt` | Attribute `TA_CLR` (4) clears the pattern on release, `TA_WMUL` (2) allows several waiters; wait mode 1 = OR | code |
| Semaphores | Counting, created with a name by a wrapper; wait with a timeout (`twai_sem`; plain `wai_sem` = timeout -1) | | code |
| Mutexes | With priority inheritance (attribute 2) or a ceiling (3) | The owner's task id is in the block, which makes "who holds it" readable | code |
| Data queues, mailboxes, message buffers | Mailboxes and message buffers carry requests to service tasks (application, UI input, shooting, recording) | | code, camera (task dump) |
| Message queues (`OlyQeWr` / `OlyQeRd`) | Requests to servers. A synchronous request creates a temporary reply queue, posts the request and waits for the reply **without a timeout** | The file system, power and storage servers are called this way | code |
| Fixed and variable memory pools | The kernel knows them (wait reasons exist), but no firmware wrapper uses them | OMKit does not use them | code |

Limits per CPU (0 / 1 / 2): tasks 255 / 128 / 64, semaphores 255 / 128 / 32, event flags 350 / 64 / 32, mutexes 31 / 64 / 32
(code).

## 4. Rules for code that runs on this kernel

- **Kernel services work only on objects of the caller's CPU.** A call on an object of another CPU returns `E_CTX` (code).
  The `Oly*` wrappers compare the CPUs and switch to a cross-CPU variant: it sends an inter-processor interrupt to the target
  CPU and **waits without a timeout** until that CPU has done the request. A task calling such a wrapper stands still until
  the other CPU answers (code). In power-off and sleep paths, where a CPU may be busy or stopped, do not use them.
- **`dly_tsk` cannot be cut short.** The kernel build has no `rel_wai`, and `wup_tsk` does not end a delay (code). A task
  that must react quickly to an event waits on an event flag with a timeout instead (that is what `core` does).
- **`wup_tsk` is counted**: a wake-up sent before the task sleeps is kept (up to 255), so it is not lost (code).
- **Task services are refused inside an interrupt handler** (`E_CTX`); the `i*` services only work inside one (code).
- **The firmware lock** (`drv_lock` in OMKit) disables interrupts and dispatching on the caller's CPU only. It does not
  stop the other CPUs. Keep it short; never call file functions or wait under it.
- **Hooks run in the firmware's tasks** (often the application or UI task, sometimes tasks of CPUs 1 and 2). They must
  use little stack and time, take no firmware locks for long, and never wait on something that the hooked task itself
  provides. Heavy work goes to the `core` task.
- **Time**: kernel time is per CPU and 48 bits (no wrap in practice); the microsecond timer wraps every ~1073 s
  ([hardware.md](hardware.md#7-time)). Arithmetic on the microsecond counter must allow for the wrap.

### How `core` waits

`core` runs in the loader's task `OMK_LOAD` at priority 66 on CPU 0, on its own 16 KiB stack inside the `core` image. Its
service loop waits up to 250 ms on its own event flag (`OMK_CORE`, created with `TA_CLR`), and the power hooks set the flag
from CPU 0 so that `core` reacts at once instead of on the next tick. If the flag cannot be created, or if the wait keeps
returning immediately (50 times in a row), `core` falls back to plain `dly_tsk` and logs it (code of `core`, camera).

## 5. Kernel structures (firmware 1.1)

Read from the kernel code of firmware 1.1 and checked on CPU 0 in an emulator; CPUs 1 and 2 use the same code on their own
control blocks (camera: the task dump reads all three). Other firmware versions probably keep the layout but must be
checked.

### 5.1. Kernel control block (KCB)

The pointer is at `0x8ff00004 + 4 * cpu` (shared memory). In firmware 1.1 the three blocks are static data of the image;
their addresses are in [fw1100/system.md](fw1100/system.md#4-rtos-services).

| Offset | Content |
|---|---|
| `+28` | -> interrupt nesting byte |
| `+60` | -> u16 pairs {static count, maximum} per kind: tasks `+0`, semaphores `+4`, flags `+8`, data queues `+12`, mailboxes `+16`, mutexes `+20`, message buffers `+24` |
| `+68` | -> ready queue |
| `+96` | -> word holding the running task's TCB |
| `+124`, `+128` | -> static task table (28-byte rows `{id, atr, exinf, task, pri, stksz, stk}`, ends at `id = 0`) and its end |
| `+204` | -> TCB pointers, `[number - 1]` |
| `+208`, `+212`, `+216`, `+220`, `+224`, `+232` | -> object block pointers: semaphores, flags, data queues, mailboxes, mutexes, message buffers |
| `+380` | number of CPUs (3) |

### 5.2. Task control block (TCB, 120 bytes)

| Offset | Field |
|---|---|
| `+0`, `+4` | Link in the ready or wait queue |
| `+8`, `+10` | u16 current and base priority |
| `+12` | u16 state: 1 RUN, 2 RDY, 4 WAI, 8 SUS, 12 WAI+SUS, 16 DMT |
| `+14` | u16 wait reason: 1 SLP, 2 DLY, 4 SEM, 8 FLG, `0x10`/`0x20` data queue send/receive, `0x40` MBX, `0x80` MTX, `0x100`/`0x200` message buffer send/receive, `0x2000` MPF, `0x4000` MPL |
| `+16` | u16 task number (0: slot free) |
| `+18` | u16 id of the object waited for (none for SLP, DLY) |
| `+20`, `+21`, `+22` | u8 activation, wake-up and suspend counts |
| `+24` | saved `sp` of a task that is not running |
| `+36` | timeout ticks left + 1 (0: no timeout) |
| `+64` | wait result code |
| `+68..+91` | copy of the creation record: `atr`, `exinf`, entry `+76`, initial priority `+80` (u16), stack size `+84`, stack `+88` (filled in by the kernel when it allocated the stack) |
| `+92` | -> head of the queue the task waits in (object block + 4) |
| `+96`, `+100` | event flag wait: pattern, mode (1 = OR) |

### 5.3. Object blocks

| Object | Fields |
|---|---|
| Semaphore | `+0` id, `+2` = 4, `+4` wait queue, `+12` attribute, `+20` maximum, `+24` count |
| Event flag | `+0` id, `+2` = 8, `+4` wait queue, `+12` attribute, `+20` pattern |
| Mutex | `+0..+7` link in the owner's list (assumption), `+8` id, `+12` wait queue, `+20` attribute, `+22` ceiling, `+24` owner task (0: free) |

### 5.4. Saved context and names

- A task switched out by the dispatcher has a fixed return address (inside the dispatcher) at saved `sp + 16`; above it
  are the frames of the wait service, its `Oly*` wrapper and the firmware code that called it (code, emulator).
- MN10300 frames are not chained by a frame pointer, so callers are found by scanning the stack: a word is a return address
  if it points into code and the instruction before it is a call (`cd` call d16, `dd` call d32, `fa ff` calls d16, `fc ff`
  calls d32, `f0 f0..f3` calls (an)). False hits are possible.
- Names: static tasks have 26-byte records `{char name[24]; u16 number}` in per-CPU tables; `acre_Task` tasks have 18-byte
  records `{char name[16]; u16 id}`, 64 per CPU, in uncached memory.

## 6. The task dump

When the UI stops responding, the useful evidence is which firmware tasks are waiting, and on what. The firmware has a
task list command for its debug port, but it uses `ref_tsk`, which takes the kernel lock and sees only its own CPU. OMKit
therefore reads the kernel structures itself (`core/taskdump.c`, the driver's `kernel.c`).

### 6.1. How it reads (safety)

- Read only: no locks, no firmware calls. The `core` task has the lowest priority on CPU 0, so while it reads, no other
  task of CPU 0 runs; only interrupts can change CPU 0's structures.
- CPU 0's words are read through the cache (as that kernel writes them); CPUs 1 and 2 through the uncached alias. A word
  another CPU wrote a moment ago may still be old in DRAM, so state, waited object and `sp` are read twice; a task whose
  words changed is marked `unstable`.
- Every pointer is range-checked (`0x60000000..0x64000000`, aligned) before it is followed, and each KCB pointer must equal
  the address expected for this firmware; otherwise that CPU is not read. A read outside memory would restart the camera.
- The snapshot of all tasks goes into a buffer from the arena (320 tasks x 92 bytes), then into the log at 12 lines per
  250 ms tick, only while the log ring has at least 8 KiB free, so the dump never pushes other lines out.
- Measured on the camera: the snapshot of all three CPUs took 17 ms.

### 6.2. When

| Trigger | Condition | Level |
|---|---|---|
| Power-off or sleep did not happen | `core` saw the hook but the camera kept running | warning |
| The application seems stuck | Its state stays "half-press" or "transition" for `taskdump_stuck` seconds (6) while the user pressed keys; half-press without key changes after four times that | warning |
| On request | `taskdump_every = N` (every N s, at least 10); `taskdump_base = N` (one baseline dump at second N) | info |

At most 8 dumps per power-on; automatic dumps closer than 20 s to each other count as one. Keys in `[core]` of
`OMKIT.INI`: `taskdump` (on by default; it writes nothing without a trigger), `taskdump_every`, `taskdump_base`,
`taskdump_stuck`.

### 6.3. Reading the output

```
taskdump 1 (baseline): app 5100 keys 06, snapshot 0 ms
taskdump 1: cpu0 168 tasks 151 dormant run 00a8 bad 1, cpu1 0 tasks 0 dormant run ? bad 1, ...; kept 17 dropped 0, objects 9 mutexes held 0
td c0 0011 FILEFGND WAI FLG 0022 ptn 00000001/1 p55/55 sp 164/2048 r 600511eb 606b9479 606c0b3a 606a9dce 606a9e6f
td c0 00a0 BF_Timer WAI FLG 012b ptn 00000001/0 tmo 51 p45/45 sp 128/1024 never 880 r 60b3c455 60b3c48f
td c0 00a8 OMK_LOAD RUN p66/66 sp -/2048 never 1792
td obj FLG 0022 ptn 00000000 atr 4 waiters 1
td obj SEM 0091 cnt 0/1 atr 1 waiters 1
taskdump 1 end
```

| Field | Meaning |
|---|---|
| `app`, `keys` | Application state word and the raw key input byte at the time of the dump |
| `cpuN T tasks D dormant run X bad B` | Tasks read, how many dormant (counted only), the running task, unreadable slots |
| `td cN ID NAME` | CPU, task number, name (`?` if it has none) |
| State | `RUN RDY WAI SUS WAS DMT` (`WAS` = waiting and suspended) |
| Reason and object | `SLP DLY SEM FLG SDTQ RDTQ MBX MTX SMBF RMBF MPF MPL` and the object id |
| `ptn P/M` | Event flag wait: pattern and mode (1 = OR) |
| `tmo` | Timeout left, ms; missing = waits forever |
| `pC/B` | Current / base priority |
| `wup`, `sus` | Queued wake-ups, suspend count (only when not 0) |
| `sp U/S` | Bytes used from the top of the stack / stack size |
| `never` | Bytes at the bottom of the stack never written (only for stacks the firmware filled) |
| `unstable`, `noctx`, `badstack` | Words changed while read; saved context is not the dispatcher's; stack data out of range |
| `r a1 a2 ...` | Up to 6 return addresses above the saved `sp`, innermost first, kernel addresses left out |
| `td obj` | Objects waited for: semaphore count/max, flag pattern, mutex owner and ceiling; `waiters` = tasks of the snapshot waiting on it; `gone` = no longer exists. Mutexes with an owner are always listed |

How to interpret a dump:

- A deadlock shows as tasks waiting on semaphores or mutexes held by each other: look at `td obj` owners and waiters.
- If every task sits in its usual wait (flags, mailboxes, message buffers, delays) and no mutex is held, it is not a
  deadlock: some request was never answered. On the camera, the UI hangs investigated this way were of this kind
  (the application waiting in its mailbox).
- The first return address of a waiting task is usually inside an `Oly*` wrapper; the next ones are the firmware code that
  waits. Addresses of the wrappers in firmware 1.1 are in [fw1100/system.md](fw1100/system.md#4-rtos-services); the rest
  must be looked up in a disassembly of the firmware. A cross-CPU wait wrapper in the chain means the task waits for
  another CPU to answer.
- Compare with a baseline dump (`taskdump_base`) of a normally working camera: most tasks wait in the same places.

Limits: a task that was preempted by an interrupt (`RDY`) has its interrupted `pc` in an interrupt frame that the scan does
not decode; the stale-word protection for CPUs 1 and 2 is the double read only.
