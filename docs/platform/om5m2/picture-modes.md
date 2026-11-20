# Picture modes and profiles

How the OM-5 Mark II firmware numbers, stores and dispatches its picture modes; how a stored setting becomes the value
that shooting and the ISP use; what is left of the colour and monochrome profiles in this firmware; how custom modes
keep picture mode settings; and how OMKit plugs in (the CREATPRF module and the film look slot).

Related pages: the colour path from the parameter block to the ISP registers - [color-pipeline.md](color-pipeline.md);
the picture mode screen, editor column, pictures and graphic editors - [ui.md](ui.md); the still job and its
parameters - [still-pipeline.md](still-pipeline.md); grain - [grain.md](grain.md); the `.isp` preset format -
[../../formats/isp.md](../../formats/isp.md); firmware 1.1 addresses of everything named here by role -
[fw1100/imaging.md](fw1100/imaging.md). OMKit structure: [../../architecture.md](../../architecture.md),
[../../modules.md](../../modules.md), [../../sdk.md](../../sdk.md).

Basis tags: (static analysis) - firmware code read statically; (emulator) - run in the emulator; (measured on the
camera); (assumption) - not verified; (OMKit code) - what OMKit does, from its sources.

## 1. Overview

- The picture mode is setting 111 `MODE_COLORTONE`. It has two numberings: the **raw value** kept in the settings
  store, in the shooting parameters and in the development job, and the **mode number** the UI and the dispatcher use
  (section 2.1). (static analysis)
- The firmware has the lower layers of colour and monochrome profiles (storage cells, defaults, most descriptors, the
  ISP path for raw values 11 and 12) but not the upper ones: no profile handlers in the dispatcher, no 11/12 among the
  allowed values of 111, no UI to choose or edit them. Its leftovers are sized for three profiles of each kind, not
  four. (static analysis)
- OMKit never stores a profile in 111. A profile is a *virtual picture mode*: the UI is shown modes 54..59, the store
  keeps Natural, and the profile is substituted in RAM into the shooting parameters and the live view context. The film
  look reuses the same UI hooks to show a look in place of a stock picture mode. (OMKit code, measured on the camera)

## 2. Picture modes

### 2.1. Raw value and mode number

The group read of 111 maps the raw value (with setting 361, the ART variant) to the mode number through two arrays of
the 111 descriptor: `allowed` (41 elements of 64 bytes; word 0 is the raw 111, word 1 is 361) and `values` (the mode
number of each element). (static analysis, emulator)

| Mode | Raw 111 (store, job code) | Mode number (UI, dispatcher) | Notes |
|---|---|---|---|
| i-Enhance | 7 | 7 | the default film look slot; raw 7 enables the i-Enhance scene analysis in the still job |
| Vivid | 1 | 0 | |
| Natural | 2 | 1 | the base mode of OMKit's profiles and film look |
| Muted | 3 | 2 | |
| Portrait | 4 | 5 | |
| Monotone | 5 | 3 | monochrome substitution in the colour path for raw 5 and 12 |
| Custom | 6 | 6 | no own entry in the editor column table |
| e-Portrait | 8 | 20 | |
| Color Creator | 9 | 21 | |
| Underwater | 10 | 28 | |
| ART filters | 100..124 (+ 361 variant) | 29..63 | 31 elements; mode 63 is the last element of the group |
| Colour profile 1..3 | 11 (+ 903 = n) | 54..56 | not allowed in stock; OMKit shows them virtually |
| Monochrome profile 1..3 | 12 (+ 989 = n) | 57..59 | not allowed in stock; OMKit shows them virtually |
| Profile 4 (colour / mono) | 11 / 12 | 64 / 65 | absent: the dispatcher table ends at 63 |

The picture mode strip of the UI lists, in stock, the ten picture modes in the order 7, 0, 1, 2, 5, 3, 6, 20, 28, 21,
then the ART filters (41 entries). (static analysis)

