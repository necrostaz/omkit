# OM-5 Mark II platform notes (`om5m2`)

What OMKit knows about the OM-5 Mark II: the hardware as the firmware uses it, the RTOS, memory, power, the card, the
firmware itself, and the imaging side. The pages directly in this folder describe the camera and hold across firmware
versions; everything tied to one firmware build (addresses, the loader patch) is in a subfolder per version.

The camera's manufacturer publishes none of this. It was learned by reading the firmware, by running it in an emulator,
and by experiments on a camera. Read [Basis labels](#basis-labels) before relying on a statement.

## Pages

### System

| Page | Contents |
|---|---|
| [hardware.md](hardware.md) | CPUs and what runs on them, memory map, caches and coherency, the DbgSup memory protection unit, interrupts, NMI, watchdog, time sources |
| [rtos.md](rtos.md) | The uITRON kernel: tasks, priorities, kernel objects, rules for code in hooks and tasks, kernel structures, the task dump and how to read it |
| [memory.md](memory.md) | Firmware heaps and pools, the OMKit arena and allocator, the image allocator, what is safe to allocate and to write |
| [power.md](power.md) | Power-off, sleep, the power lever, what must happen before the card and power go away, card write ordering |
| [storage.md](storage.md) | The card file API, 8.3 and long names, "card busy", what OMKit keeps on the card, the settings storage in flash |
| [firmware.md](firmware.md) | Firmware files and versions, startup, updates, recovery |

### Imaging

| Page | Contents |
|---|---|
| [picture-modes.md](picture-modes.md) | Picture modes and how the firmware selects and stores them |
| [ui.md](ui.md) | The user interface: screens, lists, editors, pictures |
| [color-pipeline.md](color-pipeline.md) | The colour pipeline and its tables |
| [still-pipeline.md](still-pipeline.md) | How a still is developed: jobs, passes, buffers |
| [grain.md](grain.md) | Film grain in the firmware's pipeline |

### Firmware 1.1 (`fw1100`)

| Page | Contents |
|---|---|
| [fw1100/README.md](fw1100/README.md) | The 1.1 build, how OMKit checks that it runs on it, what is version-specific |
| [fw1100/patch.md](fw1100/patch.md) | The loader patch: why, where, what it changes, what it does at boot, return to stock |
| [fw1100/system.md](fw1100/system.md) | Catalogue of the system-side functions, data and structures OMKit uses |
| [fw1100/imaging.md](fw1100/imaging.md) | Catalogue of the imaging-side functions, data and structures OMKit uses |

## Basis labels

Every statement that matters says what it rests on:

| Label | Meaning |
|---|---|
| **code** | Read in the firmware code (static analysis) |
| **emulator** | Seen when running the firmware in an emulator. The emulator models neither the caches, nor the memory protection unit, nor CPUs 1 and 2, nor the real startup, so memory and timing questions are never settled there |
| **camera** | Measured or observed on a real camera |
| **inference** | Follows from the above, not tested directly |
| **assumption** | A working guess |

Where a page says "code" for a whole table, the rows were read in the firmware unless a row says otherwise.

## Conventions

- Addresses are the execution addresses of the main firmware in its cached view (`0x6...`), unless noted. The same DRAM byte
  is also reachable through the uncached alias, address `+ 0x40000000` (`0xa...`).
- CPUs are numbered 0, 1, 2 as the firmware numbers them.
- Task priorities: a smaller number is a higher priority.
- MN10300 call forms are written as in the assembler: `call f,[d2,d3,a2,a3],24` names the registers saved and the frame.
- Names of firmware functions are the firmware's own where its strings give them (`CameraCommon::shutDown`, `OlyEvWt`),
  otherwise descriptive.
