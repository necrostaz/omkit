/* Graphic profile editors for OM-5 Mark II 1.1.
 *
 * The firmware has no drawing of its own for the profile editor items Color/Level (monochrome), Color/Vivid (colour) and the
 * tone curve (both). Their item handlers get the "background" of the Color Creator item (thunks.s,
 * _drv_item_level, _drv_item_vivid, _drv_item_curve), so the
 * GUI orders the native Color Creator drawing for them: start when the cursor comes to the item, an order per dial step, end
 * when it leaves - all native; so are the values: the dials, INFO (one hue / all) and the reset by a long OK. The GUI task
 * publishes the profile's values here (drv_edp_level, drv_edp_vivid) before the order goes out; the Color Creator renderer's
 * entry on CPU 1 (_drv_tramp_c3d) calls drv_edp_draw, which draws our editor with the firmware's OpenVG instead of the ring.
 * The native Color Creator item (5029) and the core loop turn our mode off, so its own ring is drawn there.
 *
 * drv_edp_draw runs on CPU 1, in the firmware's C3D_MAIN task. Its data cache does not see what CPU 0 wrote through its
 * cache, so it touches no core variable: the block below only through the uncached alias, the constant tables only through
 * the uncached alias of the image (loaded past the cache), the work buffer in the arena. No log,
 * no services; no floating point (coordinates are integers, the constants are bit patterns).
 * Every uncached read is a bus access, so the one table read at every point - the sines - is copied to the renderer's
 * stack once a frame (a quarter wave, 74 bytes: edtrig.h); the other tables are read a few dozen times a frame, in place. */
#include "omk_drv.h"
#include "omk_core.h"
#include "omk_log.h"
#include "edtrig.h"

#define UNCACHED DRV_UNCACHED

/* drv_edp_shared, words; thunks.s uses EDP_ITEMS..EDP_MODE by offset */
#define EDP_ITEMS 0                         /* editor item handler calls (GUI task) */
#define EDP_RENDERS 1                       /* Color Creator renderer calls (CPU 1) */
#define EDP_CPU 2                           /* the CPU of the last renderer call */
#define EDP_SEQ 3                           /* odd while the GUI task writes MODE..VALUE and SAT..CUR */
#define EDP_MODE 4                          /* EDP_OFF, EDP_LEVEL, EDP_VIVID; the Color Creator item writes 0 (thunks.s) */
#define EDP_VALUE 5                         /* Level: filter | level << 8; curve: Hi + 7 | Mid + 7 << 8 | Sh + 7 << 16 |
                                               Mid edited << 24 */
#define EDP_DRAWN 6                         /* frames drawn by us */
#define EDP_FAILED 7                        /* frames left to the native code: no snapshot, no surface, no objects */
#define EDP_WORK 8                          /* arena work buffer for the path data */
#define EDP_SAT 9                           /* Vivid: 3 words, saturation + 5 (0..10) of hue i in byte i */
#define EDP_CUR 12                          /* Vivid: the hue under the cursor 0..11 (255 none) | all << 8 */
#define EDP_NAMES 13                        /* film look: the entries' names, LOOK_NAME bytes each (arena, uncached) */
#define EDP_INDEX 14                        /* film look: the chosen entry | the count << 16 */
#define EDP_ARTS 15                         /* film look: arena u32[] of the entries' cassette drawings (CANI), 0 - none */
#define EDP_T_ITEM 16                       /* film look, GUI task: drv_ticks of the last item handler call */
#define EDP_GAP_MIN 17                      /* film look: the shortest time between two item handler calls (us, under 2 s) - */
#define EDP_GAP_LAST 18                     /* how fast the GUI turns a dial step into a new frame; the last such time */
#define EDP_GAPS 19                         /* film look: the times counted */
#define EDP_GNAMES 20                       /* film look's grain item: the tiles' names, LOOK_NAME bytes each (arena) */
#define EDP_GVALUE 21                       /* ... the tile shown (0xff none) | the number of tiles << 8 | the strength << 16 */
#define EDP_DROPPED 22                      /* points that did not fit a path (MAX_PTS, MAX_SEG): that drawing is incomplete */
#define EDP_WORDS 32
#define EDP_OFF 0
#define EDP_LEVEL 1
#define EDP_VIVID 2
#define EDP_CURVE 3
#define EDP_LOOK 4                          /* the film look's item: the native Color Creator item 5029 in its column */
#define EDP_LOOK_LEVEL 5                    /* the film look's editors (item 5374 of a black-and-white preset): Color/Level */
#define EDP_LOOK_VIVID 6                    /* item 5374 of a colour preset: Color/Vivid on the preset's sectors (drv_edp_lk) */
#define EDP_LOOK_CURVE 7                    /* item 5375: the preset's curve (drv_edp_lk) and its three points */
#define EDP_LOOK_GRAIN 8                    /* item 5374 of the look as its grain item: the tile's name, the four strengths */
#define EDP_LAST EDP_LOOK_GRAIN
#define LOOK_NAME 32
#define HUES 12
#define MAX_PTS 320                         /* points per path (one fill) */
#define MAX_SEG 352                         /* segments: the points and the closes of the subpaths */
#define EDP_SEG_SIZE MAX_SEG                /* work buffer: the segments, then MAX_PTS points of 2 x s16 */
#define EDP_WORK_SIZE (EDP_SEG_SIZE + 4 * MAX_PTS)

u32 drv_edp_shared[EDP_WORDS] __attribute__((aligned(64), section(".data.cpu_shared")));

/* The film look's editors, written with the block above under its EDP_SEQ (GUI task), read by the renderer: through the
 * uncached alias only. Words: Color/Vivid - EDL_MASK the sectors the preset has, EDL_ANGLE 12 bytes of the vertices'
 * angles (2.5 degrees a step, clockwise from the top), EDL_RGBA their colours; the curve - EDL_CURVE 64 bytes, the output
 * 0..255 at the inputs k / 63 */
#define EDL_MASK 0
#define EDL_ANGLE 1
#define EDL_RGBA 4
#define EDL_CURVE 16
#define EDL_WORDS 32
u32 drv_edp_lk[EDL_WORDS] __attribute__((aligned(64), section(".data.cpu_shared")));

static volatile u32 *edp(void)
{
    return (volatile u32 *)((u32)drv_edp_shared + UNCACHED);
}

static volatile u32 *edl(void)
{
    return (volatile u32 *)((u32)drv_edp_lk + UNCACHED);
}

#define CELL_M_FILTER 1007                  /* + k: 0 none, 1 Ye, 2 Or, 3 R, 4 G, 5 GY, 6 M, 7 B, 8 C */
#define CELL_M_LEVEL 1012                   /* + k: 1..3 */
#define RESET_LEVEL 2                       /* a long OK on Color/Level: no filter, level 2 (emulator) */
#define CELL_C_SAT 928                      /* + 12k + i: -5..5 */
#define CELL_C_ONE 2197                     /* + k: 1 - one hue, 0 - all (INFO) */
#define CELL_C_CURSOR 2201                  /* + k: the cell of the hue under the cursor, 928 + 12k + i; 0 - all */
#define ID_ITEM 2148                        /* the picture mode screen's item under the cursor */
#define ITEM_LEVEL 5373
#define ITEM_VIVID 5374
#define ITEM_CURVE 5375
#define ITEM_CC 5029
#define CELL_C_TONE 976                     /* + 3k: Hi, Mid, Sh (-7..7) */
#define CELL_M_TONE 1021
#define CELL_C_TONE_MID 2189                /* + k: 1 - INFO chose Mid (both dials edit it), 0 - Hi and Sh */
#define CELL_M_TONE_MID 2193
#define GUI_STATE 0xa234f838                /* uncached: byte 0 group, byte 1 state; the direct picture mode screen 04 18 */

/* The dial labels above the drawing: widgets 0x3d9 and 0x3db of the Color Creator layout. "front" and "rear" in this
 * file name the widgets, not the dials: 0x3d9 ("Color") is over the camera's rear dial (the cursor, setting 2201) and
 * 0x3db ("Vivid") over its front dial (the value, cells 928..) - checked on the camera on Color/Vivid and Color/Level.
 * Shared by 5029, 5373, 5374. The GUI reads their picture from these state words (section 7 data, outside the
 * protected range, CPU 0 only) at every draw and draws it centred in the widget, clipped by it.
 * Every label is one face, size and place (OMKit/docs/platform/om5m2/ui.md, section 4.5): the camera's own words where
 * it has them, a module's in the same face (tools/dial_label.py), all at most 56 x 24 in the stock 56 x 24 widgets, which
 * are never widened. Color/Level shows the camera's "Color" and the module's "Level" */
#define LABEL_FRONT 0x610c1774
#define LABEL_REAR 0x610c181c
#define PIC_COLOR 4221
#define PIC_VIVID 4222
#define PIC_LEVEL 4224                      /* pictprof.c IMG_LABEL_LEVEL */
#define PIC_HI 4215                         /* the camera's own, of its H&S screen */
#define PIC_SH 4216
#define PIC_MID 4339
/* The dial pictograms above the labels, widgets of the same layout with one state each {0xffff, 7, 7, picture}, read the
 * same way at every draw (emulator): 0x181 (30 x 20, rectangles 0x610c187e, descriptor 0x610c1914, states 0x610c1740) over
 * the front label shows 10922, the rear dial's pictogram; 0x3d8 (rectangles 0x610c188e, descriptor 0x610c1968, states
 * 0x610c1754) over the rear label shows 10923, the front dial's. Which dial: on Color Creator 10922 stands over "Color",
 * the rear dial's value (camera), and the firmware's own H&S screen puts 10923 over "Sh" on the left and 10922 over "Hi"
 * on the right (state lists 0x61005124, 0x6100534c; firmware data). The curve item takes that screen's layout: the
 * labels and the pictograms swapped, "Sh" on the left over the graph's shadows, "Hi" on the right */
#define ICON_FRONT 0x610c174c
#define ICON_REAR 0x610c1760
#define PIC_DIAL_REAR 10922
#define PIC_DIAL_FRONT 10923

/* LABELS_STOCK is the native Color Creator item (and every other screen); the rest are our items. LABELS_PLAIN: the stock
 * words and pictograms on one of our items (Color/Vivid of a colour profile, the look's editor item without values) */
enum { LABELS_STOCK, LABELS_PLAIN, LABELS_LEVEL, LABELS_CURVE, LABELS_CURVE_MID, LABELS_LOOK, LABELS_LOOK_VIVID,
       LABELS_GRAIN };
#define PIC_PREV 4234                       /* pictprof.c IMG_LOOK_PREV, IMG_LOOK_NEXT: the film look's dial labels */
#define PIC_NEXT 4235
#define PIC_TYPE 4240                       /* pictprof.c IMG_LOOK_TYPE, IMG_LOOK_STRENGTH: the grain item's dial labels */
#define PIC_STRENGTH 4241                   /* (the tile's and the strength's: the module's words, e.g. "Tile", "Level") */
/* The colour sample next to "Color" (widget 0x3da): the picture words of its state list (4227..4233, one per hue group);
 * on the film look's item they show the empty stub 4236, so nothing is drawn there. On the items 5373, 5374 and 5375
 * the GUI does not draw the sample at all (emulator: no draw of it in the widget trace) */
#define SAMPLE_WORD 0x610c1788
#define SAMPLES 7
#define PIC_SAMPLE 4227
#define PIC_BLANK 4236
/* More picture words of the Color Creator layout (block 0x610c1968) blanked the same way. The comma after the sample
 * (widget 0xf6d, 10834, 10 x 24 at (546, 85): the stock screen reads "Color <sample>, Vivid"): the GUI draws it on the
 * items 5373, 5374 and 5375 too, where it draws no sample (emulator widget trace; seen on the camera as a dot between the
 * labels), so it is blank on every item of ours and stock only on the native Color Creator item. The frame of the Reset
 * button (widget 0x3d6: 10180, 10181, 10180): blank on the film look's item */