### 2.2. The 111 group

- Descriptor of 111: type 6, a **group**. The group table (34-byte entries `{u16 group id, u16 members[16]}`, list end
  3200) gives 111 two members on this camera: 111 and 361. Firmware with native profiles also has 903 and 989 as
  members, so that `(11, 0, n, 0)` and `(12, 0, 0, n)` select the profile modes. (static analysis)
- Group read: a 64-byte buffer is built from the members' raw values, compared (`memcmp`) with the `allowed` elements,
  and `values[idx]` is returned. The search covers elements 0..n-2 only: a miss returns the **last** element, mode 63.
  A raw 11 or 12 in the store therefore reads as an ART filter in the UI while the lower layers see 11/12.
  (static analysis, emulator)
- Write paths:

| Path | Checks | With a mode the camera does not know |
|---|---|---|
| Value write (what the UI uses) | mode number looked up in `values` | not found: element 0 is written, i.e. i-Enhance, no error |
| Raw group write | none; the argument is a **pointer** to a 64-byte element | an integer passed instead would be dereferenced |
| Raw property write | none | stored as is |

  After a value write the firmware notifies the dependants of 111 (a dependency table), calls the 111 callback, which
  rebuilds the picture mode, and runs the observers. Writing the same value again (Natural over Natural) does not
  rebuild. (static analysis, emulator)
- Availability of a mode (asked by the strip and the control panel): the 111 availability handler applies the picture
  mode restriction rule (e.g. during focus stacking, HDR) and the `COLORTONE_*_ONOFF` table (53 pairs mode -> setting,
  none for 54..65); a disabled mode is replaced by mode 1. (static analysis) OMKit hooks this handler as the
  `OMK_EV_PICTMODE_CHECK` event (see [../../sdk.md](../../sdk.md)).

### 2.3. ART filters and picture modes

ART filters are picture modes of the same setting: raw values from 100, the effect variant in 361 (and the effect
selection in 867 `MODE_ART_EFFECT`). They differ downstream: the job code of an ART still carries the variant
(`0x01vv00nn`), they go to the ART planner of the development job, and the image memory allocator is set to its large
layout (mode 1) for raw values from 100, for raw 8 and raw 12; all other raw values get layout mode 2. The allocator mode
is chosen at every picture mode rebuild from the raw 111. (static analysis, measured on the camera)

### 2.4. Monochrome

- Monotone (raw 5) uses the stock settings 130 (filter) and 131 (toning). The monochrome profile (raw 12) carries filter,
  toning, level, grain and shading of its own. (static analysis)
- Filter values: 0 none, 1 yellow, 2 orange, 3 red, 4 green, 5 green-yellow, 6 magenta, 7 blue, 8 cyan. The settings
  name component of this camera lacks the value names of four of them, but the colour path takes all nine. (static
  analysis)
- The filter is a 3x3 matrix (nine s16, fixed point, every row sums to 256). The firmware finds it in a 32-entry table
  `{u32 code, u32 handler}` searched from index `code & 31` with linear probing, `code = 0x80 | level << 4 | filter`,
  levels 1..3; each handler returns the address of a constant matrix in the firmware image. (static analysis)

### 2.5. Profile leftovers in this firmware

| Present | Missing |
|---|---|
| The COLOR processing table and the whole path to the ISP for raw 11 (12 sector saturations) and 12 (filter, level, grain, shading) | Dispatcher handlers for 54..59 (they lead to the common handler) and entries 64/65 |
| Storage cells for four colour and four monochrome sets, defaults for all of them | Elements 11/12 in the 111 group; 903/989 as group members |
| Descriptors of sets 1..3 (except colour shading 912..915) | Descriptors of set 4, of colour shading, of 1075/1079 |
| Dial memory groups 901/988 for profiles 1..3; 903/989 descriptors `[0..3]` | Value names `MODE_COLORTONE_COLOR_CREATOR_2`, `..._MONOCHROME_CREATOR`, `MODE_SELECT_4` |
| MakerNote PictureMode for profiles 1..3: colour 9/10/11, mono 12/13/14 (profile 4 logs an error and writes 0) | The wrapper's writes of the effective profile values (section 4.4) |
| Live view support: the image module takes the 12 saturations when 903 != 0 and level/grain/shading when 989 != 0 | Shading of the colour profile beyond the shooting parameters (section 5.1) |

