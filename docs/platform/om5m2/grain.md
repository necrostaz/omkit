# Film grain

How the camera's firmware makes grain, and how OMKit puts film grain from its own tiles on the stills of the film look.
Firmware addresses of every function, hook site and table named here by role: [fw1100/imaging.md](fw1100/imaging.md).
The tile file format: [../../formats/grn.md](../../formats/grn.md). Development jobs, passes, work lists and the image
memory allocator: [still-pipeline.md](still-pipeline.md). The look itself: [picture-modes.md](picture-modes.md); its
colour tables and tone: [color-pipeline.md](color-pipeline.md); its editor column: [ui.md](ui.md).

Basis tags: "(firmware code)" - static analysis of the firmware's code and data; "(emulator)" - the firmware's own code run in the
emulator; "(measured on the camera)"; "(assumption)".

## 1. Summary

| What | How | Basis |
|---|---|---|
| Grain source | the firmware's Grainy Film texture: a 1536 x 1536 tile, replicated over the frame; OMKit refills the tile buffer with its own tile, cyclically shifted per still | firmware code; measured on the camera |
| Blend | the blend block `0x0e00`, case 1 (texture into input B), with OMKit's values: colour kept, tile mean removed, gain per strength | firmware code; measured on the camera |
| Route (default) | `mask = film`: the monochrome profile's work list (mask pass `0x900`, development `0x901`), the texture weighted by a tone mask | measured on the camera |
| Route (fallback) | `mask = off`: the Grainy Film work list, one amplitude in every tone | measured on the camera |
| Strength | Off / Low / Med / High per preset; gains per tile up to 2047; Med = the film's grain in the mid tones | measured on the camera |
| Colour grain | optional, the blend's noise generator in Cb and Cr (`chroma` in `[grain]`, default 0); never on black-and-white presets | measured on the camera |
| Where | stills of the active film look only; no grain in live view or video | firmware code; a design decision |
| Control | FILMLOOK's "Film Grain" item: tile (rear dial) and strength (front dial), stored per preset; core's grain service (API 17) | code |

## 2. The firmware's grain

The firmware has two unrelated grain mechanisms: the Grainy Film ART filter (a texture) and the grain of the monochrome
picture profile (a noise generator with a tone mask). OMKit combines a part of each.

### 2.1. The Grainy Film texture

- **The tile.** A flash resource (resource group `0x8001`, element 0): 1536 x 1536 bytes, values 0..63, mean 25.7,
  deviation 19.1, white noise (neighbour correlation below 0.002) (firmware code). The
  high-frequency luma of stock Grainy Film stills of a flat field matches this tile with correlation 0.994 (measured on
  the camera).
- **The texture.** Built anew for every still job by the texture builder: a buffer for the tile is allocated, the tile is
  read from flash into it, the texture node (`0xF002`) converts it into a 2-byte-per-pixel texture the width of the frame
  in 1536-wide column passes, then DMA copies replicate the rows to the full height (5184 x 3904 for a full-size still,
  about 40.5 MB) and the tile buffer is freed (firmware code). On the frame, `T[y][x] = tile[y mod 1536][x mod 1536]`,
  one tile pixel per frame pixel; input B is not scaled for case 1 (firmware code; measured on the camera).
- **Parameters.** The builder takes 6 parameter bytes (the job type, two job request bytes, a class byte that is 1 or 3,
  and one more request byte); the texture cache keys on them (firmware code). There is no strength, size or I/II
  parameter: Grainy Film I and II share one texture; their different look comes from a tone table after the blend
  (firmware code).
- **The texture cache.** 4 records; a record lives while a job references it (reference count). Jobs whose parameters
  match share a live texture; with 4 records in use the builder returns none and the still has no grain (firmware code).
- **Shift.** For a still the seed step writes a shift of 0 and the shift mask is 0: the stock pattern is the same on every
  still (firmware code; measured on the camera). Live view and video textures are 256 columns wider and an xorshift32
  moves their start inside that margin; a still texture has no margin, so its read position cannot be moved (firmware
  code).
- **Attaching.** The Grainy Film development step's callback copies the texture's 16-word descriptor into input B of the
  pass request (plus the shift) and sets the pass stage to `0x100`; for this key and stage the blend-case selector gives
  case 1 (firmware code). The development at stage `0x100` skips the detail block `0x1800` (sharpening, noise filter):
  stock Grainy Film is noisier than Natural (firmware code; measured on the camera).

