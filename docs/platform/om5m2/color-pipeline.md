# OM-5 Mark II: the ISP colour path

How the camera's image signal processor (ISP) turns white-balanced sensor RGB into the YCbCr of the JPEG and of the live
view, which tables each stage uses, how OMKit replaces those tables, and what of this was measured on the camera.

Related documents:

- the `.isp` profile file and its `ISPB` block (the tables in the camera's own formats):
  [../../formats/isp.md](../../formats/isp.md) - this document does not repeat its layout;
- picture modes and the slot the film look takes: [picture-modes.md](picture-modes.md); the development job of a still:
  [still-pipeline.md](still-pipeline.md); the look's grain: [grain.md](grain.md); the editors: [ui.md](ui.md);
- firmware 1.1 addresses of every function and table named here by its role: [fw1100/imaging.md](fw1100/imaging.md).

Basis tags: **(firmware code)** - static analysis of the firmware; **(emulator)** - the firmware's own code run in an
emulator (here a narrow bench that executes the colour builders, the register writer and the table loader with prepared
inputs; see the **emulator** label in [README.md](README.md#basis-labels)); **(measured on the camera)** - an experiment on the camera with a number against an expectation;
**(camera, indirect)** - explains real shots, but no dedicated experiment; **(assumption)**.

## 1. The path

```
sensor RAW -> white balance (R/B gains) -> MATRIX 3x3 -> GAMMA R, G, B (256 nodes -> 12 bit)
  -> RGB to YCbCr (BT.601) x k on Cb/Cr -> TONE on Y (256 nodes -> 10 bit)
  -> 12 HUE SECTORS (hue shift, chroma addition, luma addition) -> YCbCr 8 bit (JPEG, live view)
```

| Stage | Data | Size, range | Replaced by OMKit | Basis of the order |
|---|---|---|---|---|
| White balance | R and B gains on the RAW (G = 1) | chosen by the camera | no | measured on the camera (MakerNote) |
| Matrix | 3 x 3 s16, Q7 (128 = 1.0), rows sum to 128 | one per frame | yes, as F x the camera's matrix | firmware code; measured on the camera |
| Gamma | 3 curves R, G, B | 256 x u16, output 0..4095 | yes | measured on the camera |
| YCbCr, saturation | k_Cb, k_Cr, Q8 | u16; Natural 158 / 200 | yes | measured on the camera |
| Tone | one curve on Y | 256 x u16, output 0..1023 | yes | measured on the camera |
| Sectors | 12 records, 12 x 7 profiles of 16 nodes, an enable mask | see section 3.5 | yes | measured on the camera |

There is no 3D LUT and no other cross-channel table in the colour block (firmware code): the only cross-channel tools
are the matrix (before the gamma) and the sectors (after the tone). Everything between white balance and the sectors is
per pixel; nothing here depends on the neighbourhood.

## 2. The colour structure S and how it reaches the hardware

The colour block's parameters live in one structure, here called **S** (5776 bytes), that belongs to a processing job
(firmware code, emulator):

- **development jobs** (the job type byte is 0 or 16 - live view and the main passes of a still) have a **persistent S**
  owned by the job handle; its address is the job's "ext". It survives between frames;
- other job types take an S from a per-CPU pool and build it from scratch every time.

For every job the firmware runs, in order and on the same CPU (firmware code):

1. **the job context** is filled: the request block (here B: job type, pipe, subtype, settings of the picture mode), S,
   and a **change mask** - a fingerprint compares about 30 input fields of B (picture mode code, sensor readout, ISO,
   sharpness, contrast, gradation, saturation, colour space, Highlight & Shadow, ...) with copies kept in S and sets one
   bit per field that changed;
2. **the colour builder** fills S. The matrix and the gamma pointers are written in every build; Cb/Cr, the sectors, the
   tone and most other fields only when their bits are set in the change mask. Fields whose bits are clear keep whatever
   the persistent S held - including values OMKit wrote there;
3. **the register dispatcher** calls, back to back in one call, the **colour register writer** (S to the registers,
   no conversion) and the **table loader** (tables by pointer from S into the pipe's table RAM).

The writer copies the register part of S to the colour block of the pipe at a fixed offset (S+x to register +x+64),
with sub-blocks skipped by bypass bits in S+4; the sector records go only when the sector mask is non zero (firmware
code; the emulator confirms the register image equals S shifted by 64 and that the writer does not change S).

Fields of S that matter here (offsets are within S):

| Offset | Contents | Goes to |
|---|---|---|
| +4 | bypass bits: 0x1 block S+496, 0x10 the luma table S+3164, **0x100 the tone off**, 0x1000 the second curve S+128 | writer, loader |
| +80 | the matrix, 9 x s16 Q7 | registers |
| +100..123 | two sets of 6 x s16 ("six axes", see 3.6) | registers |
| +124, +126 | k_Cb, k_Cr, Q8 | registers |
| +128..319 | a second curve, identity in Natural | registers |
| +688..975 | 12 sector records of 24 bytes | registers |
| +976 | sector enable mask (12 bits) | registers |
| +2596, +2600, +2604 | pointers to the gamma R, G, B (256 x u16) | loader |
| +2608 | pointer to the tone (256 x u16); +2612..3123 is a working copy for the ISO remap and Highlight & Shadow | loader |
| +3124..3160 | 10 pointers to the sector weight half-tables G1..G5 | loader |
| +3164 | pointer to a 256-entry luma table (pipe A only; not colour) | loader |
| +3680.., +4388.. | the detail block (sharpness, noise filter) and another block, written by their own writers | other blocks |
| +5672..5775 | the fingerprint copies and the builder's own state | nobody |

**The pipe's table RAM** (offsets in the table window of the colour block; firmware code):

| Offset | Entries | Contents |
|---|---|---|
| +0x0000 | 256 | one gamma for all channels (used when the three pointers are equal) |
| +0x0400, +0x0800, +0x0C00 | 3 x 256 | gamma R, G, B, 12 bit |
| +0x1000 | 256 | tone, 10 bit |
| +0x1400..+0x2600 | 10 x 128 words | sector weight half-tables G1..G5 (the hardware reads 96 words: 6 sectors x 16 nodes) |
| +0x2800 | 256 | luma table by ISO (pipe A only) |

**When the loader reloads** (firmware code): it keeps, per pipe, the ext it loaded last (checked under an inter-CPU
lock). A different ext, or no ext, is a miss. The **tone is loaded at every call** when its pointer is not 0. The
**gamma** is loaded on a miss or when bit 0 of the job's reload word is set; the **sector weights** on a miss or bit 1,
when the sector mask is not 0. The matrix, Cb/Cr and the sector records are registers and are written by every job.

## 3. The stages

### 3.1. White balance and the matrix

- White balance is R and B gains on the RAW before the matrix; each channel is clipped at the sensor's white after it,
  so blown highlights stay neutral (measured on the camera, three balances).
- The camera's matrix S+80 (Q7, rows sum to 128 so grey stays grey) changes with the light: the same matrix for auto
  balance and 5300 K, another for 3000 K (measured on the camera). It is the matrix of the frame's balance, not a style.
  The MakerNote `ColorMatrix` tag holds twice this matrix (Q8), without OMKit's look matrix (measured on the camera, 177
  frames).
- Exposure: the camera meters mid grey to about 0.075 of RAW white; Natural maps it to L* 50 (measured on the camera,
  five shots, 0.074..0.076).
- **The look's matrix F** (OMKit, `ISPB` +8): core writes S+80 = F x S+80 of the frame, rounded to Q7 with each row
  brought back to 128 on the diagonal, as the firmware's own row normalisation does. White balance and the camera's
  adaptation to the light stay. The product is applied to the RAW once and clipped once, at the gamma's input (firmware
  code; the model does the same arithmetic). Real scenes show F at 0.99 of the modelled strength and never applied twice
  (camera, indirect, 125 frames). Saturated colours outside the sRGB gamut were not checked.

### 3.2. Gamma R, G, B

- 256 nodes per channel, linear between nodes; **node i is the linear input i/255 of the full scale, and the full scale
  is 1.06 x RAW white** (measured on the camera: a stepped gamma gave constant step widths in linear RAW, so the domain
  is linear; scale 1.06 +- 0.004 from 22 preset shots). Output 0..4095 (12 bit). The pointers are R, G, B in that order
  (measured on the camera).
- Resolution limit: node 1 is 0.0039 of full scale, about 4.2 stops below mid grey; deep shadows cannot be shaped more
  finely (consequence of the format).
- Natural uses the same ROM gamma at every ISO (measured on the camera, ISO 200..6400). The firmware picks other gammas
  for log, HLG, the gradation Auto and special codes (firmware code, emulator).
- The gamma is the look's main tool for tone and for the tint of neutrals (different R, G, B curves).

### 3.3. YCbCr and the saturation k

- BT.601: Cb = (B - Y) / 1.772 x k, Cr = (R - Y) / 1.402 x k, with the gains stored as Q8 values 144 x k and 182 x k
  (144 / 182 is exactly k = 1). Natural is 158 / 200 (k ~ 1.10); the menu saturation +2 / -2 multiplies by about 1.20 /
  0.81; Vivid and Underwater have their own values; the firmware also lowers k with ISO (firmware code, emulator).
- k scales chroma uniformly, **before the sector block**: the menu saturation changes the sectors' input, not their
  output (measured on the camera). k = 0 gives pure grey; the luma is then Y = 0.299 R + 0.587 G + 0.114 B after the
  gamma (consequence of BT.601; used by black-and-white looks).
- Model against camera on field targets: gamma, tone and k together agree within the method's own noise (median dE2000
  0.46..0.97 against a null of 0.94; measured on the camera).

### 3.4. Tone on Y

- 256 nodes, output 0..1023 (10 bit, full scale of Y). It acts **only on Y**: a stepped tone gave exactly k/16 levels of
  Y while chroma and hue stayed smooth (measured on the camera).
- **Node i sits at Y = i/256** (the curve is indexed by the top 8 bits of Y). A dedicated experiment separated this from
  i/255: the error stayed under 0.3 L* for i/256 and grew to 4.8 L* for i/255 (measured on the camera). Above node 255
  the last segment continues (assumption).
- Natural's ROM tone is exactly 4 x i up to node 194 and then rises to 1023. The firmware's tone builder starts from a
  curve chosen by mode, contrast and gradation, remaps it by ISO and applies the Highlight & Shadow spline to a copy
  inside S (firmware code). At ISO 6400 Natural's tone is a table built in RAM (measured on the camera).
- The tone is turned off by bit 0x100 of S+4; an override must clear it (firmware code; core does).

### 3.5. The 12 hue sectors

Local adjusters on (Y, Cb, Cr) after the tone. Coordinates (10-bit units): Y 0..1023 = the JPEG's luma (the luma window
lines up with JPEG Y, measured on the camera); Cb, Cr +-512 after k; C = |Cb, Cr|; hue h = atan2(Cr, Cb), 32768 = one
turn, zero on the +Cb axis growing towards +Cr (measured on the camera).