(static analysis)

## 3. Dispatch

```
write of 111 -> callback of 111 -> getter(111) = mode number
  -> picture mode parameter builder: reads common settings
       mode > 63 -> common handler; else jump table (64 entries) -> mode handler
       handler reads its settings into a stack frame; common tail -> output structure (20 fields)
  -> wrapper: each output field written to its "effective" setting (value write, part 2)
```

- The builder dispatches on the **mode number**. Modes 0..53 and 60..63 have their own handlers; 54..59 lead to the
  common handler, which reads only 111 and treats the mode as "other". The output structure has 20 fields here (38 on
  firmware with native profiles, whose extra fields carry the profile values). (static analysis)
- Everything below the UI uses the **raw value** from the store or from the shooting parameters, not the mode number:
  the shooting parameter reader, the job code of the still, the i-Enhance branches of the still preparation, the memory
  allocator, the live view image module, MakerNote, in-camera RAW development. (static analysis)
- The rebuild runs on a value write of 111 (the callback, in the task that wrote it) and inside the mode change
  transaction of custom modes (section 6). (static analysis; the transaction trigger - assumption)

## 4. Settings descriptors and the effective values chain

### 4.1. Storage model

- Every setting ID (0..2264, plus 3000+, 4000+, 4500+, 10000+, 15000+ ranges) maps to `{u16 kind, u16 index}`: an
  element of a data object. 111 is kind `0x9481` index 0; the 12 effective saturations are one object `0xf6ea`
  (elements 0..11). Code below the UI often reads objects by kind directly, never naming the ID. (static analysis)
- The general getter takes `(bank, part, ID)`. Part 0 is the user layer, part 1 the working store (raw), part 2 the
  value (descriptor-checked; a write goes to both the user layer and the working store). Bank 0 is the normal course,
  bank 1 the inside of a mode change transaction (working array and snapshot). (static analysis)
- The UI never calls these directly: it uses one **UI settings object** whose method table has write (+0, +4), read
  (+8), availability (+36) and others. All other code (camera side, rules, custom modes, settings export, PTP) reads and
  writes past it. (static analysis, emulator)

### 4.2. Descriptors

- Records of 20 bytes `{u16 id, u16 key, u32 *allowed, u32 *values or 0, u16 bytes, u16 element_size, u8, u8 type}`,
  not sorted; the first record of an ID wins. 1589 records. (static analysis)
- A setting without a descriptor is read and written as a raw value without range checks; the value path cannot set it.
  (static analysis, emulator)
- Profile IDs 903..1016: 72 of 114 have a descriptor. Without one: 907, 911, 912..915, 964..975, 985..987, 993, 997,
  1002, 1006, 1010 (set 4 and colour shading) and the effective values 916..927, 998, 1011. Example: 928 (saturation of
  sector 1, set 1) - `[-5..5]`, u16, type 4. (static analysis)

### 4.3. The chain

```
stored settings --> picture mode build (section 3) --> effective settings (wrapper)
effective settings --> shooting parameter reader --> S (RAM buffer, refilled from a template every time)
S --> copy S -> D (processing parameters): 12 saturations only if the raw mode in S is 11;
                                           level, grain, shading only if it is 12
D --> block B (job parameters) --> colour selector / builder / writer --> ISP   (color-pipeline.md)
S --> MakerNote (PictureMode from the raw mode and the profile number), EXIF
live view: image module context <-- stored settings by kind (raw 111, 903/989, effective values), rebuilt per context
```

