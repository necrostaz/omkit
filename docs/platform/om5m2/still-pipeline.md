# Still development pipeline

How the OM-5 Mark II turns a shot into a JPEG and an ORF: the image processing jobs, the planner that gives a job its
list of steps, the hardware passes of those steps, the output step, previews, EXIF, and the image memory each route
needs. It also shows where the film look's stills (FILMLOOK, core grain) enter this pipeline.

Related pages:
- [color-pipeline.md](color-pipeline.md) - the colour structure of a pass and the tables OMKit replaces;
- [grain.md](grain.md) - the grain itself (texture, blend, gains, tone mask, chroma grain);
- [picture-modes.md](picture-modes.md) - picture modes, job keys of the profiles and the look's slot;
- [fw1100/imaging.md](fw1100/imaging.md) - firmware 1.1 addresses of every function and field named here by role;
- [../../formats/isp.md](../../formats/isp.md), [../../formats/grn.md](../../formats/grn.md) - the preset and tile files.

Basis tags: **(static)** firmware code, static analysis; **(emulator)** run in an emulator or a pass-level emulator
bench ([README.md](README.md#basis-labels)); **(camera)** measured on the camera; **(assumption)** not proven; **(OMKit code)** what the OMKit sources do.

## 1. Overview

```
sensor -> front end (AE/AWB/AF statistics) -> Bayer frame in DRAM -> record task chain
  |- ORF: Bayer data compressed -> file (+ previews from a development job)
  '- imgpro development job: planner -> steps -> passes (DRAM -> ISP -> DRAM)
       -> YCbCr 4:2:2 frame -> JPEG encoder (main image, previews, thumbnail) -> file
live view / EVF / HDMI: the movie task builds a request per run -> the same job creator -> imgpro -> display
playback: JPEG decode -> YC -> resize -> JPEG encode; in-camera RAW development: the same imgpro jobs
```

- The record task chain names its events Bayer2Ycc (development), Ycc2Jpeg and Ycc2View; one Ycc2Jpeg run encodes the
  main image, the thumbnail, the playback picture and the view pictures (static). There is no HEIF encoder (static).
- The YC frame is 4:2:2, 8 bits, 2 bytes a pixel (a row of 5184 pixels is 10368 bytes) (static). The still may be
  developed in bands (a descriptor of a third of the frame was seen) (assumption).
- The image processor (imgpro) has two register pipes, A and B; a pass's request names its pipe (static). Pipe A full,
  pipe B reduced (assumption).

| Engine / CPU | Runs | Basis |
|---|---|---|
| CPU 0 | record task chain, shooting parameter reader, creation of still jobs (planner, setup gate, grain texture build) | camera (hook census), static |
| CPU 1 | imgpro worker task, codecs | assumption |
| CPU 2 | movie task: live view requests (about 45 a second), live view jobs | camera (hook census: about 15 500 selector calls on CPU 2 against 62 on CPU 0 in one session) |
| imgpro hardware | every pass of every job; the CPU does no per-pixel work in any route, ART filters included | static |
| any CPU | job steps and pass setup (colour writer, blend setup) run in the firmware's job tasks on any CPU | camera (OMKit hooks fire on all three) |

## 2. The job

### 2.1. Request and job

The record chain prepares a 32-byte job request (P) from the record request (C) and the shooting parameter block (S);
the job creator allocates a job (15 484 bytes, from the firmware's small heap) that holds P, the step table, the per-pass
request B (1884 bytes), buffer slots (48) and the effect resources (static). Field offsets: fw1100/imaging.md.

| P field | Meaning | Basis |
|---|---|---|
| key | the job code, built from S: picture mode (raw setting 111), scene, ART type. Natural 2, colour profile 11, monochrome profile 12, ART filters `0x01VV00CC` (VV the variant I/II, CC the ART code: Soft Focus `0x01010065`, Grainy Film `0x01010068` / `0x01020068`) | static, camera (keys logged) |
| kind of job (P+8) | 0 or 2 ordinary, 1 ART, others refused by the planner selector | static |
| effect flags | from the ART effect index of S: Star Light 0x40, Soft Focus 0x80, Diorama 0x100 / 0x200, Pin Hole 0x400, Shade 0x800 / 0x1000, White Edge 0x2000, Frame 0x8000; 0x4000 and 0x10000 always cleared. The creator adds 0x800000 and 0x80000 for key 12 only (0x80000 not for type 0x4x) | static |
| type | 0x1000 a still (JPEG and the second job of RAW+JPEG, RAW Edit); 0x0110 / 0x0120 for record modes 0x2001 / 0x2006; 0x0030 photo live view; 0x0130 live view of the monochrome profile; 0x2005 video; 0x0035 seen with Rec View and video; 0x0000 High Res Shot | static, camera (census) |
| ImgAspect (P+18, P+19) | the frame's aspect code; P+19 is forced to 1 for RAW Edit. Seen: JPEG job 2/2 (3:2), the second RAW+JPEG job 1/1 | static, camera |
| P+21, P+22, P+24, P+27 | P+21 = 2 selects another Grainy Film list; P+22 not 0 or 4: multi-frame jobs (their own planner); P+27: another planner | static |
| class | derived from the type: classes 4..9 behave as live view (no mask pass, reduced ART chains) | static |

### 2.2. The planner selector

At creation the job goes through the planner selector, then the setup gate; at deletion through the cleanup gate (static):

1. P+22 not 0 or 4 - the multi-frame planner; else P+27 not 0 - another planner;
2. kind 1 (ART) - the ART planner (the ART list builder);
3. kind 0 or 2: key 12 - the ART planner as well; any other key - the ordinary planner;
4. other kinds - error, no planner;
5. after a successful planner, if P+19 is not 1 and (the job is not of a live view class or P+24 = 1), a tail step is
   appended. It does not produce the frame (static); a crop to the aspect ratio (assumption).

The selector does not read the effect flags, and the ART builder does not check the key for them: a flag such as Soft
Focus on Natural works only if the job reaches the ART builder (static). The ART builder reads the job key itself at
several places (a table of 32 key handlers, a second table for the still lists) (static).

The setup gate (only for ART and key 12) builds the effect resources: the grain texture for the Grainy Film keys, maps
and frames for the flags 0x400, 0x1800, 0x2000 or 0x80000, 0x8000, 0x4000, 0x20000; the cleanup gate releases them. For
Natural (kind 0/2, key not 12) both gates do nothing (static).

### 2.3. Steps

- A step is `{function, callback, context}`, 12 bytes; at most 16 steps a job, with a step count and a current-step
  index; an executor runs them in order (static). The creator zeroes the table, the ordinary planner writes only the
  functions (static).
- A step builds one or more passes. Its development node builds the pass request B and, if the step has a callback,
  calls it with (job, B) before the pass: callbacks set the stage and attach extra inputs (static).
- Job time from creation to cleanup: 0.72-0.90 s for a still job of the look with grain, either route, also in a
  6-frame series (camera).

## 3. Passes

### 3.1. The pass request B

| Field | Meaning | Basis |
|---|---|---|
| stage | 0 unless a step's callback sets it (table below) | static, camera |
| type | the job type, copied from P | static |
| kind | 0 development; 3 a pass from a buffer to the job's outputs (or to another buffer); 4, 6 other builders | static |
| output descriptor | a copy of the job's output descriptor: address, format, sizes. The format's high byte is its class: still 0x0107, live view 0x0101 (class 1) | static, camera |
| effect code | selects a blend case when the job key has none (below 0x104: none) | static |
| second / third input | descriptors of blend inputs B and C (texture, mask plane) | static |

The pass context carries B, the register image slot, the output descriptor and the job key; the key comes from P, not
from the job's key field (static). Pass setup, in order (static): the blend case selector (by job key and stage, else
by effect code; see [grain.md](grain.md) for the cases), the blend block's register image, input enabling (accepts
output classes 1, 3, 4, else error); the detail block check; then the colour structure's writer and the register
dispatcher ([color-pipeline.md](color-pipeline.md)).

**Detail block** (sharpening and noise filter, block 0x1800): set up for an output of class 1 or 2, except at the
stages 0x9000, 0xA000, 0xA200, 0x100, and except Grainy Film jobs of type 0x1xx; stage 0xB02 always gets it (static).
So the stock Grainy Film development (stage 0x100) runs without sharpening and noise filtering (camera: visibly noisier
than Natural).

| Stage | Pass | Basis |
|---|---|---|
| 0 | development of Natural, colour profile, the one-pass ART filters; live view of Natural | static, camera |
| 0x100 | Grainy Film development, texture on input B (blend case 1) | static, camera |
| 0x101 | Grainy Film: buffer to the job's outputs (kind 3) | static, emulator |
| 0x900 | monochrome profile: mask development into a 1-byte plane | static, camera |
| 0x901 | monochrome profile: development with the mask plane as input C (case 31) | static, camera |
| 0x902 | monochrome profile in live view: development with the block's noise generator (case 32) | static, camera |
| 0xF005 | monochrome profile: buffer to the job's outputs with the shading callback | camera (census) |
| 0x9000, 0xA000 | service passes of key 2 (pipe B) | static |
| 0xA01, 0xA02, 0xB01..0xB04, 0x504, 0x600, 0x602 | other ART filters' passes | static |

| Format | Pixel | Used for | Basis |
|---|---|---|---|
| 0x302 | 2 bytes, YC 4:2:2 | developed frames, the grain texture | static |
| 0x304 | 1 byte | the grain tile, the mask plane | static |
| 0x305 | 4 bytes | Soft Focus's small frame | static |

| Buffer id | Content | Basis |
|---|---|---|
| 0x1000 | developed frame (format 0x302) of the multi-pass lists | static |
| 0x1010 | Grainy Film's developed frame before its output pass | static, emulator |
| 0x1060 | Soft Focus's development output | static |
| 0x1070 | Diorama's full-size working frame | static |
| 0x10A0 | the monochrome mask plane (format 0x304, aligned to 64) | static |

## 4. Step lists

### 4.1. Stock lists

| Job | Steps (in order) | Basis |
|---|---|---|
| Natural, colour profile (ordinary planner) | two preparation steps, development straight into the job's outputs, tail | static, camera |
| Monochrome profile, still | ART builder's first step, mask development 0x900, development 0x901, output step (stage 0xF005, from flag 0x80000), tail | static, emulator, camera |
| Monochrome profile, live view classes | one development (stage 0x902), no mask pass | static, camera |
| Monochrome profile, type 0x1x / 0x2x | development only | static |
| Grainy Film, still | first step, seed (0 for a still), development 0x100 into 0x1010 with the texture callback, pass 0x101 from 0x1010 to the outputs, tail | static, emulator |
| Grainy Film, P+21 = 2 | development without callback, two more steps; the texture is blended on a kind 3 pass | static |
| Grainy Film, photo live view | development without texture (no grain on the screen) | static, camera |
| Soft Focus, still | first step, ART development (output 0x1060), blur step, final step | static, emulator |
| Soft Focus, live view classes | one step of two passes | static |

The ART builder's own lists for one key exist twice (two key tables): the still list of Grainy Film above is the one
the builder produced on a key-2 job in the emulator; the handler list ends in a pass 0x1010 -> new 0x1000 instead
(static, emulator).

### 4.2. Development passes 0x900 and 0x901

The monochrome profile's still is developed twice (static; camera census shows 0x900 -> 0x901 -> 0xF005):

1. **0x900, the mask development.** A full development whose tone curve is replaced, for key 12 at this stage only, by
   the firmware's luminance mask curve (0 below node 36 of 256, 1023 from node 120). Output: a 1-byte plane 0x10A0, the
   mask frame (5184 x 3888, about 20 MB). The curve is applied to luminance before the look's tone.
2. **0x901, the development.** The real development into 0x1000 (format 0x302). Its callback puts the 0x10A0 plane's
   descriptor into input C of B. Blend case 31: the block's noise generator on Y, weighted by the mask, gain from the
   grain level (110, 300, 600). Input C is stretched (Q12) if its size differs; its format is not checked (static).
3. **Output step.** Moves 0x1000 to the job's outputs through the common transfer helper, with a shading callback that
   reads the shading map the key-12 setup builds. The planner adds this step only for job flag 0x80000, which the
   creator gives only to key 12 (static, emulator).

The mask is per pixel and has the job's own size and crop; the detail block runs normally on both developments (they
are not in its exception list) (static).