static const struct { u32 word, pic; } blanks[] = {
    {0x610c1808, 10834}, {0x610c1830, 10180}, {0x610c1840, 10181}, {0x610c1850, 10180}};

/* A label word: written only while it holds one of the pictures we put there */
static void label_put(u32 a, u32 stock, u32 to)
{
    volatile u32 *w = (volatile u32 *)a;
    u32 v = *w;

    if (v == stock || v == PIC_LEVEL || v == PIC_HI || v == PIC_SH || v == PIC_MID || v == PIC_PREV ||
        v == PIC_NEXT || v == PIC_TYPE || v == PIC_STRENGTH)
        *w = to;
}

/* The colour sample, the comma after it and the Reset frame: blank or the stock pictures; each word only while it holds
 * one of the two */
static void samples(u32 sample, u32 comma, u32 frame)
{
    volatile u32 *w;
    u32 i, blank;

    for (i = 0; i < SAMPLES; i++) {
        w = (volatile u32 *)(SAMPLE_WORD + 16 * i);
        if (*w == PIC_SAMPLE + i || *w == PIC_BLANK)
            *w = sample ? PIC_BLANK : PIC_SAMPLE + i;
    }
    for (i = 0; i < sizeof(blanks) / sizeof(blanks[0]); i++) {
        w = (volatile u32 *)blanks[i].word;
        blank = i ? frame : comma;          /* blanks[0] is the comma */
        if (*w == blanks[i].pic || *w == PIC_BLANK)
            *w = blank ? PIC_BLANK : blanks[i].pic;
    }
}

/* A dial pictogram's word: written only while it holds one of the two pictograms */
static void icon_put(u32 a, u32 to)
{
    volatile u32 *w = (volatile u32 *)a;
    u32 v = *w;

    if (v == PIC_DIAL_REAR || v == PIC_DIAL_FRONT)
        *w = to;
}

/* The dial labels of the editor: Color/Level's (the camera's "Color" over the filter's dial, the module's "Level" - only
 * with its picture in place), the curve's (the camera's own "Sh" on the left and "Hi" on the right with their pictograms
 * swapped along; "Mid" on both while INFO chose it - then both dials edit Mid, the pictograms stay where they were), or
 * the stock ones. GUI task and core task, CPU 0 */
static void labels(u32 state)
{
    u32 front = PIC_COLOR, rear = PIC_VIVID, swap = 0;

    if (state == LABELS_LEVEL && drv_prof_picture_set(OMK_PIC_EDITOR(OMK_EDITOR_LABEL_LEVEL))) {
        rear = PIC_LEVEL;
    } else if (state == LABELS_CURVE) {
        front = PIC_SH;                     /* Sh is edited by the front dial ("main" in the emulator), */
        rear = PIC_HI;                      /* Hi by the rear dial ("sub" in the emulator) */
        swap = 1;
    } else if (state == LABELS_CURVE_MID) {
        front = rear = PIC_MID;
        swap = 1;
    } else if (state == LABELS_LOOK && drv_look_labels()) {
        front = PIC_PREV;
        rear = PIC_NEXT;
    } else if (state == LABELS_GRAIN && drv_look_grain_labels()) {
        front = PIC_TYPE;                   /* over the dial of the cursor 2201 (the emulator's "sub", the rear dial): the
                                               tile */
        rear = PIC_STRENGTH;                /* over the dial of the cells 928.. (the emulator's "main", the front dial): the
                                               strength */
    }
    label_put(LABEL_FRONT, PIC_COLOR, front);
    label_put(LABEL_REAR, PIC_VIVID, rear);
    icon_put(ICON_FRONT, swap ? PIC_DIAL_FRONT : PIC_DIAL_REAR);
    icon_put(ICON_REAR, swap ? PIC_DIAL_REAR : PIC_DIAL_FRONT);
    /* the film look's Color/Vivid: the camera's sample of hue i is not the colour of the preset's sector i. The comma:
       without a sample before it on every item of ours */
    samples(state == LABELS_LOOK || state == LABELS_LOOK_VIVID || state == LABELS_GRAIN, state != LABELS_STOCK,
            state == LABELS_LOOK);
}

/* GUI task, from _drv_item_level, after the item's own handler: the values the order is about to be drawn with.
 * 1 - they differ from what a long OK resets them to (no filter, level 2): the trampoline shows the Reset hint */
u32 drv_look_item(u32 item);                /* pictprof.c: the film look's editor items */

u32 drv_edp_level(void)
{
    volatile u32 *e = edp();
    u32 p = drv_prof_get(), k, v;

    e[EDP_ITEMS]++;
    if (!OMK_PROF_IS_MONO(p) || !OMK_PROF_NUM(p)) {
        e[EDP_MODE] = EDP_OFF;
        labels(LABELS_STOCK);
        return 0;
    }
    k = OMK_PROF_NUM(p) - 1;
    v = (drv_setting_read(CELL_M_FILTER + k) & 0xff) | (drv_setting_read(CELL_M_LEVEL + k) & 0xff) << 8;
    e[EDP_SEQ]++;
    e[EDP_VALUE] = v;
    e[EDP_MODE] = EDP_LEVEL;
    e[EDP_SEQ]++;
    labels(LABELS_LEVEL);                   /* after the mode: the core loop puts the stock labels back only without it */
    return v != (RESET_LEVEL << 8);
}

/* GUI task, from _drv_item_vivid, after the item's own handler: the 12 saturations, the cursor and "all".
 * 1 - a saturation is not 0 (a long OK resets them all to 0): the trampoline shows the Reset hint */
u32 drv_edp_vivid(void)
{
    volatile u32 *e = edp();
    u32 p = drv_prof_get(), k, i, cur, sat[3] = {0, 0, 0}, changed = 0;
    s32 v;

    e[EDP_ITEMS]++;
    if (drv_look_active())
        return drv_look_item(ITEM_VIVID);
    if (OMK_PROF_IS_MONO(p) || !OMK_PROF_NUM(p)) {
        e[EDP_MODE] = EDP_OFF;
        labels(LABELS_STOCK);
        return 0;
    }
    k = OMK_PROF_NUM(p) - 1;
    for (i = 0; i < HUES; i++) {
        v = (s32)drv_setting_read(CELL_C_SAT + 12 * k + i);
        v = v < -5 ? -5 : v > 5 ? 5 : v;
        changed |= v != 0;
        sat[i / 4] |= (u32)(v + 5) << (8 * (i % 4));
    }
    i = drv_setting_read(CELL_C_CURSOR + k);  /* the front dial: a hue's cell, or 0 - all hues (emulator: the rear dial then
                                                 changes all 12); 3200 before the first step */
    cur = i - (CELL_C_SAT + 12 * k);
    cur = cur < HUES ? cur : 255;
    if (!i || !drv_setting_read(CELL_C_ONE + k))
        cur |= 1u << 8;
    e[EDP_SEQ]++;
    for (i = 0; i < 3; i++)
        e[EDP_SAT + i] = sat[i];
    e[EDP_CUR] = cur;
    e[EDP_MODE] = EDP_VIVID;
    e[EDP_SEQ]++;
    labels(LABELS_PLAIN);                   /* the stock words, no comma; after the mode, as in drv_edp_level */
    return changed;
}

static s32 tone_cell(u32 id)
{
    s32 v = (s32)drv_setting_read(id);

    return v < -7 ? -7 : v > 7 ? 7 : v;
}

/* GUI task, from _drv_item_curve, after the item's own handler: Hi, Mid, Sh of the profile and what INFO chose.
 * 1 - one of them is not 0 (a long OK resets them to 0): the trampoline shows the Reset hint */
u32 drv_edp_curve(void)
{
    volatile u32 *e = edp();
    u32 p = drv_prof_get(), k, base, mid, v;
    s32 hi, md, sh;

    e[EDP_ITEMS]++;
    if (drv_look_active())
        return drv_look_item(ITEM_CURVE);
    if (!OMK_PROF_NUM(p)) {
        e[EDP_MODE] = EDP_OFF;
        labels(LABELS_STOCK);
        return 0;
    }
    k = OMK_PROF_NUM(p) - 1;
    base = (OMK_PROF_IS_MONO(p) ? CELL_M_TONE : CELL_C_TONE) + 3 * k;
    hi = tone_cell(base);
    md = tone_cell(base + 1);
    sh = tone_cell(base + 2);
    mid = drv_setting_read((OMK_PROF_IS_MONO(p) ? CELL_M_TONE_MID : CELL_C_TONE_MID) + k) == 1;
    v = (u32)(hi + 7) | (u32)(md + 7) << 8 | (u32)(sh + 7) << 16 | mid << 24;
    e[EDP_SEQ]++;
    e[EDP_VALUE] = v;
    e[EDP_MODE] = EDP_CURVE;
    e[EDP_SEQ]++;
    labels(mid ? LABELS_CURVE_MID : LABELS_CURVE);
    return hi || md || sh;
}

/* The film look's editor items (pictprof.c, GUI task, from the item trampolines): what the order is about to draw */
void drv_edp_look_level(u32 filter, u32 level)
{
    volatile u32 *e = edp();

    e[EDP_SEQ]++;
    e[EDP_VALUE] = (filter & 0xff) | (level & 0xff) << 8;
    e[EDP_MODE] = EDP_LOOK_LEVEL;
    e[EDP_SEQ]++;
    labels(LABELS_LEVEL);
}

void drv_edp_look_vivid(const signed char *sat, u32 cur, u32 mask, const u8 *angle, const u32 *rgba)
{
    volatile u32 *e = edp(), *l = edl();
    u32 i, w[3] = {0, 0, 0}, a[3] = {0, 0, 0};

    for (i = 0; i < HUES; i++) {
        w[i / 4] |= (u32)(sat[i] + 5) << (8 * (i % 4));
        a[i / 4] |= (u32)angle[i] << (8 * (i % 4));
    }
    e[EDP_SEQ]++;
    for (i = 0; i < 3; i++) {
        e[EDP_SAT + i] = w[i];
        l[EDL_ANGLE + i] = a[i];
    }
    for (i = 0; i < HUES; i++)
        l[EDL_RGBA + i] = rgba[i];
    l[EDL_MASK] = mask;
    e[EDP_CUR] = cur;
    e[EDP_MODE] = EDP_LOOK_VIVID;
    e[EDP_SEQ]++;
    labels(LABELS_LOOK_VIVID);
}

void drv_edp_look_curve(const signed char *tone, u32 mid, const u8 *curve)
{
    volatile u32 *e = edp(), *l = edl();
    u32 i, w;

    e[EDP_SEQ]++;
    e[EDP_VALUE] = (u32)(tone[2] + 7) | (u32)(tone[1] + 7) << 8 | (u32)(tone[0] + 7) << 16 | (mid & 1) << 24;
    for (i = 0; i < 16; i++) {
        w = curve[4 * i] | curve[4 * i + 1] << 8 | curve[4 * i + 2] << 16 | (u32)curve[4 * i + 3] << 24;
        l[EDL_CURVE + i] = w;
    }
    e[EDP_MODE] = EDP_LOOK_CURVE;
    e[EDP_SEQ]++;
    labels(mid ? LABELS_CURVE_MID : LABELS_CURVE);
}

/* The film look's grain item (pictprof.c, GUI task): the tiles' names (arena), the tile shown (0xff none), how many, the
 * strength 0..3 */
