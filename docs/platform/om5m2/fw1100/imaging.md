# Firmware 1.1: the imaging catalogue

Functions, data and structures of the OM-5 Mark II firmware 1.1 on the imaging side: picture modes and settings, the
picture mode screen and its editors, the ISP colour path, still development jobs and the grain. For each entry: the
address, what it is, how sure we are and where OMKit uses it. The mechanisms are described in the platform documents:
[picture modes](../picture-modes.md), [UI](../ui.md), [colour pipeline](../color-pipeline.md),
[still pipeline](../still-pipeline.md), [grain](../grain.md). The system side (memory, tasks, the memory guard, the
loader) is in [system.md](system.md) and [patch.md](patch.md).

## Conventions

- Addresses are firmware 1.1 addresses. `0x60...`-`0x61...` is the main image (code, ROM data and its RAM sections) through
  the cached alias; `0xa...` is RAM through the uncached alias. A function's address is its entry; the MN10300 has
  instructions of any byte length, so addresses are often odd.
- A "hook site" is the place where OMKit puts a 5-byte jump; "back" is where the trampoline returns, past the displaced
  instructions. The driver checks the bytes of every site before it writes the jump.
- Firmware code and data in `0x60000200..0x60cbe7ff` are watched by the memory guard: OMKit writes there only through the
  uncached alias (see [system.md](system.md)). Tables at `0x611...` (section 7 data) are outside the range.
- `S`, `B`, `ctx`, `h`, `P` are the structures of section 6; offsets are in decimal unless written in hex.
- Basis: **camera** - OMKit uses it on the camera and its effect was checked there, or it was measured on the camera;
  **emu** - run in the emulator (the firmware's own code on a PC); **code** - firmware code, static analysis only;
  **assumption** - a reading not yet confirmed.
- Used by: the OMKit file that relies on the entry (`ispdrv.c` is `src/platform/om5m2/fw1100/ispdrv.c`, and so on);
  "-" means identified, not used.

## 1. Settings and picture modes

| Address | What | Basis | Used by |
|---|---|---|---|
| `0x602f4c5b` | read a setting by its number (raw value; Natural reads as 2) | camera | `drv.c` (`drv_setting_read`) |
| `0x6005faa9` / `0x6005fa7f` / `0x6005fa96` | settings store: getter by key and index / pointer to a value / number of elements | code | - |
| `0x6051e8e8`; `0xa26391bc + 4*id` | settings descriptor table; lazy cache of value pointers | code | - |
| `0x605193e8` | setting ID -> {store key, index}; e.g. 111 -> key `0x9481` | code | - |
| `0x602f4d95` | ID -> {kind, index} select (table `0x605193e8`, IDs up to 2264) | code | - |
| `0x602d2fc2` / `0x602d2fdb` | settings getter / writer by bank (d0), part (d1), ID (a0); tables `0x60478098` / `0x604780b0` | code | - |
| `0x6050dad4` (1589 x 20 bytes); `0x6050de44` | settings descriptor table; the descriptor of 111 (a group, type 6) | code | - |
| `0x60472158`; `0x60472664`; `0x602d131e` | group table (34-byte entries); the group of 111 = {111, 361}; group read (raw -> mode number) | code | - |
| `0x602d1de4`; `0x60496130` | post-write notification by the dependency table (440 entries) | code | - |
| `0x604db258` | settings defaults (base + 4 x ID) | code | - |
| `0x604991f8` | `COLORTONE_*_ONOFF` table (53 pairs) read by the availability handler of 111 | code | - |
| `0x602e00bf` / `0x602e010d` / `0x602e06ca` | picture mode parameter builder / its dispatcher (jump table `0x60499718`, 64 modes) / the common handler | code | - |
| `0x602e082f` | wrapper that writes the effective settings after the builder | code | - |
| `0x60bb6ad0` | method table of the UI settings object: `+0` write `0x608efc53` (d0 id, d1 kind, a1 value), `+4` write `0x608efeea`, `+8` read `0x608ef257` (d0 id, a1 out) | camera | `pictprof.c` (the table words replaced) |
| `0x608ef279`, `0x608ef4a1`, `0x608eee71`, `0x608ef2b4` | UI settings object `+24` count of values, `+28` value by index, `+36` availability, `+152` default | code (addresses), assumption (meaning) | - |
| `0x604785a8` | availability table entry `{111, handler}`; handler `0x602d7964` | camera | `pictprof.c`, `drv.c` (event `OMK_EV_PICTMODE_CHECK`, back `0x602d7969`) |
| `0x602d78a3` | availability check of a picture mode; answer `0x100`: a rule or ONOFF replaced the mode | code | `drv.c` |
| `0x602dd18d` | stock callback of setting 111: rebuilds the picture mode (`calls` convention) | camera | `pictprof.c` (`f_mode_changed`) |
| `0x6031a179` | after a picture mode rebuild: trace marks, then synchronously `0x604262be` -> `0x60426608` (allocator mode), then `0x602e9482` | code | - |
| `0x60426608`; site `0x60426675` (back `0x6042667c`) | picture mode rebuild reads the raw 111 and sets the image allocator mode: raw >= 100 (ART), 8 and 12 -> mode 1 (large layout), else mode 2 | camera | `pictprof.c` (`drv_alloc_filter`) |
| `0x604131dc` / `0x604132b7`; `0xa263fbd4` | image allocator mode (0 normal, 1 ART, 2 high ISO): set / get; the uncached u16 holding it | camera | `pictprof.c` (read only) |
| `0x604eeae0` (41 x 64 bytes), `0x604e1a6c` | raw 111 <-> UI mode number (`allowed`, `values`): 7<->7 i-Enhance, 1<->0 Vivid, 2<->1 Natural, 3<->2 Muted, 4<->5 Portrait, 5<->3 Monotone, ART from 100 | code | - |
| `0x604daf4c` (64 rows, end `0x604db24c`) | picture mode variant per camera mode (`MODE_CAMERA` 26, `MODE_SCENE_NAVIGATION` 109): movie and scene variants keep their own mode | code | `pictprof.c` (camera modes list) |
| `0x6037f16a` | picture mode -> processing code: raw 111 < 100 -> the raw value; ART `0x01vv00nn`; scenes `0x0200nnnn` (hash `0x6052df28`); special 1..4 -> 2000..2003 | emu | `ispdrv.c` (which codes get the look) |
| `0x604237fc` / `0x60423663` | job code (`P+8..11`) from S and the record request; job type `P+16` from the record mode | code | - |
| `0x602f5243` / `0x602f5396` | write a line of the Current settings: the snapshot / the working array (`call ...,[d2],4`; d0 ID, d1 value) | camera | `import.c` |
| `0x602f5173` | write a setting into a user slot (Myset) | code | - |

### Picture mode screen lists

| Address | What | Basis | Used by |
|---|---|---|---|
| `0x61199b58` | picture mode screen lists `{u32 id, u32 count, u32 list}`, 41 entries; the list of 111 at `0x61199464` (entries of 20 bytes `{mode, ONOFF id, mode, variant, availability}`) | camera | `pictprof.c` (strip with the profiles before the ART filters) |
| `0x61199b08` / `0x61199ab8` | stock entries of modes 54..56 (colour profiles) / 57..59 (monochrome profiles) | camera | `pictprof.c` |
| `0x611b8d1c` | mode names `{mode, string number}` (7 -> 145 "i-Enhance", ...) | camera | `pictprof.c` (film look slot name) |
| `0x611a19fc` | Super Control Panel picture mode list (41 entries) | emu | - |
| `0x608eedd3` | getter of the UI settings object (object `0x6218e9f8`) | code | - |
| `0xa407f540` | the loaded UI language: `+0` array of string pointers, `+4` its end; 4909 UTF-16LE strings; reloaded on a language change | camera | `pictprof.c` |

## 2. Shooting parameters and live view

| Address | What | Basis | Used by |
|---|---|---|---|
| `0x602c8139`; site `0x602c8ad6` (back `0x602c8adb`) | shooting parameter reader: fills the buffer `S` (d3 at the site) | camera | `pictprof.c` (`drv_tramp_take`: profile fields into S) |
| `0x620ef32c` | template of `S` (31708 bytes) copied into `S` at the start of every read; filled at start by `0x60331b87` | code | - |
| `0x620d12b4` | the reader's client buffers: 33 x 20 bytes, `S` pointer at `+12`; given by `0x602bc593(id)` | code | - |
| `0x602c8821`, `0x602c8851`, `0x602c8877` | inside the reader: raw 111 read; 11 -> 903, 12 -> 989; 12 reads of the hue saturations into `S+28740` | code | - |
| `0x602c9dee`; `0x60471610` | ART bracketing frame table: sets `S+29496` = 1 | code | `pictprof.c` (no profile in such frames) |
| `0x60421a65`, `0x6041eeea` | S -> D copy: the 12 saturations when raw 111 = 11 (`D+5538`); level, grain, shading when 12 (`D+5550..5552`) | code | - |
| `0x6038aac0` (`0x6038ad84`) | request B builder: `D+5538` -> `B+1816` (the 12 saturations) | code | - |
| `0x6041924c` (jump table `0x6057def0`) | MakerNote PictureMode from `S+28735` / `S+28765` | emu | - |
| `0x60343fe3` | live view `set_imgprm` handler (per frame, CPU 2) | camera | - |
| `0x6033e03e` / `0x6036114f` | image module context: update from the store / activation | code | - |
| context `A` | `+260` mode, `+263/264` filter / toning, `+705` colour profile number, `+706..729` 12 saturations, `+730` mono profile number, `+731` level, `+732` shading, `+733` grain, `+792..794` tone | camera | `pictprof.c` (LVA) |
| `0x60323a64` (back `0x60323a6a`) | API `0x80001` (half press) | camera | `drv.c` (event `OMK_EV_S1_API`) |
| `0x6034f966` | image module's reader of the raw 111 (`mov (0xa26eca5c),a0; movbu (a0),d0; retf`) | camera | `pictprof.c` (LVR: Natural becomes the profile's 11/12 in live view) |
| site `0x6033e131` (back `0x6033e139`) | right after the image module's context was filled from the settings | camera | `pictprof.c` (LVA: profile values into the context) |
| `0xa26e5766` / `0xa26e5768` / `0xa26e57b4` | image module contexts: last slot, active slot, 5 slots of 988 bytes (live view reads the active one every frame) | camera | `pictprof.c` (read only) |
| `0x6034f60f` / `0x6034f63d` | image module: build a pointer array / copy by a table `{kind, type, offset}` | code | - |
| `0x60355a15` / `0x603454c2` | live view: P from the request R / effects dropped when the context mode `A+260` < 100 | code | - |
| `0x6035680c` -> `0x6038a315` (gate `0x6038a4cf`) | live view resource pre-creation (the texture for video) | code | - |