### 4.3. Grainy Film

- The setup gate builds the texture: a buffer for the tile, the tile read from the flash resources (1536 x 1536, 1 byte),
  4 passes of the texture node, then DMA row replication to the full frame (5184 x 3904 x 2 bytes, 40.5 MB); the tile
  buffer is freed before the replication (static). A cache of 4 records shares a texture between jobs with the same
  parameters while one of them holds it (RAW+JPEG, a series) (static, camera). Details: [grain.md](grain.md).
- The development's callback attaches the texture as input B and sets stage 0x100; case 1 blends it, with zero chroma
  gain on input A (why the stock filter is black and white) (static).
- The pass 0x101 (kind 3) writes 0x1010 to the job's outputs; no blend there for key 2 (static).
- The still has no seed shift (the pattern is the same every shot); live view and video shift every frame (static).

### 4.4. Other ART filters (brief)

| Filter | Chain | Basis |
|---|---|---|
| Pop Art, Pale & Light Color, Light Tone, Cross Process, Partial Color, Vintage III | one development pass; the look is in the tables (own tone curve, sectors, often own gamma per channel) | static, emulator |
| Soft Focus | development, then the resample node: shrink to a small frame (160 x 120, 144 x 108 or 144 x 80 by class), enlarge, blend with the sharp frame at fixed 600 : 424 of 1024 (or enlarge and blend in one pass). No highlight threshold: the blend is in YC after the tone | static, emulator |
| Diorama | two developments, 3-4 passes; blur by decimated reading (1/2, 1/4) blended by a one-dimensional mask; live view without the middle pass | static, emulator |
| Pin Hole, Shade, Frame, White Edge, profile shading | a map image on a second input, blended in the same development pass | static |
| Dramatic Tone, Key Line, Watercolor, Bleach Bypass, Instant | 2-5 passes; spatial parts not analysed | static |
| Star Light | small development and resample-node passes; dropped in live view | static |