### 2.2. The blend block `0x0e00`

Every development pass may program the blend block: a selector picks a case (by the job key and the pass stage, or else
by the effect code), the case's handler fills the block's register image, then the input-enable step programs the inputs
from the mode word (firmware code). The block mixes input A (the developed frame), input B (from memory, a constant or the
block's noise generator) and optionally a weight plane, input C.

**Mode word** (image `+4`):

| Bit | Meaning | Basis |
|---|---|---|
| 0 | input B from memory (its descriptor in the pass request) | firmware code |
| 1 | input C from memory (a 1-byte or 16-bit plane; scaled to the output size when it differs) | firmware code |
| 4-5 | input B format (`0x302` 0, `0x305` 1, `0x304` 2, `0x307` 3; another format is an error) | firmware code |
| 12-13 | input C 16-bit | firmware code |
| 20 | blend weighted by input C | firmware code (field), measured on the camera (meaning) |
| 21 | blend by the gains only | firmware code; assumption (meaning) |
| 24 | enabled (set by the input-enable step) | firmware code |

**Select words** (`+88` input A, `+132` input B): 2 bits per channel in each byte (Y bits 0-1, Cb 2-3, Cr 4-5).

| Byte | Value per channel | Basis |
|---|---|---|
| 1 - source | 0 memory, 1 the constant at `+120`, 2 the noise generator | firmware code (all case handlers) |
| 2 - weight | 2 the gain, 0 the gain x C, 1 the gain x (1 - C) | firmware code; gain x C measured on the camera |
| 0 | with the generator: its stream per channel (stock `0x24`: Cb 1, Cr 2) | assumption |

**Other image fields used for grain** (firmware code): `+92` input A gains (low half Y, high half chroma) and `+96` (second
chroma); `+124` input B offset before the gain (low half Y, high half chroma; the generators use -128 for their centre);
`+136` input B gains (low half Y, high half chroma) and `+140` (second chroma); `+172` the generator seed (bit 31 restarts
it); 1024 = gain 1.0. Gains are signed: the stock writes -1024 for one case (firmware code).

**Grain-related cases:**

| Case | Used by | Mode | `+132` | Weights A / B | Basis |
|---|---|---|---|---|---|
| 1 | Grainy Film development, stage `0x100` | `0x00200001` | `0x002A0000` | Y 1024, chroma 0 / Y 1024 | firmware code |
| 31 | monochrome profile still, stage `0x901` | `0x00100002` | `0x00002A00` (generator, x C) | 1 / 110, 300, 600 by level | firmware code; measured on the camera |
| 32 | monochrome profile live view, stage `0x902` | `0x00200000` | `0x002A2A00` | 1 / 50, 75, 100 by level | firmware code; measured on the camera |
| 10, 19 | other effects: two images mixed by a mask | `0x00100003` | memory | A and B mixed by C | firmware code |
| 36 | an ART filter: texture in Y, generator in Cb and Cr | `0x00201021` | `0x002A2824` | 1 / 1024, 24, 24 | firmware code |

Case 1 zeroes the chroma gain of input A, so stock Grainy Film is black and white, and it leaves the input B offset at its
default, so the tile's mean brightens the frame (firmware code). Another stock case that reads the same flash tile (an ART
filter in live view) offsets input B by -26, about the tile mean: the block can remove the mean itself (firmware code).

### 2.3. The monochrome profile's grain

- For a still, the work list of job key 12 always has two passes, whatever the grain level: a **mask development** (stage
  `0x900`) into a full-frame 1-byte plane, then the **development** (stage `0x901`), whose callback puts the plane's
  descriptor into input C (firmware code).
- At stage `0x900` the tone selector gives key 12 a fixed mask curve in place of the tone: u16[256], 0..1023, indexed by
  Y before the tone; 0 below node 36, 1023 from node 120 (firmware code). So the plane holds a weight by brightness.
- Stage `0x901` is case 31: the generator in input B weighted by C, so `Y' = Y + C x g x noise`; no grain in the shadows
  (firmware code; measured on the camera).
- The generator's noise is per pixel, with no grain size (measured on the camera), and luma only in the stock cases 31/32
  (firmware code).

### 2.4. Live view in the firmware

The Grainy Film handler builds its list without a texture for photo live view; only video gets the texture (and a shift
per frame) (firmware code; measured on the camera). The monochrome profile uses the generator (case 32) in live view
(measured on the camera).