**A sector record** (24 bytes, copied to the registers as is; layout in [isp.md](../../formats/isp.md) section 5):

| Field | Meaning | Range | Basis |
|---|---|---|---|
| hue window start, width | where the sector acts by hue | 0..32767; width 32767 = the whole circle | measured on the camera |
| chroma window start, width | by C | 0..1023; C 256 ~ 0.25 of full chroma | measured on the camera |
| luma window start, width | by Y after the tone | 0..1023; **width 0 = window off, weight 1**; 1024 reads as 0 (10-bit field) | measured on the camera |
| three reciprocal widths | E << 8 \| M, M = round(2^(E+8) / width) in 128..255; the hardware divides with them | computed from the widths; the firmware stores them precomputed | firmware code (all 222 ROM fields fit) |
| hue shift | added hue | s16, 32768 = 360 degrees | measured on the camera up to 100 degrees |
| chroma addition | **added** to C (not a gain; C stays >= 0) | s16, 256 = +0.25 C | measured on the camera (+0.25, -0.5) |
| luma addition | added to Y | s16, 128 ~ +0.12 Y | measured on the camera (+-0.125) |

Natural's ROM uses hue widths 2367..32767, chroma widths up to 1023, luma windows up to 0..1023, and never a luma
addition without a luma window (firmware code).