void drv_edp_look_grain(u32 names, u32 tile, u32 count, u32 level)
{
    volatile u32 *e = edp();

    e[EDP_SEQ]++;
    e[EDP_GNAMES] = names;
    e[EDP_GVALUE] = (tile & 0xff) | (count & 0xff) << 8 | (level & 0xff) << 16;
    e[EDP_MODE] = EDP_LOOK_GRAIN;
    e[EDP_SEQ]++;
    labels(LABELS_GRAIN);
}

/* The look's editor item without the module's editors (an older module, before its first values, enable_edits = off until
 * the next list): the chosen entry's picture, as on the look's item, rather than the Color Creator ring; the stock labels
 * (without the comma: the item is 5374 or 5375) */
void drv_edp_look_none(void)
{
    volatile u32 *e = edp();

    e[EDP_SEQ]++;
    e[EDP_MODE] = EDP_LOOK;
    e[EDP_SEQ]++;
    labels(LABELS_PLAIN);
}

/* Core task, every loop: off when the editor item is not open. The native Color Creator item turns it off by itself;
 * this covers the other places that order a Color Creator drawing. Also the renderer's dropped points to the log: the
 * renderer (CPU 1) cannot log, it only counts; a warning at most every DROP_TOLD_MS while frames keep losing points */
#define DROP_TOLD_MS 5000

void drv_edp_service(void)
{
    static u32 drop_seen, drop_ms;
    volatile u32 *e = edp();
    const volatile u8 *g = (const volatile u8 *)GUI_STATE;
    u32 mode = e[EDP_MODE], item, dropped = e[EDP_DROPPED], now;

    if (dropped != drop_seen) {
        now = omk_ms();
        if (!drop_ms || now - drop_ms >= DROP_TOLD_MS) {
            log_printf(OMK_LOG_WARN, "editor: %u points did not fit their paths since the last report (mode %u): "
                       "the drawing is incomplete", dropped - drop_seen, mode);
            drop_seen = dropped;
            drop_ms = now | 1;
        }
    }

    if (mode != EDP_OFF) {
        item = g[0] == 0x04 && g[1] == 0x18 ? drv_setting_read(ID_ITEM) : 0;
        if (item != (mode == EDP_LEVEL ? ITEM_LEVEL :
                     mode == EDP_VIVID || mode == EDP_LOOK_VIVID || mode == EDP_LOOK_LEVEL || mode == EDP_LOOK_GRAIN ? ITEM_VIVID :
                     mode == EDP_LOOK ? ITEM_CC : ITEM_CURVE) &&
            !(mode == EDP_LOOK && (item == ITEM_VIVID || item == ITEM_CURVE)))   /* the look's editors without values */
            e[EDP_MODE] = mode = EDP_OFF;
    }
    if (mode == EDP_OFF || mode > EDP_LAST)   /* every mode of ours keeps its words (labels, pictograms, the blank comma) */
        labels(LABELS_STOCK);
}

/* GUI task, from _drv_item_cc (the native Color Creator item): its ring and its labels - or, while the film look is active
 * (its column holds this item), our rectangle with the chosen entry's name */
u32 drv_edp_cc(void)
{
    volatile u32 *e = edp();

    u32 now, gap;

    if (drv_look_active()) {
        now = drv_ticks();
        gap = (now - e[EDP_T_ITEM]) & DRV_TICKS_MASK;
        if (e[EDP_T_ITEM] && gap < 2000000) {
            e[EDP_GAP_LAST] = gap;
            if (!e[EDP_GAPS] || gap < e[EDP_GAP_MIN])
                e[EDP_GAP_MIN] = gap;
            e[EDP_GAPS]++;
        }
        e[EDP_T_ITEM] = now;
        e[EDP_ITEMS]++;
        e[EDP_SEQ]++;
        e[EDP_MODE] = EDP_LOOK;
        e[EDP_SEQ]++;
        labels(LABELS_LOOK);
        return 1;                           /* the trampoline runs the item's handler and drops its Reset hint */
    }
    e[EDP_MODE] = EDP_OFF;
    labels(LABELS_STOCK);
    return 0;
}

/* The film look's cassette drawings: an arena table of the entries' CANI sections (checked by core), or 0 */
void drv_edp_look_arts(u32 table)
{
    volatile u32 *e = edp();

    e[EDP_SEQ]++;
    e[EDP_ARTS] = table;
    e[EDP_SEQ]++;
}

/* The film look's entries for the drawing: names (arena, LOOK_NAME bytes each), the chosen one, how many. GUI task (a dial
 * step) and core task, CPU 0, under drv_lock */
void drv_edp_look(u32 names, u32 index, u32 count)
{
    volatile u32 *e = edp();

    e[EDP_SEQ]++;
    e[EDP_NAMES] = names;
    e[EDP_INDEX] = (index & 0xffff) | count << 16;
    e[EDP_SEQ]++;
}

/* The firmware code the editor calls, as in this firmware: the first 8 bytes of each entry (thunks.s, fw.ld) */
static const struct {
    u32 addr;
    u8 bytes[8];
} fingerprint[] = {
    {0x613dab09, {0x93, 0x87, 0x82, 0x96, 0xcd, 0x55, 0xf6, 0x00}},   /* vgCreatePath */
    {0x613dab37, {0x87, 0x82, 0xcd, 0x29, 0xf6, 0x00, 0x04, 0xb0}},   /* vgClearPath */
    {0x613dabd5, {0x93, 0x87, 0x82, 0x96, 0xcd, 0x89, 0xf5, 0x00}},   /* vgAppendPathData */
    {0x613dacf8, {0x46, 0x0c, 0x83, 0xcd, 0x67, 0xf4, 0x00, 0x04}},   /* vgDrawPath */
    {0x613dab53, {0x82, 0xcd, 0x0e, 0xf6, 0x00, 0x04, 0xb0, 0x00}},   /* vgDestroyPath */
    {0x613dad41, {0xcd, 0x21, 0xf4, 0x00, 0x04, 0x00, 0xb0, 0x00}},   /* vgCreatePaint */
    {0x613dadc0, {0x87, 0x82, 0xcd, 0xa0, 0xf3, 0x00, 0x04, 0xb0}},   /* vgSetColor */
    {0x613dad89, {0x87, 0x82, 0xcd, 0xd7, 0xf3, 0x00, 0x04, 0xb0}},   /* vgSetPaint */
    {0x613dad5a, {0x82, 0xcd, 0x07, 0xf4, 0x00, 0x04, 0xb0, 0x00}},   /* vgDestroyPaint */
    {0x613da315, {0x87, 0x82, 0xcd, 0x4b, 0xfe, 0x00, 0x04, 0xb0}},   /* vgSeti */
    {0x613da331, {0x92, 0x87, 0x82, 0xcd, 0x2e, 0xfe, 0x00, 0x04}},   /* vgSetfv */
    {0x613da235, {0xcd, 0x2d, 0xff, 0x00, 0x04, 0xb0, 0x00, 0xc8}},   /* vgLoadIdentity */
    {0x613daad8, {0x93, 0x46, 0x08, 0x83, 0xf1, 0xd6, 0xcd, 0x84}},   /* vgClear */
    {0x60ccf0f8, {0xfc, 0xa0, 0x44, 0x47, 0x5b, 0x62, 0xfc, 0xa1}},   /* the renderers' eglSwapBuffers */
    {0x613da2f9, {0x87, 0x82, 0xcd, 0x67, 0xfe, 0x00, 0x04, 0xb0}},   /* vgSetf */
    {0x60e4af24, {0x43, 0x0c, 0x00, 0x42, 0x14, 0x42, 0x18, 0x97}},   /* handler of the Color/Level item */
    {0x60e4ac9b, {0x43, 0x18, 0x47, 0x14, 0x00, 0x42, 0x24, 0x42}},   /* handler of the Color/Vivid item */
    {0x60e4b018, {0x80, 0x01, 0xfa, 0x51, 0x07, 0x01, 0xfa, 0x21}},   /* handler of the tone curve item */
    {0x60e49e1f, {0x9c, 0x91, 0xcd, 0x19, 0x0d, 0xf0, 0x2c, 0xca}},   /* the dispatcher's Color Creator branch */
    {0x60e4ab3a, {0x00, 0x42, 0x10, 0x42, 0x14, 0x42, 0x18, 0xf1}},   /* handler of the Color Creator item (film look) */
};

/* Core task, at the profiles' install, before the renderer hook. 0, 1 - the firmware code differs, 2 - no memory */
int drv_edp_install(void)
{
    void *work;
    u32 i, k;

    for (i = 0; i < sizeof(fingerprint) / sizeof(fingerprint[0]); i++)
        for (k = 0; k < sizeof(fingerprint[0].bytes); k++)
            if (((const volatile u8 *)fingerprint[i].addr)[k] != fingerprint[i].bytes[k]) {
                log_printf(OMK_LOG_WARN, "profiles: editor: firmware code at %x differs", fingerprint[i].addr);
                return 1;
            }
    work = mem_alloc(EDP_WORK_SIZE);
    if (!work)
        return 2;
    edp()[EDP_WORK] = (u32)work;
    return 0;
}

#ifdef OMK_DEBUG
void drv_edp_gaps(u32 *min, u32 *last, u32 *count)
{
    volatile u32 *e = edp();

    *min = e[EDP_GAP_MIN];
    *last = e[EDP_GAP_LAST];
    *count = e[EDP_GAPS];
}
#endif

void drv_edp_stats(u32 *items, u32 *renders, u32 *cpu, u32 *drawn, u32 *failed)
{
    volatile u32 *e = edp();

    *items = e[EDP_ITEMS];
    *renders = e[EDP_RENDERS];
    *cpu = e[EDP_CPU];
    *drawn = e[EDP_DRAWN];
    *failed = e[EDP_FAILED];
}

/* ------------------------------------------------------------------------------------------------------------------
 * The renderer, CPU 1. OpenVG 1.1 of the firmware through thunks.s (registers as the firmware's own call sites) */
u32 drv_vg_create_path(u32 format, u32 type, u32 scale, u32 bias, u32 seg_hint, u32 coord_hint, u32 caps);
void drv_vg_clear_path(u32 path, u32 caps);
void drv_vg_append(u32 path, u32 nseg, const u8 *seg, const void *data);
void drv_vg_draw_path(u32 path, u32 modes);
void drv_vg_destroy_path(u32 path);
u32 drv_vg_create_paint(void);
void drv_vg_set_color(u32 paint, u32 rgba);
void drv_vg_set_paint(u32 paint, u32 modes);
void drv_vg_destroy_paint(u32 paint);
void drv_vg_seti(u32 type, u32 value);
void drv_vg_setfv(u32 type, u32 count, const u32 *values);
void drv_vg_setf(u32 type, u32 value);      /* value: the bits of a float */
void drv_vg_load_identity(void);
void drv_vg_clear(u32 x, u32 y, u32 w, u32 h);
void drv_c3d_swap(void);                    /* the renderer's own end: eglSwapBuffers of its display and surface */

#define VG_MATRIX_MODE 0x1100
#define VG_MATRIX_PATH_USER_TO_SURFACE 0x1400
#define VG_FILL_RULE 0x1101
#define VG_NON_ZERO 0x1901
#define VG_BLEND_MODE 0x1104
#define VG_BLEND_SRC_OVER 0x2001
#define VG_MASKING 0x1130
#define VG_SCISSORING 0x1131
#define VG_CLEAR_COLOR 0x1121
#define VG_PATH_DATATYPE_S_16 1
#define VG_PATH_CAPABILITY_ALL 0xfff
#define VG_STROKE_PATH 1
#define VG_FILL_PATH 2
#define VG_STROKE_LINE_WIDTH 0x1110
#define VG_STROKE_CAP_STYLE 0x1111
#define VG_CAP_BUTT 0x1700
#define VG_STROKE_JOIN_STYLE 0x1112
#define VG_JOIN_ROUND 0x1801
#define F_1 0x3f800000u                     /* 1.0f, 1.5f, 2.5f: stroke widths in pixels */
#define F_1_5 0x3fc00000u
#define F_2_5 0x40200000u
#define F_2 0x40000000u
#define F_3 0x40400000u
#define VG_MOVE_TO_ABS 2
#define VG_LINE_TO_ABS 4
#define VG_CUBIC_TO_ABS 12
#define VG_CLOSE_PATH 0
#define F_QUARTER 0x3e800000u               /* 0.25f: path coordinates in quarter pixels */
#define SURFACE_W 0x625b475c                /* C3D_MAIN globals: the surface size */
#define SURFACE_H 0x625b4760

