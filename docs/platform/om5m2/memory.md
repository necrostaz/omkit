# OM-5 Mark II: memory for OMKit

Where the firmware keeps its memory, where OMKit takes its own, and what is safe to allocate or write. Hardware background
(address views, caches, the protection unit) is in [hardware.md](hardware.md); the numbers for firmware 1.1 are in
[fw1100/system.md](fw1100/system.md#1-memory-layout).

Basis labels: [README.md](README.md#basis-labels).

## 1. What the firmware has

| Mechanism | What it is | Use for OMKit | Basis |
|---|---|---|---|
| `scheap` (cached) | Small-block heap, **one per CPU**, three size classes: up to 128, 2048 and 16384 bytes. Larger requests fail (`scheap_alloc: Size Over`). Under the firmware lock; a block never handed out is taken first, then the stack of freed blocks. CPU 0: 3013 x 128, 430 x 2048, 57 x 16384 bytes (2.2 MiB); CPUs 1 and 2: 320 x 128, 32 x 2048, 32 x 16384 | Not used: the UI and recording live on it (video recording, for one, fails with "REC heap(cache) not enough" when it is short) | code |
| `scheap_phyalloc` (uncached) | One heap for all CPUs, under a hardware semaphore, uncached addresses. Classes 178 x 128, 21 x 2048, 43 x 16384 bytes; 64 bytes of overhead per block | Optional reserve for module data (section 3) | code |
| Shared 128-byte blocks | A third small allocator for 128-byte blocks shared between CPUs | Not used | code |
| Kernel memory pools | The kernel supports fixed and variable pools, but the firmware has no wrappers for them | Not used | code |
| Large buffers | Image buffers and other big areas are at fixed addresses, not from a heap; the image pipeline has its own pool (section 4) | Never touched | code |
| Module loader | None (no `dlopen`, no relocatable images) | OMKit has its own | code |

Free space in the CPU 0 cached heap a few seconds after start, the same in every power-on measured: about 1890 of 3013
small blocks, 294 of 430 medium, 36 of 57 large (about 1.4 MiB in pieces of at most 16 KiB) (camera). During bursts,
video and Wi-Fi it was not measured.

## 2. The OMKit arena

OMKit needs a few MiB in one piece for `core`, the modules and their data. The firmware's heaps cannot give that (16 KiB at
most per block). The arena is a **gap in DRAM that the firmware reserves but does not use**: the slot for the first record
of the resources component has a fixed size, and the record in the current firmware does not fill it.

| Evidence that the gap is free | Basis |
|---|---|
| No literal in the firmware code points into the gap (in either address view) | code |
| The resource loader only writes the record's own length into the slot | code |
| An emulator run with shooting, menus, playback: no access to the gap at all | emulator |
| **On the camera**: 3.06 MiB filled with a pattern and compared word by word 76 times during a session with shooting: 0 differences | camera |

Not covered by that test: Pro Capture, Live ND, high-resolution shot, focus bracketing, Wi-Fi, USB, HDMI, sleep. A
device that writes memory directly (DMA, image engines) is invisible to static analysis, so "free" is strong evidence, not
proof. If a future firmware puts more resources into that record, the gap shrinks; the driver of that firmware version
must check it again.

How OMKit lays it out (firmware 1.1 addresses in [fw1100/system.md](fw1100/system.md#1-memory-layout)):

| Part | Address view | Notes |
|---|---|---|
| Loader buffer | uncached | `CORE.BIN` is read here by the loader |
| `core` image | executes through the **cached** address | Code, data, BSS, then its 16 KiB stack. A linker check stops the build if the image with stack passes its ceiling |
| Arena | **uncached** | Starts after the end of the `core` image with its stack, rounded up to 4 KiB; never below its fixed floor; ends below the end of the gap |
| Module image (code, constants, `data`, `bss`) | loaded through the uncached alias; executed, and its variables read and written, through the **cached** address on CPU 0 | In the arena, in whole 64-byte cache lines of its own. Never at an address that held code which already ran in this power-on |

The 4 KiB separation between the cached `core` image and the uncached arena is not cosmetic: a build whose stack touched
the first module (shared cache lines) corrupted it and caused a reboot loop on the camera.

## 3. The `core` allocator

- **First fit** over the arena; neighbouring free blocks are merged on free. Every block has a 16-byte header `{size, size
  of the previous block, owner, flags, magic 'OMKB'}`; data are 16-byte aligned. The region ends with a stub header.
- **Module images get whole cache lines**: the data of an image block start on a 64-byte boundary and its size is
  rounded up to 64, so the block's header lies in the line before and the next block's header in the line after. The
  piece skipped in front (80 to 112 bytes when the free block does not happen to fit) stays a free block. Every other
  block of the arena is used past the cache; the image's variables are not (section 2).
- **Owners**: `core` and each module are separate owners with optional limits; usage, peak, allocations and failures are
  counted per owner and logged (`memory: ...` lines at debug level, a warning when the arena is 90% full and on the first
  failure of an owner).
- **CPU 0 only.** The allocator's lock is the firmware lock, which works on its own CPU; calls from tasks of CPUs 1 and 2 are
  refused and logged.
- **Firmware heap as a reserve** (off by default): with `fw_heap = on` and `fw_heap_limit = <size>` in `[core]`, an
  allocation that does not fit in the arena may take a block of the uncached `scheap_phyalloc` heap, never more than the
  limit in total, and only while more than a quarter of that class stays free for the firmware. Module images never come
  from there. Blocks have the same header, so free works the same way; blocks still held when a module is unloaded are
  reported.
- A free of a damaged or foreign block is refused and logged; the block is kept rather than corrupting the chain.

## 4. The image allocator (for awareness)

The firmware's image pipeline has its own pool on the windowed DRAM (`0xb0000000` view), about 31 MiB, created at startup.
Its **layout mode** is chosen on every picture mode change (not per frame): ART filters and monochrome looks get mode 1,
which reserves nine blocks for their extra passes; the ordinary picture modes get mode 2. The mode word is readable
(address in [fw1100/system.md](fw1100/system.md#1-memory-layout)) (code, emulator).

This matters to any module that changes how a still is processed: a still that needs the extra buffers but is shot in
mode 2 hung the recording task on the camera (the camera kept running, the picture was never written) (camera). OMKit's
driver can switch the layout through the firmware's own picture-mode rebuild (`drv_alloc_large`) and checks the mode
before it changes a still. Details belong to the imaging side ([still-pipeline.md](still-pipeline.md)).

## 5. What is safe to allocate

| Need | Where | Why |
|---|---|---|
| Module code, tables, buffers | The arena (`api->alloc`) | Checked free on the camera; private to OMKit |
| Data shared with hooks that may run on CPUs 1 and 2 | The arena (`api->alloc`; it is uncached). Not a module's static variables: they are in CPU 0's cache | No cache coherency between CPUs |
| A few small blocks when the arena is full | Firmware heap reserve, only if the user turned it on | Shares memory with the firmware; capped |
| Anything larger than 16 KiB from the firmware | Nothing | The firmware heaps do not have it |
| Memory "nobody seems to use" elsewhere in DRAM | Never without a pattern test on the camera | An area that did not change in a test can still belong to a mode that was not exercised |

## 6. Rules for core and modules

1. **Modules never write firmware memory.** Only `core` does, through driver functions, after checking the bytes it
   replaces.
2. **Writes into firmware code or data in the protected range go through the uncached alias** (`address + 0x40000000`);
   through the cached address they cause an NMI and a restart ([hardware.md](hardware.md#5-memory-protection-dbgsup)).
   Every new place `core` writes is checked against the range.
3. **After such a write, the writing CPU may still hold the old line.** Either the code never reads that word through the
   cache again, or it invalidates the line (`drv_dcache_inv`, which only drops lines of the protected range: they can never
   be dirty there, so nothing is lost).
4. **What hooks read or write lives in the arena or is reached through the uncached alias.** Ordinary (cached) variables of
   `core` are not touched from hooks that can run on CPUs 1 and 2. The OMKit driver keeps per-CPU counters for such hooks
   in their own 64-byte block used only through the uncached alias.
5. **A hook patch is 5 bytes** (`jmp` to a trampoline), does not cross a 16-byte line, and the instructions it displaces must
   not depend on their address (no branches, calls, PC-relative forms) and must have no branch into them. The first byte is
   written last, under the firmware lock, so CPU 0 never runs a half-written patch. CPUs 1 and 2 are not stopped: a site
   that they run at that moment is a known residual risk.
6. **The `core` image and stack (cached) and the arena (uncached) never share memory**, see section 2.
7. **Do not write into the firmware's display or image buffers** while the firmware may be rebuilding them. Drawing text
   straight into on-screen plane buffers near a change of output (screen <-> viewfinder) or of the shown picture led to UI
   hangs some seconds later on the camera; OMKit no longer does it (camera).
8. **Read firmware words the way the firmware does**: words the firmware keeps cached on CPU 0 are read through the cache
   from CPU 0; words it keeps uncached (for example the key input bytes) through the uncached alias. One word is read
   past the cache everywhere although the firmware writes it through CPU 0's cache: the GUI state (group and state
   bytes), because most of its readers are hooks that must not bring firmware data into another CPU's cache (rule 4).
   Such a read can be a moment old; on the camera the screens were always told apart this way.
9. **Reading outside memory restarts the camera.** Check every pointer taken from firmware data before following it.