**Weight profiles.** Each sector has 7 profiles of 16 nodes: hue and chroma profiles for each of the three operations
(chroma, hue, luma) and one luma profile shared by all three. For a pixel, the weight of an operation is
H_op(h) x C_op(C) x Y(Y); the 16 nodes split the window into 16 equal parts, the weight is linear between nodes, from
node 15 it falls to 0 at the window's edge (a whole-circle hue window wraps to node 0), and it is 0 outside the window.

| Property | Value | Basis |
|---|---|---|
| node value in hardware | 7-bit signed, 64 = 1.0: -1.0..63/64 in steps of 1/64 | firmware code (ROM words), measured on the camera |
| hardware word | node value in the high byte, slope to the next node in the low byte; the hardware interpolates inside the node | firmware code (2520 ROM node pairs), measured on the camera (smooth, no 1/16 steps) |
| 16384 in the `.isp` Q14 value | reads as -1.0 in hardware; refused by core | measured on the camera |
| luma profile | weighs all three operations; interpolated like the others | measured on the camera |
| last node | slopes to 0 at the window's edge; whole-circle hue windows wrap to node 0 | firmware code (ROM), camera, indirect |

**The weight tables.** The ten half-tables (both banks of G1..G5, 128 words each) hold the profiles; node n of sector k
is entry (k mod 6) x 16 + n of the half k / 6 (firmware code; measured on the camera for the slot order):