#define STEPS EDP_STEPS                     /* angle unit: 2.5 degrees, clockwise from the top (edtrig.h) */
#define Q 4                                 /* quarter pixels */
#define R_OUT (125 * Q)                     /* background disc */
#define R_RING (117 * Q)                    /* the hue ring 107..117 */
#define R_IN (107 * Q)
#define SECTORS 48                          /* 7.5 degrees each */

/* The hue ring, RGBA by sector from the top, clockwise: the filters at 40 degrees each in the dial's order - Ye 40, Or 80,
 * R 120, M 160, B 200, C 240, G 280, GY 320, none at the top - with the hues in between; a fifth grey */
static const u32 ring_tab[SECTORS] = {
    0xbfc640ffu, 0xc6c63fffu, 0xcdc73effu, 0xd4c73dffu, 0xdac83dffu, 0xe0c63cffu, 0xe0bd3cffu, 0xe0b33cffu,
    0xe0a93cffu, 0xe09f3cffu, 0xe0963cffu, 0xde883effu, 0xda7a42ffu, 0xd86c44ffu, 0xd45d48ffu, 0xd24f4affu,
    0xce4854ffu, 0xca4965ffu, 0xc74a75ffu, 0xc34b86ffu, 0xbf4b96ffu, 0xba4ca5ffu, 0xaa50aeffu, 0x9c52b6ffu,
    0x8c56beffu, 0x7e58c6ffu, 0x6e5ccfffu, 0x6467d0ffu, 0x5c75d0ffu, 0x5282d0ffu, 0x4a90d0ffu, 0x409dd0ffu,
    0x3da5c6ffu, 0x3ea7b2ffu, 0x40aa9dffu, 0x41ac89ffu, 0x43ae75ffu, 0x46b063ffu, 0x56b45dffu, 0x64b657ffu,
    0x74ba51ffu, 0x82bc4bffu, 0x92c045ffu, 0x9ac143ffu, 0xa2c243ffu, 0xa9c342ffu, 0xb1c441ffu, 0xb8c540ffu,
};

/* Filter value (cell 1007) -> position on the ring, 40 degrees each (0 - none, at the top) */
static const u8 filter_pos[9] = {0, 1, 2, 3, 7, 8, 4, 5, 6};
static const u16 level_r[4] = {0, 38 * Q, 75 * Q, 112 * Q};

/* Color/Vivid: the 12 hues at 30 degrees each from the top, clockwise (hue 0 - yellow) */
static const u32 hue_tab[HUES] = {
    0xe8d42cffu, 0xe8962cffu, 0xe65032ffu, 0xe03246ffu, 0xd8327affu, 0xc437aaffu,
    0x8c46c8ffu, 0x465ad7ffu, 0x3282d7ffu, 0x28afc8ffu, 0x32b982ffu, 0x50b93cffu,
};
#define V_R0 (24 * Q)                       /* radius of saturation -5; +9 px a step: 0 at 69, +5 at 114 */
#define V_STEP 36
#define V_FRAME_IN (116 * Q)                /* the frame 12-gon */
#define V_FRAME_OUT (125 * Q)

typedef struct {
    u32 path, paint, color;                 /* color: the paint's colour now, 0 - not set */
    u8 *seg;                                /* arena */
    s16 *xy;
    u32 ns, np, start;                      /* start: the next point begins a subpath */
    u32 dropped;                            /* points left out, the path was full (drv_edp_draw adds them to EDP_DROPPED) */
    s32 cx, cy;                             /* centre, quarter pixels */
    const volatile u32 *ring;               /* the hue ring's table through the uncached alias */
    s16 sin[EDP_SIN_N];                     /* the frame's own copy of the quarter sine table (edtrig.h) */
} pen;

static s32 sin_at(const pen *p, u32 a)
{
    return edp_sin(p->sin, a);
}

static s32 cos_at(const pen *p, u32 a)
{
    return edp_cos(p->sin, a);
}

static void put(pen *p, s32 x, s32 y)
{
    if (p->np >= MAX_PTS || p->ns >= MAX_SEG - 1) {
        p->dropped++;                       /* a shape too long for one path: fill() between its parts */
        return;
    }
    p->seg[p->ns++] = p->start ? VG_MOVE_TO_ABS : VG_LINE_TO_ABS;
    p->start = 0;
    p->xy[2 * p->np] = (s16)x;
    p->xy[2 * p->np + 1] = (s16)y;
    p->np++;
}

/* The end of a subpath: shapes of one colour go into one path, filled once (non-zero: overlaps are one area, no seams) */
static void end_sub(pen *p)
{
    if (!p->start && p->ns < MAX_SEG)
        p->seg[p->ns++] = VG_CLOSE_PATH;
    p->start = 1;
}

/* A point at radius r, angle a; y is up in OpenVG */
static void polar(pen *p, s32 r, u32 a)
{
    put(p, p->cx + ((r * sin_at(p, a)) >> 14), p->cy + ((r * cos_at(p, a)) >> 14));
}

static void fill(pen *p, u32 rgba)
{
    end_sub(p);
    if (p->np >= 3) {
        drv_vg_clear_path(p->path, VG_PATH_CAPABILITY_ALL);
        drv_vg_append(p->path, p->ns, p->seg, p->xy);
        if (rgba != p->color)
            drv_vg_set_color(p->paint, rgba);
        p->color = rgba;
        drv_vg_draw_path(p->path, VG_FILL_PATH);
    }
    p->ns = p->np = 0;
}

/* A ring sector r_in..r_out from angle a0 to a1 (r_in 0 - a pie slice) */
static void sector(pen *p, s32 r_in, s32 r_out, u32 a0, u32 a1)
{
    u32 a;

    for (a = a0; a <= a1; a++)
        polar(p, r_out, a);
    if (!r_in)
        put(p, p->cx, p->cy);
    else
        for (a = a1 + 1; a-- > a0;)
            polar(p, r_in, a);
    end_sub(p);
}

static void disc(pen *p, s32 r)
{
    u32 a;

    for (a = 0; a < STEPS; a += 3)
        polar(p, r, a);
    end_sub(p);
}

static void stroke(pen *p, u32 rgba, u32 width);

/* A thin circle is its 48-gon (disc: a corner every 7.5 degrees, at most a quarter pixel inside the true circle at these
 * radii) stroked: disc(), then stroke() with the width - 48 points, where the filled ring this replaces took 304 (four
 * quarter rings with both edges at 2.5 degrees). Circles of one colour that do not touch go into one path */

/* A bar from radius r0 to r1 along angle a, half width hw */
static void bar(pen *p, s32 r0, s32 r1, u32 a, s32 hw)
{
    s32 sx = sin_at(p, a), cy = cos_at(p, a);
    s32 x0 = p->cx + ((r0 * sx) >> 14), y0 = p->cy + ((r0 * cy) >> 14);
    s32 x1 = p->cx + ((r1 * sx) >> 14), y1 = p->cy + ((r1 * cy) >> 14);
    s32 nx = (hw * cy) >> 14, ny = -((hw * sx) >> 14);

    put(p, x0 + nx, y0 + ny);
    put(p, x1 + nx, y1 + ny);
    put(p, x1 - nx, y1 - ny);
    put(p, x0 - nx, y0 - ny);
    end_sub(p);
}

static void rect(pen *p, s32 x, s32 y, s32 hw, s32 hh)
{
    put(p, x - hw, y - hh);
    put(p, x + hw, y - hh);
    put(p, x + hw, y + hh);
    put(p, x - hw, y + hh);
    end_sub(p);
}

static u32 dim(u32 rgba, u32 k)             /* RGB * k / 256, alpha kept */
{
    return (((rgba >> 24) * k >> 8) << 24) | ((((rgba >> 16) & 0xff) * k >> 8) << 16) | ((((rgba >> 8) & 0xff) * k >> 8) << 8) |
           (rgba & 0xff);
}

/* About 90 fills: one per colour */
static void draw_level(pen *p, u32 filter, u32 level)
{
    u32 k, a;
    s32 r, x, y;
    const volatile u8 *pos = (const volatile u8 *)((u32)filter_pos + UNCACHED);
    const volatile u16 *lr = (const volatile u16 *)((u32)level_r + UNCACHED);

    a = filter <= 8 ? pos[filter] * 16u : 0;   /* 40 degrees = 16 steps */
    r = level <= 3 ? lr[level] : lr[3];
    disc(p, R_OUT);
    fill(p, 0x262626f0u);
    if (filter) {                           /* the disc inside: the hues, dimmed, 15 degrees each */
        for (k = 0; k < SECTORS; k += 2) {
            sector(p, 0, R_IN + Q, 3 * k, 3 * k + 7);  /* one step over the next one: no seams */
            fill(p, dim(p->ring[k], 150));
        }
    } else {                                /* no filter: nearly black */
        disc(p, R_IN + Q);
        fill(p, 0x202020ffu);
    }
    for (k = 8; k > 0; k--) {               /* darker to the centre: translucent discs, the smaller over the larger */
        disc(p, R_IN / 8 * k);
        fill(p, 0x1a1a1824u);
    }
    for (k = 0; k < SECTORS; k++) {         /* the hue ring */
        sector(p, R_IN, R_RING, 3 * k, 3 * k + 4);
        fill(p, p->ring[k]);
    }
    for (k = 1; k < 9; k++)                 /* ticks at the filters */
        bar(p, R_IN - 5 * Q, R_IN - Q, 16 * k, Q / 2);
    fill(p, 0x00000060u);
    disc(p, lr[1]);                         /* the level steps, faint: 1 px */
    disc(p, lr[2]);
    stroke(p, 0xffffff28u, F_1);
    if (level == 1 || level == 2) {         /* the level chosen: 1.5 px */
        disc(p, r);
        stroke(p, level == 2 ? 0xffffffd0u : 0xb0b0b0d0u, F_1_5);
    }
    bar(p, 0, R_RING, 0, 2 * Q);            /* the fixed line to the top */
    fill(p, 0x9a9a9ac0u);
    bar(p, 0, R_RING, a, 4 * Q);            /* the needle on the filter */
    fill(p, 0x707070ffu);
    bar(p, 0, R_RING - Q, a, 3 * Q);
    fill(p, 0x181818ffu);
    for (k = 1; k <= 3; k++) {              /* the level steps on the needle */
        x = p->cx + ((lr[k] * sin_at(p, a)) >> 14);
        y = p->cy + ((lr[k] * cos_at(p, a)) >> 14);
        rect(p, x, y, 3 * Q / 2, 3 * Q / 2);
    }
    fill(p, 0xa0a0a0ffu);
    x = p->cx + ((r * sin_at(p, a)) >> 14);  /* the mark of the level: a cross */
    y = p->cy + ((r * cos_at(p, a)) >> 14);
    rect(p, x, y, 9 * Q, 3 * Q);
    rect(p, x, y, 3 * Q, 9 * Q);
    fill(p, 0x202020ffu);
    rect(p, x, y, 8 * Q, 3 * Q / 2);
    rect(p, x, y, 3 * Q / 2, 8 * Q);
    fill(p, 0xf0f0f0ffu);
}