- The wrapper writes these effective settings (output field -> ID): 8 contrast, 7 sharpness, 59, 130 filter, 131 toning,
  105, 1806, 306, 393..395, 423, 424, 591, 592, 665, 666, 396, 867. Firmware with native profiles also writes 916..927
  (effective saturations), 1011 (level), 1016 (shading), 998 (grain) and the effective tone; this camera does not.
  (static analysis)
- The shooting parameter reader (99 settings reads) takes only effective settings (8, 7, 105, 59, 130, 131, 916..927,
  1011, 998, 1016, 383/691/382), never the profile cells. All profile parameters of a still are taken from `S`; the
  processing type 11/12 of the colour selector follows the raw mode byte of `S`. `S` is never written to the settings
  store. (static analysis, emulator)
- Live view does not use the reader: the image module builds a context from the store at every new context (about 40-50
  per second, CPU 2) and draws from it every frame. (static analysis, measured on the camera)

### 4.4. Profile settings

| Setting | Colour sets 1..4 | Monochrome sets 1..4 | Range |
|---|---|---|---|
| Sharpness | 904..907 | 990..993 | -2..2 |
| Contrast | 908..911 | 994..997 | -2..2 |
| Shading | 912..915 | 1017..1020 | -5..5 |
| Saturation, 12 sectors | 928..975 (12 per set) | - | -5..5 |
| Tone H / M / L | 976..987 (3 per set) | 1021..1032 (3 per set) | -7..7 |
| Grain | - | 999..1002 | 0..3 |
| Toning | - | 1003..1006 | stock toning list |
| Filter | - | 1007..1010 | 0..8 |
| Level | - | 1012..1015 | 0..3 |
| Profile number | 903 | 989 | 0..3 |
| Effective values | 916..927 | 998, 1011, 1016 | internal |
| Dial memory / dial number | 901 / 1111 | 988 / 1112 | |
| Show in the list (`COLORTONE_*_ONOFF`) | 1072..1075 | 1076..1079 | 0, 1 |

General Highlight & Shadow is 383 / 691 / 382 (high, middle, low). (static analysis)

## 5. Profiles: colour, monochrome, Color Creator, filters

### 5.1. Colour and monochrome profiles

- A colour profile is: sharpness, contrast, shading, 12 sector saturations and its own tone curve (H/M/L). A monochrome
  profile: sharpness, contrast, grain, toning, filter, level, shading and tone. Defaults are in the firmware's defaults
  table (e.g. colour set 2: saturations 1,1,0,0,0,0,1,1,0,0,1,1). (static analysis)
- Downstream of `S`, this firmware copies **shading** further only for the monochrome profile; the colour branch
  drops it, so a colour profile's shading changes neither the picture nor MakerNote. Whether the lower layers would apply
  it for raw 11 is not established. (static analysis)
- MakerNote PictureMode: colour 1..3 -> 9, 10, 11; monochrome 1..3 -> 12, 13, 14. OM Workspace takes a profile from a
  picture by these values (it does not check the camera model). (static analysis)

### 5.2. Color Creator

Mode 21 (raw 9), a stock picture mode of this camera with two cells of its editor item 5029: 615 hue (0..29) and 616
vividness. OMKit does not change Color Creator itself; the film look borrows its editor item (section 9). (static
analysis, emulator)

### 5.3. Monochrome filters

The camera's own matrices are used for monochrome profiles and Monotone (section 2.4). The red, magenta, blue and cyan
matrices of levels 2 and 3 are stronger than the profile reference: they push the factory monochrome profile 3 (red,
level 3) hard. CREATPRF can replace them (section 8.6). (static analysis, emulator)

## 6. Custom modes

### 6.1. Layers and slots