| Table | Word | Contents |
|---|---|---|
| G1 | u32 | low: hue profile of chroma; high: hue profile of hue |
| G2 | u32 | low: chroma profile of luma; high: chroma profile of chroma |
| G3 | u16 | chroma profile of hue |
| G4 | u16 | luma profile (all operations) |
| G5 | u16 | hue profile of luma |

**Two banks.** Sectors 0..5 and 6..11 are independent banks. Within a bank a pixel gets **one** sector - the
lowest-numbered one whose hue **and** chroma windows both hold it - and the two banks add up:
C += chroma addition x w, h += hue shift x w, Y += luma addition x w, once per bank (measured on the camera: two
overlapping +30 degree sectors in one bank turned by 26.6 degrees, not 58; a sector whose chroma window missed the
pixel left it to the next one). Whether the luma window also decides which sector holds the pixel was not tested. That
luma additions of the two banks add up is seen on real scenes (camera, indirect).

**Natural's sectors** (firmware code): 8 of 12 enabled (mask 0xb8f), small hue shifts up to about +-18 degrees and
small chroma and luma additions; for example sector 0 turns hues around 133..193 degrees by +15 degrees (measured on the
camera as +11..12 degrees). Replacing the sectors removes these. Natural's sector tables are ROM tables; the picture
mode "colour profile" builds its records from 12 user saturations and keeps the shared weight tables (firmware code).

### 3.6. What else is in the colour block (not replaced by OMKit)

| Field | Natural | What it does | Basis |
|---|---|---|---|
| "six axes" S+100..123 | sRGB: 0, 0, 0, 0, 0, 0 / -1, 8, 0, 0, 0, 0; Adobe RGB: zeros | unknown (perhaps a six-axis colour correction); its place relative to the sectors is not established | firmware code, emulator; effect unknown |
| second curve S+128 | identity | 24 gain/offset pairs; non-identity only in two ART filters | firmware code |
| monochrome S+76 | off | constant Cb, Cr (Monotone, toning) | emulator |
| shadow chroma suppression | - | below JPEG Y ~30/255 chroma is 0.6..0.7 of the model, recovering to 0.97 by Y ~47..50; straight towards grey; independent of ISO, sectors and look | camera, indirect (167 real-scene frames); the register is not identified (assumption: a curve keyed by luma in S+456..472) |
| detail block | by sharpness, noise filter, ISO | sharpness and noise filter bands | firmware code |

All camera checks of the model were shot in Natural, sRGB, default settings, so these fields enter the model as
Natural's values (inference). While a look replaces the tone and Cb/Cr, Natural's contrast, gradation High/Low Key,
saturation and Highlight & Shadow lose their effect: core writes those fields over them (emulator, firmware code).
Sharpness, noise filter, colour space and the gradation Auto still act through the fields above.

## 4. Live view and stills

| | Live view | Still, main passes | Still, service pass |
|---|---|---|---|
| CPU, pipe | CPU 2, pipe B | CPU 0, pipe A | CPU 0, pipe A |
| Rate | about 60 jobs/s, a colour build every frame | 3 jobs with a colour build per frame | 1 per frame, no colour build |
| S | persistent: one ext for the whole session | persistent, a **new ext for every shot** | from the pool |
| Change mask | 0x00800000 every frame (one bit that is never cleared; no colour builder checks it) - nothing is rebuilt, values persist from the last build with the right bits | first job 0xE3FFFFFF (everything rebuilt), then 0x00800000 | all bits |
| Gamma, weights reloaded | only on a cache miss or the job's reload bits | every job (pipe A always misses) | - |
| Tone, registers | every job | every job | - |
| Subtype byte (low byte of the B subtype field) | 0x01 | 0x07 | - |