### Fields of `S` (shooting parameters) OMKit writes or reads

| Offset | Field | Basis |
|---|---|---|
| 28694 u16 / 28700 u16 / 28702 u16 | sharpness / contrast / gradation | camera |
| 28705 u8 | effect level (i-Enhance 306) | camera |
| 28732 u16 | saturation | camera |
| 28735 u8 | raw picture mode (11 colour profile, 12 monochrome profile) | camera |
| 28736 / 28737 u8 | monochrome filter / toning | camera |
| 28740 u16 x 12 | the 12 hue saturations of a colour profile | camera |
| 28765 / 28766 / 28767 / 28768 u8 | profile number / filter level / grain / shading (dropped by the firmware for colour profiles) | camera |
| 29469 / 29470 / 29471 u8 | tone Shadows / Highlights / Mid (H&S) | camera |
| 29472 / 29473 u8 | ART variant (assumption) / ART effect index 0..9 | code |
| 29496 u8 | the reader marks an ART bracketing frame | camera |
| 28604 | file type: 0 JPEG, 1 JPEG (other IFD lists), 2 ORF | code |
| `0xf7c` | EXIF UserComment: 125 bytes = 8-byte character code + 117 characters; `0xff9` DateTimeOriginal follows | camera |
| `0x5a8`, `0x568`, `0x5f2`, `0xf12` | ImageDescription (32), Artist, Software, Copyright | code |

### Profile cells (settings) and the profile's IDs

| ID | Meaning | Basis |
|---|---|---|
| 111 | picture mode (`MODE_COLORTONE`); UI modes 54..56 colour profiles 1..3, 57..59 monochrome profiles 1..3 | camera |
| 903 / 989 | which colour / monochrome profile (only the SCP cell reads them) | camera |
| 904+k, 908+k, 912+k | colour profile k: sharpness, contrast, shading | camera |
| 928 + 12k + i | colour profile k: saturation of hue i (12) | camera |
| 976 + 3k | colour profile k: tone Hi, Mid, Sh (-7..7) | camera |
| 990+k, 994+k, 999+k, 1003+k, 1007+k, 1012+k, 1017+k, 1021+3k | monochrome profile k: sharpness, contrast, grain, toning, filter, filter level, shading, tone | camera |
| 2149 | `MODE_RECENTLY_COLORTONE_SCP`: the UI writes the current mode here on OK | camera |
| 2189+k / 2193+k | INFO chose Mid in the tone curve editor (colour / monochrome) | camera |
| 2197..2200 / 2201 | Color/Vivid "one hue / all" flag per mode / the hue cursor (928 + k) | camera |
| 382 / 383 / 691 | effective tone Sh / Hi / Mid (the H&S screen) | camera |
| 615 / 616 | Color Creator dial cells: hue 0..29 (rear dial) / vividness (front dial) | camera |
| 901, 988, 1111, 1112 | profile switch position and its profile number (absent on this camera; accepted by the import) | code |
| 2148 | the picture mode screen's item under the cursor (111 the strip, 5029 the item) | camera |