/* The path stroked `width` wide (the bits of a float, pixels), its subpaths closed */
static void stroke(pen *p, u32 rgba, u32 width)
{
    end_sub(p);
    if (p->np >= 2) {
        drv_vg_clear_path(p->path, VG_PATH_CAPABILITY_ALL);
        drv_vg_append(p->path, p->ns, p->seg, p->xy);
        if (rgba != p->color)
            drv_vg_set_color(p->paint, rgba);
        p->color = rgba;
        drv_vg_setf(VG_STROKE_LINE_WIDTH, width);
        drv_vg_draw_path(p->path, VG_STROKE_PATH);
    }
    p->ns = p->np = 0;
}

static s32 at_x(const pen *p, s32 r, u32 a)
{
    return p->cx + ((r * sin_at(p, a)) >> 14);
}

static s32 at_y(const pen *p, s32 r, u32 a)
{
    return p->cy + ((r * cos_at(p, a)) >> 14);
}

/* A small 12-gon round (x, y) */
static void dot(pen *p, s32 x, s32 y, s32 r)
{
    u32 a;

    for (a = 0; a < STEPS; a += 12)
        put(p, x + ((r * sin_at(p, a)) >> 14), y + ((r * cos_at(p, a)) >> 14));
    end_sub(p);
}

/* The 12-gon with its corners at the hues, radius r */
static void hue_gon(pen *p, s32 r)
{
    u32 i;

    for (i = 0; i < HUES; i++)
        polar(p, r, 12 * i);
    end_sub(p);
}

static u32 mix(u32 a, u32 b)                 /* 3/4 a + 1/4 b, opaque */
{
    u32 c = 0, sh;

    for (sh = 8; sh < 32; sh += 8)
        c |= ((((a >> sh) & 0xff) * 3 + ((b >> sh) & 0xff)) >> 2) << sh;
    return c | 0xff;
}

/* Color/Vivid: the 12-gon frame of the hues, the scale (-5, 0, +5), the polygon of the 12
 * saturations, the dots of the hues on it; the hue under the cursor - a large dot and its spoke of steps; all hues - the
 * polygon outlined in yellow. About 60 fills and strokes */