Soft Focus costs 3 extra full-frame passes and up to about 80 MB of buffers per frame (static). In live view the ART
chains are reduced and lower the frame rate (static).

## 5. Output, previews, RAW+JPEG

- **Where the frame leaves the job.** The ordinary list develops straight into the job's outputs. Multi-pass lists
  develop into a working buffer (0x1000, 0x1010) and end with a pass that moves it to the outputs through a common
  transfer helper; that helper takes an intermediate 0x302 buffer of about 40 MB (static). A job whose list lacks this
  step ends with no picture and no previews: the camera writes a JPEG of the header alone (camera).
- **JPEG.** The record chain encodes the job's outputs: for a 3:2 still, the main image 5184 x 3456 and previews of
  1008 x 672 and 160 x 120 in the JPEG (camera). The previews come from the same job as the main image (camera: a job
  without output step left neither).
- **RAW+JPEG.** One press makes two still jobs of type 0x1000, one after the other: the JPEG job (FileType 1) and a
  second job (FileType 3, ImgAspect 1/1) (camera). The ORF carries previews of 160 x 120 and 3200 x 2400 (camera); they
  come from the second job (assumption, from the files of a route that lost its output step). The same key and list
  apply to both: the stock Grainy Film gives both jobs its list (static, camera with the look). Besides the two still
  jobs, the colour service sees more key-2 jobs per press (previews, review) (camera).