## 3. Custom modes and the settings stores

| Address | What | Basis | Used by |
|---|---|---|---|
| layer objects `0x85d4`, `0x8b64`, `0x8b54` (caches `0x620e8544`, `0x620e8548`, `0x620e854c`) | custom mode slots, the Myset buffer, the user layer | emu | - |
| `0x602f50c7` / `0x602f50f8` / `0x602f5121` / `0x602f514d` / `0x602f5173` | slot get / Myset buffer get / put / user layer get / put | code | - |
| `0x604df6a0` / `0x604e0210`; `0x602d2e97` | the set lists (1464 / 1463 IDs) and their chooser | code | - |
| `0x602e1153` / `0x602e1071` | save the user layer into the Myset buffer / restore it, by list | emu | - |
| `0x6091c4b9`; `0x602d0345` -> `0x602db10e` -> `0x602f504f` | "assign to Cn"; recall of Cn | code | - |
| `0x602f5719`, `0x602f543d`, `0x602f5524`, `0x602f5593`, `0x602f566b` | settings transaction: flag, begin, ..., raw commit, end | emu | - |
| `0x602e0de5`; `0x602d0456`, `0x602d0487`, `0x602cfdde` | full reset to defaults; basic / full / factory reset | code | - |
| `0x6005ec3b..0x6005edba`; `(0xa26c4350..0xa26c435c)` | creation of the stores F, B, E, M; their descriptors | code | - |
| `0x6005d686` | store write: compares the image with the flash first | code | - |
| `0x602f572b` | API `0xF001E`: write E and M (power off, sleep, factory reset, before a firmware update) | emu | - |
| `0x602f6a72` -> `0x600604d7`; `0xa26c857a` | API `0xF0043`: write M if dirty (menu close); the dirty flag | emu | - |
| `0x602d08f7`; `0xa26c8578` | start: a read failure -> full reset and write | code | - |

## 4. Monochrome filters

| Address | What | Basis | Used by |
|---|---|---|---|
| `0x603e4f7f` | filter matrix lookup: hash table `0x605756d0`, 32 x `{u32 key, u32 handler}`, index `key & 31`, key `0x80 \| level << 4 \| filter`; the handler returns the matrix in a0 (`mov matrix,a0; retf`, shared `retf` at `0x603e5097`) | camera | `pictprof.c` (entries replaced for the alternative filters) |
| `0x605522e2..0x605524c8` | filter matrices: 3x3 s16, scale 256, 18 bytes each | camera | `pictprof.c` (`drv_prof_mono_matrix_get`) |
| `0x60389abf..0x60389aed` | filter code of the monochrome profile: for processing type 12 `0x80 \| (level & 3) << 4 \| filter`, level 0 -> 0 | code | - |
| `0x604181f0`; `0x6057de10` | MakerNote: filter 0..8 -> code through a jump table | code | - |
| `0x603e8d4f` (call `0x603e8e9c`) | monochrome toning colour: switch 0..4, otherwise as 0 | code | - |
| `0x60552e54..0x60552e6d` | monochrome and toning `S+76` `{on, Cb, Cr, Cb, Cr}`: Monotone `0x808001`; toning 1..4 `0x8d7501`, `0x7a8b01`, `0x858301`, `0x7a7901` | emu | - |

## 5. Picture mode screen and editors (GUI)