static void draw_vivid(pen *p, const u8 *sat, u32 cur, u32 all)
{
    const volatile u32 *hue = (const volatile u32 *)((u32)hue_tab + UNCACHED);
    u32 i, n, b;
    s32 r, x0, y0, x1, y1;

    hue_gon(p, V_FRAME_OUT);                /* the frame's base and the dark inside */
    fill(p, 0x505050ffu);
    for (i = 0; i < HUES; i++) {            /* the frame: two halves an edge, each in the colour of its corner */
        n = i + 1 < HUES ? i + 1 : 0;
        x0 = (at_x(p, V_FRAME_OUT, 12 * i) + at_x(p, V_FRAME_OUT, 12 * n)) / 2;
        y0 = (at_y(p, V_FRAME_OUT, 12 * i) + at_y(p, V_FRAME_OUT, 12 * n)) / 2;
        x1 = (at_x(p, V_FRAME_IN, 12 * i) + at_x(p, V_FRAME_IN, 12 * n)) / 2;
        y1 = (at_y(p, V_FRAME_IN, 12 * i) + at_y(p, V_FRAME_IN, 12 * n)) / 2;
        polar(p, V_FRAME_OUT, 12 * i);
        put(p, x0, y0);
        put(p, x1, y1);
        polar(p, V_FRAME_IN, 12 * i);
        end_sub(p);
        fill(p, mix(hue[i], hue[n]));
        polar(p, V_FRAME_OUT, 12 * n);
        polar(p, V_FRAME_IN, 12 * n);
        put(p, x1, y1);
        put(p, x0, y0);
        end_sub(p);
        fill(p, mix(hue[n], hue[i]));
    }
    hue_gon(p, V_FRAME_IN);
    fill(p, 0x202020ffu);
    for (i = 0; i < HUES; i++) {            /* the inside: the hues, dimmed, a wedge each */
        n = i + 1 < HUES ? i + 1 : 0;
        put(p, p->cx, p->cy);
        put(p, (at_x(p, V_FRAME_IN, 12 * i + 12 * HUES - 12) + at_x(p, V_FRAME_IN, 12 * i)) / 2,
            (at_y(p, V_FRAME_IN, 12 * i + 12 * HUES - 12) + at_y(p, V_FRAME_IN, 12 * i)) / 2);
        polar(p, V_FRAME_IN, 12 * i);
        put(p, (at_x(p, V_FRAME_IN, 12 * i) + at_x(p, V_FRAME_IN, 12 * n)) / 2,
            (at_y(p, V_FRAME_IN, 12 * i) + at_y(p, V_FRAME_IN, 12 * n)) / 2);
        end_sub(p);
        fill(p, (dim(hue[i], 120) & 0xffffff00u) | 0xd0);
    }
    for (i = 6; i > 0; i--) {               /* darker to the centre */
        hue_gon(p, V_FRAME_IN / 6 * i);
        fill(p, 0x1a1a1828u);
    }
    hue_gon(p, V_R0);                       /* the centre: saturation -5 and below */
    fill(p, 0x6a6a6aff);
    hue_gon(p, V_R0);                       /* the scale: -5, 0, +5 */
    hue_gon(p, V_R0 + 5 * V_STEP);
    hue_gon(p, V_R0 + 10 * V_STEP);
    stroke(p, 0xffffff40u, F_1);
    for (i = 0; i < HUES; i++)              /* the saturations */
        polar(p, V_R0 + V_STEP * sat[i], 12 * i);
    end_sub(p);
    fill(p, 0xd8d8d8a0u);
    for (i = 0; i < HUES; i++)
        polar(p, V_R0 + V_STEP * sat[i], 12 * i);
    stroke(p, all ? 0xf0e020ffu : 0x101010ffu, all ? F_2_5 : F_1_5);
    if (!all && cur < HUES) {               /* the spoke of the hue under the cursor: 11 steps */
        for (b = 0; b <= 10; b++)
            dot(p, at_x(p, V_R0 + V_STEP * b, 12 * cur), at_y(p, V_R0 + V_STEP * b, 12 * cur), 3 * Q / 4);
        fill(p, 0xe0e0e0ffu);
    }
    for (i = 0; i < HUES; i++) {            /* the dots of the hues */
        r = V_R0 + V_STEP * sat[i];
        dot(p, at_x(p, r, 12 * i), at_y(p, r, 12 * i), 4 * Q);
    }
    fill(p, 0x101010ffu);
    for (i = 0; i < HUES; i++) {
        r = V_R0 + V_STEP * sat[i];
        dot(p, at_x(p, r, 12 * i), at_y(p, r, 12 * i), 3 * Q);
        fill(p, hue[i]);
    }
    if (!all && cur < HUES) {               /* the hue under the cursor: a large dot in a yellow ring */
        r = V_R0 + V_STEP * sat[cur];
        x0 = at_x(p, r, 12 * cur);
        y0 = at_y(p, r, 12 * cur);
        dot(p, x0, y0, 7 * Q);
        fill(p, 0xf0e020ffu);
        dot(p, x0, y0, 6 * Q);
        fill(p, 0x101010ffu);
        dot(p, x0, y0, 5 * Q);
        fill(p, hue[cur]);
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * The tone curve: as the camera's own H&S screen - a frame with a 2x2 grid, the straight line,
 * the curve through Sh at 1/4, Mid at 1/2, Hi at 3/4, crosses on the points the dials edit, the values next to the points */
#define C_HALF (100 * Q)                    /* the graph: 200 x 200 round the centre */
#define C_BACK (C_HALF + 10 * Q)            /* its window, the dark backing: 220 x 220, rows 18..237 of the 256 x 256 surface */
#define C_STEP (5 * Q)                      /* a tone step: 5 px, +-7 -> 35 */

static void cubic(pen *p, s32 x1, s32 y1, s32 x2, s32 y2, s32 x, s32 y)
{
    if (p->np + 3 > MAX_PTS || p->ns >= MAX_SEG - 1) {
        p->dropped += 3;
        return;
    }
    p->seg[p->ns++] = VG_CUBIC_TO_ABS;
    p->xy[2 * p->np] = (s16)x1;
    p->xy[2 * p->np + 1] = (s16)y1;
    p->xy[2 * p->np + 2] = (s16)x2;
    p->xy[2 * p->np + 3] = (s16)y2;
    p->xy[2 * p->np + 4] = (s16)x;
    p->xy[2 * p->np + 5] = (s16)y;
    p->np += 3;
}

/* An open path stroked (no close: the curve) */
static void stroke_open(pen *p, u32 rgba, u32 width)
{
    p->start = 1;
    if (p->np >= 2) {
        drv_vg_clear_path(p->path, VG_PATH_CAPABILITY_ALL);
        drv_vg_append(p->path, p->ns, p->seg, p->xy);
        if (rgba != p->color)
            drv_vg_set_color(p->paint, rgba);
        p->color = rgba;
        drv_vg_setf(VG_STROKE_LINE_WIDTH, width);
        drv_vg_draw_path(p->path, VG_STROKE_PATH);
    }
    p->ns = p->np = 0;
}

static void line(pen *p, s32 x0, s32 y0, s32 x1, s32 y1)
{
    put(p, x0, y0);
    put(p, x1, y1);
    end_sub(p);
}

/* Seven segments in an 8 x 14 px cell, a..g = bits 0..6; {x0, y0, x1, y1} in px from the cell's bottom left, y up */
static const u8 seg7[7][4] = {{1, 14, 7, 14}, {7, 14, 7, 7}, {7, 7, 7, 0}, {1, 0, 7, 0}, {1, 0, 1, 7}, {1, 7, 1, 14}, {1, 7, 7, 7}};
static const u8 digit7[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};

/* "+n", "-n" or "+-0" with its lower left at (x, y), quarter pixels; 2 x 10 px wide */
static void number(pen *p, s32 x, s32 y, s32 v)
{
    const volatile u8 *sg = (const volatile u8 *)((u32)seg7 + UNCACHED);
    const volatile u8 *dg = (const volatile u8 *)((u32)digit7 + UNCACHED);
    u32 m, i;

    if (v >= 0) {                           /* +: the bar up; +-: the plus raised, a minus under it */
        s32 dy = v ? 7 * Q : 9 * Q;

        line(p, x + Q, y + dy, x + 7 * Q, y + dy);
        line(p, x + 4 * Q, y + dy - 3 * Q, x + 4 * Q, y + dy + 3 * Q);
    }
    if (v <= 0)
        line(p, x + Q, y + (v ? 7 : 3) * Q, x + 7 * Q, y + (v ? 7 : 3) * Q);
    m = dg[v < 0 ? -v : v];
    x += 10 * Q;
    for (i = 0; i < 7; i++)
        if (m & 1u << i)
            line(p, x + sg[4 * i] * Q, y + sg[4 * i + 1] * Q, x + sg[4 * i + 2] * Q, y + sg[4 * i + 3] * Q);
}

static void cross(pen *p, s32 x, s32 y, s32 r)
{
    line(p, x - r, y, x + r, y);
    line(p, x, y - r, x, y + r);
}

static void draw_curve(pen *p, s32 hi, s32 md, s32 sh, u32 mid)
{
    s32 x0 = p->cx - C_HALF, y0 = p->cy - C_HALF, x1 = p->cx + C_HALF, y1 = p->cy + C_HALF;
    s32 px[7], py[7], d[5], i, c1x, c1y, c2x, c2y, ny;

    rect(p, p->cx, p->cy, C_BACK, C_BACK);  /* the backing and the frame */
    fill(p, 0x181818e0u);
    put(p, x0, y0);
    put(p, x1, y0);
    put(p, x1, y1);
    put(p, x0, y1);
    end_sub(p);
    stroke(p, 0x9a9a9aff, F_1_5);
    line(p, p->cx, y0, p->cx, y1);          /* the grid, 2 x 2 */
    line(p, x0, p->cy, x1, p->cy);
    stroke(p, 0x6a6a6aff, F_1);
    line(p, x0, y0, x1, y1);                /* the straight line */
    stroke(p, 0xffffff40u, F_1);
    d[0] = 0;
    d[1] = sh;
    d[2] = md;
    d[3] = hi;
    d[4] = 0;
    for (i = 0; i < 5; i++) {               /* the points; the ends repeated for the spline */
        s32 y = y0 + i * (C_HALF / 2) + d[i] * C_STEP;

        px[i + 1] = x0 + i * (C_HALF / 2);
        py[i + 1] = y < y0 ? y0 : y > y1 ? y1 : y;
    }
    px[0] = px[1];
    py[0] = py[1];
    px[6] = px[5];
    py[6] = py[5];
    put(p, px[1], py[1]);                   /* Catmull-Rom through the points as cubic Beziers */
    for (i = 1; i < 5; i++) {
        c1x = px[i] + (px[i + 1] - px[i - 1]) / 6;
        c1y = py[i] + (py[i + 1] - py[i - 1]) / 6;
        c2x = px[i + 1] - (px[i + 2] - px[i]) / 6;
        c2y = py[i + 1] - (py[i + 2] - py[i]) / 6;
        cubic(p, c1x, c1y, c2x, c2y, px[i + 1], py[i + 1]);
    }
    stroke_open(p, 0xf0f0f0ffu, F_2);
    for (i = 2; i <= 4; i++) {              /* the crosses of the points the dials edit: Sh and Hi, or Mid */
        if ((i == 3) != !!mid)
            continue;
        cross(p, px[i], py[i], 7 * Q);
    }
    stroke(p, 0x101010ffu, F_3);           /* the outline */
    for (i = 2; i <= 4; i++) {
        if ((i == 3) != !!mid)
            continue;
        cross(p, px[i], py[i], 6 * Q);
    }
    stroke(p, 0xf0f0f0ffu, F_1_5);
    for (i = 2; i <= 4; i++) {              /* the values next to the points: above them, below near the top */
        ny = py[i] + 8 * Q;
        if (ny > y1 - 18 * Q)
            ny = py[i] - 22 * Q;
        rect(p, px[i] + 4 * Q + 9 * Q, ny + 7 * Q, 11 * Q, 9 * Q);
        fill(p, 0x000000a0u);
        number(p, px[i] + 4 * Q, ny, d[i - 1]);
        stroke(p, (i == 3) == !!mid ? 0xf0f0f0ffu : 0x9a9a9aff, F_1_5);
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * The film look's editors. Color/Vivid on the preset's sectors: a vertex on the hue of its sector's centre, in its colour;
 * the angles are not 30 degrees apart, so the frame is the hue ring (the Color/Level ring: yellow at the top, the hues
 * falling clockwise, as the vertices are placed); a sector the preset does not have is drawn at 0, dimmed, and the cursor
 * passes it by. The rest as Color/Vivid above: the scale (-5, 0, +5), the polygon of the saturations, the dots, the sector
 * under the cursor - a large dot in a yellow ring and its spoke of steps. About 70 fills and strokes */
static void draw_look_vivid(pen *p, const u8 *sat, u32 cur, u32 all, u32 mask, const u8 *ang, const volatile u32 *rgba)
{
    u8 ord[HUES], s[HUES];
    u32 i, j, k, b;
    s32 r, x0, y0;

    for (i = 0; i < HUES; i++) {
        ord[i] = (u8)i;
        s[i] = mask >> i & 1 ? sat[i] : 5;
    }
    for (i = 1; i < HUES; i++)              /* by angle: the polygon goes round once */
        for (j = i; j > 0 && ang[ord[j - 1]] > ang[ord[j]]; j--) {
            k = ord[j];
            ord[j] = ord[j - 1];
            ord[j - 1] = (u8)k;
        }
    disc(p, R_OUT);
    fill(p, 0x262626f0u);
    for (k = 0; k < SECTORS; k++) {         /* the frame: the hue ring */
        sector(p, V_FRAME_IN, V_FRAME_OUT, 3 * k, 3 * k + 4);
        fill(p, p->ring[k]);
    }
    disc(p, V_FRAME_IN);
    fill(p, 0x202020ffu);
    for (i = 6; i > 0; i--) {               /* darker to the centre */
        disc(p, V_FRAME_IN / 6 * i);
        fill(p, 0x1a1a1828u);
    }
    disc(p, V_R0);                          /* the centre: saturation -5 */
    fill(p, 0x6a6a6aff);
    disc(p, V_R0 + 5 * V_STEP);             /* the scale: 0 and +5, 1 px */
    disc(p, V_R0 + 10 * V_STEP);
    stroke(p, 0xffffff40u, F_1);
    for (i = 0; i < HUES; i++)              /* the sectors' spokes, faint */
        if (mask >> i & 1)
            bar(p, V_R0, V_FRAME_IN, ang[i], Q / 2);
    fill(p, 0xffffff20u);
    for (i = 0; i < HUES; i++)              /* the saturations */
        polar(p, V_R0 + V_STEP * s[ord[i]], ang[ord[i]]);
    end_sub(p);
    fill(p, 0xd8d8d8a0u);
    for (i = 0; i < HUES; i++)
        polar(p, V_R0 + V_STEP * s[ord[i]], ang[ord[i]]);
    stroke(p, all ? 0xf0e020ffu : 0x101010ffu, all ? F_2_5 : F_1_5);   /* all hues: outlined in yellow */
    if (all) {                              /* and every vertex in a yellow ring: the rear dial moves them all */
        cur = HUES;
        for (i = 0; i < HUES; i++)
            if (mask >> i & 1) {
                r = V_R0 + V_STEP * s[i];
                dot(p, at_x(p, r, ang[i]), at_y(p, r, ang[i]), 6 * Q);
            }
        fill(p, 0xf0e020ffu);
    }
    if (cur < HUES && mask >> cur & 1) {    /* the spoke of the sector under the cursor: 11 steps */
        for (b = 0; b <= 10; b++)
            dot(p, at_x(p, V_R0 + V_STEP * b, ang[cur]), at_y(p, V_R0 + V_STEP * b, ang[cur]), 3 * Q / 4);
        fill(p, 0xe0e0e0ffu);
    }
    for (i = 0; i < HUES; i++) {            /* the dots */
        r = V_R0 + V_STEP * s[i];
        dot(p, at_x(p, r, ang[i]), at_y(p, r, ang[i]), 4 * Q);
    }
    fill(p, 0x101010ffu);
    for (i = 0; i < HUES; i++) {
        r = V_R0 + V_STEP * s[i];
        dot(p, at_x(p, r, ang[i]), at_y(p, r, ang[i]), 3 * Q);
        fill(p, mask >> i & 1 ? rgba[i] | 0xff : dim(rgba[i], 110) | 0xff);
    }
    if (cur < HUES && mask >> cur & 1) {    /* the sector under the cursor: a large dot in a yellow ring */
        r = V_R0 + V_STEP * s[cur];
        x0 = at_x(p, r, ang[cur]);
        y0 = at_y(p, r, ang[cur]);
        dot(p, x0, y0, 7 * Q);
        fill(p, 0xf0e020ffu);
        dot(p, x0, y0, 6 * Q);
        fill(p, 0x101010ffu);
        dot(p, x0, y0, 5 * Q);
        fill(p, rgba[cur] | 0xff);
    }
}

/* The film look's tone curve: the preset's curve as the module sends it (gamma and tone together, with the edits; the
 * output 0..255 at the inputs k / 63 of the perceptual axis) and its three points at 1/4, 1/2, 3/4 - Sh, Mid, Hi - with
 * the values the dials set; crosses on the points the dials edit (Sh and Hi, or Mid) */
static void draw_look_curve(pen *p, s32 hi, s32 md, s32 sh, u32 mid, const volatile u8 *c)
{
    s32 x0 = p->cx - C_HALF, y0 = p->cy - C_HALF, x1 = p->cx + C_HALF, y1 = p->cy + C_HALF;
    s32 px[3], py[3], d[3], i, k, q, ny;

    d[0] = sh;
    d[1] = md;
    d[2] = hi;
    rect(p, p->cx, p->cy, C_BACK, C_BACK);  /* the backing and the frame */
    fill(p, 0x181818e0u);
    put(p, x0, y0);
    put(p, x1, y0);
    put(p, x1, y1);
    put(p, x0, y1);
    end_sub(p);
    stroke(p, 0x9a9a9aff, F_1_5);
    line(p, p->cx, y0, p->cx, y1);          /* the grid, 2 x 2 */
    line(p, x0, p->cy, x1, p->cy);
    stroke(p, 0x6a6a6aff, F_1);
    line(p, x0, y0, x1, y1);                /* the straight line */
    stroke(p, 0xffffff40u, F_1);
    for (k = 0; k < 64; k++)
        put(p, x0 + 2 * C_HALF * k / 63, y0 + 2 * C_HALF * (s32)c[k] / 255);
    stroke_open(p, 0xf0f0f0ffu, F_2);
    for (i = 0; i < 3; i++) {               /* the points: input (i + 1) / 4, k = 63 (i + 1) / 4 - in quarters */
        q = 63 * (i + 1);
        k = q / 4;
        px[i] = x0 + C_HALF * (i + 1) / 2;
        py[i] = y0 + 2 * C_HALF * ((s32)c[k] * (4 - q % 4) + (s32)c[k < 63 ? k + 1 : 63] * (q % 4)) / (4 * 255);
    }
    for (i = 0; i < 3; i++)
        if ((i == 1) == !!mid)
            cross(p, px[i], py[i], 7 * Q);
    stroke(p, 0x101010ffu, F_3);            /* the outline */
    for (i = 0; i < 3; i++)
        if ((i == 1) == !!mid)
            cross(p, px[i], py[i], 6 * Q);
    stroke(p, 0xf0f0f0ffu, F_1_5);
    for (i = 0; i < 3; i++) {               /* the values next to the points: above them, below near the top */
        ny = py[i] + 8 * Q;
        if (ny > y1 - 18 * Q)
            ny = py[i] - 22 * Q;
        rect(p, px[i] + 4 * Q + 9 * Q, ny + 7 * Q, 11 * Q, 9 * Q);
        fill(p, 0x000000a0u);
        number(p, px[i] + 4 * Q, ny, d[i]);
        stroke(p, (i == 1) == !!mid ? 0xf0f0f0ffu : 0x9a9a9aff, F_1_5);
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * The film look: a neutral rectangle with the chosen entry's name in the middle and "n / N"
 * under it. Text in a 5 x 8 pixel font (printable ASCII; five columns a glyph, bit 0 the top row, bit 7 the descender);
 * each lit run of a column is one rectangle. The font through the uncached alias, like the other tables.
 * font5: Glyphs 0x20..0x7E of the classic 5x7 font of the Adafruit GFX Library (glcdfont.c), BSD 2-Clause, Copyright (c) 2012 Adafruit Industries; see NOTICE (OMKit/NOTICE) */
static const u8 font5[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5f, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7f, 0x14, 0x7f, 0x14},
    {0x24, 0x2a, 0x7f, 0x2a, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x08, 0x07, 0x03, 0x00},
    {0x00, 0x1c, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1c, 0x00}, {0x2a, 0x1c, 0x7f, 0x1c, 0x2a}, {0x08, 0x08, 0x3e, 0x08, 0x08},
    {0x00, 0x80, 0x70, 0x30, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x60, 0x60, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00}, {0x72, 0x49, 0x49, 0x49, 0x46}, {0x21, 0x41, 0x49, 0x4d, 0x33},
    {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3c, 0x4a, 0x49, 0x49, 0x31}, {0x41, 0x21, 0x11, 0x09, 0x07},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x46, 0x49, 0x49, 0x29, 0x1e}, {0x00, 0x00, 0x14, 0x00, 0x00}, {0x00, 0x40, 0x34, 0x00, 0x00},
    {0x00, 0x08, 0x14, 0x22, 0x41}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x59, 0x09, 0x06},
    {0x3e, 0x41, 0x5d, 0x59, 0x4e}, {0x7c, 0x12, 0x11, 0x12, 0x7c}, {0x7f, 0x49, 0x49, 0x49, 0x36}, {0x3e, 0x41, 0x41, 0x41, 0x22},
    {0x7f, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 0x09, 0x09, 0x09, 0x01}, {0x3e, 0x41, 0x41, 0x51, 0x73},
    {0x7f, 0x08, 0x08, 0x08, 0x7f}, {0x00, 0x41, 0x7f, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41},
    {0x7f, 0x40, 0x40, 0x40, 0x40}, {0x7f, 0x02, 0x1c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f}, {0x3e, 0x41, 0x41, 0x41, 0x3e},
    {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 0x09, 0x19, 0x29, 0x46}, {0x26, 0x49, 0x49, 0x49, 0x32},
    {0x03, 0x01, 0x7f, 0x01, 0x03}, {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x59, 0x49, 0x4d, 0x43}, {0x00, 0x7f, 0x41, 0x41, 0x41},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x41, 0x7f}, {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x03, 0x07, 0x08, 0x00}, {0x20, 0x54, 0x54, 0x78, 0x40}, {0x7f, 0x28, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x28},
    {0x38, 0x44, 0x44, 0x28, 0x7f}, {0x38, 0x54, 0x54, 0x54, 0x18}, {0x00, 0x08, 0x7e, 0x09, 0x02}, {0x18, 0xa4, 0xa4, 0x9c, 0x78},
    {0x7f, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7d, 0x40, 0x00}, {0x20, 0x40, 0x40, 0x3d, 0x00}, {0x7f, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7f, 0x40, 0x00}, {0x7c, 0x04, 0x78, 0x04, 0x78}, {0x7c, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0xfc, 0x18, 0x24, 0x24, 0x18}, {0x18, 0x24, 0x24, 0x18, 0xfc}, {0x7c, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x24},
    {0x04, 0x04, 0x3f, 0x44, 0x24}, {0x3c, 0x40, 0x40, 0x20, 0x7c}, {0x1c, 0x20, 0x40, 0x20, 0x1c}, {0x3c, 0x40, 0x30, 0x40, 0x3c},
    {0x44, 0x28, 0x10, 0x28, 0x44}, {0x4c, 0x90, 0x90, 0x90, 0x7c}, {0x44, 0x64, 0x54, 0x4c, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x77, 0x00, 0x00}, {0x00, 0x41, 0x36, 0x08, 0x00}, {0x02, 0x01, 0x02, 0x04, 0x02},
};
static const u8 no_presets[] = "no presets";
#define LOOK_HALF_W (112 * Q)               /* the rectangle: 224 x 92 px round the centre */
#define LOOK_HALF_H (46 * Q)
#define LOOK_TEXT_W 208                     /* px for the name at two pixels a font pixel */

static u32 text_len(const volatile u8 *t, u32 max)
{
    u32 n = 0;

    while (n < max && t[n])
        n++;
    return n;
}

/* Text centred on cx with its top at y (quarter pixels, y up), s pixels a font pixel, in one colour: a fill whenever the
 * path is about full */
static void text(pen *p, const volatile u8 *t, u32 n, s32 cx, s32 y, s32 s, u32 rgba)
{
    const volatile u8 *f = (const volatile u8 *)((u32)font5 + UNCACHED);
    s32 x = cx - ((s32)(6 * n) - 1) * s * Q / 2, x0, top, bottom;
    u32 i, c, col, bits, row, r0;

    for (i = 0; i < n; i++, x += 6 * s * Q) {
        c = t[i];
        if (c < 32 || c > 126)
            c = '?';
        for (col = 0; col < 5; col++) {
            bits = f[(c - 32) * 5 + col];
            x0 = x + (s32)col * s * Q;
            for (row = 0; row < 8;) {
                if (!(bits >> row & 1)) {
                    row++;
                    continue;
                }
                for (r0 = row; row < 8 && bits >> row & 1; row++)
                    ;
                if (p->np + 4 > MAX_PTS || p->ns + 5 > MAX_SEG)
                    fill(p, rgba);
                top = y - (s32)r0 * s * Q;
                bottom = y - (s32)row * s * Q;
                put(p, x0, top);
                put(p, x0 + s * Q, top);
                put(p, x0 + s * Q, bottom);
                put(p, x0, bottom);
                end_sub(p);
            }
        }
    }
    fill(p, rgba);
}

static u32 put_number(u8 *out, u32 v)
{
    u32 d, k = 0;

    for (d = 1; v / d >= 10; d *= 10)
        ;
    for (; d; d /= 10)
        out[k++] = '0' + v / d % 10;
    return k;
}

/* A cassette drawing (CANI v1; checked by core before it was published): records of OpenVG paths in quarter
 * pixels of a 256 x 256 surface, y up, drawn in order - fill non-zero (1), fill even-odd (2) or stroke (3) */
#define VG_EVEN_ODD 0x1900
#define CANI_HEAD 64
#define ART_SURFACE (256 * Q)
/* Every editor's area starts at one distance under the dial labels: the row where the curve's window begins (C_BACK
 * above the centre, row 18 of 256; the labels and the surface keep their places on every item). The cassettes span rows
 * 84..179 of their 256 x 256 surface (the cassette generator; their CANI data is not changed) and the grain item is drawn
 * in the same place: both are lifted by the difference, 66 px (80 px, to the top, before) */
#define ART_TOP 84
#define ART_LIFT (ART_TOP * Q - (ART_SURFACE / 2 - C_BACK))

static void draw_art(pen *p, u32 art)
{
    const volatile u8 *b = (const volatile u8 *)art;
    u32 count = b[8] | b[9] << 8, r, o = CANI_HEAD, op, caps, rgba, width, nseg, npts, i;
    s32 dx = p->cx - ART_SURFACE / 2, dy = p->cy - ART_SURFACE / 2 + ART_LIFT;
    const volatile u8 *seg;
    const volatile u8 *xy;

    for (r = 0; r < count; r++, o += 16 + ((nseg + 3) & ~3u) + 4 * npts) {
        op = b[o];
        caps = b[o + 1];
        rgba = (u32)b[o + 4] | (u32)b[o + 5] << 8 | (u32)b[o + 6] << 16 | (u32)b[o + 7] << 24;
        width = (u32)b[o + 8] | (u32)b[o + 9] << 8 | (u32)b[o + 10] << 16 | (u32)b[o + 11] << 24;
        nseg = b[o + 12] | b[o + 13] << 8;
        npts = b[o + 14] | b[o + 15] << 8;
        if (nseg > MAX_SEG || npts > MAX_PTS)
            return;
        seg = b + o + 16;
        xy = seg + ((nseg + 3) & ~3u);
        for (i = 0; i < nseg; i++)
            p->seg[i] = seg[i];
        for (i = 0; i < npts; i++) {
            p->xy[2 * i] = (s16)((s32)(s16)(xy[4 * i] | xy[4 * i + 1] << 8) + dx);
            p->xy[2 * i + 1] = (s16)((s32)(s16)(xy[4 * i + 2] | xy[4 * i + 3] << 8) + dy);
        }
        p->ns = nseg;
        p->np = npts;
        p->start = 1;                       /* the record closes its own subpaths: end_sub adds nothing */
        if (op == 3) {
            drv_vg_seti(VG_STROKE_CAP_STYLE, VG_CAP_BUTT + (caps < 3 ? caps : 0));
            stroke(p, rgba, width);
            drv_vg_seti(VG_STROKE_CAP_STYLE, VG_CAP_BUTT);
        } else if (op == 2) {
            drv_vg_seti(VG_FILL_RULE, VG_EVEN_ODD);
            fill(p, rgba);
            drv_vg_seti(VG_FILL_RULE, VG_NON_ZERO);
        } else {
            fill(p, rgba);
        }
    }
}

static void draw_look(pen *p, u32 names, u32 index, u32 count, u32 arts)
{
    const volatile u8 *t;
    u8 num[16];
    u32 n, k, art = 0;
    s32 s, cy = p->cy + C_BACK - LOOK_HALF_H;  /* the rectangle's centre: its top where the curve's window begins */

    if (arts && names && index < count)
        art = ((const volatile u32 *)arts)[index];
    if (art) {                              /* the preset's cassette, "n / N" under it */
        draw_art(p, art);
        k = put_number(num, index + 1);
        num[k++] = ' ';
        num[k++] = '/';
        num[k++] = ' ';
        k += put_number(num + k, count);
        text(p, num, k, p->cx, p->cy - 58 * Q + ART_LIFT, 1, 0xb4b4b4ffu);
        return;
    }
    rect(p, p->cx, cy, LOOK_HALF_W, LOOK_HALF_H);
    fill(p, 0x5a5a5ae8u);
    put(p, p->cx - LOOK_HALF_W, cy - LOOK_HALF_H);
    put(p, p->cx + LOOK_HALF_W, cy - LOOK_HALF_H);
    put(p, p->cx + LOOK_HALF_W, cy + LOOK_HALF_H);
    put(p, p->cx - LOOK_HALF_W, cy + LOOK_HALF_H);
    end_sub(p);
    stroke(p, 0x9a9a9aff, F_1_5);
    if (!names || !count || index >= count) {
        text(p, (const volatile u8 *)((u32)no_presets + UNCACHED), sizeof(no_presets) - 1, p->cx, cy + 8 * Q, 2,
             0xf0f0f0ffu);
        return;
    }
    t = (const volatile u8 *)(names + LOOK_NAME * index);
    n = text_len(t, LOOK_NAME - 1);
    s = 12 * (s32)n - 2 <= LOOK_TEXT_W ? 2 : 1;   /* two pixels a font pixel while the name fits, one for a long name */
    text(p, t, n, p->cx, cy + (s == 2 ? 14 : 10) * Q, s, 0xf8f8f8ffu);
    k = put_number(num, index + 1);
    num[k++] = ' ';
    num[k++] = '/';
    num[k++] = ' ';
    k += put_number(num + k, count);
    text(p, num, k, p->cx, cy - 22 * Q, 1, 0xb4b4b4ffu);
}

/* ------------------------------------------------------------------------------------------------------------------
 * The film look's grain item: a cassette in the style of the presets' (the same place, lifted as they are), split 2/3 :
 * 1/3 - the tile's name on the label in font5, and a strip of four square pictograms Off, Low, Med, High with
 * the chosen strength outlined; "n / N" of the tile under it. Pixels of the 256 x 256 surface, y down (as the cassette
 * generator draws them, the grain panel of the cassette generator); whole pixels only, so nothing is blurred. About
 * 12 fills */
#define GR_X0 20                            /* the cassette's outline: x 20..235, y 84..179 */
#define GR_Y0 84
#define GR_X1 236
#define GR_Y1 180
#define GR_SPLIT 148                        /* the line between the label (rows 85..147) and the strip (149..179) */
#define GR_SQ 22                            /* a pictogram: 22 x 22 at rows 153..174 */
#define GR_SQ_Y 153
#define GR_DOTS 24
static const u8 gr_sq_x[4] = {36, 90, 143, 197};
static const u8 gr_dots[GR_DOTS][2] = {   /* 2 x 2 dots in the pictogram: Low the first 6, Med 13, High all */
    {8, 10}, {19, 2}, {17, 18}, {1, 2}, {5, 18}, {17, 9}, {12, 2}, {12, 14}, {6, 4}, {3, 9}, {4, 13}, {11, 6},
    {17, 14}, {2, 16}, {7, 1}, {14, 17}, {14, 5}, {18, 5}, {15, 1}, {8, 18}, {12, 11}, {8, 13}, {8, 7}, {1, 5}};
static const u8 gr_ndots[4] = {0, 6, 13, 24};
static const u8 none_name[] = "NONE";

static void box(pen *p, s32 x0, s32 y0, s32 x1, s32 y1)
{
    s32 dx = p->cx - ART_SURFACE / 2, dy = p->cy - ART_SURFACE / 2 + ART_LIFT;

    put(p, dx + x0 * Q, dy + (256 - y0) * Q);
    put(p, dx + x1 * Q, dy + (256 - y0) * Q);
    put(p, dx + x1 * Q, dy + (256 - y1) * Q);
    put(p, dx + x0 * Q, dy + (256 - y1) * Q);
    end_sub(p);
}

/* Text from (x, y) - its left column and top row in pixels of the surface - s pixels a font pixel */
static void text_at(pen *p, const volatile u8 *t, u32 n, s32 x, s32 y, s32 s, u32 rgba)
{
    s32 dx = p->cx - ART_SURFACE / 2, dy = p->cy - ART_SURFACE / 2 + ART_LIFT;

    text(p, t, n, dx + x * Q + ((s32)(6 * n) - 1) * s * Q / 2, dy + (256 - y) * Q, s, rgba);
}

static void draw_look_grain(pen *p, u32 names, u32 tile, u32 count, u32 level)
{
    const volatile u8 *sx = (const volatile u8 *)((u32)gr_sq_x + UNCACHED);
    const volatile u8 *dots = (const volatile u8 *)((u32)gr_dots + UNCACHED);
    const volatile u8 *nd = (const volatile u8 *)((u32)gr_ndots + UNCACHED);
    const volatile u8 *t;
    u8 name[LOOK_NAME], num[16];
    u32 n, k, i, c;
    s32 s, x, y;

    box(p, GR_X0, GR_Y0, GR_X1, GR_Y1);     /* the outline and the line between label and strip */
    fill(p, 0x101418ffu);
    box(p, GR_X0 + 1, GR_Y0 + 1, GR_X1 - 1, GR_SPLIT);
    fill(p, 0x3a3f46ffu);                   /* the label, its light rows on top */
    box(p, GR_X0 + 1, GR_Y0 + 1, GR_X1 - 1, GR_Y0 + 3);
    fill(p, 0x808287ffu);
    box(p, GR_X0 + 1, GR_SPLIT + 1, GR_X1 - 1, GR_Y1 - 1);
    fill(p, 0x24272bffu);                   /* the strip */
    for (k = 0; k < 4; k++)                 /* the frames: the chosen one yellow, 2 px, with a dark pixel inside */
        if (k == level)
            box(p, sx[k] - 3, GR_SQ_Y - 3, sx[k] + GR_SQ + 3, GR_SQ_Y + GR_SQ + 3);
    fill(p, 0xf0e020ffu);
    for (k = 0; k < 4; k++)
        box(p, sx[k] - 1, GR_SQ_Y - 1, sx[k] + GR_SQ + 1, GR_SQ_Y + GR_SQ + 1);
    fill(p, 0x6a6e74ffu);
    if (level < 4) {
        box(p, sx[level] - 1, GR_SQ_Y - 1, sx[level] + GR_SQ + 1, GR_SQ_Y + GR_SQ + 1);
        fill(p, 0x101418ffu);
    }
    for (k = 0; k < 4; k++)                 /* the paper */
        box(p, sx[k], GR_SQ_Y, sx[k] + GR_SQ, GR_SQ_Y + GR_SQ);
    fill(p, 0xd8d6d0ffu);
    for (i = 0; i < 7; i++)                 /* Off: a stepped slash */
        box(p, sx[0] + 4 + 2 * (s32)i, GR_SQ_Y + 16 - 2 * (s32)i, sx[0] + 6 + 2 * (s32)i, GR_SQ_Y + 18 - 2 * (s32)i);
    for (k = 1; k < 4; k++)                 /* Low, Med, High: more grains */
        for (i = 0; i < nd[k]; i++)
            box(p, sx[k] + dots[2 * i], GR_SQ_Y + dots[2 * i + 1], sx[k] + dots[2 * i] + 2, GR_SQ_Y + dots[2 * i + 1] + 2);
    fill(p, 0x2a2a2affu);
    if (names && tile < count) {            /* the name, in capitals as on the cassettes */
        t = (const volatile u8 *)(names + LOOK_NAME * tile);
        for (n = 0; n < LOOK_NAME - 1 && t[n]; n++) {
            c = t[n];
            name[n] = (u8)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        }
    } else {
        for (n = 0; n < sizeof(none_name) - 1; n++)
            name[n] = ((const volatile u8 *)((u32)none_name + UNCACHED))[n];
    }
    s = 12 * (s32)n - 2 <= GR_X1 - GR_X0 - 26 ? 2 : 1;   /* two pixels a font pixel while it fits, else one */
    x = 128 - ((s32)(6 * n) - 1) * s / 2;
    y = GR_Y0 + 1 + (GR_SPLIT - GR_Y0 - 1 - 7 * s) / 2;
    text_at(p, name, n, x, y, s, 0xf0f0f0ffu);
    if (count && tile < count) {            /* "n / N" under the cassette, as under the presets' */
        k = put_number(num, tile + 1);
        num[k++] = ' ';
        num[k++] = '/';
        num[k++] = ' ';
        k += put_number(num + k, count);
        text_at(p, num, k, 128 - ((s32)(6 * k) - 1) / 2, 186, 1, 0xb4b4b4ffu);
    }
}

/* From _drv_tramp_c3d. 1 - the frame is drawn and shown (the trampoline returns as the native renderer does), 0 - the native
 * renderer draws it */
int drv_edp_draw(void)
{
    volatile u32 *e = edp();
    u32 s, mode = EDP_OFF, value = 0, cur = 0, names = 0, index = 0, arts = 0, w, h, paint, k, i, gnames = 0, gvalue = 0;
    u32 zero[4], satw[3], lk[EDL_WORDS];
    volatile u32 *l = edl();
    const volatile s16 *sq = (const volatile s16 *)((u32)edp_sin_q + UNCACHED);
    u8 sat[HUES], ang[HUES];
    pen p;

    mode = e[EDP_MODE];
    if (mode < EDP_LEVEL || mode > EDP_LAST)  /* not ours: the native ring */
        return 0;
    for (k = 0; k < 3; k++) {
        s = e[EDP_SEQ];
        if (s & 1)
            continue;
        mode = e[EDP_MODE];
        value = e[EDP_VALUE];
        for (i = 0; i < 3; i++)
            satw[i] = e[EDP_SAT + i];
        cur = e[EDP_CUR];
        names = e[EDP_NAMES];
        index = e[EDP_INDEX];
        arts = e[EDP_ARTS];
        gnames = e[EDP_GNAMES];
        gvalue = e[EDP_GVALUE];
        for (i = 0; i < EDL_WORDS; i++)
            lk[i] = l[i];
        if (e[EDP_SEQ] == s)
            break;
    }
    w = *(volatile u32 *)SURFACE_W;
    h = *(volatile u32 *)SURFACE_H;
    if (k == 3 || mode < EDP_LEVEL || mode > EDP_LAST || !e[EDP_WORK] || w < 256 || h < 256 || w > 1024 || h > 1024) {
        e[EDP_FAILED]++;
        return 0;
    }
    p.seg = (u8 *)e[EDP_WORK];
    p.xy = (s16 *)(e[EDP_WORK] + EDP_SEG_SIZE);
    p.ns = p.np = 0;
    p.dropped = 0;
    p.start = 1;
    p.color = 0;
    p.cx = (s32)(w * Q / 2);
    p.cy = (s32)(h * Q / 2);
    for (i = 0; i < EDP_SIN_N; i++)         /* the sines: once from the image, then from the stack */
        p.sin[i] = sq[i];
    p.ring = (const volatile u32 *)((u32)ring_tab + UNCACHED);
    p.path = drv_vg_create_path(0, VG_PATH_DATATYPE_S_16, F_QUARTER, 0, MAX_SEG, 2 * MAX_PTS, VG_PATH_CAPABILITY_ALL);
    if (!p.path) {
        e[EDP_FAILED]++;
        return 0;
    }
    p.paint = paint = drv_vg_create_paint();
    if (!paint) {
        drv_vg_destroy_path(p.path);
        e[EDP_FAILED]++;
        return 0;
    }
    for (k = 0; k < 4; k++)
        zero[k] = 0;
    drv_vg_setfv(VG_CLEAR_COLOR, 4, zero);
    drv_vg_clear(0, 0, w, h);
    drv_vg_seti(VG_MATRIX_MODE, VG_MATRIX_PATH_USER_TO_SURFACE);
    drv_vg_load_identity();
    drv_vg_seti(VG_FILL_RULE, VG_NON_ZERO);
    drv_vg_seti(VG_BLEND_MODE, VG_BLEND_SRC_OVER);
    drv_vg_seti(VG_MASKING, 0);
    drv_vg_seti(VG_SCISSORING, 0);
    drv_vg_seti(VG_STROKE_CAP_STYLE, VG_CAP_BUTT);
    drv_vg_seti(VG_STROKE_JOIN_STYLE, VG_JOIN_ROUND);
    drv_vg_set_paint(paint, VG_FILL_PATH | VG_STROKE_PATH);
    for (i = 0; i < HUES; i++) {
        sat[i] = (u8)(satw[i / 4] >> (8 * (i % 4)));
        sat[i] = sat[i] > 10 ? 10 : sat[i];
        k = lk[EDL_ANGLE + i / 4] >> (8 * (i % 4)) & 0xff;
        ang[i] = (u8)(k >= STEPS ? k - STEPS : k);   /* a byte: below two turns */
    }
    if (mode == EDP_LOOK)
        draw_look(&p, names, index & 0xffff, index >> 16, arts);
    else if (mode == EDP_LOOK_GRAIN)
        draw_look_grain(&p, gnames, gvalue & 0xff, gvalue >> 8 & 0xff, gvalue >> 16 & 0xff);
    else if (mode == EDP_LEVEL || mode == EDP_LOOK_LEVEL)
        draw_level(&p, value & 0xff, (value >> 8) & 0xff);
    else if (mode == EDP_CURVE)
        draw_curve(&p, (s32)(value & 0xff) - 7, (s32)((value >> 8) & 0xff) - 7, (s32)((value >> 16) & 0xff) - 7,
                   (value >> 24) & 1);
    else if (mode == EDP_LOOK_CURVE)
        draw_look_curve(&p, (s32)(value & 0xff) - 7, (s32)((value >> 8) & 0xff) - 7, (s32)((value >> 16) & 0xff) - 7,
                        (value >> 24) & 1, (const u8 *)&lk[EDL_CURVE]);
    else if (mode == EDP_LOOK_VIVID)
        draw_look_vivid(&p, sat, cur & 0xff, (cur >> 8) & 1, lk[EDL_MASK], ang, &lk[EDL_RGBA]);
    else
        draw_vivid(&p, sat, cur & 0xff, (cur >> 8) & 1);
    drv_vg_destroy_paint(paint);
    drv_vg_destroy_path(p.path);
    drv_c3d_swap();
    e[EDP_DRAWN]++;
    if (p.dropped)                          /* the frame is shown as it is; the core task logs the count (drv_edp_service) */
        e[EDP_DROPPED] += p.dropped;
    return 1;
}