- **Series.** Every frame of Sequential L is its own job (camera: 6 frames in about 2 s, 12 jobs with RAW+JPEG).
- **RAW Edit** in the camera: the record request 0x1003 runs the same development job (Bayer2Ycc) with P+19 = 1, so no
  tail step; it does not change the allocator mode (static). The job key follows the picture mode chosen in the edit;
  another request 0x1004 is also seen (camera).
- **Rec View, video, High Res Shot** have their own job types (0x0035, 0x2005, 0x0000) (camera census).

## 6. EXIF

- The JPEG and ORF headers are built by one header builder that copies the first 4472 bytes of S - a TIFF/EXIF
  template the firmware fills at start-up - into the file (static, camera).
- **UserComment** is a 125-byte field of that template: an 8-byte character code, then 117 characters, which the
  shooting parameter reader fills with spaces for every shot. DateTimeOriginal follows directly, so never more than
  117 characters (static, camera).
- **What OMKit writes.** The profile / look shooting hook (in the shooting parameter reader, CPU 0) copies a module's
  text into every S: the code `ASCII` and three zero bytes, then the text padded with spaces (printable ASCII, cut to
  117); if the text is rewritten during the copy, the camera's spaces stay. Both files of a press get it; in-camera RAW
  development uses its own template copy and gets none (OMKit code; camera). The text comes from the SDK call
  `exif_comment` ([../../sdk.md](../../sdk.md)), available when the profile or look service is in place.