| Layer | Content | Role |
|---|---|---|
| Working store | objects by `{kind, index}` | what processing reads |
| User layer | one object, u32 x 2300 | the source for saving into a slot |
| Myset buffer | one object, u32 x 4600 (two variants x 2265) | staging between the user layer and a slot |
| Slots | one object, 9 slots x 4600 u32 | element `slot x 4530 + variant x 2265 + ID` |

| Slot | Meaning | Name in the settings text format |
|---|---|---|
| 0 | the normal mode's state: saved when the mode dial enters a C mode, restored on leaving it | `Current` |
| 1..4 | Myset1..4 | `Myset1..4` |
| 5..8 | DialMyset1..4 = C1..C4 on the mode dial | `DialMyset1..4` |

"Assign to Custom Mode Cn" writes both Myset n and DialMyset n. (static analysis, emulator)

### 6.2. What a slot keeps

- Saving and recalling go by a **set list** (one of two lists, 1464 / 1463 IDs, chosen by the camera mode type). A
  slot is a full copy of IDs 0..2264, but only the listed ones are ever saved or restored. (static analysis)
- A variants table (64 rows keyed by `MODE_CAMERA` and `MODE_SCENE_NAVIGATION`) gives some IDs a second value per mode
  type: 111, 361, 383/691/382, 886..888. Profile cells and effective values have variant 0 only. Variant 0 of 111 is the
  still modes: `MODE_CAMERA` 3..6, 34, 59, 76, 77, 89, 90, 99, 100 with scene navigation 0; movie and scene modes keep
  their own picture mode. (static analysis)
- In the set: 111, 361, all profile cells 904..1032, effective 916..927 / 998 / 1011 / 1016, H&S 383/691/382,
  886..888, dial memory 898/900/901/988, 1072..1079, 1111/1112. **Not** in the set: 903 and 989 (the profile numbers),
  2149, the editor cursors 2189..2209. Firmware with native profiles has 903/989 in the set, with the same variants as
  111. (static analysis)
- Recall: slot -> Myset buffer -> user layer (by the list), then a transaction: the user layer is copied into a working
  array, and every ID whose working value differs is written **raw** (no descriptors, no group check), followed only by
  the observers; the dependency table and the 111 callback are not called. A value written only into the working store
  (part 1) is rolled back by any mode change. (static analysis, emulator)
- Consequence: on this camera a stock C slot with raw 111 = 11 would restore 11 but not 903, and the group would read as
  mode 63. With OMKit the slot always holds a stock mode (section 8). (emulator)

## 7. How the camera stores a profile

### 7.1. Stores and flash writes

- Four settings stores (F, B, E, M) are created at start. **M** (256 KiB of flash) holds all settings 0..2264 with all
  their layers: working store, user layer, Myset buffer and all nine slots. (static analysis)
- M is written as a whole image, and only if it differs from the flash copy, at: closing the menu (if anything changed),
  every power-off, entering sleep, before a firmware update from the running camera, a factory reset, the crash handler.
  Not on a setting change; no periodic write. Taking the battery out keeps the last written state. (static analysis,
  emulator)
- OMKit's power-off and sleep events can arrive **after** that write (and closing the menu and the crash handler have no
  event at all), so they cannot be used to tidy settings before the flash write. (emulator, static analysis)
- At start M is read as an image with no value checks; if the read fails, a full reset to defaults is done and written
  at once. (static analysis)

### 7.2. Profile cells are stock-safe

Cells of sets 1..3 (904..1029) exist with the same element counts and defaults as on firmware with native profiles, are
saved with M, survive a firmware update, are copied into custom mode slots and reset by a full reset (not by the basic
reset). Stock code never reads them in normal shooting (only the in-camera RAW development has a profile branch, which
its own groups 504/505 never enable). They are not exported to `*.set`. (static analysis)

Rules OMKit follows from this:

| Do not | Why |
|---|---|
| store 11/12 in 111, or 54..59 in 2149 `MODE_RECENTLY_COLORTONE_SCP` | the stock firmware would read an ART filter or an unknown value from flash (section 2.2) |
| write 903 / 989 | no slot or reset list keeps them in step with 111 |
| write the effective values 916..927, 998, 1011, 1016 | values the stock firmware never sets |
| use set 4 | no descriptors, no MakerNote value |
| write colour shading 912..914 outside -5..5 | no descriptor: only raw writes reach it |

### 7.3. Loading settings from outside

The camera's own settings exchange (vendor PTP operations `0x9700..0x970C`, the Wi-Fi `*mysetdata*` CGIs, one common
backend) moves text lines `2,<SETTING_NAME>,<VALUE_NAME>`. The loader accepts a line only if the setting is in its white
list (table A, 1573 IDs; table B gives the value list) - **no profile ID is in it** - and writes: for `Current` into the
transaction snapshot and the working array, then the rules engine and a raw commit (no descriptors); for a slot into the
user layer and then the slot, only for IDs in the set list. Numeric settings take any number. The parse itself writes no
flash; M is written later as usual. The export (`Current` by table A, slots by set list and table A) never contains
profile cells. The camera does not implement `0x970B` (GetSupportedMySetFunction), so OM Workspace does not offer
"Load profile". (static analysis)

## 8. OMKit: CREATPRF

CREATPRF (`[modules] CREATPRF = on`) gives the camera three colour and three monochrome profiles: chosen in the picture
mode screen, edited with the camera's own editor column, applied to stills, live view and video recorded from the still
modes. The driver does the firmware-specific work; `core` owns the profile service; the module remembers the choice.
(OMKit code, measured on the camera)

### 8.1. Virtual picture mode for the UI

| Hook (all data in RAM unless noted) | What it does |
|---|---|
| UI settings object, read method (+8) | after the stock read: while a profile is active and the stored 111 reads Natural (mode 1), returns 54..59; 903 / 989 read the profile number. If the stored mode is not Natural (reset, custom mode recall, remote control), the profile turns off |
| UI settings object, write methods (+0, +4) | 111 = 54..59: the profile is chosen and Natural is stored; another 111: profile off; 2149 never receives 54..59 |
| Availability handler of 111 | lets 54..59 through, otherwise the stock handler |
| Picture mode strip list | a new 47-entry list in the arena: the ten stock modes, colour 1..3, mono 1..3 (copies of the firmware's own entries for 54..59), then the ART filters; the pointer is written before the count |
| Rebuild | storing Natural over Natural does not rebuild, so after a profile change the driver calls the 111 callback itself, in the GUI task, once |

All readers outside the UI see Natural, so nothing non-stock reaches M, slots, `*.set` or the rules. The control panel
cell shows the active profile, but its dial does not step through profiles (choice is in the picture mode screen).
(OMKit code, emulator, measured on the camera)

### 8.2. Shooting

A jump at the end of the shooting parameter reader fills the profile fields of `S` from the cells of the active set:
raw mode 11/12, profile number, contrast, sharpness, 12 saturations (colour), tone H/M/L, filter, toning, level, grain,
shading; overall saturation 0, gradation 1, effect 0. MakerNote then reads Color/Monochrome Profile n. The values are
prepared by the core task (22 settings reads each loop) and published under a sequence counter; the hook only copies.
Not substituted when the raw mode in `S` is not Natural, in an ART bracketing frame, when 111 is not at variant 0
(movie and scene modes), while the block is being rewritten, or for a monochrome shot when the allocator is not in its
large layout (Natural is shot instead of a hang). (OMKit code, measured on the camera)

### 8.3. Live view and the allocator

- Live view: two assembler trampolines (they may run on CPU 2 and touch only one 64-byte uncached block): one replaces
  the image module's read of the raw 111 (Natural 2 -> 11/12), the other copies the profile values into the context
  right after the module filled it from the settings. The core task publishes the block, at once after the rebuild of
  the picture mode that a profile change causes (the GUI task wakes it). On a change within one kind (colour to colour,
  monochrome to monochrome) the old block stays until the new one; on leaving the profiles or a change of kind the mode in
  it is cleared at once, and live view shows Natural until the new block. Until core 1.31 every change cleared it, and
  live view showed Natural for up to one 250 ms pass of the core task. (OMKit code, measured on the camera)
- Allocator: at its setup after each rebuild the raw 111 is reported as 11/12 while a profile is active, so a monochrome
  profile gets the large layout. The grain texture of the film look uses the same hook to request the large layout.
  (OMKit code, measured on the camera)

### 8.4. Editing

- The native editor column of 54..59 writes the profile's own cells (stock settings, value path). The colour column is
  given a list without the shading item (section 5.1). Reads of the effective settings (7, 8, 383/691/382, 1016, 998,
  131, 1011) by the picture mode screen code return the profile's cells instead, so the column shows the profile.
  (OMKit code, measured on the camera)
- The stock Highlight & Shadow screen edits the active profile's tone cells (reads and writes of 383/691/382 on that
  screen are redirected); the control panel's H&S symbol shows the profile tone. In a stock mode nothing changes.
  (OMKit code, measured on the camera)