## 3. OMKit's grain

Three parts: core's grain service (the tile containers, the slot, the strength, the route and the mask curve), the
platform grain driver (the hooks below) and the ISP driver (the mask curve on the mask pass).

| Hook (role) | Runs | What OMKit does |
|---|---|---|
| W - the planner selector, where it reads the job key | job creator task, CPU 0 | for a still of the look: builds the texture (the firmware's builder, OMKit's marker), swaps the job key for the time of the planner so that the ART planner builds the chosen work list, puts the key back, checks the list |
| H2 - the texture node builder, after the flash tile was read | job task | refills the tile buffer with OMKit's tile, cyclically shifted |
| H3 - the job cleanup gate | job task | releases OMKit's texture |
| S - the blend-case selector | any CPU | case 1 for OMKit's jobs on the route's stage |
| A - after the pass's blend setup | any CPU | OMKit's values in the blend image (section 3.5) |
| D - the detail-block check | any CPU | Grainy Film route: the development gets the detail block as at stage 0 |
| the ISP writer (ISP driver) | any CPU | mask route: the mask curve in place of the tone on the mask pass |

The hooks run in the firmware's job tasks on any CPU: they touch only an arena block (through its uncached address), the
job's own handle and pass data, and their stack (code). Hook mechanics and the memory rules:
[fw1100/system.md](fw1100/system.md).

### 3.1. Which stills get grain

- Stills (job type `0x1000`) with Natural's job key 2 (the look's stills develop with Natural's key, see
  [picture-modes.md](picture-modes.md)), while the look is active and the chosen strength is not Off (code).
- Every such job: JPEG, both jobs of RAW+JPEG (the JPEG and, by assumption, the ORF's embedded preview), each frame of a burst, in-camera
  RAW edit when the look is applied (code; measured on the camera).
- Not: multi-frame jobs (High Res Shot and the like), a job variant whose stock Grainy Film list blends on a later pass,
  jobs created off CPU 0, jobs while the allocator is not in its large layout (code). Live view and video never (type).
- A refused still develops normally, without grain, and the log says why (code).

### 3.2. The tile: slot, generations, fill and shift

- Core holds **one tile slot** in the arena, the size of the largest container (at most 768 x 768 = 576 KiB). The chosen
  tile is loaded 300 ms after the last change of choice (the dial may still be turning), not while the camera writes a
  picture and not while a still's fill reads the slot; the header is checked again (the file must not have changed), and
  the pixels' CRC and largest value (code). Loading takes about 0.2 s for 256 KiB and 0.5 s for 576 KiB (measured on the
  camera).
- A still taken while the slot does not hold the chosen tile has no grain (logged) - no grain rather than the wrong tile
  (code).
- Each load is a new **generation** (16 in turn); the marker in the texture parameters is `0x80 +` the generation, a value
  the firmware never uses there (it writes 1 or 3) and which does not enter the texture size. So the firmware's texture
  cache never gives a job a texture of an earlier tile, and H2 recognises OMKit's textures (code; firmware code).
- **Fill (H2):** the firmware has just read its own tile into the buffer. For OMKit's marker the hook checks the buffer
  descriptor (1-byte format, 1536 x 1536, inside the allocation, in the CPU's direct memory window) and writes
  `side x side` source bytes repeated to 1536 x 1536 through the uncached alias: row `y` is source row `(y + dy) mod side`
  rotated left by `dx` bytes. `dx` (a multiple of 4) and `dy` come from an xorshift32 stirred by the clock (code). The node
  then makes the texture from it as usual. Up to core 1.29 every word of the result was read from the tile and the
  fill took 280-320 ms (measured on the camera). From core 1.30 each source row is read once: on CPU 0, where the
  builder runs, it is put together in a row buffer in cached memory and written to every row of the result that shows
  it; on another CPU the fill reads word by word as before (counted in the log). Same bytes (host test
  `tests/host/test_texfill.c`); expected 140-200 ms, not measured on the camera yet.
- H2 keeps the result of a fill by the firmware's texture record (4 a generation, as the cache has 4 records), and W
  takes the one of the record the builder returned: two textures of one generation alive at once (jobs with different
  parameters) each keep their own (code).
- A tile side must divide 1536 (128..768), so the repeated tile is seamless across the 1536 period and across the frame.
  The shift gives each new texture a different pattern position; jobs that share a live texture through the firmware's
  cache (for example the two jobs of one RAW+JPEG press) share its pattern (code; firmware code).
- A failed check, fill or build: the firmware's own tile with OMKit's marker never reaches a picture - the texture is
  released and the job keeps its own list (code).

### 3.3. Route `GF`: the Grainy Film work list (`mask = off`)

W gives the still the Grainy Film texture and Grainy Film II's key for the time of the planner; the ART planner builds
Grainy Film's still list (seed, development into an intermediate buffer with the texture attached at stage `0x100`, a pass
from that buffer to the job's outputs). The job keeps its own key afterwards, so colour and tone are the look's (code;
emulator; measured on the camera). S gives case 1 at stage `0x100`, A writes OMKit's values, D lets the development set
up the detail block as for a normal still: with gain 0 this route matches the normal development in brightness, colour
and noise (measured on the camera). The grain has one amplitude in every tone.

### 3.4. Route `mask`: the tone mask (`mask = film`, the default)

W gives the still job key 12's list instead (the monochrome profile's): the mask development (stage `0x900`) and the
development (stage `0x901`, input C = the mask plane). The texture is built the same way but is not attached by a step:

- The ISP driver puts OMKit's mask table on the `0x900` pass of key 2 (a stage the stock never gives key 2) in place of
  the look's tone, and puts the tone back on the next pass that uses the same ISP state (code; emulator).
- On the `0x901` pass S gives case 1 and A writes, over case 1's image, mode `0x00100003` (B and C from memory, weighted by
  C), select word 0 (input B from memory, gain x C in Y, Cb, Cr) and the texture's descriptor into input B of the request
  (what the Grainy Film callback would write) (code; emulator).
- The stock monochrome list ends in an internal buffer; the step that copies it to the job's outputs is added by the
  planner only for a job flag that the job creator gives key 12. Without it the camera writes a JPEG of the header only and
  an ORF without previews. W therefore appends the firmware's plain buffer-to-outputs step after the `0x901` development
  (code; measured on the camera). Details: [still-pipeline.md](still-pipeline.md).
- The `0x900`/`0x901` passes get the detail block anyway; D is not needed (firmware code).

Measured on the camera: `flat` (weight 1 everywhere) gives the same grain as the `GF` route in every tone; the weight is
gain x C with the expected sign and scale (`step` and `film` follow their curves); the mask route changes the look's tone by about -0.5 of 8-bit Y in shadows and
highlights and chroma by under 0.05; no softening of edges was seen; job time 0.73-0.9 s, about the same as `GF`.

### 3.5. The blend image OMKit writes (A)

| Field | Stock case 1 | OMKit | Why |
|---|---|---|---|
| `+92`, `+96` input A gains | Y 1024, chroma 0 | `0x04000400`, 1024 | keep the colour |
| `+124` input B offset | `0xFF800000` | Y = -(the tile's integer mean) | the grain does not brighten the frame |
| `+136` input B gains | Y 1024 | Y = the strength's gain; high half = the chroma gain | strength |
| `+4` mode | `0x00200001` | mask route: `0x00100003` | weight by the mask |
| `+132` select B | `0x002A0000` | mask route: 0; with colour grain bytes 0-1 = `0x2824` | mask weight; generator in Cb/Cr |
| `+140`, `+172` | - | with colour grain: the Cr gain; seed `0x80000000 + n` | colour grain |
| input B descriptor | the Grainy Film callback | mask route: the texture's descriptor | no step attaches it there |

The grain response is linear: at gain 1024 one
tile unit adds 1.00 of 8-bit Y in the final picture, the grain lands after the look's tone, and the negative half is not
clipped (measured on the camera).

### 3.6. The tone mask curve

Core defines the weight `w` (0..1023) by the picture's final 8-bit Y; the mask pass indexes its table by Y before the tone,
so the ISP driver builds the table as `mask[i] = w((tone[i] + 2) / 4)` from the look's 10-bit tone (identity when the look
has no tone) and rebuilds it when the tone changes (a ring of 4 tables, at most one new table every 40 ms). The mask route
is given to new stills only once its curve is in the ISP driver; until then they take the `GF` route (code).