All rows: measured on the camera (OMKit's job census), except the reload rules (firmware code). Live view and stills
both run with picture mode code 2 for Natural. The stock tables of live view were not read on the camera; the ISO
remap of the tone at ISO 6400 and the adaptive analyses (i-Enhance, gradation Auto) are not reproduced by the emulator.

Consequences:

- **Stills** get a full rebuild at every shot; replaced values cannot leak from one shot to the next.
- **Live view** never rebuilds by itself. Anything written into its S stays until a build with the right bits; tables
  in pipe B's RAM stay until a reload is forced. An override must therefore force the reload of gamma and weights when
  they change, and force a full rebuild from stock when it stops (section 5).
- **Autofocus and metering see the live view after the gamma.** A stepped test gamma in live view stopped the
  half-press cycle (no focus confirmation) and froze the camera's application (measured on the camera). Smooth look
  curves in live view work: live view follows each preset change and edit (measured on the camera, film look sessions).
  This is why OMKit's default scope is stills only and a module opts into live view.

## 5. How OMKit replaces the tables

### 5.1. The two hooks

Core's platform driver installs two code hooks (firmware addresses: [fw1100/imaging.md](fw1100/imaging.md)):

| Hook | Place | What it does |
|---|---|---|
| writer hook | entry of the colour register writer: S is complete and not yet copied | census (optional); for an eligible job writes the replaced parts into S; at a new slow generation for this ext sets the reload bits 0 and 1 (the loader reloads gamma and weights); the grain mask (5.6) |
| builder hook | entry of the colour builder | for an ext that holds OMKit's values and must lose some (back to stock, or a new slow generation that leaves parts out): sets the whole change mask (0xE3FFFFFF), so the builder rebuilds from stock first |

The writer's entry is the only point where S is ready and not yet applied (firmware code); the writer and the loader
then apply OMKit's values themselves. Writing registers after the writer was rejected: the loader writes the same
hardware and owns the order (firmware code).

**What the writer hook writes** (each part only when its bit is set in the block's `present` field):

| Part | S fields |
|---|---|
| matrix | S+80 = F x S+80 (section 3.1); for a persistent S the original and the written matrix are remembered per ext, so F is not applied twice |
| Cb/Cr | S+124, S+126 |
| sectors | the 12 records, the mask, the 10 weight pointers |
| gamma | the 3 gamma pointers |
| tone | the tone pointer, and clears bit 0x100 of S+4 |

**Eligible jobs:** development jobs (type 0 or 16) with a picture mode code below 100 (no ART, scene or special codes),
no log gamma and no HLG, and in the module's scope (live view by the subtype byte 0x01, stills otherwise). The service
pass and the pool jobs are never touched. The hooks run on any CPU inside the firmware's job task; they touch only the
service's block in the arena (through the uncached address), the job's own S and context, and the stack.

### 5.2. Publication

The usual way for "one writer, readers on other CPUs that must not wait": a two-copy latch with a sequence counter for
what the reader copies, and a ring of buffers with a time margin for what the reader dereferences later. Core
prepares everything in its own task; the hooks only copy.

| Class | Parts | Published as | Minimum interval | Reload, rebuild |
|---|---|---|---|---|
| fast | matrix F, Cb/Cr, sector records and mask, tone | a descriptor in a two-copy latch (the hook copies the stable copy and re-checks the counter, up to 3 tries; a torn read is counted and the next job takes the new copy); the tone through a ring of 4 tables | 40 ms | none: the writer and the loader take these parts at every job |
| slow | gamma, sector weights G1..G5 | two table sets (double buffer); a slow generation counter | 1 s | reload bits for each ext at a new slow generation; rebuild by the builder hook where parts are dropped |

- A **generation counter** is incremented last, after the descriptor and the tables are complete; while it is 0 (never
  overridden) the hooks do nothing but the census.
- The driver decides the class itself: a CRC of the gamma and the profiles and the set of parts against the published
  ones. A preset change is slow; an edit of the tone, the sector saturations or the black-and-white filter is fast.
- The reader's window is from the writer hook to the end of the loader in the same dispatcher call (well under 2 ms;
  firmware code). A tone table is rewritten 120 ms after it was replaced and a table set 1 s after; these are margins,
  not measured worst cases. A race cannot fault memory: tables are never freed and every pointer stays inside the arena;
  the worst case is one torn frame.
- Tables are 64-byte aligned in uncached arena memory; the loader copies them with the CPU into the pipe's RAM (no DMA).
- Turning the override off is a slow change: the persistent S of live view is rebuilt from stock and its tables
  reloaded at its next job.

### 5.3. Checks and conversion of the block

`drv_isp_set` accepts only the `ISPB` of layout 1 (5064 bytes) and refuses it (-1) when:

- an enabled sector has a hue start or width over 32767, or a chroma or luma start or width over 1023;
- `present` has the matrix and a row of F does not sum to 128;
- a profile node value is outside -16384..16383.

Conversion into hardware form: gamma values clipped to 4095, tone to 1023; each profile node becomes
round(Q14 / 256) clamped to -64..63, and the hardware word is that value in the high byte and the slope to the next
node in the low byte (to 0 after node 15, or to node 0 for a whole-circle hue window). From the stock node values this
reproduces Natural's and the colour profile's ROM words byte for byte (firmware data, unit test). The record fields,
including the reciprocal widths, are copied as given: the `.isp` writer computes the reciprocals.

### 5.4. The API and what modules may do

The ISP service is API 13 of the SDK (`omk_api.h`; the module side is described in [../../sdk.md](../../sdk.md)):

| Call | Effect |
|---|---|
| `isp_scope(m, scope)` | which jobs get the tables: `OMK_ISP_LIVE` and/or `OMK_ISP_STILLS`; default stills; call before `isp_override` |
| `isp_override(m, block, size)` | publish an `ISPB` block (copied; the caller may free it); `NULL` - back to the camera's own tables |
| `isp_census(m, on)` | log every colour job (CPU, pipe, type and subtype, code, ext, change mask) at debug level |

Results: 0; -1 a bad block; -2 too soon (the class's minimum interval - the module retries later); -3 no service
(other firmware, hooks not installed); -4 the first call was not from `module_init`; -5 another module owns the
service. The first module to call owns the service, and its first call must come from `module_init`: the hooks are
installed then, before the camera is in use. Modules never write firmware memory themselves.

The film look module (FILMLOOK) uses the service like this (module code):

- it takes the service with `isp_scope` (its `scope` setting, default both live view and stills);
- it reads the chosen preset's `.isp` from the card once, keeps its `ISPB`, and on every change publishes a copy with
  the user's edits;
- the edits map to the block as follows:

| Editor | Block field | Step, range |
|---|---|---|
| tone (three points) | the tone curve: a smoothstep bump through points placed where the green gamma sends 1/4, 1/2 and 3/4 of a perceptual axis; clipped to 0..1023 and kept non-decreasing | 20 / 1023 (~2 %) per step, -7..+7 |
| Color / Vivid (12 vertices) | the chroma addition of each sector record, from the preset's value | 51 (0.05 of C) per step, -5..+5, result within +-1023 |
| black-and-white filter and level | the matrix F: the firmware's own filter matrices (Q8) halved to Q7, rows brought back to 128; sets the matrix bit of `present` | 8 filters x 3 levels |

All three are fast-class changes (a new filter that adds the matrix part to a preset without one is slow, because the
set of parts changes). Whether a block is black and white follows one rule in the SDK header, described in
[isp.md](../../formats/isp.md) section 5.1.

### 5.5. The grain mask

The look's grain uses a development pass (stage 0x900) whose tone curve is a weight mask. For the jobs of the grain at
that stage the writer hook puts core's mask table in place of the tone, built from the grain's weights through the
published look tone, and restores the job's own tone on its next pass (driver code). Details: [grain.md](grain.md).

### 5.6. What the log shows

Every 5 s, per CPU: writer and builder hook calls, patched jobs, forced reloads, rebuilds, torn reads, and the number of
slow and fast changes; the camera's matrix, Cb/Cr, gamma and tone pointers and S+4 of every still job (core never
writes those inputs, so they are the camera's own); with the census, the latest jobs. "no hook calls" means changes
were published but no colour job ran (for example in the emulator, which does not execute the colour chain).

## 6. Camera against the model

A PC tool fits looks to a numerical model of this path (sections 1 and 3); the model follows the camera
arithmetic, including the rounded product F x S+80. This is how far the model and the camera agree.

**Verified on the camera:**

| What | Result |
|---|---|
| gamma domain, scale, channel order | linear, node i = i/255 of 1.06 x RAW white; R, G, B |
| tone | only on Y; nodes at i/256 |
| k | before the sectors; field targets within the null |
| sector windows | hue zero on +Cb; chroma, luma windows; luma width 0 = off; 1024 = off |
| profiles | node + slope words, linear interpolation; 1/64 steps; 16384 = -1 |
| operations | chroma addition additive; luma profile on all three; banks (one sector per bank, hue and chroma decide) |
| amplitudes | chroma addition +0.25 and -0.5 (down to grey, within 0.005 of the model); luma +-0.125; hue up to 100 degrees |
| whole looks | real scenes, 167 frames: median dE2000 1.30 (colour looks at ISO <= 800: 0.83, ISO 3200: 1.65; neutral black and white 0.05..0.34); the preset named in EXIF is the best of 37 candidates in 141 of 148 frames, never the previous one (no mixed or stale tables) |

**Discrepancies** (most important first):

| What | Camera | Model | Basis |
|---|---|---|---|
| shadow chroma | 0.6..0.7 of the model below JPEG Y ~30/255, 0.97 above ~50 | no such stage | camera, indirect; register not identified |
| negative sector luma | acts at 25..65 % of the model in one preset with strong negative luma additions in blues; positive luma 0.9..1.07; over all presets the sector luma strength is 0.78 | full strength | camera, indirect; cause not found (assumption: a steep chroma profile of luma with a negative amplitude and a large k) |
| hue shift | about 96 % of the request (+60 -> +55.9, +100 -> +95.8 degrees) | 98 % (the 63/64 profile ceiling) | measured on the camera; the extra ~2 % is not modelled |
| luma addition at high Y | +0.10 instead of +0.12 Y for Y > 0.7 | +0.12 | measured on the camera; assumption: compression after the block |
| overall chroma | 2..3 % lower on saturated colours (strength of k 0.93, sector chroma 1.21; together consistent with one factor ~0.975) | 1.0 | camera, indirect; cause not found |
| radial luma residual | +0.1 L* in the centre to +0.6..0.7 in the corners at 12..18 mm (shading compensation was off) | none | camera, indirect |
| high ISO | larger scatter (ISO 3200 1.65) | - | inference: measurement noise of small blocks, not the tables |

**Not verified:** saturated colours outside the sRGB gamut (clipping between matrix and gamma); whether the luma
window takes part in choosing a sector; the role of the "six axes" fields; the camera's tone at extended ISO; a torn
latch read on the camera (the emulator does not produce one); the matrix bookkeeping is kept per CPU, so a persistent S
that moved to another CPU without a rebuild of S+80 would get F twice (driver code; not seen in the data).

**Emulator coverage.** The full-system emulator does not execute the colour chain at all (no ISP registers mapped),
so the hooks never run there. The narrow bench runs the firmware's own builders, dispatcher, writer and loader: there
the stock Natural S matches the values read on the camera, and a slow change, a fast change, a refusal "too soon" and
the rebuild back to stock all produce the expected registers and table windows (emulator). It does not cover live view
(pipe B), the ISO 6400 tone, the adaptive analyses or the code above the S build.

## 7. Geometry and blur blocks (brief)

Outside the colour block the ISP has no convolution with a programmable kernel in any of the blocks analysed (firmware
code). Blur in the firmware is made by downscale-upscale-blend (the Soft Focus filter: a small frame mixed with the
sharp one at fixed weights), by decimation on read with a one-dimensional blend mask (Diorama), and by the four-band
detail block (sharpness and noise filter). Block 0x0800 is geometry and resampling (a 3 x 3 matrix, rotation, scale,
per-line shifts) used by electronic stabilisation in live view and by Keystone; it works in YC 4:2:2 with one matrix
for all planes, so it cannot do per-channel effects such as chromatic aberration. Block 0x0e00 blends 2..3 inputs
towards a colour by a map (vignetting, shading of the profiles). All of these act after the tone, not in linear light
(firmware code). None of them is replaced by OMKit's ISP service.

## 8. Open questions

- The stage behind the shadow chroma suppression and its depth.
- Why negative sector luma acts weaker than modelled.
- Whether the luma window takes part in choosing a sector within a bank.
- What the "six axes" fields do and where they sit relative to the sectors.
- The camera's colour at extended ISO and with the gradation Auto under a look.