- Graphic editors (Color/Vivid, Color/Level, curve) and pictures: see [ui.md](ui.md).

### 8.5. The profile service of core

API 6 (`omk_api.h`): `prof_count`, `prof_enable` (only from `module_init`; one owner; installs the hooks), `prof_set`
(refuses with -4 when the stored mode is not Natural), `prof_get`; profiles are `OMK_PROF_COLOR(n)`,
`OMK_PROF_MONO(n)`. The user's choice reaches the owner in the core task, once per change. Once a second `core` checks
that the redirected words are still in place and puts them back if the firmware unpacked its data sections again (sleep,
language change); foreign values are left alone. (OMKit code)

CREATPRF keeps the active profile in its store (key 1, written to the card 1.5 s after a change or at power-off) and
restores it at the next start; if the stored mode is no longer Natural, the profile is forgotten. Without the card or
without OMKit the camera shoots Natural; it also does so for the first ~1.4 s after power-on, before `core` starts.
(OMKit code, measured on the camera)

### 8.6. Monochrome filter matrices

`prof_mono_matrix` (owner only) replaces a filter/level entry of the matrix table with an OMKit handler that returns a
matrix from an uncached block; passing no matrix puts the stock handler back. `prof_mono_matrix_get` reads the camera's
own matrix through the stock handler. CREATPRF's `mono_filters = alternative` computes softer red, magenta, blue and cyan
filters from the camera's own matrices: level 3 becomes the camera's level 2, level 2 the midpoint of the camera's
levels 1 and 2 (rows re-summed to 256). Default `original` keeps the camera's matrices. (OMKit code)

### 8.7. Loading profiles from OM Workspace

The driver extends the camera's own loader (section 7.3), only in RAM (OMKit code; working on the camera with OM
Workspace "Load Color/Monochrome Profile" - measured on the camera):

- registers `0x970B` (answer `p1 = 3`) next to the stock operations at every USB connection; OM Workspace falls back to
  three profiles because `0x970D` is absent;
- adds 88 IDs (profiles 1..3: dial positions 901/988, numbers 1111/1112 and all cells 904..1029) with value lists from the
  settings name component plus its own lists (dial positions, all nine filters); the names are checked during each
  parse, since the component is not in memory when `core` starts;
- applies the lines: cells are written into `Current` the way the loader writes any line; "profile n" (1111/1112) stores
  Natural and chooses the profile as the user would; dial positions are dropped; lines for Myset/DialMyset slots are
  skipped (profiles are not kept in custom modes); a stock line of 111 turns the profile off at the next UI read;
