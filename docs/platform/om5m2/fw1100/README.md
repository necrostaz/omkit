# OM-5 Mark II firmware 1.1 (`fw1100`)

Everything in OMKit that depends on the exact firmware build lives under `fw1100`: the driver
`src/platform/om5m2/fw1100/`, the loader patch `src/stub/platform/om5m2/fw1100/` and these pages. The camera-level
knowledge that holds across versions is one level up ([../README.md](../README.md)).

| Page | Contents |
|---|---|
| [install.md](install.md) | For camera owners: installing OMKit from a release, going back to stock, warnings, troubleshooting |
| [patch.md](patch.md) | The loader patch: why, where it hooks, what it changes, what it does at boot, how to return to stock |
| [system.md](system.md) | Catalogue of the firmware 1.1 functions, data and registers OMKit uses on the system side |
| [imaging.md](imaging.md) | The same for the imaging side (picture modes, UI, colour, stills, grain) |

## 1. The firmware

| Item | Value |
|---|---|
| Model | OM-5 Mark II, model code `E148` |
| Version | 1.1 (`1100`), the latest official version when OMKit was written |
| Official file | `OLY_E_148_1100_0000_0000.BIN`, 90 832 896 bytes, SHA-256 `9de5d77e1d290fa2ed4ebc7d75cee0ed490befbc89566722467d1d421d66d637` |
| Main code | Flash address `0x40780000`; executes in DRAM from `0x60021020` |
| Sections | Ten, two of them compressed (6 and 7); table and load ranges in [system.md](system.md#1-memory-layout) |

All addresses on the `fw1100` pages are execution addresses of this build (the cached view), unless a page says otherwise.

## 2. Identity check

The loader does not check the firmware: it is itself part of the patched 1.1 image. `core` does, before anything else:

| Check | Where | On failure |
|---|---|---|
| **Fingerprint**: the first 8 bytes of 22 firmware functions the driver calls or hooks (file functions, `dly_tsk`, the setting reader, the firmware lock, the uncached heap, the event flag wrappers, the data cache invalidation, one picture-mode callback) are compared with the bytes of firmware 1.1 | `drv_check` in `drv.c` | `core` returns at once, before it calls any firmware function (so it writes no log line: the log needs the firmware's lock and file functions, whose addresses are those of 1.1); the loader task ends and the camera works as stock |
| **Kernel blocks**: the three kernel control block pointers must equal the 1.1 addresses | `kernel.c` | The task dump does not read that CPU |
| **Hook sites**: every hook compares the bytes it replaces with the expected ones before writing | `drv_code_jump` | That hook is not installed; the log says which |
| **Tables and structures** the imaging side changes are checked against their expected contents before use | imaging driver files | The feature is not installed |

The fingerprint covers the functions OMKit depends on most; it is not a checksum of the whole firmware. A different build
with identical prologues would pass it, and the per-hook byte checks are the second line of defence.

## 3. What is specific to 1.1

- Every address in the driver (`drv.c`, `kernel.c`, `thunks.s`, `fw.ld`, the imaging files) and in the loader
  (`gen.py`, `spec.json`).
- The calling convention of each firmware function: on MN10300 the `call` instruction names the saved registers and the
  stack frame, and they must match the callee's `ret` ([system.md](system.md#calling-firmware-functions)).
- The free memory gap behind the first resource record (its size depends on the resources of this version).
- The DbgSup protected range, which ends where section 7 starts.
- Task and object numbers, the layout of the kernel structures, setting numbers and the hook sites.

Porting OMKit to another firmware version means a new `platform/om5m2/<version>/` driver and a new loader patch, with
every address found again and checked; the platform pages tell what to look for.