| Address | What | Basis | Used by |
|---|---|---|---|
| `0x60e375d2` | the column function of the picture mode screen: picks the item list by a jump table `{mode + 1, label}` at `0x6119cee0` (32 entries, index `& 31`); common part after the list and count `0x60e376d5` | camera | `pictprof.c` (`drv_column_color`, film look column) |
| `0x60e376c3` | column label of the colour profiles 54..56: list `0x6119a0f0`, 6 items {111, 5374, 5375, 5376, 7, 8} | camera | `pictprof.c` (replaced by a list of five without shading) |
| `0x60e3769d`; `0x6119a0ac` | column label of Color Creator: list `[111, 5029]` | camera | `pictprof.c` (the film look slot's column) |
| `0x60e376b9`; `0x6119a0d0` | column label and list of the monochrome profiles | code | - |
| `0x60e37671`, `0x60e37679`, `0x60e37683`, `0x60e37693` | column labels of i-Enhance; Vivid, Muted, Portrait; Monotone; Underwater | camera | `pictprof.c` (the film look slots) |
| `0x611988c0` | panel event table of the picture mode screen `{handler, key}`; key 31 -> the column function | code | - |
| `0x611b79a8` | item redraw handler table: 32 x `{u32 key = item - 6, u32 branch}`; entries for 5373 `0x611b7a60`, 5374 `0x611b7a68`, 5375 `0x611b7a70`, 5029 `0x611b7aa0` | camera | `pictprof.c` (`item_put`) |
| `0x60e49e3a` -> `0x60e4af24` | dispatcher branch and handler of item 5373 (Color/Level) | camera | `thunks.s` (`drv_item_level`, back `0x60e49e4a`) |
| `0x60e49e31` -> `0x60e4ac9b` | ... item 5374 (Color/Vivid) | camera | `thunks.s` (`drv_item_vivid`) |
| `0x60e49e43` -> `0x60e4b018` | ... item 5375 (tone curve) | camera | `thunks.s` (`drv_item_curve`) |
| `0x60e49e1f` -> `0x60e4ab3a` | ... item 5029 (Color Creator) | camera | `thunks.s` (`drv_item_cc`) |
| `0x60e4a60d` / `0x60e4a94a` | column item handlers by kind: lists (5377 grain, 130, 131, ...) / sliders (5376, 7, 8, 59); "editor" branch `0x60e3daef` only for 5029, 662, 5374, 5373 | emu | - |
| `0x60e3d091`, `0x60e3d0c3`, `0x60e3d0f5`, `0x60e3d127`, `0x60e3d159` | getters of the editor item IDs by mode (Color/Vivid, Color/Level, grain, curve, shading); they read 111 through method `+8` (returns `0x60e3d0a7`, `0x60e3d0d9`, `0x60e3d10b`, `0x60e3d13d`, `0x60e3d16f`) | camera | `pictprof.c` (returns of the Color/Vivid and curve getters recognised) |
| `0x611a02d8`, `0x611a02b8`, `0x611a0318`, `0x611a02f8` | `{mode, ID}` tables: Color/Vivid 5417..5420, Color/Level 5413..5416, curve 5421..5428, grain cell {57: 999, 58: 1000, 59: 1001, 65: 1002} | camera | `pictprof.c` |
| `0x611b59a0` | "one / all" flag cell per mode {54: 2197, 55: 2198, 56: 2199, 64: 2200} (key 901, read from `0x60e4ad1c`) | camera | `pictprof.c` |
| `0x611a0358`; `0x611b59c0..0x611b5ad0` | `{mode, cell}` tables of shading; filter and level, toning, sharpness, contrast, tone | code | - |
| panel `+4278` | the item's background block: `+2` rear value text, `+8` Reset hint flag, `+9` button kind, `+10` background id `0xfff8`; panel `+17` = 1 | camera | `thunks.s` (`drv_item_*` give it the Color Creator background) |
| `0x611b4cd0..0x611b4ce8` | item picture records `{u16 3, u16 number, u32 text}`: 5029 -> 11397, 662 -> 11398, 5374 -> 11397 (word `0x611b4ce2`), 5373 -> 14539 | camera | `pictprof.c` (`REC_VIVID_PIC`) |
| `0x6120e9dc` | wheel editor records (`Plural_Direct_Actuator`): 139 x 56 bytes, `+0` item ID, `+4..+52` handlers | code | - |
| `0x60e9b815` and `0x60e9bdba..0x60e9e88a` | wheel editor parent (204 bytes, methods `0x61211c90`): factory, enter, step up/down, events, INFO | code | - |
| `0x60e2c008`, `0x60e2c539`, `0x60e2e90f` | wheel editor child (`Cursor_Move_Set_Mode_Actuator`, 160 bytes): factory, refresh from settings, event handler (`0x971d` refresh, `0x900c` write the cell) | code | - |
| `0x61210d14`; `0x61198024` | editor child descriptors (`0x1529` -> 2201, 928..939); child functions by setting ID (928..975 -> `0x60e30ddf`) | code | - |
| `0x610c1774` / `0x610c181c` | picture state words of the dial labels (widgets `0x3d9` front, `0x3db` rear) of the Color Creator layout | camera | `editor.c` |
| `0x610c174c` / `0x610c1760` | picture state words of the dial pictograms (widgets `0x181` over 0x3d9, 10922 the rear dial's; `0x3d8` over 0x3db, 10923 the front dial's); the camera's H&S screen: `0x61005124` 10923 over "Sh", `0x6100534c` 10922 over "Hi" | code; emu | `editor.c` (swapped on the curve item) |
| `0x610c189e` / `0x610c18ce` | rectangles `{x, y, w, h} x 2` of the dial label widgets 0x3d9 (over the rear dial) / 0x3db (over the front dial) | camera | `editor.c` (widened) |
| `0x610c1788 + 16k` | colour sample next to "Color" (pictures 4227..4233) | camera | `editor.c` (blanked on the look's item) |
| `0x610c1808` | comma after the sample (widget `0xf6d`, picture 10834); drawn on the items 5373, 5374, 5375 too, where no sample is | camera; emu | `editor.c` (blanked on every OMKit editor item) |
| `0x610c1830`, `0x610c1840`, `0x610c1850` | frame of the Reset button (10180, 10181, 10180) | camera | `editor.c` (blanked on the look's item) |
| `0x610c4076` / `0x610c3e96` | monochrome column: shading item icon / value rectangles (5 x `{x, y, w, h}`) | camera | `pictprof.c` (moved to this camera's rows) |
| `0xa234f838` | GUI state (uncached): byte 0 group, byte 1 state (`02 02` direct setting screen, `04 03` SCP, `04 18` direct picture mode screen, `01 08` shooting screen) | camera | `pictprof.c`, `editor.c`, `drv.c` (the power probe) |
| `0x61e589c8` | the setting of the direct setting screen (`0x60ef6451`) | camera | `pictprof.c` (H&S screen edits the profile's tone) |
| `0x60f01fb3..0x60f01fdb` | the H&S screen's graph reads | camera | `pictprof.c` |
| `0x610784d4` / `0x6107852c` | SCP picture mode cell widget descriptors (the second one's states 8..23 -> pictures 15746..15761) | code | - |
| `0x60eacb2a..0x60eacdac`; `0x60f15950` / `0x60f158e0` | shooting screen picture mode sign; its edited-state checks (colour / monochrome) | emu | - |
| `0x9c000000 + 0x178` | video compositor: OSD scale (screen or viewfinder) | camera | `drv.c` (log only) |
| `0x60e13572` | `GuiCallerImp::sendEvent` (index 1..344 -> code by `0x6116d664`); index 137 hangs the GUI - never send | emu | - |
| `0x60f10ba8` / `0x60f10bfa` | GUI `PostEvent` (d0 code, d1 data) / `PostEventToCA` | code | - |
| `0x60d52207` | GUI queue `GESetEvent`: 500 entries; overflow is dropped silently | code | - |

### Pictures

| Address | What | Basis | Used by |
|---|---|---|---|
| `0xa3091004` -> `0xa3435ccc` | the UI picture table (word 1 of record 0 of resource block `0x0101`, loaded at boot): 17006 pointers to 24-byte objects `{14, w, h, 8, 0, size, data, 0}`, data an OSD `BITMAP` command format `0x0B`; unused numbers point to one stub `0xa3435cb8` | camera | `pictprof.c` (module pictures replace stub words) |
| - | `BITMAP 0x0B`: 12-bit codes {palette index 8 bits, repeat - 1 4 bits}, five to a 64-bit word, a run does not cross a row | camera | `pictprof.c` (`encode`) |
| - | picture numbers: profile mode icons 14527 (colour) / 14515 (mono) + 3(n - 1) + variant (strip, large, large edited); wide labels 14583 / 14575 + 2(n - 1) + edited; SCP cell 15746..15761 and shooting screen sign 15706..15721 (edited +4); editor column 14539..14574; dial labels 4215, 4216, 4220..4224, 4339; dial pictograms 10922 (rear), 10923 (front); module stubs 4234..4241 | camera | `pictprof.c`, `editor.c` |
| `0x611b4960..`, `0x611b4ee2..`, `0x611e69dc`, `0x61215724`, `0x61073320..` | picture records of the mode slots: strip, large, menu line, shooting screen pairs, SCP | code | - |
| - | film look slots: pictures {strip, large, menu label, SCP cell, shooting sign} i-Enhance {6408, 6678, 6596, 10584, 11226}, Vivid {6409, ...}, Muted {6411, ...}, Portrait {6412, ...}, Monotone {566, 548, 1505, 10551, 11193}, Underwater {6413, 6570, 6569, 10547, 11189} | camera | `pictprof.c` (`look_slots`) |
| `0x60f57a20` | Color Creator picture sizes (resource group `0x8006`) | code | - |

### Drawing (C3D, OpenVG)

| Address | What | Basis | Used by |
|---|---|---|---|
| `0x60ccf4f7` (back `0x60ccf4fe`) | entry of the Color Creator renderer (`C3D_MAIN` task, CPU 1) | camera | `thunks.s` (`drv_tramp_c3d`: our editor drawn instead of the ring) |
| `0x60ccf0f8` | the C3D renderers' `eglSwapBuffers(display, surface)` | camera | `editor.c` |
| `0x625b475c` / `0x625b4760` | `C3D_MAIN` surface width / height | camera | `editor.c` |
| `0x625b4744` / `0x625b4748` / `0x625b4750` | `C3D_MAIN` EGL display / context / surface | code | - |
| API `0x10308` / `0x10309` (`0x608deb35` / `0x608de7b5`); `0x60887956`, `0x60888668`, `0x60886f46`; `0x60887fba`; `0x608882f9`; `0x6030f09a` | the GUI's drawing order chain: background control start / step / end, order builder, send, START to the camera side | emu | - |
| task `0x44` (`0x60cd0314`, parse `0x60cd039d`); task `0x1002` `C3D_MAIN` (`0x60ccea04`) | C3D library interface task (CPU 0) and the C3D renderer task (CPU 1) | code | - |
| `0x60293bc0` / `0x602939d8` | name/address tables of OpenVG/VGU/DMP (103) and EGL (44); a direct entry is the address past the prologue | camera | `fw.ld` |
| `0x613dab09`, `0x613dab37`, `0x613dabd5`, `0x613dacf8`, `0x613dab53` | `vgCreatePath`, `vgClearPath`, `vgAppendPathData`, `vgDrawPath`, `vgDestroyPath` | camera | `editor.c` |
| `0x613dad41`, `0x613dadc0`, `0x613dad89`, `0x613dad5a` | `vgCreatePaint`, `vgSetColor`, `vgSetPaint`, `vgDestroyPaint` | camera | `editor.c` |
| `0x613da315`, `0x613da331`, `0x613da2f9`, `0x613da235`, `0x613daad8` | `vgSeti`, `vgSetfv`, `vgSetf`, `vgLoadIdentity`, `vgClear` | camera | `editor.c` |
| `0x613cbfa6`, `0x613cb6d3`, `0x613cbb2f`, `0x613cb197`, `0x613cc6b0` | OpenVG image filters (Gaussian, Convolve, SeparableConvolve, ColorMatrix, Lookup): software, floating point | code | - |

## 6. Loading profiles from outside (USB/PTP)

| Address | What | Basis | Used by |
|---|---|---|---|
| `0x614c0156` | PTP vendor operation registration (d0 code, d1 data phase, a0 handler, a1 0); the set is cleared and registered again at every USB connection | camera | `import.c` (0x970B added) |
| site `0x614ba920` (back `0x614ba926`) | the registration sequence, at `mov 0x970c,d0` | camera | `import.c` |
| `0x61609ecb` | settings text loader (backup protocol; PTP and Wi-Fi alike) | camera | - |
| site `0x6160a694` (back `0x6160a69a`; return `0x6160a850` = the value parse) | ID -> table B entry | camera | `import.c` (our entries for the profile IDs) |
| site `0x6160a7b7` | miss of name -> ID | camera | `import.c` (our names) |
| site `0x6160a087` (back `0x6160a08d`); `0x6160a135` | a parsed line about to be applied; the tail of a line | camera | `import.c` (profile lines applied by the driver) |
| `0x6160ac3c` (1573 u16) / `0x6160b888..0x61611ad8` | white lists: table A (IDs), table B (16-byte entries `{type, 0, u16, name offset, list offset, count}`) | camera | `import.c` |
| `0xae563750` / `0xae589740` | component `0x106` in RAM: setting names (52 bytes each) / value lists `{s32, char[52]}`; not yet present when core starts | camera | `import.c` |
| `0x61609813` | settings export to text (keeps the stock table) | code | - |

## 7. Colour path (ISP)

| Address | What | Basis | Used by |
|---|---|---|---|
| `0x6037bd59` | imgpro register write dispatcher (pipes A `0x9f00....`, B `0x9f80....`); the colour writer and the colour table loader run in the same call (A `0x6037be2a` -> `0x6037be36`, B `0x6037be4c` -> `0x6037be58`) | code | - |
| `0x6037c519` (back `0x6037c51f`) | colour register writer: a0 register base, a1 `S`, a3 `ctx`, d3 the job's register mask; `S+x` -> base + x + 64 | camera | `ispdrv.c` (H2: tables into S, census, grain mask) |
| `0x6037ccd2` | colour table loader (a0 table window base, a1 ctx): the tone `S+2608` every call; gamma and sector weights on a cache miss (`0xa26ed720[pipe]`, under cross-CPU lock 4) or bits 0/1 of `ctx+108` | camera | `ispdrv.c` (`ctx+108 \|= 3` forces a reload) |
| `0x603e8fba` (back `0x603e8fbf`) | colour builder of a job (a0 ctx): matrix, gamma, Cb/Cr, sectors | camera | `ispdrv.c` (H1: `ctx+60 = -1` forces a full rebuild) |
| `0x6037a810` | fills the job ctx: `+12` S (`B+1484`), `+56` ext, `+60` change mask `0x60379ea7(S, B)`, `+64` H&S | emu | `ispdrv.c` (offsets) |
| `0x603e2e67` | log gamma branch of the build (`B+1808` 1..3) | code | `ispdrv.c` (log jobs left alone) |
| `0x60379ea7` / `0x6037a15e` | change fingerprint: compare B with the copy in `S+5672..5735` -> mask `ctx+60` / write the copy | code | - |
| `0x603e2d71` | S build dispatcher by `B+16`, `B+28`: full build `0x603e933a`, others `0x603e936d`, `0x603e9397`, `0x603e93b5` (bypass), `0x603e9409`, `0x603e9421` | emu | - |
| `0x603e509a` / `0x603e4f0c` / `0x603e4cad` | colour matrix 3x3 Q7: build / row normalisation (rows sum to 128) / 3x3 Q8 multiply | code | `ispdrv.c` (same normalisation) |
| `0x604242b2` | AWB matrix -> D | code | - |
| `0x603e644a` / `0x603e6974`; `0x603ed1a8` / `0x603e63f3` | gamma choice / R, G, B gamma pointers; gamma/tone flag by `B+16`, `B+28`, `B+48` / gamma pair by `B+14` | code | - |
| `0x603e73a7` -> `0x603e6f79` -> `0x603e7214` | Cb/Cr gains (saturation k): from the i-Enhance descriptor or the mode and saturation, x 144/182, weakened by ISO | code | - |
| `0x603e7f72` / `0x603e85de` / `0x603e863e` / `0x60385f08` | tone: choice / build (ROM curve or ISO remap into `S+2612`) / tone collector (H&S copy, `S+2608 = S+2612`) / H&S spline | code | - |
| `0x603e80d2` -> `0x603e80dd` | tone selector, handler of code 12: at stage `0x900` the mask curve `0x60545fde` instead of the tone; effect `0x101` -> no curve, stage `0xF00D` -> Natural | code | `ispdrv.c` (grain mask route) |
| `0x60423a0e` / `0x60385c47` | H&S: `S+29469..29471` -> `D+5056` -> `B+1512` / -> `ctx+64` (x 4096/7) | code | - |
| `0x603e7e29` | sector selector: i-Enhance descriptor -> `0x603e776b` (whole table); code 11 -> profile `0x603e7c3f` (12 saturations `B+1816..`, gains `0x60575220`); otherwise the mode `0x603e78fd` | code | - |
| `0x60383ccf`, `0x60384b60..0x60384bcb` | sector table copy in RAM with smooth value changes; builds the "node + slope" words | camera | `ispdrv.c` (same word format) |
| `0x603e5ed6` / `0x603e5e66` / `0x603e5ebd` | "six axes" `S+100..123`: choice by code (hash `0x60575820`) / write / clear | code | - |
| `0x603e8744`, `0x603e8e7e`, `0x603e5db2` | `S+68/72`; monochrome `S+76`; block `S+496..687` by ISO | code | - |
| `0x6037ccad` | microcode load of block `0x9c00` (3369 words from `0x6052a8a4`) | code | - |
| `0x6041eaae` | copy S -> D before recording; for raw 111 = 7 calls the i-Enhance analysis `0x6041ed09` | code | - |
| `0x60423b09`; `0x60386d82` | i-Enhance frame analysis: an auxiliary imgpro job | code | - |

### Colour path data

| Address / offset | What | Basis | Used by |
|---|---|---|---|
| `S` (a1 at `0x6037c519`) | colour structure of a development job, 5776 bytes: `+4` flags (bit `0x100` tone off), `+80` matrix s16[9] Q7, `+124/126` Cb/Cr gains, `+688` sector records 12 x 24, `+976` sector mask, `+2596` gamma pointers R, G, B, `+2608` tone pointer, `+2612..3123` tone work buffer, `+3124` 10 sector weight table pointers | camera | `ispdrv.c` |
| `ctx` | job context: `+4` B, `+8` register image slot, `+12` S, `+52` job code, `+56` ext (persistent S), `+60` change mask, `+64` H&S, `+108` reload bits | camera | `ispdrv.c` |
| `ctx+52` | job code `B+8 << 24 \| B+9 << 16 \| u16 B+10`: picture mode raw 111, ART `0x01vv00nn`, scenes `0x0200nnnn`, 2000..2003 | camera | `ispdrv.c` |
| `B+16`, `B+17`, `B+28`, `B+1808`, `B+1809` | job kind (0 development), pipe (1 A, 2 B), subtype (low byte `0x01` live view, `0x07` still), log gamma, HLG | camera | `ispdrv.c` |
| `B+1795..1804` | sharpness, contrast, gradation, saturation, noise filter, colour space, -, monochrome filter, toning, ... | code | - |
| `0x60533338` / `0x60533138` | Natural gamma R/G/B (all ISO) / the other gamma of the pair | camera | - |
| `0x6053ff42` / `0x6053fd42` | Natural tone (ISO 200..1600) / linear tone `round(i * 1023 / 255)` | camera | `ispdrv.c` (`TONE_LINEAR` fallback) |
| `0x60540342`, `0x60541742`, `0x60540b42`, `0x60541142`, `0x60540f42`, ... | tones of Muted, Portrait, Underwater, contrast +2 / -2, ... | code | - |
| `0x60555ed4` / `0x60561040` | sector tables of Natural / the colour profile: 12 records of 28 bytes (h0, hw, c0, cw, y0, yw, A_hue/A_sat/A_lum, inverse widths), half tables G1..G5 `t+336..t+2832`, mask `t+3024` | emu | - |
| `0x605748fc..0x60575220` | sector table descriptors per mode (15) | code | - |
| `0x60575220` | sector profile gains per half and k (pairs for "+" and "-" user saturation) | code | - |
| `0x605748d0`; `0x60551bd8` | Cb/Cr gains (Q8, 282 at 0); weakening by ISO | code | - |
| `0x9f001000` / `0x9f805000` | colour block registers, pipes A / B | code | - |
| `0x9f00a400` / `0x9f00d000` | colour table windows (gamma, tone, sector weights), pipes A / B | code | - |
| `0x620f71d8`, `0x6259f390`, `0x625c89bc` | per-CPU pools of `S` for jobs that are not development jobs (9596-byte slots) | code | - |
| `ctx+196` | the pipe (0 A) that the loader's cache is indexed by | code | - |
| `0xa26ed720` | table loader's cache: last ext per pipe | code | - |
| `0x8ff001cc + (n >> 8)`, bit `n & 0xff`; `0x603eac01`/`0x603eac3b`/`0x603eac5d` | cross-CPU lock words and functions (assumption: hardware semaphore) | code | - |

## 8. Still development jobs

| Address | What | Basis | Used by |
|---|---|---|---|
| `0x6038958c` (`0x60389587`) | job creator: handle `h` of 15484 bytes; P = `h+4` | camera | `graindrv.c` (offsets) |
| `0x603877f1` / `0x60387860` | start a job asynchronously / synchronously (preview) | code | - |
| `0x6041e8dc` | record preparation: job request P (32 bytes): `P+4` record request C, `P+18` aspect, `P+19` 1 for RAW Edit, `P+21` 1 for the monochrome profile | code | - |
| `0x6038a4f0`; site `0x6038a531` (back `0x6038a537`, after the planner `0x6038a549`) | planner selector: reads the job key `h+11308` for `P+8` 0 or 2; 12 -> the ART planner, otherwise the normal planner | camera | `graindrv.c` (W: the grain route) |
| `0x6038c072` -> `0x6038d263` | ART planner | camera | `graindrv.c` (called with the grain film's or key 12) |
| `0x6039ef2d` -> `0x603a1708` | normal still planner: steps `0x603a1045`, `0x603a105e`, `0x603a10be` | code | - |
| `0x603895ec` | creator zeroes the step table `h+10984` (16 x `{fn, callback, ctx}`) | code | `graindrv.c` |
| `0x6038997a` / `0x6038999c` / `0x603899bf` | the creator calls the planner selector / the setup gate / deletion | code | - |
| `0x6038a558`; `0x6038a565..0x6038a57e` | the selector's `P+19` check; the tail step appended (not for `P+19` = 1) | code | `graindrv.c` (the output step goes before it) |
| `0x60394315` / `0x6038c07b` | planners for `P+22` not in {0, 4} (multi-frame) / for `P+27` not 0 | code | - |
| `0x6038e4d9` / `0x6038e8b2` | the setup gate's ART part (textures, maps, frames) / the stock cleanup | code | - |
| `0x605650a0`, `0x60565120` | the ART builder's still lists by key | emu | - |
| `0x60389f71`, `0x6038a203`; `0x6038bef1` | step executor; tail step | code | - |
| `0x6038a584` / site `0x6038a5d0` (back `0x6038a5d6`) | job setup / cleanup gates (for `P+8` = 1 or key 12) | camera | `graindrv.c` (H3: our texture released) |
| `0x6038aa95`; `0x6038aac0`, `0x6038ae73`, `0x6038b01c`, `0x6038b11e` | request B builders: common part; kind 0 (development) / 4 / 6 / 3 (zeroes B) | code | `graindrv.c` (offsets) |
| `0x6039f787` | development node of the normal planner: B, then the step's callback if not 0; extra outputs `{descriptor*, n, flag}` | code | - |
| `0x60391b23` / `0x60391b8f` | plain pass `0x1000` -> the job's outputs (callback `0x60391516`, nothing) / the same with the shading callback `0x60391d48` (monochrome list) | camera | `graindrv.c` (`STEP_OUT` added to the mask route) |
| `0x60391a94..0x60391ae4` | effect callback by job flags (`0x10000`, `0x20000` -> effect `0x900`, `0x40000`, `0x1800` -> `0xA00`, `0x80000`) | code | - |
| `0x6036f790`, `0x6036f8f7`; `0x6036f7a1` | pass setup; its check call | code | - |
| `0x6036fbbb` (back `0x6036fbc0`) | pass setup check whether the detail block `0x1800` (sharpening, noise filter) is set up: class 1 or 2 outputs, not at stages `0x100`, `0x9000`, `0xA000`, `0xA200` | camera | `graindrv.c` (D: the look's development keeps the detail block) |
| `0x603864d5` / `0x603867b8` / `0x603868ea` | imgpro buffer: allocate / free / find | code | - |
| `0x6038c50f` (`0x6038c52c`) | transfer of a buffer to the job's outputs (a 40 MB intermediate of format `0x302`) | code | - |
| `0x60386415`, `0x604209d0`, `0x60420a64` | job buffers taken at REC event 19 with `buff_alloc` flag 0 (waiting) | code | - |
| `0xa263ce08`; `0x604118b3`; `0x60412fbc`; `0x6057d0cc`; `0x6057d4a8` | image pool (at `0xb0000000`, `0x1F47600` bytes); its creation; block reservation; blocks per allocator mode (0, 9, 2); heap table per mode | code | - |
| `0x604139ed` / `0x60413b3a` | `buff_alloc` / `buff_free` (heap from `0x6057d514`); without flag 2 it waits forever | code | - |
| `0x604125b2`, `0x6041271e`, `0x60412932`, `0x60412c0d` | image buffer registry `0x6057d0d4`: allocate / free / address / free space | code | - |
| `0x614de0e3` / `0x614de2dd` | `play_rec_raw_edit` (requests `0x1001` sets allocator mode 1, `0x1003` RAW Edit, `0x1004`, `0x100a`) / `play_rec_raw_to_yc` (`0x1006`, sets the mode) | code | - |
| `0x6041e4a7` / `0x6041e53d`; `0x60424d06` | `rec_make_image` (by `C+194`, table `0x605805c4`) / `..._finish`; `rec_jpeg_exec` | code | - |
| `0x6041f6e5` / `0x6041f779` / `0x6041f98b`; `0x6042281e` | REC events Bayer2Ycc / Ycc2Jpeg / Ycc2View; Ycc2Jpeg runs once per Prim, Thum, Playpic, ViewJpeg, View | code | - |
| `0x6037a2c6`, `0x6038a8a5`; `0x60580684` | the job sends REC events 13, 19 (buffer allocation, waits), 21, 33; REC handler table | code | - |
| `0x6041dd00..0x6041e2c0` | debug print of record request C: `+194` Mode, `+206` Src, `+283` ImgAspect, `+293` RawJpeg, `+298` FileType | code | `graindrv.c` (logged) |
| `0x6033085c` | TIFF/EXIF header builder (a0 S, a1 TIFF start); copies `S[0..4472)` | camera | `pictprof.c` (UserComment through S) |
| `0x60330841` / `0x60331b87` | copy of the template into `S` / template fill at start (the 117 UserComment spaces; another reading puts that fill at `0x60331c70`) | code | - |
| `0x6051ea90`; `0x6051ecdc` | EXIF tag table `{tag, type, size, offset in S}`; the UserComment entry `{0x9286, UNDEFINED, 125, S+0xf7c}` | code | - |
| `0x60424f6f` / `0x6042b6f2` | `rec_jpeg_make_header` / `rec_raw_make_header` | code | - |
| `0x6038d248` -> `0x6038cbc4` -> `0x603932cb` -> `0x603934ba` | Soft Focus still chain: preparation, ART development, blur, final | code | - |
| `0x6038c800` | xorshift32 (13/17/5) used for the grain shift | code | `graindrv.c` (same generator) |

### Job data

| Object | What | Basis |
|---|---|---|
| handle `h` | `+0` u8 creator's CPU, `+4..35` P, `+36` D, `+10984` step table, `+11308` job key, `+11312..11318` state words, `+11314` step count, `+11341` class, `+11364` request B, `+14216` texture shift, `+14224` texture record | camera |
| P | `+8..11` job code, `+12` effect flags, `+16` type, `+19` RAW Edit, `+21`, `+22` (multi-frame jobs), `+30` | code |
| B (`h+11364`, 1884 bytes) | `+8..11` code, `+12` u16 stage (set by step callbacks: `0x100`, `0x101`, `0x900`..`0x902`, `0xA01`, ...), `+14` type, `+16` kind, `+20..83` output descriptor, `+28` format (class in the high byte), `+468` input B descriptor, `+532` input C descriptor, `+1838` effect code | camera |
| `ctx+8` register image slot | masks M0/M1 at `+0/+4`; block images `+112` (`0x0200`), `+208` (`0x0400`), `+7364` (`0x0e00` blend), `+7612` (`0x0800`), ... | code |
| `0x605808a8`; `0x60564c68` | effect flags of ART (from `S+29473`); ART code hash of the creator | code |
| `0x60564fa0` | ART planner key handlers (32 x `{key - 11, handler}`) | code |

## 9. Grain

| Address | What | Basis | Used by |
|---|---|---|---|
| `0x01020068` / `0x01010068` | job key of Grainy Film II / I; handler `0x6038d7a5` | camera | `graindrv.c` |
| `0x6038d3ec..0x6038d4b6` | the grain film's still list (keys `0x605650a0`) | emu | `graindrv.c` |
| `0x60390f9a` -> `0x60390f21` | the grain film's development step into buffer `0x1010` (format `0x302`); its callback attaches the texture (`B+468..`, `B+12 = 0x100`) | camera | `graindrv.c` |
| `0x60391313` (`0x603911b5` in the handler's list) | the grain film's pass after it: `0x1010` -> the output, kind 3, stage `0x101` (callback `0x60391384`) | camera | `graindrv.c` |
| `0x6038c828` / `0x6038c84a` | grain seed step (classes 4..8, xorshift of `h+14216`) / seed 0 | code | - |
| `0x6038db19` | monochrome profile builder (key 12): a still gets the mask pass and the development `0x901` whatever the grain level | camera | `graindrv.c` (mask route) |
| `0x60390190` / `0x603902a0`; `0x603903c9`, `0x603903dd`, `0x60390426` | monochrome grain passes: mask development stage `0x900` into the 1-byte plane `0x10A0` (format `0x304`) / development `0x901` (input C = `0x10A0`); their callbacks and the live view one (stage `0x902`) | camera | `graindrv.c` |
| `0x603a3bdb` | texture builder (`call ...,[d2,d3,a2,a3],28`, a0 P, a1 6 bytes of parameters -> record or 0); texture cache `0xa26ed764` (4 x 100 bytes, refcount `+96`, state `+98`) | camera | `graindrv.c` (`drv_fw_tex_build`) |
| `0x603a452a` | texture release (`call ...,[a2,a3],12`, a0 P, a1 record) | camera | `graindrv.c` (`drv_fw_tex_release`) |
| `0x6038c0c1` | byte `+4` of the texture parameters: 1 or 3 by class | camera | `graindrv.c` (`0x80` + generation as our marker) |
| `0x603a5167`; site `0x603a5314` (back `0x603a5319`) | tile node builder (node `0xF002`): the flash tile read into a buffer at `(72,sp)` before the node's DMA | camera | `graindrv.c` (H2: our tile, repeated, shifted) |
| - | the firmware tile: 1536 x 1536, flash resource `0x8001` / 0 | camera | - |
| `0x60179a13` / `0x603a4800`; `0x60386958` / `0x60386990`; `0x603876d3` | flash resource length / read; tile buffer allocate / free; DMA row copy (replication) | code | - |
| `0x6038e67c..0x6038e6a6` / `0x6038e6b1` | the grain film's stock texture parameters / its setup writing `h+14224` | code | `graindrv.c` (parameters copied) |
| `0x6038e0a2` / `0x60389926` | the planner adds the output step only for job flag `0x80000` / the creator gives that flag to key 12 | code | `graindrv.c` (adds the step itself) |
| `0x603a5dca` (node `0xF003`) | live view / video texture node (256 columns wider, shifted per frame) | code | - |
| `0x603a4785` / `0x603a47a1` | texture cache lock (cross-CPU lock `0x340`) | code | - |
| `0x6036fc58` (from `0x6036f801`); site `0x6036fc74` (back `0x6036fc79`) | pass blend setup: selector, then the case handler, then `0x60372fdb` | camera | `graindrv.c` (A: gain, offset, mode, select words) |
| `0x603ecda1` (back `0x603ecda8`) | blend case selector (ctx in a0, case in d0); case table `0x6056766c` (64 x `{case, handler}`), effect code -> case `0x605769a8`; key table `0x60576aa8` | camera | `graindrv.c` (S: case 1 for our jobs) |
| `0x60372fdb` | enables the blend inputs from mode word `+4` (bit 0 input B from `B+468`, bit 1 input C from `B+532`, bit 20 weights by C); input B format -> mode bits 4-5; other output class -> error `0x9E4C` | camera | `graindrv.c` |
| `0x603d7de1..0x603d8b40` | blend case handlers; case 1 `0x603d8145` (grain film), 31 `0x603d8547` / 32 `0x603d85ef` (monochrome grain still / live view), 34 `0x603d881f` (generator, gain 100), 35 `0x603d88a4` (texture, 256), 11/12 `0x603d844b`/`0x603d8494` (Soft Focus) | code | - |
| `0x603d80ec` | the blend block's image in the register image (`+7364`) | camera | `graindrv.c` |
| `0x6053138e` / `0x60531394` | monochrome grain gains (110, 300, 600) / (50, 75, 100) | code | - |
| `0x60545fde` | monochrome grain mask curve: u16[256], 0..1023, by Y before the tone; 0 below node 36, 1023 from 120 | code | - |
| `0x60531388` | input B gains of cases 36/38: 1024, 24, 24 (Y, Cb, Cr) | code | - |
| `0xa26eed0c` / `0xa26eed08` | blend seed words (case 32 counter, case 21 xorshift) | code | - |
| `0x6036fcd8`; `0x603d48aa` | LUT block `0x0400` (3 x 8-bit) setup only at stages `0xA00`, `0xB02`, `0xB03`; grain film I/II tables `0x60542bde` / `0x60543bde` | code | - |

### Blend block `0x0e00` image (at `(ctx+8)+7364`)

| Offset | Field | Basis |
|---|---|---|
| `+4` | mode: bit 0 input B, bit 1 input C, bit 20 weights by C; `0x00100003` programs B and C together | camera |
| `+88` / `+132` | select words of input A / B: 2 bits per channel (Y bits 0-1, Cb 2-3, Cr 4-5); byte 1 source (0 memory, 1 constant `+120`, 2 generator), byte 2 weight (2 gain, 0 gain x C, 1 gain x (1 - C)) | camera |
| `+92` / `+96` | input A gains: Y (low half), chroma (high half) / second chroma | camera |
| `+120` | constant colour (`0x808000` to black, `0x8080FF` to white) | code |
| `+124` | input B offset before the gain (Y low half, chroma high half) | camera |
| `+136` / `+140` | input B gains (Y low half, chroma high half) / second chroma | camera |
| `+164`, `+168` | shifts | code |
| `+172` | generator seed; bit 31 restarts it | camera |

## 10. Open points

- The meaning of several `S` fields the colour builders fill but OMKit does not touch (`S+332..492`, the six axes, the
  `0x2800` group) is not established.
- The live view path of the colour tables is identified and works (scope bit), but OMKit keeps it off by default: a stepped
  test curve there disturbed the autofocus (camera).
- Names in this catalogue taken from firmware strings (`rec_*`, `play_*`, `Plural_Direct_Actuator`, ...) are the
  firmware's own; names in plain words are ours.