- the export keeps the stock table: files from the camera carry only what the stock firmware knows.

## 9. OMKit: the film look slot

FILMLOOK shows a film look in the place of one stock picture mode of the strip, the **slot** (`[FILMLOOK] slot =`
`ienhance` by default, or `vivid`, `muted`, `portrait`, `monotone`, `underwater`). Natural, Color Creator (its item serves
the look), Custom (no column entry), e-Portrait and the ART filters cannot be slots. (OMKit code)

- **UI.** The same UI object hooks as CREATPRF: while the look is active the read of 111 gives the slot's mode number
  over the stored Natural; writing the slot's mode turns the look on and stores Natural. A profile and the look exclude
  each other. (OMKit code, emulator, measured on the camera)
- **Column.** The editor column table (`{mode + 1, label}`, 32 entries, probed from `key & 31`) gets the Color Creator
  label for the slot, so the column is `[111, 5029]` - the native Color Creator item, plus the look's editors when
  enabled. (OMKit code, emulator)
- **Dials.** On the look's item the cells 615 and 616 are virtual: their writes are swallowed and step the preset at
  once in the GUI task; reads return the last value the UI was given. The 111 = 21 write that the item makes on every
  dial step is swallowed too. Nothing reaches the store. (OMKit code, emulator, measured on the camera)
- **Name and pictures.** The slot's name string (in every language) and its pictures (strip, large, menu line, control
  panel cell, shooting screen sign) are replaced; see [ui.md](ui.md). (OMKit code, measured on the camera)
- **The picture.** The shot is a Natural shot whatever the slot: raw 111 is 2 for every reader outside the UI, so the job
  code is 2 and the i-Enhance analysis does not run. The look's colour comes from the ISP colour table service (the
  preset's `.isp` file, [../../formats/isp.md](../../formats/isp.md); [color-pipeline.md](color-pipeline.md)), live
  view and stills. (static analysis, measured on the camera)
- **Turning on.** `look_set` (the power-on restore) and the UI accept the look only while the store holds Natural. If the
  slot's own mode is stored (e.g. i-Enhance chosen before OMKit), the strip shows the look but the picture is a real
  i-Enhance shot until the look is chosen again. (OMKit code; the shot - static analysis)
- **Presets.** `OMKIT\FILMLOOK\*.ISP` (8.3 or long names), shown by the title in their header, up to 64; the chosen
  preset, the on/off state and per-preset edits are kept in the module's store. API 14..17: `look_enable`, `look_list`,
  `look_set`, `look_get`, `look_name`, `look_picture`, `look_art`, `look_edits`, `look_grain`. (OMKit code)

## 10. Open points

- **Custom modes.** The active profile and the film look are global module state, not per C slot: recalling a slot that
  holds Natural keeps whatever is active; recalling one with another mode turns the profile off at the next UI read. A
  per-slot record (slot, profile, fingerprint of the slot) is designed but not implemented.
- **Resets.** A reset turns the profile off only through the stored 111 changing away from Natural; resets that leave
  Natural keep it. (assumption)
- **Colour profile shading** is dropped by this firmware below `S`; the item is hidden. Whether the lower layers would
  apply it is not established.
- **Movie mode on the dial** (own variant of 111): the profile is not substituted; not verified on the camera. Video
  recorded from a still mode carries the profile (measured on the camera).
- **In-camera RAW development, ART bracketing, Picture Mode Settings menu, remote control** (OI.Share, OM Capture) see
  Natural. (static analysis; remote control - assumption)
- **Control panel dial** cannot step through profiles: its backward search is limited to the stock 41 entries.
  (emulator)
- **Profile 4** is not offered: no descriptors, no dispatcher entries, no MakerNote value on this camera.
- **Flash write guard.** A notify-only check of 111/903/989/2149 at the M write is designed but not implemented; the
  virtual mode makes it a safety net only.
