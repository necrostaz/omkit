# OM-5 Mark II: the camera GUI as OMKit uses it

This page describes the parts of the camera's user interface that OMKit touches: the picture mode screen and its editor
column, the graphic editors (Color/Level, Color/Vivid, the tone curve, the film look's item and grain item), dial and key
handling, the pictures the firmware draws and the pictures modules supply, and what the screen shows compared with what a
picture gets. Picture mode numbers and the virtual mode of the profiles are in [picture-modes.md](picture-modes.md); the
colour math behind the editors is in [color-pipeline.md](color-pipeline.md); still processing in
[still-pipeline.md](still-pipeline.md); film grain in [grain.md](grain.md). Firmware 1.1 addresses of everything named
here: [fw1100/imaging.md](fw1100/imaging.md).

OMKit does not draw over the screen itself. Everything the user sees is drawn by the firmware: OMKit changes data the GUI
reads (words of the picture table, of the item handler table, of widget state lists and rectangles, a string pointer) and
borrows the firmware's own Color Creator drawing order for its editors.

## 1. Layers and tasks

| Part | What it is | Who draws | Basis |
|---|---|---|---|
| OSD plane | widgets with pictures from the picture table (icons, labels, numbers, sliders, hint buttons), 720 x 480 coordinates | the GUI task on CPU 0, through the OSD engine, which reads picture data straight from memory | firmware code, static analysis; emulator |
| Image layer | the "background" drawings of the GUI (Color Creator ring, part colour, ART preview and the like) over live view | the camera side: a C3D interface task on CPU 0 and the C3D renderer task on CPU 1, OpenVG 1.1 + EGL on the graphics engine | firmware code, static analysis |
| UI settings object | one object with a method table; the only way the GUI reads and writes settings (method +8 read, +0 and +4 write, +24 count of values, +28 value by index, +36 availability, +152 default) | - | firmware code, static analysis; emulator |
| UI strings | one language at a time loaded into one place: an array of string pointers (4909 UTF-16LE strings) | - | firmware code, static analysis |

Everything the GUI does runs synchronously in its task: a setting write from the GUI runs the firmware's value path, rules
and notifications in the same task (emulator). Because the GUI reads settings only through the UI settings object,
replacing that object's method words separates "what the UI shows" from "what the camera stores and shoots" without
touching code (firmware code, static analysis; emulator) - the base of the profiles' virtual mode, see
[picture-modes.md](picture-modes.md).

GUI state: a byte pair (group, state) in the GUI object, read through the uncached alias:

| State | Screen | Basis |
|---|---|---|
| 01 08 | shooting screen | emulator |
| 01 04 | black screen (power-off in progress) | measured on the camera |
| 04 03 | Super Control Panel (SCP) | emulator |
| 04 18 | direct picture mode screen (from the SCP picture mode cell and from the menu line "Picture Mode") | emulator |
| 02 02 | direct setting screen of the shooting group; with setting 382 in its slot it is the Highlight & Shadow screen | emulator |

## 2. The picture mode screen

Layout (emulator, firmware code): the mode strip at the bottom, its caption (the mode name, a UI string) above it, the
editor column of the current mode on the left. Up/down move the cursor along the column; the item under the cursor is
setting **2148** (its value is the item number; 111 means the strip). The dials change the item under the cursor. OK leaves
the screen and writes the current mode to 2149.

Items 5029, 662, 5373 and 5374 are "editor" items: while the cursor is on them the strip and caption are hidden; the other
items use the common redraw (firmware code, static analysis).

### 2.1. Item numbers

An item number is either a setting id or a pseudo number of a composite item:

| Item | Meaning | Cells it edits (profile k = 0..2) | Basis |
|---|---|---|---|
| 111 | the picture mode itself (top of the column, the strip) | 111 | emulator |
| 7, 8 | sharpness, contrast (slider -2..+2) | stock: 7, 8; profiles: 904+k / 990+k, 908+k / 994+k | emulator |
| 59, 105 | saturation, gradation | - | emulator |
| 130, 131 | Monotone filter, toning | toning of a monochrome profile: 1003+k | emulator |
| 5029 | Color Creator, picture "Color Vivid" | 615 hue 0..29, 616 vividness | emulator |
| 5373 | Color/Level (monochrome profiles) | 1007+k filter 0..8, 1012+k level | emulator |
| 5374 | Color/Vivid (colour profiles) | 928+12k+i (i = 0..11), -5..5; cursor 2201+k; one hue / all 2197+k | emulator |
| 5375 | tone curve | Hi, Mid, Sh: 976+3k.. (colour), 1021+3k.. (mono), -7..7; "INFO chose Mid" 2189+k / 2193+k | emulator |
| 5376 | shading (slider -5..+5) | 1017+k (mono); the colour profile's 912+k is not used by the firmware | emulator; firmware code |
| 5377 | grain (monochrome profiles) | 999+k, four values Off, Low, Med, High in a ring | emulator |

The item handlers find a profile's cells by tables keyed by the UI's picture mode (54..56 colour, 57..59 monochrome; 64/65
are fourth profiles this camera never reaches) and, for Color/Vivid's one-hue flag, by setting 901. Internally the items
have per-profile ids 5413..5428. With a mode that is in none of these tables (a stock mode) a dial step writes setting 0,
and INFO and long OK write nothing (emulator).

### 2.2. Column lists

The column function picks the item list by the UI's picture mode through a jump table keyed by mode + 1.

| Mode | Stock list | With OMKit | Basis |
|---|---|---|---|
| Color Creator (21) | 111, 5029 | unchanged | firmware code, static analysis |
| colour profile 1..3 | 111, 5374, 5375, 5376, 7, 8 | 111, 5374, 5375, 7, 8 (shading removed: the firmware drops a colour profile's shading) | firmware code; emulator |
| monochrome profile 1..3 | 111, 5373, 5375, 5376, 7, 8, 5377, 131 | unchanged | firmware code |
| film look slot | the slot mode's own list | 111, 5029 and, as the module asks, 5375 (curve) and 5374 (Color/Vivid, Color/Level or the grain item) | OMKit code; emulator |

The list is built only when the screen opens and after a step of the strip - not on a dial step of an item, up/down, INFO
or long OK (emulator). A list that has to change without a mode change (the film look stepping from a colour preset to a
black-and-white one) therefore cannot change; OMKit keeps one list for the look and lets item 5374 change its meaning
instead (section 4.4).

### 2.3. Column geometry

Rows are 60 px apart from y = 60 (firmware data): row background (0, 60 + 60k, 88, 60), item picture (11, 65 + 60k, 66, 50),
small icon (12, 71 + 60k, 38, 38), its number (52, 78 + 60k, 24, 24). The shading item's widgets kept a 74 px row pitch the
firmware carries from a larger layout and were drawn one row lower, over the sharpness; OMKit rewrites their ten rectangles (icon
(4, 67 + 60k, 46, 46), number (52, 78 + 60k, 24, 24)), only while they hold the old values (firmware data; emulator). Widget
rectangles are copied into the widget objects when the screen opens the first time; later writes to the data change
nothing until the copies are made again (emulator).

### 2.4. Where the shown values come from

The number in the screen header reads the profile cell. The number in the column, the slider and the value pictures (the
curve item's picture, the grain and toning pictures) read the **effective** settings 7, 8, 383/691/382 (tone Hi, Mid, Sh),
1016, 998, 131, 1011, which the firmware fills from the stored mode - Natural while a profile is active. OMKit answers
such reads made by the picture mode screen's code with the profile's cell; reads from anywhere else keep the effective
values (firmware code, static analysis; emulator). The curve item's picture is chosen by the signs of the three tone values
(27 pictures).

Two more screens follow the profile's tone the same way: the camera's Highlight & Shadow screen (state 02 02 with 382)
reads and writes the active profile's tone cells instead of the camera-wide H&S, and the SCP "Exposure Comp. / H&S" cell
(state 04 03) shows the profile's tone (OMKit code; measured on the camera).

## 3. Dials, keys and events

### 3.1. Dials on the editor items

The emulator names the dials "sub" and "main". On the camera, on the Color Creator item, the **rear** dial writes 615 (the
emulator's sub) and the **front** dial 616 (main) (measured on the camera). The other items follow the same pairing: the
first function below is the rear dial's, the second the front dial's (measured on the camera on Color/Vivid and Color/Level
and on the grain item).

| Item | Rear dial (sub) | Front dial (main) | INFO | Long OK (about 2.5 s) | Basis |
|---|---|---|---|---|---|
| 5029 Color Creator | 615 hue, a ring 0..29 | 616 vividness | - | 615 = 616 = 0 | emulator; measured on the camera |
| 5373 Color/Level | 1007 filter, a ring of 9 places (none, Ye, Or, R, M, B, C, G, GY by angle) | 1012 level; written on every click, the same value again at an end; silent while no filter is set | - | filter 0, level 2 | emulator |
| 5374 Color/Vivid | 2201: the cursor, 13 places in a ring (0 = all hues, then 928..939) | the cell under the cursor; at "all" all 12, each from its own value | 2197: one hue / all (a toggle) | all 12 saturations 0 | emulator |
| 5375 tone curve | Hi (976) | Sh (978); after INFO both dials edit Mid (977) | 2189: Mid / Hi and Sh | Hi, Sh, Mid = 0 | emulator |
| 5377 grain | 999, -1 a click | 999, +1 a click (left/right too) | - | - | emulator |

The native editor is a parent object with one child per cell (for 5374: the cursor and 12 saturation cells). A child steps
its own index and writes the cell when the step is committed; it reads the cell only when it is created (the cursor arrives
on the item) or on a refresh event, so the native code steps from what it wrote last, not from what a read returns
(firmware code, static analysis; emulator). Any substitute for a cell must answer the native code's reads with what it
wrote ("the shadow", section 4.4).

Inside the GUI these actions are events: steps 0x9004/0x9006 to the children, commit 0x900c, long OK 0x903e, INFO 0x9014, a
second reset 0xf881, a child refresh 0x971d with 0xf85d; key events 0x9000, 0x9625, 0x900d (firmware code, static
analysis). OMKit sends no GUI events; it observes the results as setting writes through the UI settings object.

Timing on the camera: on fast rotation the GUI writes a dial cell every 40..100 ms; the vividness cell (616) holds its end
values (1, 2, 3, 3, 3 to the right) (measured on the camera). Every action ends with a redraw of the item, and the drawing
order follows the item handler in the same redraw (firmware code, static analysis).

The touch panel is present, but the touch handlers of the profile editors are not in this firmware; OMKit editors are dial
only (firmware code, static analysis).

### 3.2. What a module gets

Modules see no key codes. They get:

| Source | What | Where it runs | Basis |
|---|---|---|---|
| event `OMK_EV_PICTMODE_CHECK` | the firmware asks whether a picture mode is allowed (strip build, SCP dial) | the GUI task, through core's event manager | OMKit code |
| event `OMK_EV_S1_API` | a call that accompanies a half-press (preliminary, meaning not understood) | as above | OMKit code; measured on the camera (two calls a press) |
| profile callback (`prof_enable`) | the user picked a profile or left it | core task | OMKit code |
| film look callback (`look_enable`) | `OMK_LOOK_ON`, `OMK_LOOK_OFF`, `OMK_LOOK_ENTRY` (a dial chose another entry), `OMK_LOOK_EDIT` (an editor changed a value, `OMK_EDIT_EVENT(group, index, value)`; a long OK sends the group with `OMK_EDIT_RESET`) | core task | OMKit code |

The driver turns the GUI's cell writes into these events in the GUI task, keeps them in a ring of 32 (an overflow is
counted and logged) and wakes the core task; a write the GUI did not follow with a redraw is turned into events by the core
task after 50 ms. A dial step is shown at once (the drawing of that step already has the new value); the module's own
values replace the shown ones only for a new entry or when no step of the group came for 1 s. API details:
[../../sdk.md](../../sdk.md).

## 4. The graphic editors

### 4.1. What the firmware has

On this firmware the profile editors (Color/Level, Color/Vivid, the curve graph) have item handlers and dial logic, but
no drawing: the drawing kinds for them (Color Creator 2, Monochrome Creator, Shading) are cut on all three links - the GUI's
order builder refuses them, the camera side accepts only kinds part colour, Color Creator, ART filter, scene top and
select shooting, and the renderer has neither their jobs nor their data (firmware code, static analysis). The curve item's
layout is a 900 x 600 panel the firmware carries over, without graph widgets. The touch-operated profile panel is absent.

### 4.2. The drawing path

```
GUI task (CPU 0): redraw of a column item
  item handler (table keyed by item - 6) sets the panel's "background": the Color Creator kind and two flags
  the GUI frame: start drawing when the cursor arrives, an order per redraw, end on leaving the item, the screen or to the menu
    -> background control -> request queue
C3D interface task (CPU 0): START (kind -> surface: Color Creator 256 x 256), UPDATE (the order's data), END
    -> queue
C3D renderer task C3D_MAIN (CPU 1): Color Creator job -> OpenVG paths and paints -> eglSwapBuffers
    -> 256 x 256 surface in the image layer, centred, over live view
```

Basis: firmware code, static analysis; the GUI half traced in the emulator (the emulator does not run the camera side).

OMKit uses this path as it is (OMKit code; measured on the camera):

- the item handler words of 5373, 5374 and 5375 point at small trampolines: the item's own handler, then the driver
  publishes the values to draw, then the trampoline writes the Color Creator "background" - so the GUI orders the Color
  Creator drawing for these items too; start, end, exit and power-off are handled by the GUI as for Color Creator;
- the Color Creator item 5029 gets a trampoline that switches OMKit's drawing off (its own ring is drawn) - or, while the
  film look is active, draws the look's item;
- the entry of the Color Creator renderer on CPU 1 is hooked: when the published mode is OMKit's, OMKit draws the frame with
  the firmware's OpenVG, swaps and returns as the renderer does; otherwise (or on any failure) the native code runs;
- the core task switches the mode off when the GUI is not on the picture mode screen (04 18) or 2148 is not the item.

The values cross from CPU 0 to CPU 1 in a small block used only through the uncached alias, under a sequence counter (odd
while written; the renderer retries up to three times). The renderer code touches no cached core variable, reads its
constant tables through the uncached alias of the core image (the sines, needed at every point, from a copy of a quarter
wave it makes on its stack once a frame), builds paths in an arena buffer (352 segments, 320 points a
path) and uses no floating point: paths are `VG_PATH_DATATYPE_S_16` in quarter pixels (scale 0.25), colours are RGBA8888 for
`vgSetColor`, float parameters are bit patterns. One path and one paint per frame; shapes of one colour are subpaths of one
fill. Before hooking, core checks the first bytes of every firmware entry it calls. Memory rules:
[fw1100/system.md](fw1100/system.md).

### 4.3. What OMKit draws

| Item | Drawing | Cost | Basis |
|---|---|---|---|
| 5373 Color/Level (mono profile) | dark disc, hue ring of 48 sectors (r 107..117), filter ticks every 40 degrees (none at the top, then Ye, Or, R, M, B, C, G, GY), faint level circles, a needle to the filter with a cross at the level radius | about 90 fills | measured on the camera |
| 5374 Color/Vivid (colour profile) | 12-gon frame in the hue colours (hue 0 yellow at the top, clockwise), scale -5 / 0 / +5, polygon of the 12 saturations, dots; the hue under the cursor a large dot with its spoke; "all hues" outlined in yellow | about 60 fills and strokes | measured on the camera |
| 5375 tone curve | 200 x 200 frame with a 2 x 2 grid, the straight line, the curve through Sh at 1/4, Mid at 1/2, Hi at 3/4 (cubic segments), crosses on the points the dials edit, seven-segment numbers | about 80 OpenVG calls | measured on the camera |
| 5029 on the film look | the chosen preset's cassette drawing with "n / N" under it; without one, a grey 224 x 92 rectangle with the name in a 5 x 8 font | 72..90 calls | emulator; bench of the renderer |
| 5374 on the film look | Color/Vivid on the preset's sectors (vertices at their hues and colours, sectors the preset lacks drawn at 0 dimmed and skipped by the cursor), or Color/Level for a black-and-white preset, or the grain item | about 70 fills | emulator; bench |
| 5375 on the film look | the preset's own curve (64 points from the module) and its three points | - | emulator; bench |

The renderer's stack use is about 430..630 bytes of the task's 4 KB (bench of the renderer, against the firmware's own call
frames).

### 4.4. The film look's items

The look's item is the native Color Creator item 5029 in the slot's column. Its two dial cells are virtual while the look is
active: reads get what the UI was last given, writes are swallowed and step the entry at once in the GUI task, before the
step's drawing is ordered. Both dials step the same way: the value up - the next entry. 615 (a ring 0..29) steps by the
change from the last write; 616 by the change from the last write, the same non-zero value again counting as a push against
an end. The write of 111 = 21 that follows every dial step on this item is swallowed too (OMKit code; measured on the
camera).

The editors after it (5375 and 5374) reuse the native profile items: reads of the item id getters and of 901 get colour
profile 1, so the native code works on that profile's cells; on the look's editor items every read of those cells is
answered from the shadow and every write of a profile cell (904..1033, 2189..2204, the ids 5413..5428, setting 0) is
swallowed - the stored settings do not change (OMKit code; emulator). Item 5374 is Color/Vivid for a colour preset, Color/Level
for a black-and-white one (the rear dial steps the filter by place on the ring, the front dial the level) or, when neither
is shown, the grain item (rear dial: the tile; front dial: Off, Low, Med, High; long OK: Off). Its picture changes with it
through a picture number of its own (section 5.3).

With the optics layer on (`[optics] layer = on`), two rows follow the grain item: Bloom (item 5372) and Halation (item
5370), each a slider 0..5 per preset ([modules/filmlook/optics.md](../../modules/filmlook/optics.md)). The column is then
`[111, 5029, 5375, 5374, Bloom, Halation]` and takes two pages: **Bloom is the last row of the first page, Halation is
alone on the second**. The firmware has no way to register an item, so the rows are items of OMKit's own: hooks of the
item lookup find OMKit's descriptors before the stock ones, the rows' keys take free entries of the firmware's switch
tables, and three methods of the settings object answer the rows' setting ids (`0x7e00 + row`, values 0..5). The number
next to the sign is the camera's own (`±0`, `+1`..`+5`); a long OK on a row sets it to 0, on a row at 0 it closes the
screen as on the stock items. The signs left of the number are the look's pictures 12 and 13 (46 x 46). The remembered
cursor item (2148) is stored as 111 when it is a row, so the stored settings never hold an OMKit item id. The driver
keeps every preset's strengths, so the column drawn at a preset step already shows the new preset's (OMKit code;
emulator, camera).

### 4.5. Dial labels and other widgets

The editors use the Color Creator layout's widgets (screen coordinates of this camera):

| Widget | Place | Stock picture | OMKit's use | Basis |
|---|---|---|---|---|
| 0x181, left dial pictogram | (456, 65), 30 x 20 | 10922, the rear dial's | on the curve (also after INFO) 10923, the front dial's | firmware data; emulator |
| 0x3d8, right dial pictogram | (552, 65), 30 x 20 | 10923, the front dial's | on the curve (also after INFO) 10922 | as above |
| 0x3d9, left dial label | (403, 85), 56 x 24 | 4221 "Color" | "Color" also on Color/Level, "Sh" (4216) on the curve, "Mid" (4339) after INFO, "Prev" (4234) on the look, "Tile" (4240) on the grain item | firmware data; measured on the camera |
| 0x3db, right dial label | (499, 85), 56 x 24 | 4222 "Vivid" | "Level" (4224) on Color/Level, "Hi" (4215), "Mid", "Next" (4235), "Level" (4241) on the grain item | as above |
| 0x3da, colour sample | after "Color" | 4227..4233 (by hue group) | the empty stub 4236 on the look's items | as above |
| 0xf6d, comma | after the sample: (546, 85), 10 x 24 | 10834 | the empty stub 4236 on every OMKit editor item (5373, 5374, 5375 of a profile or of the look, and the look's item); stock on the native Color Creator item | firmware data; emulator; measured on the camera |
| 0x3dc, rear dial's value | right of the label | text "+-0" | emptied on the look's item | firmware code; OMKit code |
| 0x3d6 / 0x3d7, Reset button and text | (418, 380), 146 x 42 | frame 10180 / 10181, text 10167 | see 4.6 | firmware data; emulator |

The GUI takes a label's or a pictogram's picture from the widget's state word at every draw and draws it centred in the
widget, clipped by it. OMKit writes a word only while it holds one of the pictures it expects, and puts the stock ones
back on other items. The stock screen reads "Color <sample>, Vivid"; on the items 5373, 5374 and 5375 the GUI draws no
sample but still draws the comma, which showed as a dot between the two labels (emulator widget trace; seen on the
camera), so since core 1.31 the comma's word holds the stub on every OMKit item. The label pictures from a module are used only when they are set (both of a pair where the pair is
the module's). The label widgets keep their stock 56 x 24 rectangles on every item (since core 1.29; before it 0x3d9 was
widened to 116 for "Color Filter" and 0x3db to 104 for "Strength").

The pictograms show which dial a label belongs to; the camera's own Highlight & Shadow screen pairs 10923 with "Sh" on
the left and 10922 with "Hi" on the right (its state lists), and on Color Creator 10922 stands over "Color", the value
of the rear dial (firmware data; measured on the camera). The curve item keeps its dials (the rear one edits Hi, the
front one Sh, section 3.1) and lays the labels out as that screen does: "Sh" with the front dial's pictogram on the
left, over the shadows of the graph, "Hi" with the rear dial's on the right. After INFO both labels read "Mid" and the
pictograms stay (OMKit code since core 1.28; emulator: the words and the order the widgets draw them).

#### 4.5.1. Rule: dial labels and the editing area

Every item that labels the dials follows this rule, so the labels and the drawing look the same from item to item. A new
control is designed to it; the camera's own Color/Vivid labels "Color" and "Vivid" are the reference.

1. **One face, one size, one place.** A dial label is a picture 24 px high and at most 56 px wide, shown in the stock
   56 x 24 widget (0x3d9 or 0x3db) without widening. Its letters are those of the camera's labels: capitals and ascenders
   from row 2, lower case from row 8, the base line at row 21 (rows 2..20 full), lines 2..2.5 px, the outline, edge and
   halo of the stock labels. Then every label stands at the same distance under its dial pictogram (0x181 or 0x3d8 at
   y 65..84, the label from y 85) and is centred under the same point.
2. **The camera's own word first.** Where the firmware has the word as a label picture, use its number: "Color" 4221,
   "Vivid" 4222, "Hi" 4215, "Sh" 4216, "Mid" 4339. These are the reference by definition.
3. **Otherwise draw it with the shared face.** `OMKit/tools/dial_label.py` draws a word in that face (fitted to the
   firmware's labels; tests compare it with them) and is the only drawer of dial labels: the module generators take their
   labels from it (`dial_label.label(word)`). It refuses a word wider than 56 px and letters it does not have.
4. **Shorten, never shrink.** A word that does not fit, or that needs a descender (g, j, p, q, y: the 24 rows end 2.75 px
   under the base line), is replaced by a shorter word or an abbreviation in the same size. Replacements so far: "Color"
   for "Color Filter" (Color/Level of monochrome profiles), "Tile" for "Type" and "Level" for "Strength" (the grain item).
5. **The editing area starts at one place.** The drawing of an editor item begins at the row of the surface where the
   curve's window begins: row 18 of the 256 x 256 Color Creator surface (the window, 220 x 220, is centred). The look's
   cassette and its rectangle, and the grain item, are drawn from there; a new drawing starts there too. The labels and
   the surface keep their places on every item, so the same row is the same distance under the labels.

### 4.6. Hints

"Reset OK Hold" is a native picture (10167, 140 x 40, in a 144 x 40 frame) in the Color Creator layout's Reset button. A
flag byte of the panel's background block shows it; the Color Creator handler sets it when 615 or 616 is not 0. OMKit's
trampolines set it when the values differ from what a long OK resets them to: Color/Level - a filter or a level other than
2; Color/Vivid - any saturation not 0; curve - any of Hi, Mid, Sh not 0; grain item - a strength other than Off. On the
look's item 5029 the hint and the button frame are off: nothing there is reset (OMKit code; measured on the camera).

Not shown: "Color All / Color Select INFO" and "Mid / Hi/Sh INFO" (pictures 16253..16256) and the "edited" mark 16251. Their
widgets are in layouts the firmware carries over with 900 x 600 coordinates and fall outside this camera's 720 x 480
screen (firmware data); they are left as they are.

## 5. Pictures

### 5.1. The picture table

The UI's pictures are numbered. Record 0 of the UI resource block, loaded at boot, holds a table of 17006 pointers to
picture objects. Unused numbers - and on this camera every profile and profile editor picture - point to one stub object
with no data (type 1); 11268 numbers point to it (firmware data, static analysis). The firmware may load the table again
(after sleep or a language change - assumption); core checks its words every loop and puts its own back.

A picture object is 24 bytes:

| Offset | Field |
|---|---|
| 0 | u16 type, 14 |
| 2, 4 | u16 width, u16 height |
| 6 | u16 8 |
| 8 | u32 0 |
| 12 | u32 size of the data |
| 16 | u32 address of the data |
| 20 | u32 0 |

The data is an OSD engine BITMAP command: a 32-bit header {rows - 1 in bits 0..11, width - 1 in bits 12..23, format in
bits 24..31}, then the codes. Nearly all pictures use format **0x0B**: 12-bit codes {palette index 8 bits, repeat - 1 4
bits}, five codes to a little-endian 64-bit word; a run does not cross a row (firmware data; OMKit's encoder reproduces a
firmware picture byte for byte). The indices are the OSD plane's palette; a picture stores no colours.

OMKit builds objects in the arena (uncached: the OSD engine reads memory directly), changes a table word only from the stub
(or from the firmware's object it replaces) to its own, blanks the word while it rewrites an object, and never frees an
object (the engine may still read it).

### 5.2. Palette

A module gives a picture in meaning values, 4 bits a pixel; core maps them to the palette of the place:

| Meaning (`OMK_ICON_*`) | | Icon palette (strip, large, editor items, dial labels, shooting screen) | Label palette (menu line, SCP cell) |
|---|---|---|---|
| 0 `CLEAR` | transparent | 0 | 0 |
| 1 `SIGN` | the sign, light | 10 | 13 |
| 2 `OUTLINE` | dark outline | 7 | 7 |
| 3 `EDGE_IN` | edge, nearer the sign | 11 | 14 |
| 4 `EDGE_OUT` | edge, nearer the outline | 12 | 15 |
| 5 `OUTLINE_EDGE` | outside the outline | 8 | 8 |
| 6 `HALO` | faint rim around everything | 9 | 9 |

Basis: the indices the stock pictures of the same places use (firmware data). The colour comes from the state, not the
picture: the selected cell of the strip is highlighted by the camera.

### 5.3. Picture numbers OMKit uses

| Numbers | Place | Content | Basis |
|---|---|---|---|
| 14515 + 3(n-1) + v | strip / large / large edited icon, monochrome profile n | module (`OMK_PIC_MODE`) | firmware data; emulator |
| 14527 + 3(n-1) + v | the same, colour profile n | module | as above |
| 14539 | column item Color/Level (5373) | module (`OMK_EDITOR_COLOR_LEVEL`) | as above |
| 14540..14566 | column item tone curve, 27 shapes | the firmware's own Highlight & Shadow pictures 14057, 10976..11001 (same shapes, same order, 28 x 28) | as above |
| 14567 + 2l / + 2l + 1 | grain item 5377 / its choice bar, level l | module (`OMK_EDITOR_GRAIN`, `_GRAIN_BAR`) | as above |
| 14575 + 2(n-1) + e, 14583 + ... | menu line "Picture Mode", mono / colour, e = edited | module (`OMK_PIC_LABEL`) | emulator |
| 15746 + (n-1) + 4e, 15754 + ... | SCP picture mode cell, mono / colour | module (`OMK_PIC_PANEL`) | emulator; firmware code |
| 15706 + (n-1) + 4e, 15714 + ... | shooting screen sign, mono / colour | module (`OMK_PIC_SHOOT`) | as above |
| 4220, 4224 | stubs next to 4221 "Color", 4222 "Vivid": "Color Filter" (taken but not shown since core 1.29), "Level" | module (`OMK_EDITOR_LABEL_*`) | firmware data |
| 4215, 4216, 4339 | "Hi", "Sh", "Mid" of the H&S screen | the firmware's own | firmware data |
| 10922, 10923 | the rear and the front dial's pictograms (30 x 20), swapped on the curve item | the firmware's own | firmware data |
| 4234, 4235; 4238..4241 | the look's "Prev", "Next"; Color/Level item, grain item, the grain item's dial labels ("Tile", "Level") | module (`OMK_LOOK_PIC_*`) | firmware data |
| 4236 | an empty stub | blanks widgets | firmware data |
| 4237 | item 5374's picture | the camera's "Color Vivid" (11397), the look's Color/Level or grain picture | firmware data; emulator |
| look slot's own numbers | strip, large, menu line, SCP cell, shooting sign of the slot mode (for i-Enhance 6408, 6678, 6596, 10584, 11226) | module (`OMK_LOOK_PIC_MODE` .. `_SHOOT`), only while the look is active | firmware data |

The stubs 4237..4253 next to the dial labels are free. The items 5029 and 5374 point at two different picture records that
both say 11397, so item 5374's record can be pointed at 4237 without touching 5029 (firmware data). The SCP cell and the
shooting screen work out the "edited" state themselves (any profile cell differs from its default); a module only gives the
pictures, and an edited variant it does not set shows the plain one (emulator).

### 5.4. Sizes

The height is exact; the width is exact where marked, otherwise an upper limit (`prof_picture_size`,
`look_picture_size`):

| Place | Size | Width | Alignment |
|---|---|---|---|
| strip icon / column item / grain item | 66 x 50 | exact | - |
| large icon / grain choice bar | 80 x 60 | exact for the large icon | - |
| menu line "Picture Mode" | 124 x 39 | up to | - |
| SCP cell | 128 x 39 | up to | centred |
| shooting screen sign | 80 x 36 | up to | right |
| dial labels | 24 high, up to 56 (all of them since core 1.29; "Color Filter" is still taken up to 116, not shown) | up to | centred in the widget |

### 5.5. Requirements for module pictures

- A picture is a **silhouette without colours**: the sign (letters and digits cut into it are fine, holes show the
  background), no gradients, shadows or second colours. The generators add the outline (2 px), the edge transitions and the
  halo by rules measured on the stock pictures; a designer supplies only the sign.
- Lines at least 3 px (66 x 50, 80 x 60) or 2..3 px (the shooting sign); details and gaps at least 2 px; keep 2 px for the
  outline plus 1 px for the halo from the edges (a working area of about 60 x 44 in 66 x 50). Avoid thin parallel lines,
  1..2 px diagonals and small text.
- Stock composition: a pictogram on the left ~55 % and a short sign on the right ~40 %, the sign about 56 % of the height;
  rounded sans-serif letters, capitals 26..30 px at 66 x 50, 22..24 px at 39 px high, 20..22 px at 36 px.
- The smallest place, the shooting sign, is the test: the silhouette must read at 43 px wide on a dark background.
- Texts that are UI strings are not pictures: the look's name over the strip is a string (`look_name`), the preset's name on
  the editor is drawn by the renderer.

Generators: `OMKit/tools/mkicons_creatprf.py` (the 40 CREATPRF pictures) and `OMKit/tools/mkicons_film.py` (the 12
FILMLOOK pictures); both take their dial labels from `OMKit/tools/dial_label.py` (section 4.5.1). Both paint on a supersampled canvas and map coverage to meanings (>= 0.75 sign, >= 0.4 meaning 3, > 0.1
meaning 4). Module resource: type 1, id = the picture id, file `1_<id>.bin` = u16 width, u16 height, then the 4-bit pixels,
the high nibble first, each row a whole number of bytes. A profile's pictures may also come in the `ICON` section of an
`.isp` file: [../../formats/isp.md](../../formats/isp.md), section 4.

### 5.6. Strings

The look's name replaces the slot mode's name string in the loaded language's string table (a pointer to a UTF-16 copy in
the arena); after a language change core puts the pointer back. The string is used everywhere the UI shows that mode name
(firmware data; emulator). The profiles' names ("Color Profile 1..3", "Mono Profile 1..3") are the firmware's own strings.

## 6. Cassette pictures

A film look preset may carry a drawing - the `CANI` section of its `.isp` file, a list of OpenVG paths for a 256 x 256
surface in quarter pixels, y up (format: [../../formats/isp.md](../../formats/isp.md), section 5a). Paths rather than images
because the firmware itself never calls the OpenVG image functions; paths need no new firmware entry.

- The module passes the section to core (`look_art`); core checks every field once (version, kind, sizes, each record's
  operation, caps, width, segments, points, coordinate range) and copies it to the arena; a refused section is logged and the
  preset keeps the rectangle with its name. The renderer on CPU 1 trusts what core accepted.
- The renderer replays the records in order (fill non-zero, fill even-odd, stroke with butt / round / square caps), lifted
  66 px towards the top of the surface: the cassettes span rows 84..179 of 256 and start at row 18, where the curve's
  window begins (section 4.5.1; 80 px, to row 4, before core 1.29). It prints "n / N" under it. A preset without a
  drawing gets its grey rectangle at the same top.
- The grain item is drawn in the same style and place: a cassette split 2/3 : 1/3 - the tile's name in capitals on the label,
  a strip of four 22 x 22 pictograms Off, Low, Med, High with the chosen one framed in yellow, "n / N" of the tile under it;
  whole pixels only (about 12 fills).

## 7. What the UI shows and what a picture gets

| | The UI shows | Live view gets | A still gets | Basis |
|---|---|---|---|---|
| a profile | the profile (strip, column, SCP, shooting sign, menu line), its cells in the column | the profile's values, copied into the image module's context when it is rebuilt (about every second) | the profile's values written into the shooting parameters; not in movie variants, not in ART bracketing frames | measured on the camera |
| stored picture mode with a profile | the profile | - | Natural is stored; settings files, remote apps and other readers outside the UI see Natural | firmware code; remote apps: assumption |
| the film look | the slot mode's place, the look's pictures and name | Natural with the preset's ISP tables when the module's scope includes live view; edits at once (an edit every 40 ms at most) | Natural with the preset's ISP tables, plus grain when the strength is not Off | measured on the camera |
| grain | the grain item, the cassette | no grain | grain on stills of the look | OMKit code |
| the editors' drawings | over live view, in the image layer | - | - | measured on the camera |

Picture-mode details: [picture-modes.md](picture-modes.md); the ISP tables: [color-pipeline.md](color-pipeline.md); the still
path and in-camera RAW development: [still-pipeline.md](still-pipeline.md); grain: [grain.md](grain.md).

## 8. Limits and open points

- The column list cannot be rebuilt on demand; no trigger for the column function was found (emulator).
- On Color/Level the front dial is silent while no filter is set: the native code decides by its child's state, not by a
  read, so a substitute answer does not change it (emulator). This is why the grain item uses 5374, not 5373.
- Item 5374 serves both the look's Color/Vivid or Color/Level and the grain item; one of them needs another item if both
  are to be shown at once.
- The hint widgets with 900 x 600 coordinates (section 4.6) are not shown; making them visible would need their
  rectangles rewritten before the screen first opens (assumption, not tried).
- The emulator does not run the camera side, so every drawing was checked on a renderer bench and on the camera only.