| `mask` | `w(Y)` |
|---|---|
| `film` (default) | the relative grain deviation of film by tone: 0.47, 0.90, 1.03, 0.94, 0.56 x 1023 at Y 25, 75, 125, 175, 225 (bands of 50), linear between, constant outside, at most 1023. At Y 0, 64, 128, 192, 255: 481, 824, 1023, 830, 573 |
| `flat` | 1023 everywhere: the same grain as `off`, by the mask route |
| `step` | 0 below Y 128, 1023 from it (a test) |

The curve follows grain measured on scanned 35 mm film. On the camera the
`film` mask gives about 0.5 of the mid-tone grain in the deep shadows and 0.57-0.6 in the highlights, as the curve says;
the peak sits slightly towards darker JPEG tones, because the mask is taken before the tone (measured on the camera).

### 3.7. Colour grain

`chroma` in `[grain]` (0..1024, default 0) adds the blend generator's noise to Cb and Cr of input B: select bytes 0-1 as the
stock case 36 (Y from the texture, Cb and Cr from the generator), the gain in the high half of `+136` and in `+140`, a new
seed every pass (code). On the mask route it is weighted by the mask too. It is per-pixel noise without clumps (firmware
code). Core never gives it to a black-and-white preset: a Cb or Cr gain of 0 in the preset, or matrix rows within 2 of
each other (code; rule: [../../formats/isp.md](../../formats/isp.md)). At 100 the chroma deviation is about 4 levels; on a
black-and-white preset Cb = Cr = 128 everywhere (measured on the camera). There is no menu item for it.

### 3.8. Strength and gains

- The strengths Off, Low, Med, High take the gains of the chosen container (0 for Off). A container either gives its three
  gains (each 1..2047, not falling) or uses the defaults 0.25 / 0.55 / 1.0 x its tile gain (1..1024) (code; format:
  [../../formats/grn.md](../../formats/grn.md)).
- **Gains above 1024 work** up to 2047, the limit core accepts (assumed field width: at least 12 bits signed). Measured on
  the camera on the mask route: at 1536 and 2047 the grain grows x1.5 and x2.0 in the shadows and x1.34 and x1.8 in the
  highlights (the tone shoulder compresses large excursions), with no inversion, no artefacts and no darkening (mean
  shift +0.15 of 8-bit Y); bursts behave like single stills. At 2047 the deepest shadows clip: Y <= 1 in about 1.8 % of
  the pixels (0.01 % at 1024); Y >= 254 in under 0.1 %.
- The bundled tiles set **Med = the film's grain deviation in the mid tones** (with the `film` mask), High = 1.4 x Med,
  Low = 0.6 x Med, never below a visibility floor, steps at least 1.3 apart, gains capped at 2047. Nine of the 11 tiles
  have gains above 1024; every tile reaches these values except the coarsest film (`CINE500`), which stops at High 2047
  (0.60 / 0.83 / 1.08 of the film). The per-tile table: [../../formats/grn.md](../../formats/grn.md). Steps measured on the camera: L : M : H about 0.7-0.76 :
  1 : 1.3.
- Expected mid-tone grain deviation in 8-bit Y = tile deviation x gain / 1024 (measured on the camera).

### 3.9. Memory and time

- A grain still needs the texture (about 40.5 MB) and the tile buffer (2.36 MB) on top of the development: core keeps the
  image allocator in its large layout (mode 1, as the ART filters) only while the look is on with a strength, and asks the
  firmware to rebuild the picture mode when that changes. A still while the allocator is not in mode 1 gets no grain
  (code). The mask route needs about 20 MB more than the `GF` route (the mask plane) (firmware code; fits on the camera:
  measured).
- Per still: texture build with fill about 0.44 s; job time 0.72-0.91 s on both routes, also in bursts (measured on the
  camera, core 1.29). At most 4 OMKit textures alive at once (the firmware cache), otherwise a still gets no grain
  (code). A job that never reached the cleanup gate does not keep its place for good: W takes it back when a new job
  comes at the same handle (its texture is released) or, with no place free, when it is older than 300 s (code; a
  warning in the log, never seen on the camera).

Allocator, job lists and passes: [still-pipeline.md](still-pipeline.md).

### 3.10. The log

Core's log gets a line per grain still (route, list built, tile, gain, shift, build and fill time, or the reason for no
grain), a line per finished job (steps, time from W to H3), per-CPU counters every 5 s when they change, the allocator
mode when it changes, and each tile load (code).

## 4. No grain in live view

OMKit puts no grain on live view or video (W takes job type `0x1000` only) (code). Reasons:

- The firmware builds no texture for photo live view (section 2.4). A texture for live view would have to be built with
  the video parameters for every frame - a 40 MB build and a DMA replication per frame (firmware code; not tried).
- The generator route works in live view (case 32 forced for the look's live view: measured on the camera), but per-pixel
  noise "boils" on the screen and looks stronger than on the still at the same gain (the live frame is downscaled less),
  and it has neither the tile's structure nor the tone mask - a misleading preview (measured on the camera; the reason for
  the difference is an assumption).
- Design decision: grain on stills only. The shooting screen does not change when grain is on.

## 5. User control

### 5.1. FILMLOOK's "Film Grain" item

- With `enable_edits = on` and `grain = on` in `[FILMLOOK]` and at least one valid container, the look's editor column
  gets a grain item (the firmware's own Color/Vivid item, repurposed; see [ui.md](ui.md)) (code).
- **Rear dial** - the next / previous tile (all containers, in file-name order, round); **front dial** - the strength
  Off / Low / Med / High; **long OK** - Off. Dial labels "Type" and "Strength"; the item shows a cassette with the tile's
  name and four strength icons (code).
- Stored per preset with the preset's edits: the strength and the tile's container file name. A preset never edited: Off,
  with the default tile `grain_tile =` (`[FILMLOOK]`, e.g. `CLASS400`), or the first by name. A stored tile whose file is
  gone: "NONE", no grain (code).
- `exif = on` adds ` g=NAME,L` (container without `.GRN`, strength 1..3) to the preset text in the JPEG (code).

### 5.2. `OMKIT.INI`, section `[grain]` (core)

| Key | Values | Default |
|---|---|---|
| `mask` | `film`, `flat`, `off` (the `GF` route; the fallback); test values `step`, `list` (list alone, no blend, no grain) | `film` |
| `chroma` | 0..1024: colour grain on colour presets | 0 |
| `mask_test`, `mask_jobs`, `mask_run`, `chroma_test` | for diagnostics only, not in the sample `OMKIT.INI`: a test of up to 8 levels over power-ons (one level a run, recorded on the card before it is armed; a reset is attributed to its level) | none |

### 5.3. core's grain service (API 17)

- `grain_list(m, files, max)` - the containers' 8.3 file names (a tile's number is its index), their count. The first
  call, only from `module_init`, takes the service: core scans `OMKIT\GRAIN\*.GRN` (at most 32), checks every header
  (bad ones are logged and left out), allocates the slot (keeping 768 KiB of the arena free for later modules), installs
  the hooks and gives the tile names to the grain item. 0 - no containers or no service.
- `look_grain(m, entry, tile, level)` - the owner of the look sets entry `entry`'s tile (-1 none) and strength 0..3; the
  item shows them and the look's stills get them. 0; -1 not the owner or bad; -2 no grain service.
- `OMK_EDIT_GRAIN` in `look_edits` shows the item; `OMK_LOOK_EDIT` events: index 0 the tile number, index 1 the strength
  (a long OK: index 1, value 0). Pictures `OMK_LOOK_PIC_GRAIN`, `OMK_LOOK_PIC_TYPE`, `OMK_LOOK_PIC_STRENGTH`.

Full API: [../../sdk.md](../../sdk.md).

### 5.4. Tiles

Bundled set (`resources/modules/filmlook/grain/`, 11 containers): `CINE250`, `CINE500`, `CLASS400`, `CLOUD200`,
`COLOR400`, `DYE160`, `FINE100`, `HALF400`, `LAB400`, `PUSH1600`, `TAB400` (sides 512 or 768). They follow measured film
grain spectra at the scale of a 35 mm frame (one pixel of a 3:2 still = 6.94 um of film). `tools/grainset.py check`
verifies the set against its manifest, `copy DEST` puts it on a card. Any valid container in `OMKIT\GRAIN\` is offered too.
Format, checks and the gains of each tile: [../../formats/grn.md](../../formats/grn.md).

## 6. Not established

- JPEG sizes M and S: the texture is built at the job's output size and input B is not scaled, so the grain keeps its
  pixel size on a smaller picture (firmware code; not measured).
- Tile values above 63, and what the texture node puts into the texture's chroma (assumption: unused).
- The meaning of image `+8` and `+148`, and of mode bit 12 together with bit 21.
- The width of the gain field beyond 2047 (not tried).