- **FILMLOOK** writes it only with `exif = on` (default off):
  `FILMLOOK <file> CRC <crc32> [E [t=sh,mid,hi] [v=12 shifts] [l=filter,level] [g=tile,strength]] [F <matrix rows>] '<title>'`,
  cut to 117 characters with `~` at the end (OMKit code). `F` is the look's matrix (Q7) from the preset; the frame's
  final matrix (look matrix x the camera's matrix for the light) is not known when the reader runs, core logs it.
- **MakerNote.** A look's still is a Natural still: PictureMode 2, effect 0, in any slot. The ColorMatrix tag is twice
  the camera's own matrix of the frame, without the look's matrix (camera).

## 7. Image memory

### 7.1. The allocator

- The CPU heaps give no large memory (largest small-heap block 16 KiB); all frame buffers come from the image buffer
  allocator (27 heaps) and the image buffer registry (static).
- **The wait flag.** Without its no-wait flag the buffer allocator waits for free memory forever (a sleep loop). The
  job buffers are taken that way, at record event 19 (static). A route that needs a buffer the current layout cannot
  give hangs the record task: the write never ends and the camera cannot power off (camera).
- **Modes.** After every picture mode rebuild the firmware reads the raw picture mode (setting 111) from storage and
  sets the allocator mode: 1 for ART filters (raw 100 and up), raw 8 and the monochrome profile (12); otherwise, by other
  settings, usually 2 (Natural, the other stock modes, the colour profile 11) (static, emulator). The firmware's strings
  name modes 0, 1, 2 NORMAL, ART, HIGH_ISO (static). The mode sets how many blocks the firmware reserves in its image
  pool (about 31 MiB beyond the CPU's direct window; mode 1: 9 blocks, mode 2: 2) and which heap table the allocator
  uses (static). Block sizes per mode: not established.
- The mode is chosen at a rebuild (mode change, power-on), not per shot (emulator: a shot does not call the choice).
  In-camera RAW processing sets its own mode for some requests (Image Overlay, RAW to YC), not for RAW Edit (static).

### 7.2. Memory per route

Peak firmware memory of one still job, frame 5184 x 3888 (static, buffer descriptors; the totals are not measured):

| Route | Buffers | About | Mode | Fits |
|---|---|---|---|---|
| Natural, colour profile | development straight into the outputs | - | 2 | yes (camera) |
| Monochrome profile | mask plane 0x10A0 20 MB, frame 0x1000 40 MB, output transfer 40 MB | 100 MB | 1 | yes in mode 1; in mode 2 the record task waited forever (camera) |
| Grainy Film (stock or the look's GF route) | frame 0x1010 40 MB, output transfer 40 MB, texture 40.5 MB (+ tile buffer 2.36 MB while it is built) | 121-123 MB | 1 | yes (camera) |
| The look's mask route | mask plane 20 MB, frame 0x1000 40 MB, output transfer 40 MB, texture 40.5 MB | 141 MB | 1 | yes: single shots and a 6-frame RAW+JPEG series, no memory stalls (camera) |
| Soft Focus | small frame (77 KB) and full-frame working buffers | up to 80 MB | 1 | yes (stock) |

The texture is shared by the jobs of one press while alive; at most 4 textures exist (the texture cache's 4 records);
a fifth job gets none (static).

### 7.3. What OMKit does

- The allocator's setup reads the raw picture mode at one place; OMKit filters that value (OMKit code):
  - the monochrome profile, held only in OMKit's cells, is reported as 12 (the camera's storage holds Natural), so its
    shots get mode 1 as with the stock profile;
  - while the look is on with a grain strength other than Off, any raw value that would give mode 2 is reported as 12:
    mode 1, the layout of the ART filters. Nothing else reads the value after that place (static).
- A change of that need asks the GUI task for a picture mode rebuild: at its next read of setting 111 or after a UI
  write, the firmware's own setting-111 callback runs, which rebuilds the picture mode and the allocator mode (OMKit
  code; emulator: mode 1 right after power-on with the look and a strength restored).
- A still taken while the mode is not 1 gets no grain (log: "allocator mode"); a monochrome profile shot is then made
  as Natural rather than risk the hang (OMKit code).

## 8. The look's stills in this pipeline

A still of the film look is a Natural still (key 2, type 0x1000, format 0x0107) in any slot (camera: job census and
MakerNote); its colour comes from the ISP tables ([color-pipeline.md](color-pipeline.md)). With grain on, OMKit gives
the job a different list at the planner selector. The grain values themselves: [grain.md](grain.md).

| Role (hook) | Where | What OMKit does | Basis |
|---|---|---|---|
| list (W) | planner selector, where it reads the job key | for a still of the look: builds the texture with the firmware's builder (own marker in a parameter byte the sizes do not read), sets a stock list key for the time of the ART planner, puts the job key back, checks the list | OMKit code, camera |
| tile fill (H2) | texture node builder, after the flash tile was read | writes the chosen tile into the tile buffer (repeated to 1536 x 1536, random cyclic shift) before the node's DMA reads it | OMKit code, camera |
| release (H3) | the job's cleanup gate | releases the texture | OMKit code, camera |
| case (S) | blend case selector | case 1 for the passes of OMKit's jobs at the route's stage | OMKit code, camera |
| blend image (A) | after the pass's blend setup | colour kept, mean offset and gain, mask weight on the mask route | OMKit code, camera |
| detail (D) | detail block check | GF route: the development gets the detail block as at stage 0 | OMKit code, camera |
| mask tone | colour structure writer | at stage 0x900 of the look's jobs, the mask curve replaces the tone | OMKit code, camera |

**Routes** (`[grain] mask` in `OMKIT.INI`; default `film`) (OMKit code):

| Route | Key during the planner | List | Blend |
|---|---|---|---|
| GF (`mask = off`) | Grainy Film II | first step, seed, development 0x100 into 0x1010, pass 0x101 to the outputs, tail | case 1 on the 0x100 development: texture, one amplitude at every tone |
| mask (`film`, `flat`, `step`) | 12 | first step, 0x900 into 0x10A0, 0x901 into 0x1000, **output step added by OMKit**, tail | case 1 on the 0x901 development: texture weighted by the mask plane (input C) |
| list (test) | 12 | as mask | none: the same frame without grain |

- **The added output step.** A key-2 job does not get flag 0x80000, so the planner leaves the mask list without its
  output step and the frame stays in 0x1000. After the planner OMKit appends the firmware's plain transfer of 0x1000 to
  the outputs (its callback does nothing) after the 0x901 step and before the tail. The stock key-12 output step is not
  used: its shading callback reads a map that only the key-12 setup builds (static, emulator, camera).
- **Which jobs.** Every still job of type 0x1000 with the look's key, as the stock Grainy Film does: JPEG, both RAW+JPEG
  jobs, every frame of a series, RAW Edit with the look. Not taken (stock list, no grain, a log line): P+21 = 2,
  multi-frame jobs, a job not created on CPU 0, allocator mode not 1, 4 textures alive, the tile still loading. Video,
  live view, Rec View and High Res Shot have other types (OMKit code, camera).
- **Measured** (camera): GF route at strength 0 equals a Natural still in brightness, colour and noise (with D); the
  mask route matches the stock still within 0.6 of a level in shadows and highlights (midtones and sharpness not
  compared); job time GF 0.72 / 0.83 s, mask 0.80-0.90 s (RAW / JPEG job).

## 9. Live view and still

| | Live view | Still |
|---|---|---|
| Request | built by the movie task on CPU 2 every run, same job creator | prepared by the record chain on CPU 0 |
| Type, format | 0x0030 (0x0130 monochrome profile), 0x0101 | 0x1000, 0x0107 |
| Effect flags | cleared, then set from the ART effect index only for picture modes from 100 | from the ART effect index |
| Natural / look | one development, stage 0 | ordinary list (or OMKit's grain list) |
| Monochrome profile grain | generator, case 32, stage 0x902, gains 50 / 75 / 100 | mask route, case 31, gains 110 / 300 / 600 |
| Grainy Film | no texture in photo live view; texture in video (pre-created with the live view resources, shifted every frame) | texture rebuilt every still |
| ART chains | reduced (Soft Focus one step, Diorama without its middle pass, Star Light dropped) | full |
| Colour tables | the look's ISP tables (OMKit, scope `both` or `live`) | the same (scope `both` or `stills`) |
| OMKit grain | none | as section 8 |

Basis: static and camera census. Whether a live view job is created per frame or once is not established (static).

## 10. Open points

- The two preparation steps of the ordinary list and the tail step's exact job.
- Block sizes of the allocator layouts; why mode 2 lacks the buffer the monochrome list waits for.
- Which job writes which ORF preview (assumed: the second RAW+JPEG job).
- Pipes A and B: which passes run on which.
