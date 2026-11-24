/* Picture profiles for OM-5 Mark II 1.1 (core/prof.c).
 *
 * The stored picture mode (setting 111) always holds a stock mode - Natural while a profile is active - so the flash never
 * gets anything the stock firmware does not know. The profile lives in memory:
 *  - the UI sees it: the UI settings object (table 0x60bb6ad0) is the only way the UI reads and writes settings; its read
 *    method answers 111 = 54..59 while a profile is active, its write methods turn a choice of 54..59 into "profile n,
 *    store Natural". The native editor column then edits the profile's cells,
 *    which are stock settings (decision R3);
 *  - the picture mode screen strip lists the six profiles; the availability handler of 111 lets 54..59 through;
 *  - shooting gets it: after the shooting parameter reader fills its buffer S, the driver writes the profile fields from
 *    the profile cells. Not in movie variants and not in ART bracketing frames.
 * Everything here is data in RAM (pointers, a list, one jmp); nothing is written to settings except Natural by the firmware.
 *
 * The hooks run in firmware tasks, some of them on CPUs 1 and 2, whose data caches do not see what CPU 0 wrote through its
 * cache. core 0.62 kept the pointer to the shared state in an ordinary (cached) variable: on another CPU it read as 0 and
 * the camera crashed at start. So: the shared state is in the arena (uncached), and
 * the pointer to it sits alone in its own cache lines in .data (not .bss: core clears .bss through the cache) and is only
 * ever read and written through the uncached alias. No hook touches any other core variable.
 *
 * The method table and the availability table lie in 0x60000200..0x60cbe7ff, which the memory protection unit watches: a
 * write there through the cached alias raises an NMI and the camera restarts. core 0.63
 * did exactly that and looped. Those words are written through the uncached alias,
 * like drv_code_jump does for code; the cached copy CPU 0 may still hold is pushed out by reading past the data cache.
 * The picture mode lists (0x611...) are outside the range and are written through the cache, in order (pointer, count),
 * so that the UI on CPU 0 never sees a count without its list.
 *
 * The film look uses the same UI hooks: a stock picture mode of the strip (the slot) shows the
 * look; while it is active the stored mode is Natural, the slot's editor column is the Color Creator's [111, 5029] and the
 * two dial cells of that item (615, 616) are virtual - their writes step through the module's entries. Nothing of the
 * profiles' shooting path is used: the look's shot is Natural, the module changes it through the ISP service. */
#include "omk_drv.h"
#include "omk_core.h"
#include "omk_log.h"

#define NATURAL 1                           /* setting 111 value of Natural as the UI object reads and writes it */
#define NATURAL_RAW 2                       /* the same as the raw settings read (drv_setting_read) returns it, and in S;
                                               camera, core 0.64: comparing the raw value with 1 refused every restore */
#define S_NATURAL NATURAL_RAW
#define FIRST_COLOR 54                      /* UI picture modes of the profiles: colour 1..3 = 54..56, mono 1..3 = 57..59 */
#define FIRST_MONO 57
#define PROFILES 3
#define ID_COLORTONE 111
#define ID_RECENT_SCP 2149                  /* MODE_RECENTLY_COLORTONE_SCP: the UI writes the current mode here on OK */
#define ID_COLOR_SET 903                    /* which colour / mono profile: only the SCP cell reads them */
#define ID_MONO_SET 989
#define ID_CAMERA 26                        /* MODE_CAMERA */
#define ID_SCENE_NAV 109                    /* MODE_SCENE_NAVIGATION */

/* UI settings object methods (section 6 data) and the availability handler table */
#define UI_VT 0x60bb6ad0
#define UI_WRITE 0x608efc53                 /* +0: d0 id, d1 kind (0 - value), a1 value */
#define UI_WRITE4 0x608efeea                /* +4: d0 id, a1 value */
#define UI_READ 0x608ef257                  /* +8: d0 id, a1 where to put the value; d0 result, 0 - ok */
#define AVAIL_SLOT 0x604785a8               /* {111, handler}: the handler for 111 */
#define AVAIL_111 0x602d7964
/* Picture mode screen lists (section 7 data): {u32 id, u32 count, u32 list} */
#define LISTS 0x61199b58
#define LIST_111 0x61199464
#define LIST_N 41
#define ENTRY 20                            /* {mode, ONOFF id, mode, variant, availability} */
#define FIRST_ART 10                        /* index of the first ART filter (mode 29) in the stock list: the profiles go
                                               right before it, after the stock modes */
#define FIRST_ART_MODE 29
#define PROFILE_ENTRY_COLOR 0x61199b08      /* entries 54, 55, 56 of the colour profile list */
#define PROFILE_ENTRY_MONO 0x61199ab8       /* entries 57, 58, 59 */
#define TAKE_SITE 0x602c8ad6                /* in the shooting parameter reader 0x602c8139, d3 = S; fields below */
/* The picture mode the image memory allocator is set up for: after each
 * picture mode rebuild 0x60426608 reads the raw 111 and sets the allocator mode 0x604131dc - ART filters and the
 * monochrome profile (raw 12) take mode 1, 9 blocks; Natural and the colour profile (raw 11) mode 2, 2 blocks. With the
 * profile only in memory the allocator stays at Natural's layout: a monochrome shot then most likely waited for memory
 * forever (camera, core 0.64). The filter reports the profile there; the mode is kept at ALLOC_MODE */
#define ALLOC_SITE 0x60426675               /* d0 = raw 111 just read */
#define ALLOC_MODE 0xa263fbd4               /* uncached u16 (movhu in 0x604131dc); 1 - the large layout. The next
                                               halfword is another variable: the camera read the word as 0x20002 */
#define ALLOC_RAW_LARGE 12                  /* the monochrome profile's raw 111: mode 1 */
/* Live view. The image module rebuilds its context from the
 * settings about every second (camera, core 0.71). Two assembler trampolines (thunks.s), no C, no core variables: they
 * may run on CPU 2 (live view) and see only drv_lv_shared, through the uncached alias:
 *  LVR at 0x6034f966 - the module's reader of the raw 111 (a leaf: mov (0xa26eca5c),a0; movbu (a0),d0; retf): Natural 2
 *  becomes the profile's 11/12 when LV_MODE says so; the request, the context mode A+260 and the processing type follow;
 *  LVA at 0x6033e131 - right after the context's parameter block was filled from the settings (A at (12,sp)): when A+260
 *  is our mode, the profile's values are copied into A. The core task prepares the block (drv_prof_prepare) */
#define LVR_SITE 0x6034f966
#define LVA_SITE 0x6033e131
/* drv_lv_shared, bytes; the trampolines use the same offsets */
#define LV_SEQ 0                            /* u32, odd while the core task writes the block */
#define LV_MODE 4                           /* 0, 11 or 12 */
#define LV_SHARP 6
#define LV_CONTRAST 7
#define LV_FILTER 8
#define LV_TONING 9
#define LV_CC 10                            /* A+705: colour profile number */
#define LV_MC 11                            /* A+730: monochrome profile number */
#define LV_LEVEL 12
#define LV_SHADING 13
#define LV_GRAIN 14
#define LV_TONE_H 15
#define LV_TONE_M 16
#define LV_TONE_L 17
#define LV_SAT12 20                         /* u16 x 12 */
#define LV_READS 52                         /* u32: LVR substitutions */
#define LV_APPLIED 56                       /* u32: LVA copies */
/* Graphic profile editors (editor.c): the Color/Level item gets the
 * Color Creator item's "background", so the GUI frame orders the native Color Creator drawing for it (start on the item, an
 * order per dial step, end on leaving); the renderer's entry on CPU 1 draws our indicator instead of the ring; the Color
 * Creator item turns that off. The item handler table 0x611b79a8 {item - 6, branch} is section 7 data, outside the protected
 * range */
#define ITEM_LEVEL_KEY 0x611b7a60           /* 5373 - 6 */
#define ITEM_LEVEL_STUB 0x60e49e3a          /* the dispatcher's branch for 5373 */
#define ITEM_VIVID_KEY 0x611b7a68           /* 5374 - 6 */
#define ITEM_VIVID_STUB 0x60e49e31
#define ITEM_CURVE_KEY 0x611b7a70           /* 5375 - 6 */
#define ITEM_CURVE_STUB 0x60e49e43
#define ITEM_CC_KEY 0x611b7aa0              /* 5029 - 6 */
#define ITEM_CC_STUB 0x60e49e1f
#define C3D_SITE 0x60ccf4f7
#define TAKE_RECENT_MS 3000                 /* the camera is writing a picture for a while after the reader ran */
/* Profile cells (settings): colour profile k (0..2), monochrome k */
#define CELL_C_SHARP 904                    /* + k */
#define CELL_C_CONTRAST 908                 /* + k */
#define CELL_C_SHADING 912                  /* + k */
#define CELL_C_SAT 928                      /* + 12k + i, 12 hues */
#define CELL_C_TONE 976                     /* + 3k: H, M, L */
#define CELL_M_SHARP 990
#define CELL_M_CONTRAST 994
#define CELL_M_GRAIN 999
#define CELL_M_TONING 1003
#define CELL_M_FILTER 1007
#define CELL_M_LEVEL 1012
#define CELL_M_SHADING 1017
#define CELL_M_TONE 1021                    /* + 3k: H, M, L */

/* S fields */
#define S_SHARP 28694                       /* u16 */
#define S_CONTRAST 28700                    /* u16 */
#define S_GRADATION 28702                   /* u16 */
#define S_EFFECT 28705                      /* u8 */
#define S_SATURATION 28732                  /* u16 */
#define S_MODE 28735                        /* u8: 11 colour profile, 12 mono profile */
#define S_FILTER 28736                      /* u8 */
#define S_TONING 28737                      /* u8 */
#define S_SAT12 28740                       /* u16 x 12 */
#define S_NUMBER 28765                      /* u8 */
#define S_LEVEL 28766                       /* u8 */
#define S_GRAIN 28767                       /* u8 */
#define S_SHADING 28768                     /* u8; for colour profiles the firmware drops it (decision R9) */
#define S_TONE_L 29469                      /* u8 */
#define S_TONE_H 29470
#define S_TONE_M 29471
#define S_ART_BKT 29496                     /* u8: the reader marks an ART bracketing frame */

/* EXIF UserComment in S: the builders of the JPEG and ORF headers (0x6033085c) copy
 * S[0..4472) into the file; UserComment is S+0xf7c, 125 bytes - an 8-byte character code, then 117 characters (the reader
 * fills them with spaces from its template every time). DateTimeOriginal follows at S+0xff9: never a byte more */
#define S_USERCOMMENT 0xf7c
#define EXIF_TEXT 117

typedef struct {
    u16 contrast, sharp, sat[12];
    u8 tone_h, tone_m, tone_l, filter, toning, level, grain, shading;
} prof_block;

/* From the shared block (uncached, volatile) and back. Bytes: a character type may access any object (C aliasing rules),
 * and there is no memcpy in core; 36 accesses */
static void block_get(prof_block *b, const volatile prof_block *from)
{
    const volatile u8 *s = (const volatile u8 *)from;
    u8 *d = (u8 *)b;
    u32 i;

    for (i = 0; i < sizeof(prof_block); i++)
        d[i] = s[i];
}

static void block_put(volatile prof_block *to, const prof_block *b)
{
    volatile u8 *d = (volatile u8 *)to;
    const u8 *s = (const u8 *)b;
    u32 i;

    for (i = 0; i < sizeof(prof_block); i++)
        d[i] = s[i];
}

#define DIAL_TRACE 64                       /* the dial trace: writes kept for the log */
#define ED_RING 32                          /* the look's editor events between two core loops (all hues: 12 at once) */
#define ED_ODD 8                            /* editor writes not understood, kept for the log */

/* The look's editors (API 16). What the module last published (pub) and what the UI is shown (view). The native item
 * code keeps the values it wrote itself and steps from them, so the driver keeps the cells as the native code knows them
 * (the shadow: what it wrote last, what its reads get) and takes a step as the written value - the shadow's. A dial step
 * changes the shadow and the view at once, in the GUI task; the module's values of the entry shown replace them only on a
 * new entry or when no step of the group is newer (its hold). The events go to the core task through a ring */
typedef struct {
    signed char tone[3];                    /* shadows, middle, highlights */
    signed char vivid[12];                  /* the sectors' shifts from the preset's saturation */
    u8 filter, level;
} look_vals_t;

typedef struct {
    u32 known;                              /* the module published once at least */
    u32 groups, edited;                     /* OMK_EDIT_* bits */
    look_vals_t pub, view;
    u16 mask;
    /* What the item handler takes at every redraw, as the drawing needs it: made once, when the module publishes
     * (drv_look_edits, outside drv_lock), and only copied under the lock - the block is outside the cache, so every
     * access under the lock is a bus access with the interrupts off. Word aligned (ED_ALIGNED): copied by words */
    u8 curve[64];                           /* the module's curve, of pub.tone */
    u8 angle[12];                           /* the vertices' angles, 2.5 degrees a step clockwise from the top */
    u32 rgba[12];                           /* their colours */
    signed char base[12];                   /* the sectors' own saturation, 0.05 a step */
    u8 ord[12], pos[12];                    /* the native cursor's place k (cell 928 + k) -> sector by angle, and back */
    s32 sh_sat[12];                         /* the shadow: the saturation cells 928 + k */
    u32 sh_cursor;                          /* 2201: 928 + k, 0 - all hues */
    u32 sh_one;                             /* 2197: 1 one hue, 0 all (INFO) */
    s32 sh_tone[3];                         /* 976 Hi, 977 Mid, 978 Sh */
    u32 sh_mid;                             /* 2189: INFO chose Mid */
    u32 fplace;                             /* Color/Level: the filter's place on the ring */
    u32 wr_sat, wr_tone, wr_t;              /* cells written since the last commit (bits) and when */
    s32 old_sat[12];                        /* the saturation shadow before those writes */
    u32 hold_t[OMK_EDIT_GROUPS];            /* drv_ticks of the view's last step of a group; 0 - no hold */
    u32 stale;                              /* a dial chose another entry: the values are of the one before */
    u32 ring[ED_RING], head, tail, lost;    /* events: the GUI task writes head, the core task tail */
    u32 odd[ED_ODD], odd_head, odd_tail;    /* writes not understood: id | kind << 16, the value apart */
    u32 odd_val[ED_ODD];
    u32 since;                              /* drv_ticks when the look came on without the module's editors yet */
    /* The grain item (API 17) on the native Color/Vivid item 5374 (while the module shows neither Color/Vivid nor
     * Color/Level): the rear dial (the cursor 2201, a ring of 13 places, "sub" in the emulator) steps the tile, the front
     * dial (the cell under the cursor 928 + k, or all 12 at the "all" place; "main" in the emulator) the strength; a long
     * OK writes 0 into all 12: the strength Off. The tiles' names (arena, LOOK_NAME bytes each) and their number come
     * from core once; the tile and strength of the entry shown from the module (look_grain), the view at once from a step */
    u32 g_names, g_count;                   /* g_count 0 - no grain item */
    u32 g_known, g_stale;                   /* the module's values came for the entry shown; a dial chose another entry */
    s32 g_tile;                             /* the view: the tile (-1 none), the strength */
    u32 g_level;
    u32 g_cursor, g_one;                    /* the shadow: 2201 and 2197 as the native code wrote them last */
    s32 g_sat[12];                          /* ... 928..939 */
    u32 g_old_cursor;                       /* ... before the writes since the last commit */
    s32 g_old_sat[12];
    u32 g_wr, g_hold;                       /* written since the last commit: bits 0..11 the cells 928 + k, bit 12 the
                                               cursor; the view's last step (drv_ticks | 1), 0 - none */
} look_ed_t;
#define ED_ALIGNED(f) typedef char ed_##f##_is_word_aligned[__builtin_offsetof(look_ed_t, f) % 4 ? -1 : 1]
ED_ALIGNED(curve);
ED_ALIGNED(angle);
ED_ALIGNED(rgba);
ED_ALIGNED(base);
typedef struct {
    u32 prof;                               /* active profile, OMK_PROF_*; 0 - none */
    u32 ui_seq, ui_prof;                    /* the UI changed the profile: counter and what to */
    u32 reads, writes;                      /* UI hooks */
    u32 takes, applied, skip_mode, skip_cam, skip_bkt;
    u32 other_cpu;                          /* hook calls on CPUs 1 and 2 */
    u32 recalc, in_recalc, recalcs;         /* the picture mode must be rebuilt for a profile change (GUI task) */
    u32 last_take;                          /* drv_ticks of the last shooting parameter build */
    u32 alloc_sub, skip_alloc, skip_block;
    u32 alloc_large, alloc_large_n;         /* the grain texture: keep the large layout (mode 1); substitutions made */
    /* The profile values, prepared by the core task; the shooting hook only copies them. blk_seq is odd while the block is
     * being written (a sequence lock): the hook copies, then checks that blk_seq did not change */
    prof_block blk;
    u32 blk_seq, blk_prof;
    u32 still;                              /* still_variant() as the core task last saw it, for hooks on CPUs 1 and 2 */
    /* The film look: its slot (the picture mode + 1; 0 - no look service), active, the user's choices (a counter and on/off),
     * the entries - names in the arena, the chosen one and a counter of dial choices; the dial cells as the UI was last
     * given them; swallowed writes and the kind and id of the last one (for the log) */
    u32 look_slot, look, look_seq, look_ui;
    u32 look_names, look_count, look_index, look_pick;
    u32 cc_last[2];
    u32 swallowed, swallow_kind;
    u32 dial_last;                          /* the last write of 615/616, for the log: kind << 24 | (616: 1 << 16) |
                                               value; the hue's step from the stored hue 1 << 20, no step 1 << 21 */
    /* Every write of 615/616 on the look's item, for the log (GUI task writes, the core task reads): drv_ticks and
     * kind << 28 | (616: 1 << 24) | (the step taken, s8) << 16 | value; dial_head - how many were written */
    u32 dial_head;
    u32 dial_t[DIAL_TRACE], dial_w[DIAL_TRACE];
    /* A text for the EXIF UserComment of the pictures (the core task writes it under exif_seq, odd while written); 0 in
     * exif_len - the camera's own spaces */
    u32 exif_seq, exif_len, exif_put;
    u8 exif[EXIF_TEXT];
    look_ed_t ed;                           /* the look's editors (core task and GUI task, CPU 0, under drv_lock) */
} shared_t;

#define UNCACHED DRV_UNCACHED

/* Only through slot(): the uncached alias. 64 bytes aligned: nothing else shares its cache lines */
static u32 shared_slot[16] __attribute__((aligned(64), section(".data.cpu_shared"))) = {1};

/* 64 bytes, own cache lines, used only through the uncached alias: the live view trampolines read it by address */
u32 drv_lv_shared[16] __attribute__((aligned(64), section(".data.cpu_shared")));

static volatile u8 *lv(void)
{
    return (volatile u8 *)((u32)drv_lv_shared + UNCACHED);
}

static volatile u32 *slot(void)
{
    return (volatile u32 *)((u32)shared_slot + UNCACHED);
}

static volatile shared_t *shared(void)
{
    return (volatile shared_t *)slot()[1];
}

static u8 *strip;                           /* the new strip list */
static int installed;                       /* the profiles */
static int base_installed;                  /* the shared state and the UI hooks (profiles, film look or both) */
static int look_installed;
#define ID_CC_HUE 615                       /* the Color Creator item's dial cells: the hue 0..29 (on the camera the rear */
#define ID_CC_VIVID 616                     /* dial; "sub" in the emulator) and the vividness (the front dial; "main" in
                                               the emulator; a dial test on the camera) */
static int look_changed_ui(volatile shared_t *sh, u32 on);
static int look_column_put(void);
static u32 look_icons_put(void);
static void look_item_picture(u32 on);
static void ed_item_picture(void);

void drv_tramp_take(void);
void drv_tramp_alloc(void);
void drv_tramp_lvr(void);
void drv_column_color(void);                /* thunks.s: the colour profile column of the editor, without shading */
void drv_mf0(void);                         /* thunks.s: filter matrix handlers, one per slot */
void drv_mf1(void);
void drv_mf2(void);
void drv_mf3(void);
void drv_mf4(void);
void drv_mf5(void);
void drv_mf6(void);
void drv_mf7(void);
static int column_put(void);
static int column_set;                      /* the colour profile column without shading is in place */
static u32 shading_rects(void);
static int editor_put(void);
static int editor_set;                      /* the Color/Level item orders the Color Creator drawing */
static int editor_ok;                       /* the renderer hook is in place */
void drv_item_level(void);                  /* thunks.s: the Color/Level item handler with the "background" */
void drv_item_vivid(void);                  /* thunks.s: the Color/Vivid item handler with the "background" */
void drv_item_curve(void);                  /* thunks.s: the tone curve item handler with the "background" */
void drv_item_cc(void);                     /* thunks.s: the Color Creator item: our drawing off */
void drv_tramp_c3d(void);                   /* thunks.s: entry of the Color Creator renderer, CPU 1 */
void drv_tramp_lva(void);
void drv_fw_mode_changed(void);             /* thunks.s: the stock callback of 111 0x602dd18d, GUI task only */
void drv_ui_read(void);
void drv_ui_write(void);
void drv_ui_write4(void);
void drv_ui_avail(void);

#define GUARD_LO 0x60000200u                /* memory protection: writes through the cached alias are fatal here */
#define GUARD_HI 0x60cbe800u
#define EVICT_FROM 0x61100000u              /* section 7 data (outside the protected range), read through the cache */
#define EVICT_SIZE 0x40000u                 /* the data cache is 8 KB per CPU, 2 ways x 256 lines x 16 bytes
: 256 KB is far more */
#define CACHE_LINE 16

static volatile u32 *word(u32 a)
{
    return (volatile u32 *)a;
}

/* A word of firmware data as it is in memory, past the cache */
static volatile u32 *mem_word(u32 a)
{
    return (volatile u32 *)(a + UNCACHED);
}

/* Write a word of firmware data: in the protected range only through the uncached alias, then read it back the same way.
 * 0 - written */
static int put_word(u32 a, u32 v)
{
    if (a >= GUARD_LO && a < GUARD_HI) {
        *mem_word(a) = v;
        return *mem_word(a) == v ? 0 : -1;
    }
    *word(a) = v;
    return 0;
}

/* Push lines of CPU 0's data cache out by reading more than its size through the cache: clean lines are simply dropped,
 * so the next cached read of the protected words fetches what put_word wrote */
static void evict_cache(void)
{
    volatile const u32 *p = (volatile const u32 *)EVICT_FROM;
    u32 i, sum = 0;

    for (i = 0; i < EVICT_SIZE / 4; i += CACHE_LINE / 4)
        sum += p[i];
    (void)sum;
}

static u32 ui_mode(u32 prof)
{
    u32 n = OMK_PROF_NUM(prof);

    if (!n || n > PROFILES)
        return 0;
    return (OMK_PROF_IS_MONO(prof) ? FIRST_MONO : FIRST_COLOR) + n - 1;
}

static u32 prof_of_mode(u32 mode)
{
    if (mode >= FIRST_COLOR && mode < FIRST_COLOR + PROFILES)
        return OMK_PROF_COLOR(mode - FIRST_COLOR + 1);
    if (mode >= FIRST_MONO && mode < FIRST_MONO + PROFILES)
        return OMK_PROF_MONO(mode - FIRST_MONO + 1);
    return OMK_PROF_NONE;
}

int drv_prof_count(u32 *colors, u32 *monos)
{
    *colors = *monos = PROFILES;
    return 1;
}

/* Whether live view must drop the current profile block at once on a change from `from` to `to`. Within one kind (colour to
 * colour, monochrome to monochrome) the old block stays until the core task publishes the new one: the allocator's layout
 * is the same, so the old profile is a valid picture, and live view goes straight from the old profile to the new one.
 * Leaving the profiles, or a change of kind, drops it: live view shows the base mode (Natural) until the new block, which
 * the core task publishes right after the rebuild (recalc_if_needed wakes it). Until core 1.31 every change dropped it, and
 * live view showed Natural for up to one 250 ms core pass - a monochrome profile flashed in colour (camera, 2026-11-20) */
static int lv_drop(u32 from, u32 to)
{
    return !to || !from || OMK_PROF_IS_MONO(from) != OMK_PROF_IS_MONO(to);
}

/* The UI changed the profile (GUI task, CPU 0): the core loop picks it up by the counter */
static void ui_changed(volatile shared_t *sh, u32 prof)
{
    if (prof && sh->look)
        look_changed_ui(sh, 0);             /* a profile and the film look exclude each other */
    if (lv()[LV_MODE] && prof != sh->prof && lv_drop(sh->prof, prof))
        lv()[LV_MODE] = 0;
    sh->prof = prof;
    sh->ui_prof = prof;
    sh->ui_seq++;
    sh->recalc = 1;
}

/* The profile changed while 111 stays Natural, so the firmware does not rebuild the picture mode by itself: call its
 * callback of 111, in the GUI task, where the firmware calls it after a real change. The allocator filter then sees the
 * profile. Not nested: the rebuild reads settings and may come back through the UI hooks */
static void recalc_if_needed(volatile shared_t *sh)
{
    u32 st;

    if (!sh->recalc || sh->in_recalc || drv_cpu())
        return;
    st = drv_lock();                        /* check and take in one step: two GUI-side tasks must not both rebuild */
    if (!sh->recalc || sh->in_recalc) {
        drv_unlock(st);
        return;
    }
    sh->recalc = 0;
    sh->in_recalc = 1;
    drv_unlock(st);
    drv_fw_mode_changed();
    sh->recalcs++;
    sh->in_recalc = 0;
    drv_wake();                             /* the core task publishes the new live view block now, after the rebuild (the
                                               allocator's layout of a monochrome profile is set by it), not at the end of its
                                               250 ms wait */
}

/* After the UI write method returned (thunks.s) */
void drv_ui_after_write(void)
{
    volatile shared_t *sh = shared();

    if (sh)
        recalc_if_needed(sh);
}

static void look_ed_read(volatile shared_t *sh, u32 id, u32 ret, u32 *where);

/* After the UI read method: d0 id, d1 where, the caller's return address. While a profile is active and 111 is really
 * Natural, the UI sees the profile */
void drv_ui_read_fix(u32 id, u32 *where, u32 ret)
{
    volatile shared_t *sh = shared();
    u32 p, n;

    if (!sh)
        return;
    if (drv_cpu())
        sh->other_cpu++;
    if (sh->look) {                         /* the film look: the slot's mode and the virtual dial cells */
        if (id == ID_COLORTONE) {
            if (*where != NATURAL) {        /* stored mode changed without us: the look is off */
                look_changed_ui(sh, 0);
                return;
            }
            *where = sh->look_slot - 1;
            sh->reads++;
            look_ed_read(sh, id, ret, where);
            recalc_if_needed(sh);           /* the allocator's layout changed with the look's grain */
        } else if (id == ID_CC_HUE || id == ID_CC_VIVID) {
            *where = sh->cc_last[id == ID_CC_VIVID];
        } else {
            look_ed_read(sh, id, ret, where);
        }
        return;
    }
    p = sh->prof;
    n = OMK_PROF_NUM(p);
    if (!p) {
        if (id == ID_COLORTONE)
            recalc_if_needed(sh);           /* the allocator's layout changed (the grain texture) */
        return;
    }
    if (id == ID_COLORTONE) {
        if (*where != NATURAL) {            /* stored mode changed without us (reset, custom mode, remote): profile off */
            ui_changed(sh, OMK_PROF_NONE);
            return;
        }
        *where = ui_mode(p);
        sh->reads++;
        recalc_if_needed(sh);               /* a profile restored at power-on: rebuilt at the first UI read */
    } else if (id == ID_COLOR_SET && !OMK_PROF_IS_MONO(p)) {
        *where = n;
    } else if (id == ID_MONO_SET && OMK_PROF_IS_MONO(p)) {
        *where = n;
    }
}

/* Before the UI write methods: the value to store. 111 = 54..59 - the profile, Natural stored; other 111 - profile off;
 * 2149 never gets a profile mode */
u32 drv_ui_write_fix(u32 id, u32 value)
{
    volatile shared_t *sh = shared();
    u32 p = prof_of_mode(value);

    if (!sh)
        return p && (id == ID_COLORTONE || id == ID_RECENT_SCP) ? NATURAL : value;
    if (drv_cpu())
        sh->other_cpu++;
    if (id == ID_COLORTONE) {
        sh->writes++;
        if (sh->look_slot && value == sh->look_slot - 1) {   /* the slot: the film look, Natural stored */
            if (sh->prof)
                ui_changed(sh, OMK_PROF_NONE);
            if (!sh->look)
                look_changed_ui(sh, 1);
            return NATURAL;
        }
        if (sh->look)
            look_changed_ui(sh, 0);
        if (p) {
            ui_changed(sh, p);
            return NATURAL;
        }
        if (sh->prof)
            ui_changed(sh, OMK_PROF_NONE);
    } else if (id == ID_RECENT_SCP && p) {
        return NATURAL;
    }
    return value;
}

/* Movie variants and scene modes keep their own picture mode: the profile only where 111 has variant 0
 * (table 0x604daf4c). The settings getter is called on CPU 0 only - from
 * firmware tasks there it is what the firmware itself does; a hook on another CPU takes the value the core task saved */
static int still_variant_now(void)
{
    static const u8 cams[] = {3, 4, 5, 6, 34, 59, 76, 77, 89, 90, 99, 100};
    u32 cam = drv_setting_read(ID_CAMERA), i;

    if (drv_setting_read(ID_SCENE_NAV))
        return 0;
    for (i = 0; i < sizeof(cams); i++)
        if (cam == cams[i])
            return 1;
    return 0;
}

static int still_variant(volatile shared_t *sh)
{
    return drv_cpu() ? (int)sh->still : still_variant_now();
}

static void put16(u8 *s, u32 off, u32 v)
{
    s[off] = v;                             /* little endian, may be unaligned */
    s[off + 1] = v >> 8;
}

/* The module's text into UserComment of this S: "ASCII" and the text padded with spaces; a text being written - none */
static void exif_put(volatile shared_t *sh, u8 *s)
{
    u32 seq = sh->exif_seq, n = sh->exif_len, i;

    if (!n || (seq & 1) || n > EXIF_TEXT)
        return;
    for (i = 0; i < EXIF_TEXT; i++)
        s[S_USERCOMMENT + 8 + i] = i < n ? sh->exif[i] : ' ';
    if (sh->exif_seq != seq) {              /* rewritten while copied: the camera's spaces this time */
        for (i = 0; i < EXIF_TEXT; i++)
            s[S_USERCOMMENT + 8 + i] = ' ';
        return;
    }
    s[S_USERCOMMENT] = 'A';                /* the character code "ASCII and three zero bytes" (immediates: no table read on another CPU) */
    s[S_USERCOMMENT + 1] = 'S';
    s[S_USERCOMMENT + 2] = 'C';
    s[S_USERCOMMENT + 3] = 'I';
    s[S_USERCOMMENT + 4] = 'I';
    for (i = 5; i < 8; i++)
        s[S_USERCOMMENT + i] = 0;
    sh->exif_put++;
}

/* Core task: the text (printable ASCII, cut to 117), 0 - none. 0, -1 no shared state */
int drv_exif_text(const char *t)
{
    volatile shared_t *sh = shared();
    u32 n = 0;

    if (!sh)
        return -1;
    sh->exif_seq++;
    for (; t && t[n] && n < EXIF_TEXT; n++)
        sh->exif[n] = t[n] >= 32 && t[n] < 127 ? t[n] : '?';
    sh->exif_len = n;
    sh->exif_seq++;
    return 0;
}

u32 drv_exif_count(void)
{
    volatile shared_t *sh = shared();

    return sh ? sh->exif_put : 0;
}

/* Shooting parameter reader, before its last two fields (any task, maybe another CPU) */
void drv_take_filter(u8 *s)
{
    volatile shared_t *sh = shared();
    prof_block b;
    u32 p, i, mono, seq;

    if (!sh)
        return;
    if (drv_cpu())
        sh->other_cpu++;
    sh->takes++;
    sh->last_take = drv_ticks();
    exif_put(sh, s);
    p = sh->prof;
    if (!p)
        return;
    if (s[S_MODE] != S_NATURAL) {
        sh->skip_mode++;
        return;
    }
    if (s[S_ART_BKT] == 1) {
        sh->skip_bkt++;
        return;
    }
    if (!still_variant(sh)) {
        sh->skip_cam++;
        return;
    }
    seq = sh->blk_seq;
    if ((seq & 1) || sh->blk_prof != p) {   /* being written, or this profile not prepared yet */
        sh->skip_block++;
        return;
    }
    block_get(&b, &sh->blk);                /* the arena is uncached: the copy comes from memory */
    if (sh->blk_seq != seq) {               /* rewritten while copied: Natural this time rather than a mix */
        sh->skip_block++;
        return;
    }
    mono = OMK_PROF_IS_MONO(p);
    if (mono && *(volatile u16 *)ALLOC_MODE != 1) {
        sh->skip_alloc++;                   /* the allocator is not set up for a monochrome shot: Natural, not a hang */
        return;
    }
    s[S_MODE] = mono ? 12 : 11;
    s[S_NUMBER] = OMK_PROF_NUM(p);
    put16(s, S_CONTRAST, b.contrast);
    put16(s, S_SHARP, b.sharp);
    put16(s, S_SATURATION, 0);
    put16(s, S_GRADATION, 1);
    s[S_EFFECT] = 0;
    for (i = 0; i < 12; i++)
        put16(s, S_SAT12 + 2 * i, b.sat[i]);
    s[S_TONE_H] = b.tone_h;
    s[S_TONE_M] = b.tone_m;
    s[S_TONE_L] = b.tone_l;
    s[S_FILTER] = b.filter;
    s[S_TONING] = b.toning;
    s[S_LEVEL] = b.level;
    s[S_GRAIN] = b.grain;
    s[S_SHADING] = b.shading;
    sh->applied++;
}

/* The raw 111 the allocator setup has just read (GUI task): the profile's type while a profile is active */
void drv_alloc_filter(u32 *v)
{
    volatile shared_t *sh = shared();
    u32 p;

    if (!sh)
        return;
    p = sh->prof;
    if (p && *v == NATURAL_RAW && still_variant(sh)) {
        *v = OMK_PROF_IS_MONO(p) ? 12 : 11;
        sh->alloc_sub++;
    }
    /* The grain texture needs the large layout as the ART filters do (40.5 MB texture and the 2.36 MB tile buffer a
     * still; the monochrome profile waited for memory forever in mode 2). After this site the raw value only chooses the
     * mode: 0x60426608 takes mode 1 for a raw value from 100 (ART), 8 and 12, and passes the raw value only to
     * 0x6037f16a, whose result does not take part in the choice (both branches at 0x604268da lead to mode 2) */
    if (sh->alloc_large && *v < 100 && *v != 8 && *v != ALLOC_RAW_LARGE) {
        *v = ALLOC_RAW_LARGE;
        sh->alloc_large_n++;
    }
}

u32 drv_alloc_mode(void)
{
    return *(volatile u16 *)ALLOC_MODE;
}


/* The live view block from the published profile block: mode only for a still variant, monochrome only with the large
 * memory layout (as for a shot). Written only when it changes, under its sequence counter */
static void publish_lv(volatile shared_t *sh)
{
    volatile u8 *l = lv();
    u8 v[LV_SAT12 + 24];
    u32 p = sh->blk_prof, mono = OMK_PROF_IS_MONO(p), n = OMK_PROF_NUM(p), i, same = 1;
    prof_block b;

    for (i = 0; i < sizeof(v); i++)
        v[i] = 0;
    if (p && p == sh->prof && sh->still && (!mono || *(volatile u16 *)ALLOC_MODE == 1)) {
        block_get(&b, &sh->blk);
        v[LV_MODE] = mono ? 12 : 11;
        v[LV_SHARP] = b.sharp;
        v[LV_CONTRAST] = b.contrast;
        v[LV_FILTER] = b.filter;
        v[LV_TONING] = b.toning;
        v[LV_CC] = mono ? 0 : n;
        v[LV_MC] = mono ? n : 0;
        v[LV_LEVEL] = b.level;
        v[LV_SHADING] = b.shading;
        v[LV_GRAIN] = b.grain;
        v[LV_TONE_H] = b.tone_h;
        v[LV_TONE_M] = b.tone_m;
        v[LV_TONE_L] = b.tone_l;
        for (i = 0; i < 12; i++) {
            v[LV_SAT12 + 2 * i] = b.sat[i];
            v[LV_SAT12 + 2 * i + 1] = b.sat[i] >> 8;
        }
    }
    for (i = LV_MODE; i < sizeof(v); i++)
        if (l[i] != v[i])
            same = 0;
    if (same)
        return;
    ((volatile u32 *)l)[LV_SEQ / 4]++;      /* odd: being written */
    l[LV_MODE] = 0;                         /* the readers stop first */
    for (i = LV_MODE + 1; i < sizeof(v); i++)
        l[i] = v[i];
    l[LV_MODE] = v[LV_MODE];
    ((volatile u32 *)l)[LV_SEQ / 4]++;
}

/* Core task, every step of its loop: read the cells of the active profile; publish them to the hooks under the sequence
 * lock when they differ from the block. The cells are read every time rather than on a "written" flag: they change by
 * writes of every kind (a value, a dial step, a reset - the H&S screen, decision R7), by custom mode recall, by a full reset
 * and by the import, and a flag set on one path missed the others. 22 getter calls per 250 ms */
void drv_prof_prepare(void)
{
    volatile shared_t *sh = shared();
    prof_block b, old;
    const u8 *x, *y;
    u32 p, k, i, mono, tone, same;

    if (!sh)
        return;
    sh->still = still_variant_now();
    p = sh->prof;
    if (!p) {
        sh->blk_prof = 0;
        publish_lv(sh);
        return;
    }
    for (i = 0; i < sizeof(b); i++)
        ((u8 *)&b)[i] = 0;                  /* padding too: the blocks are compared bytewise */
    mono = OMK_PROF_IS_MONO(p);
    k = OMK_PROF_NUM(p) - 1;
    tone = (mono ? CELL_M_TONE : CELL_C_TONE) + 3 * k;
    b.contrast = drv_setting_read((mono ? CELL_M_CONTRAST : CELL_C_CONTRAST) + k);
    b.sharp = drv_setting_read((mono ? CELL_M_SHARP : CELL_C_SHARP) + k);
    for (i = 0; i < 12; i++)
        b.sat[i] = mono ? 0 : drv_setting_read(CELL_C_SAT + 12 * k + i);
    b.tone_h = drv_setting_read(tone);
    b.tone_m = drv_setting_read(tone + 1);
    b.tone_l = drv_setting_read(tone + 2);
    b.filter = mono ? drv_setting_read(CELL_M_FILTER + k) : 0;
    b.toning = mono ? drv_setting_read(CELL_M_TONING + k) : 0;
    b.level = mono ? drv_setting_read(CELL_M_LEVEL + k) : 0;
    b.grain = mono ? drv_setting_read(CELL_M_GRAIN + k) : 0;
    b.shading = drv_setting_read((mono ? CELL_M_SHADING : CELL_C_SHADING) + k);
    if (sh->blk_prof == p) {                /* the only writer is this task: the block cannot change under us */
        block_get(&old, &sh->blk);
        for (i = 0, same = 1, x = (const u8 *)&b, y = (const u8 *)&old; i < sizeof(b); i++)
            if (x[i] != y[i])
                same = 0;
        if (same) {
            publish_lv(sh);
            return;
        }
    }
    sh->blk_seq++;                          /* odd: being written */
    block_put(&sh->blk, &b);
    sh->blk_prof = p;
    sh->blk_seq++;
    publish_lv(sh);
}

/* Tasks of CPU 0 (the core loop at every pass, a module's card_busy). The tick counter has 30 bits, so "less than
 * TAKE_RECENT_MS ago" alone came true again for 3 s every 1073 s after the last picture: the window is armed by a new
 * build (the counter moved) and disarmed for good the first time it is seen over. Not polled for a whole turn of the
 * counter while armed (a long sleep right after a shot) it may read "recent" once more, until the next poll that finds
 * it over - a delay of core's writes, nothing else. The state is core's own (cached memory of CPU 0); a race of two
 * callers costs at most one answer of the same kind */
int drv_prof_take_recent(void)
{
    static u32 seen;                        /* sh->takes at the last call */
    static int armed;
    volatile shared_t *sh = shared();
    u32 takes;

    if (!sh)
        return 0;
    takes = sh->takes;
    if (takes != seen) {                    /* the hook counts first and stamps after, maybe on another CPU: the stamp
                                               may still be the last picture's, so it is not looked at this time */
        seen = takes;
        armed = 1;
        return 1;
    }
    if (armed && drv_ticks_since(sh->last_take) >= TAKE_RECENT_MS * 1000)
        armed = 0;
    return armed;
}

/* The firmware data we change, what it holds in stock and what we put there */
static int stock_ok(void)
{
    u8 *list = (u8 *)LIST_111;

    return *mem_word(UI_VT) == UI_WRITE && *mem_word(UI_VT + 4) == UI_WRITE4 && *mem_word(UI_VT + 8) == UI_READ &&
           *mem_word(AVAIL_SLOT) == AVAIL_111 && *word(LISTS) == ID_COLORTONE && *word(LISTS + 4) == LIST_N &&
           *word(LISTS + 8) == LIST_111 && *(u32 *)list == 7 && *(u32 *)(list + FIRST_ART * ENTRY) == FIRST_ART_MODE &&
           *(u32 *)(list + 40 * ENTRY) == 63;
}

/* 0 - all in place; otherwise the step that failed */
/* The cached view of the redirected words (a cached read, as the firmware reads them) */
static int cached_new(void)
{
    return *(volatile u32 *)UI_VT == (u32)drv_ui_write && *(volatile u32 *)(UI_VT + 4) == (u32)drv_ui_write4 &&
           *(volatile u32 *)(UI_VT + 8) == (u32)drv_ui_read && *(volatile u32 *)AVAIL_SLOT == (u32)drv_ui_avail;
}

static int fw_cache_inv;                     /* [core] fw_cache_inv: the firmware's cache invalidation instead of reading */

static int put_pointers(void)
{
    if (put_word(UI_VT, (u32)drv_ui_write))  /* writes first: no profile mode may reach storage */
        return 21;
    if (put_word(UI_VT + 4, (u32)drv_ui_write4))
        return 22;
    if (put_word(UI_VT + 8, (u32)drv_ui_read))
        return 23;
    if (put_word(AVAIL_SLOT, (u32)drv_ui_avail))
        return 24;
    /* CPU 0 may still hold the old words in its data cache. By default the lines are pushed out by reading (proven on the
     * camera since core 0.64). The firmware's own invalidation is on request only (fw_cache_inv): it spins on the cache control
     * register with interrupts off, the emulator hangs in it, and on the camera it has not been tried - a hang there would
     * freeze the camera at every power-on. Either way the cached view is checked */
    if (!fw_cache_inv || drv_dcache_inv(UI_VT, 12) || drv_dcache_inv(AVAIL_SLOT, 4) || !cached_new()) {
        if (fw_cache_inv)
            log_printf(OMK_LOG_WARN, "profiles: cache invalidation did not take, cache pushed out by reading");
        evict_cache();
    }
    if (!cached_new())
        log_printf(OMK_LOG_WARN, "profiles: the cached view of the redirected words is still old");
    if (installed) {
        shading_rects();
        if (column_put() >= 0)
            column_set = 1;
        else
            log_printf(OMK_LOG_WARN, "profiles: editor column table not as expected, shading stays in the colour profiles");
    }
    if (editor_ok && editor_put() >= 0)
        editor_set = 1;
    else if (editor_ok)
        log_printf(OMK_LOG_WARN, "profiles: editor item table not as expected, no graphic editors");
    if (look_installed && look_column_put() < 0)
        log_printf(OMK_LOG_WARN, "look: editor column table not as expected, the slot keeps its own column");
    if (installed) {
        put_word(LISTS + 8, (u32)strip);    /* the list before the count: 41 entries of it are valid as well */
        put_word(LISTS + 4, LIST_N + 2 * PROFILES);
    }
    return 0;
}

/* The shared state, once for the profiles and the film look: 0, or 1 (tables), 2 (memory), 3 (slot) */
static int install_shared(void)
{
    volatile shared_t *sh;

    if (base_installed)
        return 0;
    log_checkpoint("profiles: checking firmware tables");
    if (!stock_ok())
        return 1;
    sh = mem_alloc(sizeof(*sh));
    if (!sh)
        return 2;
    mem_set((void *)sh, 0, sizeof(*sh));
    if (slot()[0] != 1)                     /* the slot is not where the image put it: refuse */
        return 3;
    slot()[1] = (u32)sh;                    /* before any hook can run */
    base_installed = 1;
    return 0;
}

/* The graphic editors' renderer hook, once: not needed for the profiles or the look themselves - without it only the drawing
 * is missing. The hook goes first, the item words (put_pointers) only with it in place */
static int editor_tried;

static void install_editor(void)
{
    static const u8 c3d_site[5] = {0x92, 0xfc, 0xdd, 0xa8, 0x80};     /* mov a0,a2; mov 0x60f580a8,a1 (the last two bytes
                                                                         are checked apart) */
    int rc;

    if (editor_tried)
        return;
    editor_tried = 1;
    log_checkpoint("profiles: Color Creator renderer hook at %x", C3D_SITE);
    rc = drv_edp_install();
    if (!rc)
        rc = *(volatile u8 *)(C3D_SITE + 5 + UNCACHED) == 0xf5 && *(volatile u8 *)(C3D_SITE + 6 + UNCACHED) == 0x60 ?
             drv_code_jump(C3D_SITE, c3d_site, (void *)drv_tramp_c3d) : 10;
    editor_ok = !rc;
    if (rc)
        log_printf(OMK_LOG_WARN, "profiles: no graphic editors: %d", rc);
}

/* The allocator setup hook, once: for the profiles and for the grain texture. 0, or drv_code_jump's code */
static int alloc_hooked;

static int install_alloc(void)
{
    static const u8 alloc_site[5] = {0x83, 0xfc, 0xcc, 0x91, 0xd1};   /* mov d0,d3; mov 53649,d0 */
    int rc;

    if (alloc_hooked)
        return 0;
    log_checkpoint("profiles: allocator hook at %x", ALLOC_SITE);
    rc = drv_code_jump(ALLOC_SITE, alloc_site, (void *)drv_tramp_alloc);
    alloc_hooked = !rc;
    return rc;
}

/* The grain texture (graindrv.c), core task at boot: the shared state, the flag, the hook; the mode follows at the next
 * picture mode rebuild (the firmware's own, or ours at the next UI write) */
int drv_alloc_large(int on)
{
    volatile shared_t *sh;
    int rc;

    rc = install_shared();
    if (rc)
        return rc;
    sh = shared();
    rc = install_alloc();
    if (rc)
        return 30 + rc;
    on = on != 0;
    if (sh->alloc_large != (u32)on) {
        sh->alloc_large = on;
        sh->recalc = 1;                     /* the GUI task rebuilds the picture mode at its next UI read of 111 or write */
    }
    return 0;
}

int drv_prof_install(int cache_inv)
{
    static const u8 site[5] = {0x26, 0x32, 0x73, 0xf1, 0x6e};   /* mov 29490,a2; add d3,a2 */
    static const u8 lvr_site[5] = {0xfc, 0xa0, 0x5c, 0xca, 0x6e};     /* mov (0xa26eca5c),a0 */
    static const u8 lva_site[5] = {0x5c, 0x08, 0x5d, 0x04, 0xfa};     /* mov (8,sp),a0; mov (4,sp),a1; movbu (129,a0),d0 */
    u32 i;
    int rc;

    fw_cache_inv = cache_inv;
    if (installed)
        return 0;
    rc = install_shared();
    if (rc)
        return rc;
    strip = mem_alloc((LIST_N + 2 * PROFILES) * ENTRY);
    if (!strip)
        return 2;
    /* the stock modes (entries 0..9), the six profiles, then the ART filters and the rest (entries 10..40) */
    mem_copy(strip, (void *)LIST_111, FIRST_ART * ENTRY);
    for (i = 0; i < PROFILES; i++) {
        mem_copy(strip + (FIRST_ART + i) * ENTRY, (void *)(PROFILE_ENTRY_COLOR + i * ENTRY), ENTRY);
        mem_copy(strip + (FIRST_ART + PROFILES + i) * ENTRY, (void *)(PROFILE_ENTRY_MONO + i * ENTRY), ENTRY);
    }
    mem_copy(strip + (FIRST_ART + 2 * PROFILES) * ENTRY, (void *)(LIST_111 + FIRST_ART * ENTRY), (LIST_N - FIRST_ART) * ENTRY);
    rc = install_alloc();
    if (rc)
        return 30 + rc;
    log_checkpoint("profiles: shooting hook at %x", TAKE_SITE);
    rc = drv_code_jump(TAKE_SITE, site, (void *)drv_tramp_take);
    if (rc)
        return 10 + rc;
    log_checkpoint("profiles: live view hooks at %x %x", LVR_SITE, LVA_SITE);
    rc = drv_code_jump(LVR_SITE, lvr_site, (void *)drv_tramp_lvr);
    if (rc)
        return 40 + rc;
    rc = drv_code_jump(LVA_SITE, lva_site, (void *)drv_tramp_lva);
    if (rc)
        return 50 + rc;
    install_editor();
    log_checkpoint("profiles: loading from outside");
    rc = drv_imp_install();                 /* not needed for the profiles themselves either */
    if (rc)
        log_printf(OMK_LOG_WARN, "profiles: no loading of profiles from outside: %d", rc);
    log_checkpoint("profiles: UI method and availability pointers, strip list");
    installed = 1;
    rc = put_pointers();
    if (rc) {
        installed = 0;
        return rc;
    }
    log_checkpoint("profiles: pointers in place: %x %x %x %x", *mem_word(UI_VT), *mem_word(UI_VT + 4), *mem_word(UI_VT + 8),
                   *mem_word(AVAIL_SLOT));
    return 0;
}

/* The firmware may unpack sections 6/7 again (sleep, language): put the pointers back if they are stock again.
 * 0 - ours, 1 - put back, -1 - someone else's values there (left alone) */
static u32 icons_service(void);
static u32 tables_service(void);

int drv_prof_service(void)
{
    if (!installed && !look_installed)
        return 0;
    if (icons_service())
        return 2;
    if (tables_service())
        return 3;
    if (*mem_word(UI_VT + 8) == (u32)drv_ui_read && (!installed || *word(LISTS + 8) == (u32)strip) &&
        *mem_word(AVAIL_SLOT) == (u32)drv_ui_avail)
        return 0;
    if (!stock_ok())
        return -1;
    return put_pointers() ? -1 : 1;
}

/* A profile chosen from outside (import.c: OM Workspace, the parse task, CPU 0): as the user's choice in the UI - the module
 * is told by the core loop, the picture mode is rebuilt at the next UI read */
void drv_prof_choose(u32 prof)
{
    volatile shared_t *sh = shared();

    if (sh)
        ui_changed(sh, prof);
}

void drv_prof_set(u32 prof)
{
    volatile shared_t *sh = shared();

    if (!sh)
        return;
    if (prof && sh->look) {
        sh->look = 0;
        look_item_picture(0);
    }
    if (prof != sh->prof && lv_drop(sh->prof, prof))
        lv()[LV_MODE] = 0;
    sh->prof = prof;
    sh->recalc = 1;                         /* rebuilt at the next UI read or write, in the GUI task */
}

u32 drv_prof_get(void)
{
    volatile shared_t *sh = shared();

    return sh ? sh->prof : 0;
}

int drv_prof_base(void)
{
    return drv_setting_read(ID_COLORTONE) == NATURAL_RAW;
}

u32 drv_prof_stored_mode(void)
{
    return drv_setting_read(ID_COLORTONE);
}

void drv_prof_stats(drv_prof_info *st)
{

    volatile shared_t *sh = shared();

    if (!sh) {
        mem_set(st, 0, sizeof(*st));
        return;
    }
    st->ui_seq = sh->ui_seq;
    st->ui_prof = sh->ui_prof;
    st->reads = sh->reads;
    st->writes = sh->writes;
    st->takes = sh->takes;
    st->applied = sh->applied;
    st->skip_mode = sh->skip_mode;
    st->skip_cam = sh->skip_cam;
    st->skip_bkt = sh->skip_bkt;
    st->other_cpu = sh->other_cpu;
    st->recalcs = sh->recalcs;
    st->alloc_sub = sh->alloc_sub;
    st->skip_alloc = sh->skip_alloc;
    st->skip_block = sh->skip_block;
    st->alloc_mode = *(volatile u16 *)ALLOC_MODE;
    st->lv_mode = lv()[LV_MODE];
    st->lv_reads = ((volatile u32 *)lv())[LV_READS / 4];
    st->lv_applied = ((volatile u32 *)lv())[LV_APPLIED / 4];
    drv_edp_stats(&st->ed_items, &st->ed_renders, &st->ed_cpu, &st->ed_drawn, &st->ed_failed);
}

/* Image module contexts: 5 slots of 988 bytes, shared by the module between CPUs at uncached addresses; the active one is
 * what live view reads every frame. Only read here, by the core task on CPU 0 -
 * the same uncached reads the firmware does */
#define LV_SLOT_LAST 0xa26e5766
#define LV_SLOT_ACTIVE 0xa26e5768
#define LV_SLOTS 0xa26e57b4
#define LV_SLOT_SIZE 988
#define LV_SLOT_COUNT 5

int drv_prof_lv_read(drv_lv_view *v)
{
    const volatile u8 *a;

    v->active = *(volatile u16 *)LV_SLOT_ACTIVE;
    v->last = *(volatile u16 *)LV_SLOT_LAST;
    if (v->active >= LV_SLOT_COUNT)
        return -1;
    a = (const volatile u8 *)(LV_SLOTS + LV_SLOT_SIZE * v->active);
    v->gen = a[892] | a[893] << 8 | a[894] << 16 | (u32)a[895] << 24;
    v->mode = a[260];
    v->mode2 = a[261];
    v->desc = a[954];
    v->filter = a[263];
    v->toning = a[264];
    v->cc = a[705];
    v->mc = a[730];
    v->level = a[731];
    v->shading = a[732];
    v->grain = a[733];
    v->sat = a[752];
    v->sharp = a[753];
    v->grad = a[754];
    v->contrast = a[756];
    v->s12[0] = a[706] | a[707] << 8;
    v->s12[1] = a[708] | a[709] << 8;
    v->tone[0] = a[792];
    v->tone[1] = a[793];
    v->tone[2] = a[794];
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------
 * Profile icons. The picture table of the UI: word 1 of record 0 of resource block 0x0101,
 * loaded at boot; 17006 pointers to 24-byte objects {14, w, h, 8, 0, size, data, 0}, data - the OSD engine's BITMAP command,
 * format 0x0B. Unused numbers point to one stub. Three pictures per profile mode:
 * strip, large, large edited. The objects live in the arena (uncached: the OSD engine reads memory); a table word is
 * changed only from the stub to ours, and put back by drv_prof_service if the firmware loads the table again */
#define IMG_RECORD0 0xa3091004              /* -> the table */
#define IMG_TABLE 0xa3435ccc
#define IMG_STUB 0xa3435cb8
#define IMG_COUNT 17006
#define IMG_COLOR 14527                     /* mode icons: + 3 (n - 1) + variant */
#define IMG_MONO 14515
#define IMG_LABEL_COLOR 14583               /* wide labels: + 2 (n - 1) + edited (other firmware's layout, +8 here) */
#define IMG_LABEL_MONO 14575
#define IMG_TYPE 14
#define BITMAP_0B 0x0b
/* Control panel (SCP) cell and shooting screen sign: one number per profile,
 * edited +4 (the firmware works the edited state out itself) */
#define IMG_PANEL_MONO 15746
#define IMG_PANEL_COLOR 15754
#define IMG_SHOOT_MONO 15706
#define IMG_SHOOT_COLOR 15714
#define PANEL_W 128                         /* as the stock Natural cell (10586) */
#define PANEL_H 39
#define SHOOT_W 80                          /* the stock pair Natural 56x44 -> 43x36: 90..102 -> 69..78 */
#define SHOOT_H 36
#define MODE_PICS (2 * PROFILES * OMK_PROF_ICON_VARIANTS)
#define PAIR_PICS (2 * PROFILES * 2)        /* per kind with two variants */
#define EDITOR_PICS OMK_EDITOR_PICTURES
#define ICONS (MODE_PICS + 3 * PAIR_PICS + EDITOR_PICS)
/* Editor pictures without a native analogue: Color Level, grain item and bar */
#define IMG_COLOR_LEVEL 14539
#define IMG_GRAIN 14567                     /* + 2 level: the item; + 2 level + 1: the bar */
#define IMG_LABEL_FILTER 4220               /* stubs next to the dial labels 4221 "Color", 4222 "Vivid" */
#define IMG_LABEL_LEVEL 4224
#define LABEL_W 124                         /* labels up to 154 wide, scaled by 0.8 as the stock pairs are: 154 -> 123 */
#define LABEL_H 39                          /* 48 -> 39, as the stock picture mode labels (70x48 -> 56x39) */

static u32 icon_obj[ICONS], icon_cap[ICONS];   /* our objects (uncached arena addresses) and their sizes; core task only */
static u8 icon_alias[ICONS];                /* 1 - the slot shows the plain variant's object (an "edited" one not set) */
static u32 icon_puts;

/* OSD plane palette indices, per meaning (OMK_ICON_*): the stock mode icons use 10, 7, 11, 12, the
 * wide labels 13, 7, 14, 15; both 8 and 9 around the outline */
static const u8 icon_palette[7] = {0, 10, 7, 11, 12, 8, 9};
static const u8 label_palette[7] = {0, 13, 7, 14, 15, 8, 9};

/* pic -> index in our tables, the firmware's picture number, the size; -1 not a picture we have */
static int pic_slot(u32 pic, u32 *i, u32 *number, u32 *w, u32 *h, const u8 **palette)
{
    u32 prof, n, mono, v;

    if ((pic & 0xf00) == 0x100) {
        prof = (pic >> 2) & 0x3f;
        v = pic & 3;
        n = OMK_PROF_NUM(prof);
        mono = OMK_PROF_IS_MONO(prof);
        if (v >= OMK_PROF_ICON_VARIANTS || n < 1 || n > PROFILES || (prof & ~0x1fu) != 0 || (prof & 0x0c))
            return -1;
        *i = (mono ? PROFILES : 0) * OMK_PROF_ICON_VARIANTS + (n - 1) * OMK_PROF_ICON_VARIANTS + v;
        *number = (mono ? IMG_MONO : IMG_COLOR) + 3 * (n - 1) + v;
        *w = v == OMK_PROF_ICON_NORMAL ? 66 : 80;
        *h = v == OMK_PROF_ICON_NORMAL ? 50 : 60;
        *palette = icon_palette;
        return 0;
    }
    if ((pic & 0xf00) == 0x500) {
        v = pic & 0xff;
        if (v >= EDITOR_PICS)
            return -1;
        *i = MODE_PICS + 3 * PAIR_PICS + v;
        *palette = icon_palette;
        if (v >= OMK_EDITOR_LABEL_FILTER) { /* the dial labels: the height of 4221/4222, the width of their widgets;
                                               "Color Filter" is taken but not shown since core 1.29 (Color/Level shows
                                               the camera's "Color") */
            *number = v == OMK_EDITOR_LABEL_FILTER ? IMG_LABEL_FILTER : IMG_LABEL_LEVEL;
            *w = v == OMK_EDITOR_LABEL_FILTER ? 116 : 56;
            *h = 24;
            return 0;
        }
        *number = v == OMK_EDITOR_COLOR_LEVEL ? IMG_COLOR_LEVEL : v < 5 ? IMG_GRAIN + 2 * (v - 1) : IMG_GRAIN + 2 * (v - 5) + 1;
        *w = v < 5 ? 66 : 80;
        *h = v < 5 ? 50 : 60;
        return 0;
    }
    if ((pic & 0xf00) >= 0x200 && (pic & 0xf00) <= 0x400) {
        u32 kind = ((pic & 0xf00) >> 8) - 2;    /* 0 menu label, 1 control panel, 2 shooting screen */

        prof = (pic >> 1) & 0x7f;
        v = pic & 1;
        n = OMK_PROF_NUM(prof);
        mono = OMK_PROF_IS_MONO(prof);
        if (n < 1 || n > PROFILES || (prof & ~0x1fu) != 0 || (prof & 0x0c) || (pic & 0x80))
            return -1;
        *i = MODE_PICS + kind * PAIR_PICS + (mono ? PROFILES : 0) * 2 + (n - 1) * 2 + v;
        if (kind == 0) {
            *number = (mono ? IMG_LABEL_MONO : IMG_LABEL_COLOR) + 2 * (n - 1) + v;
            *w = LABEL_W;
            *h = LABEL_H;
            *palette = label_palette;
        } else if (kind == 1) {
            *number = (mono ? IMG_PANEL_MONO : IMG_PANEL_COLOR) + (n - 1) + 4 * v;
            *w = PANEL_W;
            *h = PANEL_H;
            *palette = label_palette;
        } else {
            *number = (mono ? IMG_SHOOT_MONO : IMG_SHOOT_COLOR) + (n - 1) + 4 * v;
            *w = SHOOT_W;
            *h = SHOOT_H;
            *palette = icon_palette;
        }
        return 0;
    }
    return -1;
}

/* 1 - the module set this picture (core task and GUI task, CPU 0) */
int drv_prof_picture_set(u32 pic)
{
    u32 i, number, w, h;
    const u8 *pal;

    return !pic_slot(pic, &i, &number, &w, &h, &pal) && icon_obj[i] && !icon_alias[i];
}

int drv_prof_picture_size(u32 pic, u32 *w, u32 *h)
{
    u32 i, number;
    const u8 *pal;

    return pic_slot(pic, &i, &number, w, h, &pal);
}

static u32 icon_number[ICONS];              /* firmware numbers of the pictures we set */

static const u8 *cur_palette;               /* the palette of the picture being encoded */

static u32 pixel(const u8 *px, u32 w, u32 x, u32 y)
{
    u8 b = px[y * ((w + 1) / 2) + x / 2];
    u32 v = x & 1 ? b & 15 : b >> 4;

    return v < 7 ? cur_palette[v] : 0;
}

/* BITMAP 0x0B: 12-bit codes {palette index 8 bits, repeat - 1 4 bits}, five to a 64-bit word, a run does not cross a row.
 * out 0: count the codes only */
static u32 encode(const u8 *px, u32 w, u32 h, volatile u8 *out)
{
    u32 x, y, n = 0, c, run, k;
    u32 lo = 0, hi = 0;                     /* the 64-bit word being filled */

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x += run) {
            c = pixel(px, w, x, y);
            for (run = 1; x + run < w && run < 16 && pixel(px, w, x + run, y) == c; run++)
                ;
            if (out) {
                u32 code = c << 4 | (run - 1), bit = 12 * (n % 5);

                if (bit < 32) {
                    lo |= code << bit;
                    if (bit > 20)
                        hi |= code >> (32 - bit);
                } else {
                    hi |= code << (bit - 32);
                }
                if (n % 5 == 4) {
                    for (k = 0; k < 4; k++) {
                        out[4 + (n / 5) * 8 + k] = lo >> (8 * k);
                        out[4 + (n / 5) * 8 + 4 + k] = hi >> (8 * k);
                    }
                    lo = hi = 0;
                }
            }
            n++;
        }
    if (out && n % 5)
        for (k = 0; k < 4; k++) {
            out[4 + (n / 5) * 8 + k] = lo >> (8 * k);
            out[4 + (n / 5) * 8 + 4 + k] = hi >> (8 * k);
        }
    return n;
}

static void put32(volatile u8 *p, u32 v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static int table_ok(void)
{
    return *(volatile u32 *)IMG_RECORD0 == IMG_TABLE;
}

/* The 27 curve pictures of the profile editor (14540..14566) are the camera's own Highlight & Shadow pictures, the same
 * shapes in the same order: 14057 for the first, 10976.. for the rest. The table word is pointed
 * at the firmware's own object - nothing is drawn or kept by us */
#define IMG_CURVE 14540
#define CURVES 27

static u32 curve_native(u32 i)
{
    return i ? 10975 + i : 14057;
}

/* The curve item's pictures (the profiles' and the film look's editors). Count of words written */
static u32 put_curves(void)
{
    volatile u32 *t = (volatile u32 *)IMG_TABLE;
    u32 i, n = 0;

    if (!table_ok())
        return 0;
    for (i = 0; i < CURVES; i++)
        if (t[IMG_CURVE + i] == IMG_STUB && t[curve_native(i)] != IMG_STUB) {
            t[IMG_CURVE + i] = t[curve_native(i)];
            n++;
        }
    return n;
}

/* Our objects into the table where the stub still is. Count of words written */
static u32 put_icons(void)
{
    volatile u32 *t = (volatile u32 *)IMG_TABLE;
    u32 i, n;

    if (!table_ok())
        return 0;
    n = put_curves();
    for (i = 0; i < ICONS; i++)
        if (icon_obj[i] && t[icon_number[i]] == IMG_STUB) {
            t[icon_number[i]] = icon_obj[i];
            n++;
        }
    icon_puts += n;
    return n;
}

/* The camera shows no "*" for a changed picture mode (a design decision): an edited variant that the module does not set
 * shows the plain picture - the same object, nothing loaded or kept twice. Plain: the large mode icon (variant 1), and the
 * not-edited one of each pair */
static void edited_follows(u32 pic, u32 i)
{
    u32 e, number, w, h;
    const u8 *pal;

    if ((pic & 0xf00) == 0x100 && (pic & 3) == OMK_PROF_ICON_LARGE)
        e = pic + 1;
    else if ((pic & 0xf00) >= 0x200 && (pic & 0xf00) <= 0x400 && !(pic & 1))
        e = pic | 1;
    else
        return;
    if (pic_slot(e, &e, &number, &w, &h, &pal) || (icon_obj[e] && !icon_alias[e]))
        return;
    if (icon_obj[e] != icon_obj[i] && icon_obj[e])
        ((volatile u32 *)IMG_TABLE)[number] = IMG_STUB;   /* the table follows the new object at put_icons */
    icon_obj[e] = icon_obj[i];
    icon_cap[e] = 0;
    icon_number[e] = number;
    icon_alias[e] = 1;
}

static int build_object(u32 *objp, u32 *cap, const u8 *pal, u32 w, u32 h, const u8 *pixels);

int drv_prof_picture(u32 pic, u32 w, u32 h, const u8 *pixels)
{
    u32 i, number, ww, hh;
    const u8 *pal;

    if (pic_slot(pic, &i, &number, &ww, &hh, &pal) || !w || w > ww || h != hh || ((pic & 0xf00) == 0x100 && w != ww))
        return -1;
    if (!table_ok() || ((volatile u32 *)IMG_TABLE)[number] != (icon_obj[i] ? icon_obj[i] : IMG_STUB))
        return -3;                          /* not the table we know, or someone else's picture there */
    if (icon_alias[i]) {                    /* an edited variant of its own now, instead of the plain one */
        icon_alias[i] = 0;
        icon_obj[i] = 0;
        icon_cap[i] = 0;
    }
    icon_number[i] = number;
    ((volatile u32 *)IMG_TABLE)[number] = IMG_STUB;   /* the old picture is not drawn while being rewritten */
    if (build_object(&icon_obj[i], &icon_cap[i], pal, w, h, pixels))
        return -2;
    edited_follows(pic, i);
    put_icons();
    return 0;
}

/* A picture object in the arena from meaning values: *obj reused while *cap holds it, otherwise a new block (the old one is
 * not freed: the OSD engine may still be reading it). 0, or -2 no memory */
static int build_object(u32 *objp, u32 *cap, const u8 *pal, u32 w, u32 h, const u8 *pixels)
{
    u32 codes, size;
    volatile u8 *obj;

    cur_palette = pal;
    codes = encode(pixels, w, h, 0);
    size = 4 + (codes + 4) / 5 * 8;
    if (*cap < 24 + size) {
        obj = mem_alloc(24 + size);
        if (!obj)
            return -2;
        *objp = (u32)obj;
        *cap = 24 + size;
    }
    obj = (volatile u8 *)*objp;
    put32(obj + 4, 0);
    put32(obj + 8, 0);
    obj[0] = IMG_TYPE;
    obj[1] = 0;
    obj[2] = w;
    obj[3] = w >> 8;
    obj[4] = h;
    obj[5] = h >> 8;
    obj[6] = 8;
    obj[7] = 0;
    put32(obj + 12, size);
    put32(obj + 16, (u32)obj + 24);
    put32(obj + 20, 0);
    put32(obj + 24, ((h - 1) & 0xfff) | ((w - 1) & 0xfff) << 12 | BITMAP_0B << 24);
    encode(pixels, w, h, obj + 24);
    return 0;
}

/* drv_prof_service: icons back if the table was loaded again */
static u32 icons_service(void)
{
    return (installed ? put_icons() : look_installed ? put_curves() : 0) + look_icons_put();
}

/* ------------------------------------------------------------------------------------------------------------------
 * Editor column of the colour profiles without "Shading" (decision R9): the firmware does
 * not carry the shading of a colour profile further, the item would change nothing. The column
 * function 0x60e375d2 picks its item list by a jump table {mode + 1, label} at 0x6119cee0 (section 7 data, outside the protected
 * range); for the colour profiles 54..56 the label 0x60e376c3 sets list 0x6119a0f0 and 6 items {111, 5374 Color/Vivid,
 * 5375 curve, 5376 shading, 7, 8} - the count is in the code, so these three entries are sent to drv_column_color, which
 * sets our list of five and joins the firmware at 0x60e376d5. */
#define COLUMN_TABLE 0x6119cee0
#define COLUMN_COLOR_LABEL 0x60e376c3
static const u8 column_idx[3] = {25, 30, 31};   /* entries of modes 54, 55, 56 (keys 55, 56, 57) */
const u32 drv_color_column[5] = {111, 5374, 5375, 7, 8};

static int column_put(void)
{
    u32 k, n = 0;

    for (k = 0; k < sizeof(column_idx); k++) {
        u32 e = COLUMN_TABLE + 8 * column_idx[k];

        if (*word(e) != 55u + k)             /* the key: mode + 1 */
            return -1;
        if (*word(e + 4) == (u32)drv_column_color)
            continue;
        if (*word(e + 4) != COLUMN_COLOR_LABEL)
            return -1;                      /* not the table we know: leave it */
        put_word(e + 4, (u32)drv_column_color);
        n++;
    }
    return n;
}

/* An item's branch: ours, or put in place of the stock one. 0 - in place, 1 - put, -1 - not the table we know */
static int item_put(u32 key, u32 item, u32 stub, void (*ours)(void))
{
    if (*word(key) != item - 6)
        return -1;
    if (*word(key + 4) == (u32)ours)
        return 0;
    if (*word(key + 4) != stub)
        return -1;
    put_word(key + 4, (u32)ours);
    return 1;
}

/* The Color Creator item first: our drawing must never stay on for its ring. 0, 1 (put), -1 */
static int editor_put(void)
{
    int a = item_put(ITEM_CC_KEY, 5029, ITEM_CC_STUB, drv_item_cc), b, c, d;

    if (a < 0)
        return -1;
    b = item_put(ITEM_LEVEL_KEY, 5373, ITEM_LEVEL_STUB, drv_item_level);
    c = item_put(ITEM_VIVID_KEY, 5374, ITEM_VIVID_STUB, drv_item_vivid);
    d = item_put(ITEM_CURVE_KEY, 5375, ITEM_CURVE_STUB, drv_item_curve);
    if (b < 0 || c < 0 || d < 0)
        return -1;
    return a | b | c | d;
}

/* ------------------------------------------------------------------------------------------------------------------
 * Monochrome profile filter matrices (decision R13): the lookup 0x603e4f7f searches {code, handler} at 0x605756d0 (32 entries,
 * index code & 31, then on), code = 0x80 | level << 4 | filter, and jumps to the handler: mov matrix,a0; retf. A replaced
 * entry points at one of our handlers (thunks.s), which returns its matrix in drv_mf_shared through the uncached alias - read
 * right by any CPU. The table is in the protected range: words written uncached (put_word), CPU 0's cache pushed out after.
 * CPUs 1 and 2 may still hold the old line for a while: then the shot gets the camera's matrix */
#define MF_TABLE 0x605756d0
#define MF_SLOTS 8
/* 8 matrices of 9 words (144 bytes), padded to whole 64-byte lines: whatever the linker puts after it starts in a line
 * of its own, as with the other blocks of this section (at 152 bytes that depended on the next one's alignment) */
#define MF_WORDS 96
u16 drv_mf_shared[MF_WORDS] __attribute__((aligned(64), section(".data.cpu_shared")));
typedef char mf_shared_is_whole_lines[sizeof(drv_mf_shared) % 64 == 0 && MF_WORDS >= MF_SLOTS * 9 ? 1 : -1];
static void (*const mf_handler[MF_SLOTS])(void) = {drv_mf0, drv_mf1, drv_mf2, drv_mf3, drv_mf4, drv_mf5, drv_mf6, drv_mf7};
static u32 mf_code[MF_SLOTS], mf_stock[MF_SLOTS], mf_entry[MF_SLOTS];
static u32 mf_dirty;                        /* table words written, CPU 0's cached view not pushed out yet (core task) */

static int mf_find(u32 code)
{
    u32 i, k;

    for (k = 0; k < 32; k++) {
        i = (code + k) & 31;
        if (*mem_word(MF_TABLE + 8 * i) == code)
            return i;
        if (!*mem_word(MF_TABLE + 8 * i))
            return -1;
    }
    return -1;
}

/* The matrix address of a stock handler: "mov matrix,a0" (fc dc, the address little endian) then "retf" (de) - or, for BLUE
 * and CYAN, "bra" (ca) to the shared retf at 0x603e5097. 0 - not such a handler, or the matrix not in the firmware image */
static u32 mf_stock_matrix(u32 handler)
{
    const u8 *h = (const u8 *)handler;
    u32 a;

    if (handler < 0x60000200 || handler >= 0x60cbe800 || h[0] != 0xfc || h[1] != 0xdc ||
        (h[6] != 0xde && !(h[6] == 0xca && handler + 6 + (signed char)h[7] == 0x603e5097)))
        return 0;
    a = h[2] | h[3] << 8 | h[4] << 16 | (u32)h[5] << 24;
    return a >= 0x60000200 && a + 18 <= 0x60cbe800 ? a : 0;
}

/* The camera's own matrix: from the stock handler of the entry (the one kept aside while ours is in the table). Read only:
 * the matrices are constant firmware data (at 0x605522e2..0x605524c8 in 1.1), core task */
int drv_prof_mono_matrix_get(u32 filter, u32 level, s16 *matrix)
{
    u32 code = 0x80 | level << 4 | filter, slot, a, k;
    const u8 *m;
    int i;

    for (slot = 0; slot < MF_SLOTS && mf_code[slot] != code; slot++)
        ;
    if (slot < MF_SLOTS) {
        a = mf_stock_matrix(mf_stock[slot]);
    } else {
        i = mf_find(code);
        if (i < 0)
            return -3;
        a = mf_stock_matrix(*mem_word(MF_TABLE + 8 * i + 4));
    }
    if (!a)
        return -3;
    m = (const u8 *)a;
    for (k = 0; k < 9; k++)
        matrix[k] = (s16)(m[2 * k] | m[2 * k + 1] << 8);
    return 0;
}

int drv_prof_mono_matrix(u32 filter, u32 level, const s16 *matrix)
{
    u32 code = 0x80 | level << 4 | filter, slot, k, handler;
    volatile u16 *m = (volatile u16 *)((u32)drv_mf_shared + UNCACHED);
    int i;

    for (slot = 0; slot < MF_SLOTS && mf_code[slot] != code; slot++)
        ;
    if (slot == MF_SLOTS) {
        if (!matrix)
            return 0;                       /* not replaced: nothing to put back */
        for (slot = 0; slot < MF_SLOTS && mf_code[slot]; slot++)
            ;
        if (slot == MF_SLOTS)
            return -1;
    }
    i = mf_find(code);
    if (i < 0)
        return -3;
    handler = *mem_word(MF_TABLE + 8 * i + 4);
    if (!mf_code[slot]) {                   /* the stock handler must be the firmware's (mf_stock_matrix) */
        if (!mf_stock_matrix(handler))
            return -3;
        mf_stock[slot] = handler;
        mf_entry[slot] = MF_TABLE + 8 * i + 4;
    }
    if (!matrix) {                          /* back to the camera's own */
        put_word(mf_entry[slot], mf_stock[slot]);
        mf_code[slot] = 0;
        mf_dirty = 0;                       /* this push covers the ones owed */
        evict_cache();
        return 0;
    }
    for (k = 0; k < 9; k++)
        m[slot * 9 + k] = (u16)matrix[k];
    mf_code[slot] = code;
    if (put_word(mf_entry[slot], (u32)mf_handler[slot]))
        return -3;
    /* CPU 0's cache may still hold the entry's old word. Pushing it out reads 256 KB (evict_cache), and a module sets
     * its matrices in a row (eight of them): the push is owed and made once, by drv_prof_flush - core calls it at
     * every loop, so the first loop after the modules' start makes it. Until then a shot on CPU 0 may get the camera's
     * own matrix for the entry, as one on CPUs 1 and 2 may at any time (above). Giving an entry back (matrix 0) pushes
     * at once: its slot of drv_mf_shared may be taken by another filter right after */
    mf_dirty = 1;
    return 0;
}

/* Core task: the cache push owed by drv_prof_mono_matrix, if any */
void drv_prof_flush(void)
{
    if (!mf_dirty)
        return;
    mf_dirty = 0;
    evict_cache();
}

/* drv_prof_service: the column and the filter entries back if the firmware unpacked its data again */
static u32 tables_service(void)
{
    u32 slot, n = installed ? shading_rects() : 0;

    if (column_set && column_put() > 0)
        n++;
    if (look_installed && look_column_put() > 0)
        n++;
    if (editor_set && editor_put() > 0)
        n++;
    for (slot = 0; slot < MF_SLOTS; slot++)
        if (mf_code[slot] && *mem_word(mf_entry[slot]) == mf_stock[slot]) {
            put_word(mf_entry[slot], (u32)mf_handler[slot]);
            n++;
        }
    if (n)
        evict_cache();
    return n;
}

/* ------------------------------------------------------------------------------------------------------------------
 * The shading item of the monochrome column (icon and value) kept the sample camera's places: rows 74 px apart instead of 60,
 * so it was drawn one row lower, over the sharpness. Section 7 data (outside the protected
 * range), read by the GUI on CPU 0: five icon rectangles {x, y, w, h} twice at 0x610c4076 + 16k and five value rectangles at
 * 0x610c3e96 + 16k get the rows of this camera; only while they hold the sample's values */
#define SHADE_ICON 0x610c4076
#define SHADE_VALUE 0x610c3e96

static int rect_is(u32 a, u32 x, u32 y, u32 w, u32 h)
{
    volatile u16 *r = (volatile u16 *)a;
    u32 j;

    for (j = 0; j < 8; j += 4)
        if (r[j] != x || r[j + 1] != y || r[j + 2] != w || r[j + 3] != h)
            return 0;
    return 1;
}

static void rect_set(u32 a, u32 x, u32 y, u32 w, u32 h)
{
    volatile u16 *r = (volatile u16 *)a;
    u32 j;

    for (j = 0; j < 8; j += 4) {
        r[j] = x;
        r[j + 1] = y;
        r[j + 2] = w;
        r[j + 3] = h;
    }
}

static u32 shading_rects(void)
{
    u32 k, n = 0;

    for (k = 0; k < 5; k++) {
        if (rect_is(SHADE_ICON + 16 * k, 12, 92 + 74 * k, 46, 46)) {
            rect_set(SHADE_ICON + 16 * k, 4, 67 + 60 * k, 46, 46);
            n++;
        }
        if (rect_is(SHADE_VALUE + 16 * k, 60, 106 + 74 * k, 29, 30)) {
            rect_set(SHADE_VALUE + 16 * k, 52, 78 + 60 * k, 24, 24);
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------------------------------------------------------
 * The editor column's number, slider and value pictures read the effective settings (7 sharpness, 8 contrast, the tone
 * 383/691/382, 1016 shading, 998 grain, 131 toning, 1011 level), which the firmware fills from the stored mode - Natural.
 * While a profile is active, reads of these by the picture mode screen's code (the return address
 * of the read in 0x60e37000..0x60e4b000: the column 0x60e375d2, its items, the slider 0x60e4a94a) read the profile's cell
 * instead. Only there: the firmware's own picture mode rebuild and other screens keep the effective values */
#define EDITOR_CODE_LO 0x60e37000
#define EDITOR_CODE_HI 0x60e4b000

/* ------------------------------------------------------------------------------------------------------------------
 * The camera's own Highlight & Shadow screen edits the active profile's tone (decision R7): the direct
 * setting screen of the shooting group (GUI state 02 02) with setting 382 in the
 * screen's slot, opened from the SCP, the H&S button or a long press. There its reads and writes of 383/691/382 (Hi, Mid,
 * Sh) go to the profile's tone cells; in a stock mode, and on any other screen, nothing changes. GUI task, CPU 0 */
/* The GUI object 0x6234f838 at its uncached alias, here and wherever the driver reads it (editor.c, the power probe of
 * drv.c): the readers include hooks that may run on any CPU, and a read past the cache never brings a line of firmware
 * data into a cache that is not CPU 0's. The firmware writes it through CPU 0's cache, so a value may be a moment old
 * when a screen opens or closes; on the camera the screens were told apart this way without a miss */
#define GUI_STATE 0xa234f838                /* uncached: byte 0 group, byte 1 state */
#define SETVAL_ID 0x61e589c8                /* the setting of the direct setting screen (0x60ef6451), written by the GUI */
#define ID_HS_SH 382

static int hs_screen(void)
{
    const volatile u8 *g = (const volatile u8 *)GUI_STATE;

    return g[0] == 0x02 && g[1] == 0x02 && *(volatile u32 *)SETVAL_ID == ID_HS_SH;
}

/* The Super Control Panel (GUI state 04 03): its "Exposure Comp. / H&S" cell draws an H&S symbol from 383/691/382 (a
 * straight line at 0, a curve otherwise - emulator). With a profile it shows the profile's tone, as
 * the H&S screen opened from it does. Reads only: the panel does not write the tone */
static int scp_screen(void)
{
    const volatile u8 *g = (const volatile u8 *)GUI_STATE;

    return g[0] == 0x04 && g[1] == 0x03;
}

/* 383/691/382 -> the tone cell of profile p; 0 - not a tone id */
static u32 tone_cell_of(u32 id, u32 p)
{
    u32 base = (OMK_PROF_IS_MONO(p) ? CELL_M_TONE : CELL_C_TONE) + 3 * (OMK_PROF_NUM(p) - 1);

    switch (id) {
    case 383:
        return base;
    case 691:
        return base + 1;
    case 382:
        return base + 2;
    }
    return 0;
}

/* The writes (thunks.s, methods +0 and +4, every kind - value, step, reset): the id the firmware is to write */
u32 drv_ui_write_id(u32 id)
{
    volatile shared_t *sh = shared();
    u32 c;

    if (!sh || !sh->prof || !(c = tone_cell_of(id & 0xffff, sh->prof)) || !hs_screen())
        return id;
    return (id & 0xffff0000u) | c;
}

u32 drv_ui_read_id(u32 id, u32 ret)
{
    volatile shared_t *sh;
    u32 p, k, mono, c;

    if (ret < EDITOR_CODE_LO || ret >= EDITOR_CODE_HI) {
        sh = shared();
        if (sh && (p = sh->prof) && (c = tone_cell_of(id & 0xffff, p)) && (hs_screen() || scp_screen()))
            return (id & 0xffff0000u) | c;  /* the H&S screen (its graph reads from 0x60f01fb3..0x60f01fdb), the SCP */
        return id;
    }
    sh = shared();
    if (!sh || !(p = sh->prof))
        return id;
    mono = OMK_PROF_IS_MONO(p);
    k = OMK_PROF_NUM(p) - 1;
    switch (id & 0xffff) {
    case 7:
        return (mono ? CELL_M_SHARP : CELL_C_SHARP) + k;
    case 8:
        return (mono ? CELL_M_CONTRAST : CELL_C_CONTRAST) + k;
    case 383:
        return (mono ? CELL_M_TONE : CELL_C_TONE) + 3 * k;
    case 691:
        return (mono ? CELL_M_TONE : CELL_C_TONE) + 3 * k + 1;
    case 382:
        return (mono ? CELL_M_TONE : CELL_C_TONE) + 3 * k + 2;
    case 1016:
        return mono ? CELL_M_SHADING + k : id;
    case 998:
        return mono ? CELL_M_GRAIN + k : id;
    case 131:
        return mono ? CELL_M_TONING + k : id;
    case 1011:
        return mono ? CELL_M_LEVEL + k : id;
    }
    return id;
}

/* ------------------------------------------------------------------------------------------------------------------
 * The film look (core/look.c). A stock picture mode of the strip - the slot - shows the look:
 *  - the UI: while the look is active the read of 111 gives the slot's mode over the stored Natural, a write of the slot's
 *    mode turns the look on and stores Natural (drv_ui_read_fix, drv_ui_write_fix);
 *  - the editor column of the slot: the column function's jump table {mode + 1, label} (hashed by the key & 31, linear
 *    probing - 0x60e37643..0x60e3766f) gets the Color Creator's label 0x60e3769d, so the column is [111, 5029] - the
 *    native Color Creator item, whose frame orders the Color Creator drawing (editor.c draws the look instead);
 *  - the two dial cells of that item, 615 (hue 0..29, the front dial) and 616 (vividness, the rear dial), are virtual while
 *    the look is active: reads give what the UI was last given, writes are swallowed (drv_ui_swallow, before the firmware's
 *    write method) and step the chosen entry at once, in the GUI task, before the step's drawing is ordered;
 *  - pictures: the slot's own pictures (strip, large, menu label, SCP cell, shooting screen sign) are replaced by the
 *    module's - the table word goes from the firmware's object to ours; the item's "Color Vivid" picture (11397, shared with
 *    the colour profiles' Color/Vivid) only while the look is active.
 * The slots are the stock modes whose pictures and column labels were read from this firmware (static analysis): not
 * Natural (the base), Color Creator (its item serves the look), Custom (no column entry), e-Portrait (its SCP picture is
 * not known) and the ART filters */
#define LOOK_NAME 32
#define LOOK_MAX 64
#define COLUMN_CC_KEY 22                    /* Color Creator (21) + 1 */
#define COLUMN_CC_LABEL 0x60e3769d          /* its list [111, 5029] at 0x6119a0ac */
#define IMG_CC_ITEM 11397                   /* "Color Vivid": the picture of the items 5029 and 5374 */
#define LOOK_PIC_ITEM OMK_LOOK_PIC_ITEM
#define IMG_LOOK_PREV 4234                  /* stub numbers next to the dial labels 4221 "Color", 4222 "Vivid" */
#define IMG_LOOK_NEXT 4235
#define IMG_LOOK_LEVEL 4238                 /* a stub number for the module's Color/Level item picture (item 5374 of a
                                               black-and-white entry takes it through IMG_ED_VIVID) */
#define IMG_LOOK_GRAIN 4239                 /* stub numbers (free next to the dial labels, 4237..4253): the grain item's */
#define IMG_LOOK_TYPE 4240                  /* picture (item 5374 takes it through IMG_ED_VIVID) and its dial labels */
#define IMG_LOOK_STRENGTH 4241
#define MODE_CC 21                          /* Color Creator, the UI's picture mode */
#define ID_ITEM_NOW 2148                    /* the picture mode screen's item under the cursor: 111 the strip, 5029 the item */
#define ITEM_CC_ID 5029

typedef struct {
    char name[12];
    u32 mode;                               /* the UI's picture mode */
    u32 label;                              /* the column function's label of its own list */
    u16 pic[LOOK_PIC_ITEM];                 /* strip, large, menu label, SCP cell, shooting screen sign */
    u16 text;                               /* the mode's name: a string of the UI (list {mode, string} 0x611b8d1c) */
} look_slot_t;

static const look_slot_t look_slots[] = {
    {"ienhance", 7, 0x60e37671, {6408, 6678, 6596, 10584, 11226}, 145},
    {"vivid", 0, 0x60e37679, {6409, 6679, 6595, 10585, 11227}, 1657},
    {"muted", 2, 0x60e37679, {6411, 6681, 6592, 10587, 11229}, 183},
    {"portrait", 5, 0x60e37679, {6412, 6682, 6591, 10588, 11230}, 1661},
    {"monotone", 3, 0x60e37683, {566, 548, 1505, 10551, 11193}, 176},
    {"underwater", 28, 0x60e37693, {6413, 6570, 6569, 10547, 11189}, 2869},
};

/* The UI's strings: one language at a time is loaded into one place (records 1..18 of resource block 0x0101, all with
 * the base 0xa407f540): +0 the address of its array of string pointers, +4 the end of the array; the strings are UTF-16LE
 * with a zero. A language change loads another record there: the core loop puts our pointer back */
#define STR_RECORD 0xa407f540
#define STR_COUNT 4909
#define STR_SPAN 0x80000                    /* the record's size, at most (the largest is about 0x56000) */
#define LOOK_TEXT 24
#define LOOK_SLOTS (sizeof(look_slots) / sizeof(look_slots[0]))

/* Picture sizes (the largest width; the strip, large and item pictures exactly as wide) */
/* the dial labels (PREV, NEXT, TYPE, STRENGTH) at most 56 wide: the stock widgets, which show every label at one place */
static const u8 look_w[OMK_LOOK_PICTURES] = {66, 80, LABEL_W, PANEL_W, SHOOT_W, 66, 56, 56, 66, 66, 56, 56};
static const u8 look_h[OMK_LOOK_PICTURES] = {50, 60, LABEL_H, PANEL_H, SHOOT_H, 50, 24, 24, 50, 50, 24, 24};

static const look_slot_t *look_at;
static u32 look_obj[OMK_LOOK_PICTURES], look_cap[OMK_LOOK_PICTURES], look_native[OMK_LOOK_PICTURES];
static u16 *look_text;                      /* the slot's name, UTF-16LE in the arena; 0 - the firmware's own */

/* The slot's name into the loaded language's string table: 1 put, 0 in place or none, -1 not the table we know */
static int look_text_put(void)
{
    volatile u32 *rec = (volatile u32 *)STR_RECORD, *e;
    u32 list = rec[0], end = rec[1];

    if (!look_at || !look_text)
        return 0;
    if (end - list != 4 * STR_COUNT || list < STR_RECORD || end > STR_RECORD + STR_SPAN)
        return -1;
    e = (volatile u32 *)(list + 4 * look_at->text);
    if (*e == (u32)look_text)
        return 0;
    if (*e < STR_RECORD || *e >= STR_RECORD + STR_SPAN)
        return -1;                          /* not a string of the record: someone else's */
    *e = (u32)look_text;
    return 1;
}

/* The slot's name, printable ASCII (up to LOOK_TEXT - 1 characters). 0, -1 bad, -2 no memory, -3 the table */
int drv_look_name(const char *t)
{
    u32 n;

    if (!look_at || !t || !t[0])
        return -1;
    if (!look_text) {
        look_text = mem_alloc(2 * LOOK_TEXT);
        if (!look_text)
            return -2;
    }
    for (n = 0; t[n] && n < LOOK_TEXT - 1; n++)
        look_text[n] = t[n] >= 32 && t[n] < 127 ? (u8)t[n] : '?';
    look_text[n] = 0;
    return look_text_put() < 0 ? -3 : 0;
}

void drv_edp_look(u32 names, u32 index, u32 count);   /* editor.c */
void drv_edp_look_arts(u32 table);
static u32 *look_arts;                      /* arena: the entries' cassette drawings, LOOK_MAX addresses (0 - none) */

const char *drv_look_slots(void)
{
    return "ienhance, vivid, muted, portrait, monotone, underwater";
}

static u32 look_number(u32 k)
{
    return k == LOOK_PIC_ITEM ? IMG_CC_ITEM : k == OMK_LOOK_PIC_PREV ? IMG_LOOK_PREV : k == OMK_LOOK_PIC_NEXT ? IMG_LOOK_NEXT :
           k == OMK_LOOK_PIC_LEVEL ? IMG_LOOK_LEVEL : k == OMK_LOOK_PIC_GRAIN ? IMG_LOOK_GRAIN :
           k == OMK_LOOK_PIC_TYPE ? IMG_LOOK_TYPE : k == OMK_LOOK_PIC_STRENGTH ? IMG_LOOK_STRENGTH :
           look_at->pic[k];
}

/* The column function's entry of key (mode + 1): its index, -1 none */
static int column_find(u32 key)
{
    u32 i = key & 31, n, k;

    for (n = 0; n < 32; n++, i = (i + 1) & 31) {
        k = *word(COLUMN_TABLE + 8 * i);
        if (k == key)
            return (int)i;
        if (!k)
            return -1;
    }
    return -1;
}

void drv_column_look(void);                 /* thunks.s: the look's column, by drv_look_column */

/* The slot's column is the look's (the Color Creator's item and the editors): 0 in place, 1 put, -1 not the table we
 * know. The Color Creator's own entry must still be the stock one: its item is the look's */
static int look_column_put(void)
{
    int e = column_find(look_at->mode + 1), c = column_find(COLUMN_CC_KEY);
    u32 a;

    if (e < 0 || c < 0 || *word(COLUMN_TABLE + 8 * (u32)c + 4) != COLUMN_CC_LABEL)
        return -1;
    a = COLUMN_TABLE + 8 * (u32)e + 4;
    if (*word(a) == (u32)drv_column_look)
        return 0;
    if (*word(a) != look_at->label)
        return -1;
    put_word(a, (u32)drv_column_look);
    return 1;
}

/* The item's picture: ours while the look is active, the firmware's otherwise. CPU 0 only (GUI and core tasks); a call on
 * another CPU leaves it to the core loop */
static void look_item_picture(u32 on)
{
    volatile u32 *t = (volatile u32 *)IMG_TABLE;
    u32 obj = look_obj[LOOK_PIC_ITEM], native = look_native[LOOK_PIC_ITEM], want, now;

    if (drv_cpu() || !look_at || !obj || !native || !table_ok())
        return;
    want = on ? obj : native;
    now = t[IMG_CC_ITEM];
    if (now != want && (now == obj || now == native))
        t[IMG_CC_ITEM] = want;
    ed_item_picture();
}

/* Our pictures where the firmware's still are. Words written */
static u32 look_icons_put(void)
{
    volatile u32 *t = (volatile u32 *)IMG_TABLE;
    volatile shared_t *sh = shared();
    u32 k, n = 0, num, before;

    if (!look_at || !table_ok())
        return 0;
    for (k = 0; k < OMK_LOOK_PICTURES; k++) {
        num = look_number(k);
        if (k != LOOK_PIC_ITEM && look_obj[k] && t[num] == look_native[k]) {
            t[num] = look_obj[k];
            n++;
        }
    }
    before = t[IMG_CC_ITEM];
    look_item_picture(sh && sh->look);      /* and item 5374's picture */
    if (look_text_put() > 0)
        n++;
    return n + (t[IMG_CC_ITEM] != before);
}

/* The grain item's dial labels "Type" and "Strength" are in place (editor.c shows them over item 5373 of the look) */
int drv_look_grain_labels(void)
{
    volatile u32 *t = (volatile u32 *)IMG_TABLE;

    return look_at && table_ok() && look_obj[OMK_LOOK_PIC_TYPE] && look_obj[OMK_LOOK_PIC_STRENGTH] &&
           t[IMG_LOOK_TYPE] == look_obj[OMK_LOOK_PIC_TYPE] && t[IMG_LOOK_STRENGTH] == look_obj[OMK_LOOK_PIC_STRENGTH];
}

/* The dial labels "Prev" and "Next" are in place (editor.c shows them over the look's item) */
int drv_look_labels(void)
{
    volatile u32 *t = (volatile u32 *)IMG_TABLE;

    return look_at && table_ok() && look_obj[OMK_LOOK_PIC_PREV] && look_obj[OMK_LOOK_PIC_NEXT] &&
           t[IMG_LOOK_PREV] == look_obj[OMK_LOOK_PIC_PREV] && t[IMG_LOOK_NEXT] == look_obj[OMK_LOOK_PIC_NEXT];
}

int drv_look_picture_size(u32 pic, u32 *w, u32 *h)
{
    if (pic >= OMK_LOOK_PICTURES)
        return -1;
    *w = look_w[pic];
    *h = look_h[pic];
    return 0;
}

/* 0; -1 bad size or no look service; -2 no memory; -3 the table is not as expected */
int drv_look_picture(u32 pic, u32 w, u32 h, const u8 *pixels)
{
    volatile u32 *t = (volatile u32 *)IMG_TABLE;
    u32 num, now, exact = pic == OMK_LOOK_PIC_MODE || pic == OMK_LOOK_PIC_LARGE || pic == LOOK_PIC_ITEM ||
                          pic == OMK_LOOK_PIC_LEVEL || pic == OMK_LOOK_PIC_GRAIN;
    u32 stub = pic == OMK_LOOK_PIC_PREV || pic == OMK_LOOK_PIC_NEXT || pic == OMK_LOOK_PIC_LEVEL || pic >= OMK_LOOK_PIC_GRAIN;

    if (!look_at || pic >= OMK_LOOK_PICTURES || !w || w > look_w[pic] || h != look_h[pic] || (exact && w != look_w[pic]))
        return -1;
    if (!table_ok())
        return -3;
    num = look_number(pic);
    now = t[num];
    if (!look_native[pic]) {                /* the dial labels and Color/Level take stub numbers; the rest replace the
                                               firmware's own */
        if (!now || (now == IMG_STUB) != stub)
            return -3;
        look_native[pic] = now;
    }
    if (now != look_native[pic] && now != look_obj[pic])
        return -3;                          /* someone else's picture there */
    if (look_obj[pic])
        t[num] = look_native[pic];          /* ours is not drawn while being rewritten */
    if (build_object(&look_obj[pic], &look_cap[pic],
                     pic == OMK_LOOK_PIC_LABEL || pic == OMK_LOOK_PIC_PANEL ? label_palette : icon_palette, w, h, pixels))
        return -2;
    look_icons_put();
    return 0;
}

/* The hue the UI is given for the front dial: half a turn from the stored one, so the two never look alike (drv_ui_swallow) */
static u32 hue_base(void)
{
    u32 v;

    if (drv_cpu())                          /* the settings getter is called on CPU 0 only (see still_variant_now); the
                                               UI's tasks are there - elsewhere the value below for "no stored hue" */
        return 15;
    v = drv_setting_read(ID_CC_HUE);

    return v < 30 ? (v + 15) % 30 : 15;
}

/* The look on or off by the UI (GUI task) or by core (drv_look_set) */
static int look_changed_ui(volatile shared_t *sh, u32 on)
{
    if (on && !sh->look)
        sh->cc_last[0] = hue_base();
    sh->look = on;
    sh->look_ui = on;
    sh->look_seq++;
    look_item_picture(on);
    return 0;
}

/* A dial step on the look's item: the next or the previous entry, published for the drawing at once */
static void look_step(volatile shared_t *sh, s32 d)
{
    u32 st = drv_lock(), n = sh->look_count;

    if (n) {
        sh->look_index = (sh->look_index + (d > 0 ? 1 : n - 1)) % n;
        sh->look_pick++;
        sh->ed.stale = 1;                   /* the editors' values are the entry's before until the module's next */
        sh->ed.g_stale = 1;
        drv_edp_look(sh->look_names, sh->look_index, n);
    }
    drv_unlock(st);
}

/* A small step around the hue's circle (0..29) from -> to: -3..3, 0 for anything larger */
static s32 hue_step(u32 to, u32 from)
{
    s32 d = ((s32)to - (s32)from) % 30;

    if (d > 15)
        d -= 30;
    else if (d < -15)
        d += 30;
    return d >= -3 && d <= 3 ? d : 0;
}

static int look_ed_write(volatile shared_t *sh, u32 id, u32 kind, u32 value);

/* Before the UI write methods (thunks.s), every kind: 1 - a write of the look's dial cells, not to be done (the method
 * reports success). kind 0 carries the value (emulator: a front dial step on the Color Creator item writes 615 = hue +-1,
 * 0 -> 29 wrapping); the step kinds 1 and 2 are taken as forward and back (hypothesis: as the SCP's dial on 111).
 * Both dials alike: the value up - the next preset, down - the previous (on the camera: right forward, left back on
 * both; in the emulator sub -1 and main +1 - the next, the way sub -1 moves the strip right) */
u32 drv_ui_swallow(u32 id, u32 kind, u32 value)
{
    volatile shared_t *sh = shared();
    u32 k;
    s32 d;

    if (!sh || !sh->look)
        return 0;
    id &= 0xffff;
    if (drv_cpu()) {                        /* never seen: the UI's tasks run on CPU 0. The rest of this function is
                                               CPU 0's - drv_lock, the settings getter, the editors' state - so here
                                               only the rule that needs none of it: the virtual dial cells are not
                                               written; any other write goes to the firmware as it is */
        sh->other_cpu++;
        return id == ID_CC_HUE || id == ID_CC_VIVID;
    }
    if (look_ed_write(sh, id, kind, value))
        return 1;
    if (id == ID_COLORTONE) {               /* the Color Creator item writes its mode with every dial step (emulator): on
                                               the look's item that is not a choice of Color Creator */
        if (value == MODE_CC && drv_setting_read(ID_ITEM_NOW) == ITEM_CC_ID) {
            sh->swallowed++;
            sh->swallow_kind = kind << 16 | id;
            return 1;
        }
        return 0;
    }
    if (id != ID_CC_HUE && id != ID_CC_VIVID)
        return 0;
    k = id == ID_CC_VIVID;
    sh->swallowed++;
    sh->swallow_kind = kind << 16 | id;
    sh->dial_last = kind << 24 | (u32)k << 16 | (value & 0xffff);
    if (kind == 0) {
        if (k) {
            /* The vividness: the UI keeps its own position, held at its ends (camera trace 2026-11-10: 1, 2, 3, 3, 3 to
             * the right, 2, 1, 0, -1 ... -4, -4 to the left), synced from our reads - the last value written. A step is
             * the change from the last write; the same value again away from 0 is a push against an end: a step that
             * way. (The sign of the value alone, until then: two steps the wrong way and one lost at every turn back) */
            d = (s32)(s16)value - (s32)(s16)sh->cc_last[1];
            if (!d && value)
                d = (s16)value > 0 ? 1 : -1;
            sh->cc_last[1] = value;
        } else {
            /* The hue: the UI keeps its own position (emulator: 16, 17, 16 with the reads at 15), synced from our reads -
             * the last value written - and writes it +-n, wrapping 0..29. Camera (2026-11-10): the front dial at times
             * stepped the wrong way - the position seems to come back now and then from the stored hue (it never
             * changes: the writes are swallowed). So a write near the last one is a step from it, one near the stored
             * hue a step from that; anything else (a long OK's reset, a jump) is not a step */
            d = hue_step(value, sh->cc_last[0]);
            if (!d && value != sh->cc_last[0]) {
                d = hue_step(value, drv_setting_read(ID_CC_HUE));
                sh->dial_last |= d ? 1u << 20 : 1u << 21;      /* for the log: a step from the stored hue, or none */
            }
            sh->cc_last[0] = value;
        }
    } else {
        d = kind == 1 ? 1 : kind == 2 ? -1 : 0;
    }
    if (d)
        look_step(sh, d);
    k = sh->dial_head % DIAL_TRACE;
    sh->dial_t[k] = drv_ticks();
    sh->dial_w[k] = (kind & 15) << 28 | (id == ID_CC_VIVID) << 24 | ((u32)d & 0xff) << 16 | (value & 0xffff);
    sh->dial_head++;
    return 1;
}

/* The dial trace: up to max entries from *from on into t[], w[] (older than DIAL_TRACE back are lost: *lost); *from moves
 * past them. Returns how many */
u32 drv_look_trace(u32 *from, u32 *lost, u32 *t, u32 *w, u32 max)
{
    volatile shared_t *sh = shared();
    u32 head, i, n = 0;

    *lost = 0;
    if (!sh)
        return 0;
    head = sh->dial_head;
    if (head - *from > DIAL_TRACE) {
        *lost = head - DIAL_TRACE - *from;
        *from = head - DIAL_TRACE;
    }
    for (i = *from; i != head && n < max; i++, n++) {
        t[n] = sh->dial_t[i % DIAL_TRACE];
        w[n] = sh->dial_w[i % DIAL_TRACE];
    }
    *from = i;
    return n;
}

/* ------------------------------------------------------------------------------------------------------------------
 * The look's editors (API 16): the tone curve, Color/Vivid of a colour preset, Color/Level of a black-and-white one.
 *
 * The column. The column function builds its list only when the picture mode screen opens and at a step of the strip
 * (emulator: not at a dial step of an item, not when the cursor moves), so a list chosen by the preset could not follow a step
 * from a colour preset to a black-and-white one. The look's column is [111, 5029, 5375, 5374] while the module shows its
 * editors, and item 5374 is Color/Vivid or Color/Level by the preset (its picture follows); 5375 is the tone curve. Until
 * the module first publishes its editors the column has the curve (enable_edits is on by default; FILMLOOK 1.4 shows only
 * the curve, 5374 comes only with a module that publishes it): it then draws nothing of its own and changes nothing; the
 * module's "none" (enable_edits = off) takes it out at the next list.
 *
 * The cells. The native item code works on the cells of a profile picked by the UI's picture mode; with the slot's mode
 * (7) its lookups find none and a dial step writes setting 0 (emulator). While the look is active the getters of the items'
 * ids (both read 111) are given colour profile 1 (54), and so is the Color/Vivid handler's key 901: the native code then
 * reads and writes the cells of colour profile 1 - 2197 (one hue / all, INFO), 2201 (the cursor: cell 928 + k, 0 the
 * "all" place, 13 places), 928..939 (the saturations), 976..978 (Hi, Mid, Sh) and 2189 (INFO chose Mid). On the look's
 * editor items every UI read of these cells is answered from the shadow, and every write of a profile cell (904..1033,
 * 2189..2204, the item ids 5413..5428, setting 0) is swallowed: the stored cells stay as they are. The native code keeps
 * the values it wrote and steps from them (emulator), so the shadow is what it wrote last. A write:
 *  - 2201 (Color/Vivid): the cursor's place; place k is the k-th sector by angle (the drawing's order), "all" between the
 *    last and the first; a sector the preset does not have is passed by the same way. Color/Level: the filter's next place
 *    on the ring, by the step;
 *  - 2197, 2189: INFO - all hues or one, Mid or Hi and Sh; all hues: every sector moves, each from its own value;
 *  - the saturation and tone cells go into the shadow; the native code's action ends with the item's redraw, which turns
 *    them into events (ed_commit): values of the shadow, the reset as a whole (all back at 0) */
#define ITEM_ED_VIVID 5374
#define ITEM_ED_CURVE 5375
#define G_LEVEL_MAX 3                       /* the grain's strengths 0..3 */
#define G_HOLD_US ED_HOLD_US
#define G_ALL 0xfff                         /* the 12 saturation cells */
#define GETTER_VIVID 0x60e3d0a7             /* return of the read of 111 in the getter of the Color/Vivid item's id (table
                                               0x611a02d8 {54: 5417, 55: 5418, 56: 5419, 64: 5420}) */
#define GETTER_CURVE 0x60e3d13d             /* the same of the tone curve (table 0x611a0318 {57: 5421 .. 64: 5428}) */
#define ED_PROFILE FIRST_COLOR              /* the picture mode the native editors are given: colour profile 1 */
#define ID_ED_KEY 901                       /* MODE_COLORTONE_DIAL_COLOR_CREATOR_2: the Color/Vivid handler looks it up in
                                               {54: 2197, 55: 2198, 56: 2199, 64: 2200} (0x611b59a0, from 0x60e4ad1c) */
#define CELL_ED_ONE 2197
#define CELL_ED_CURSOR 2201
#define CELL_ED_SAT 928
#define CELL_ED_HI 976                      /* 976 Hi, 977 Mid, 978 Sh */
#define CELL_ED_MID 2189
#define ID_EFF_HI 383                       /* the effective tone: the curve item's picture is chosen by their signs */
#define ID_EFF_MID 691
#define ID_EFF_SH 382
#define ED_LATE_US 50000                    /* writes no redraw took this long ago go into events in the core task */
#define ED_HOLD_US 1000000                  /* a step's value wins over the module's for this long: a core loop (250 ms) and
                                               the module's answer, with room */
#define ED_WAIT_US 3000000                  /* the look on this long without the module's editors: none (an older module) */
#define YELLOW_HUE 171                      /* sRGB yellow at h = atan2(Cr, Cb) (BT.601): drawn at the top, as the native
                                               Color/Vivid and its ring draw it */
#define FILTERS 9
#define IMG_ED_VIVID 4237                   /* a stub number: item 5374's picture, the camera's Color Vivid object or the Color
                                               Level one */
#define IMG_COLOR_VIVID 11397
#define IMG_FILTER_N 534                    /* "Filter N" of the Monotone column: Color/Level without the profiles' picture */
#define REC_VIVID_PIC 0x611b4ce2            /* u16 picture number of item 5374's picture record {u16 3, u16 number, u32 text}
                                               0x611b4ce0 (its descriptor 0x611b52d8 + 28); section 7 data */

/* Filter value (Color/Level, 0 none, 1 Ye, 2 Or, 3 R, 4 G, 5 GY, 6 M, 7 B, 8 C) by its place on the ring (editor.c) */
static const u8 filter_at[FILTERS] = {0, 1, 2, 3, 6, 7, 8, 4, 5};

static const u32 look_col_plain[3] = {2, ID_COLORTONE, ITEM_CC_ID};
static const u32 look_col_both[5] = {4, ID_COLORTONE, ITEM_CC_ID, ITEM_ED_CURVE, ITEM_ED_VIVID};   /* the curve first */
static const u32 look_col_tone[4] = {3, ID_COLORTONE, ITEM_CC_ID, ITEM_ED_CURVE};
static const u32 look_col_colour[4] = {3, ID_COLORTONE, ITEM_CC_ID, ITEM_ED_VIVID};
/* the grain item is item 5374 itself, last (only while Color/Vivid and Color/Level are not shown) */

/* The column function, GUI task (thunks.s): {count, items} */
u32 drv_look_column(void)
{
    volatile shared_t *sh = shared();
    u32 g, c;

    if (!sh || !sh->look)
        return (u32)look_col_plain;
    /* until the module's first values: the curve, and the grain item when core has tiles */
    g = sh->ed.known ? sh->ed.groups : 1u << OMK_EDIT_TONE | (sh->ed.g_count ? 1u << OMK_EDIT_GRAIN : 0);
    c = g & (1u << OMK_EDIT_VIVID | 1u << OMK_EDIT_LEVEL | 1u << OMK_EDIT_GRAIN);
    if (g >> OMK_EDIT_GRAIN & 1 && !sh->ed.g_count)
        c &= ~(1u << OMK_EDIT_GRAIN);
    if (g >> OMK_EDIT_TONE & 1)
        return c ? (u32)look_col_both : (u32)look_col_tone;
    return c ? (u32)look_col_colour : (u32)look_col_plain;
}

static int ed_screen(void)
{
    const volatile u8 *g = (const volatile u8 *)GUI_STATE;

    return g[0] == 0x04 && g[1] == 0x18;
}

/* The look's editor item under the cursor on the picture mode screen: 5374, 5375, or 0. CPU 0 only (the settings getter) */
static u32 ed_item(volatile shared_t *sh)
{
    u32 item;

    if (drv_cpu() || !sh->look || !ed_screen())
        return 0;
    item = drv_setting_read(ID_ITEM_NOW);
    return item == ITEM_ED_VIVID || item == ITEM_ED_CURVE ? item : 0;
}

/* Item 5374 is the grain item: the module shows the grain and neither Color/Vivid nor Color/Level (until it published,
 * whenever core has tiles: the column's default) */
static int ed_grain(volatile look_ed_t *e)
{
    u32 g = e->known ? e->groups : 1u << OMK_EDIT_GRAIN;

    return e->g_count && g >> OMK_EDIT_GRAIN & 1 && !(g & (1u << OMK_EDIT_VIVID | 1u << OMK_EDIT_LEVEL));
}

static int ed_level_shape(volatile look_ed_t *e)
{
    return e->groups >> OMK_EDIT_LEVEL & 1;
}

static u32 filter_place(u32 f)
{
    u32 i;

    for (i = 0; i < FILTERS && filter_at[i] != f; i++)
        ;
    return i < FILTERS ? i : 0;
}

/* The native cursor's place: 0 "all", k + 1 for the cell 928 + k */
static u32 ed_place(u32 cell)
{
    return cell >= CELL_ED_SAT && cell < CELL_ED_SAT + 12 ? cell - CELL_ED_SAT + 1 : 0;
}

/* The step between two places of the native cursor's ring of 13 ("all", then 928..939), -6..6 */
static s32 ed_ring_step(u32 from, u32 to)
{
    s32 d = ((s32)ed_place(to) - (s32)ed_place(from) + 13) % 13;

    return d > 6 ? d - 13 : d;
}

static s32 clamp_s(s32 v, s32 lo, s32 hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int ed_read_cell(u32 id)
{
    return id == ID_ED_KEY || id == CELL_ED_ONE || id == CELL_ED_CURSOR || (id >= CELL_ED_SAT && id < CELL_ED_SAT + 12) ||
           (id >= CELL_ED_HI && id < CELL_ED_HI + 3) || id == CELL_ED_MID;
}

/* The values the UI reads on the look's editors (the read method's hook, after the firmware's read): the shadow */
static void look_ed_read(volatile shared_t *sh, u32 id, u32 ret, u32 *where)
{
    volatile look_ed_t *e = &sh->ed;
    u32 k;

    if (id == ID_COLORTONE) {
        if (ret == GETTER_VIVID || ret == GETTER_CURVE)
            *where = ED_PROFILE;
        return;
    }
    if (id == ID_EFF_HI || id == ID_EFF_MID || id == ID_EFF_SH) {
        if (ret >= EDITOR_CODE_LO && ret < EDITOR_CODE_HI && !drv_cpu() && ed_screen())   /* the column draws the curve
                                                                                         item's picture wherever the cursor is */
            *where = (u32)(s32)e->view.tone[id == ID_EFF_HI ? 2 : id == ID_EFF_MID ? 1 : 0];
        return;
    }
    if (!ed_read_cell(id) || !(k = ed_item(sh)))
        return;
    if (k == ITEM_ED_VIVID && ed_grain(e)) {   /* the grain item: the native code's own cells */
        if (id == ID_ED_KEY)
            *where = ED_PROFILE;
        else if (id == CELL_ED_ONE)
            *where = e->g_one;
        else if (id == CELL_ED_CURSOR)
            *where = e->g_cursor;
        else if (id >= CELL_ED_SAT && id < CELL_ED_SAT + 12)
            *where = (u32)e->g_sat[id - CELL_ED_SAT];
        return;
    }
    if (id == ID_ED_KEY)
        *where = ED_PROFILE;
    else if (id == CELL_ED_ONE)
        *where = e->sh_one;
    else if (id == CELL_ED_CURSOR)
        *where = e->sh_cursor;
    else if (id >= CELL_ED_SAT && id < CELL_ED_SAT + 12)
        *where = (u32)e->sh_sat[id - CELL_ED_SAT];
    else if (id >= CELL_ED_HI && id < CELL_ED_HI + 3)
        *where = (u32)e->sh_tone[id - CELL_ED_HI];
    else
        *where = e->sh_mid;
}

/* An event into the ring. Under drv_lock. 1 - queued */
static int ed_push(volatile look_ed_t *e, u32 ev)
{
    if (e->head - e->tail >= ED_RING) {
        e->lost++;
        return 0;
    }
    e->ring[e->head % ED_RING] = ev;
    e->head++;
    return 1;
}

static void ed_odd(volatile look_ed_t *e, u32 id, u32 kind, u32 value)
{
    if (e->odd_head - e->odd_tail >= ED_ODD)
        return;
    e->odd[e->odd_head % ED_ODD] = (id & 0xffff) | (kind & 0xffff) << 16;
    e->odd_val[e->odd_head % ED_ODD] = value;
    e->odd_head++;
}

static int ed_profile_cell(u32 id)
{
    return !id || id == ID_ED_KEY || (id >= 904 && id <= 1033) || (id >= 2189 && id <= 2204) || (id >= 5413 && id <= 5428);
}

/* The shadow from the view (a new entry, or the module's values taken): the cells as the native code is to read them.
 * Color/Level's saturation cells are only steps of the level: kept but for a new entry (full) */
static void ed_shadow_load(volatile look_ed_t *e, u32 full)
{
    u32 k;

    for (k = 0; k < 12; k++)
        if (!ed_level_shape(e))
            e->sh_sat[k] = e->view.vivid[e->ord[k]];
        else if (full)
            e->sh_sat[k] = 0;
    e->sh_tone[0] = e->view.tone[2];
    e->sh_tone[1] = e->view.tone[1];
    e->sh_tone[2] = e->view.tone[0];
    e->fplace = filter_place(e->view.filter);
    e->wr_sat = e->wr_tone = 0;
}

/* The sectors by angle (the native cursor's places in the order the vertices are drawn); "all" stays between the last
 * and the first. ang: the 12 angles; ord, pos: the place k -> the sector, and back. On the caller's own memory: no lock */
static void ed_order(const u8 *ang, u8 *ord, u8 *pos)
{
    u8 k, j, t;

    for (k = 0; k < 12; k++)
        ord[k] = k;
    for (k = 1; k < 12; k++)
        for (j = k; j > 0 && ang[ord[j - 1]] > ang[ord[j]]; j--) {
            t = ord[j];
            ord[j] = ord[j - 1];
            ord[j - 1] = t;
        }
    for (k = 0; k < 12; k++)
        pos[ord[k]] = k;
}

/* The writes since the last commit into the view and events (under drv_lock; GUI task, after the native code's action -
 * the item's redraw - or the core task if no redraw came). 1 - an event queued.
 *  - The tone: the shadow's values; all three back to 0 (a long OK writes 0 into all) is the group's reset.
 *  - Color/Vivid: every written place is its sector's shift (one hue: the cursor's; all hues: every one, each from its own
 *    value); a sector the preset does not have stays 0; all shifts back to 0 is the reset (the preset's values).
 *  - Color/Level: one hue writes the cursor's place, all hues all 12; the step is the written value - the shadow's (the
 *    level, only with a filter). 12 zeros not one step of all is the reset (no filter, the middle level) */
static int ed_commit(volatile look_ed_t *e)
{
    s32 v, d;
    u32 k, s, n = 0, any = 0, reset, pm1, q;

    if (e->wr_tone) {
        for (k = 0; k < 3; k++) {
            v = clamp_s(e->sh_tone[2 - k], -7, 7);
            any |= v != 0;
            if (v != e->view.tone[k]) {
                e->view.tone[k] = (signed char)v;
                n += ed_push(e, OMK_EDIT_EVENT(OMK_EDIT_TONE, k, v));
            }
        }
        if (n && !any) {                    /* the three back at 0: the reset, one event in place of the values */
            e->head -= n;
            n = ed_push(e, OMK_EDIT_EVENT(OMK_EDIT_TONE | OMK_EDIT_RESET, 0, 0));
        }
        e->hold_t[OMK_EDIT_TONE] = drv_ticks() | 1;
        e->wr_tone = 0;
    }
    if (e->wr_sat && !ed_level_shape(e)) {
        q = n;
        any = 0;                            /* this group's own: a tone value must not hide the reset of the shifts */
        for (k = 0; k < 12; k++) {
            s = e->ord[k];
            if (!(e->mask >> s & 1)) {
                e->sh_sat[k] = 0;           /* not the preset's: drawn at 0, never edited */
                continue;
            }
            v = clamp_s(e->sh_sat[k], -5, 5);
            any |= v != 0;
            if (e->wr_sat >> k & 1 && v != e->view.vivid[s]) {
                e->view.vivid[s] = (signed char)v;
                n += ed_push(e, OMK_EDIT_EVENT(OMK_EDIT_VIVID, s, v));
            }
        }
        if (n > q && !any) {                /* all shifts back at 0: the reset */
            e->head -= n - q;
            n = q + ed_push(e, OMK_EDIT_EVENT(OMK_EDIT_VIVID | OMK_EDIT_RESET, 0, 0));
        }
        e->hold_t[OMK_EDIT_VIVID] = drv_ticks() | 1;
        e->wr_sat = 0;
    } else if (e->wr_sat) {
        for (k = 0, s = 0, reset = 1, pm1 = 1; k < 12; k++) {
            s += e->wr_sat >> k & 1;
            reset &= e->sh_sat[k] == 0;
            pm1 &= e->old_sat[k] == e->old_sat[0] && (e->old_sat[0] == 1 || e->old_sat[0] == -1);
        }
        if (s == 12 && reset && !((!e->sh_one || !e->sh_cursor) && pm1)) {   /* all hues one step to 0 from +-1: a step */
            e->view.filter = 0;
            e->view.level = 2;
            e->fplace = 0;
            n += ed_push(e, OMK_EDIT_EVENT(OMK_EDIT_LEVEL | OMK_EDIT_RESET, 0, 0));
        } else {
            k = ed_place(e->sh_cursor);
            k = s == 12 || !k ? 0 : k - 1;
            d = e->sh_sat[k] - e->old_sat[k];
            v = clamp_s((e->view.level ? e->view.level : 2) + d, 1, 3);
            if (e->view.filter && v != e->view.level) {   /* the level only with a filter, as the camera's own */
                e->view.level = (u8)v;
                n += ed_push(e, OMK_EDIT_EVENT(OMK_EDIT_LEVEL, 1, v));
            }
        }
        e->hold_t[OMK_EDIT_LEVEL] = drv_ticks() | 1;
        e->wr_sat = 0;
    }
    return n != 0;
}

/* The look's editors write (GUI task, before the firmware's write method): 1 - swallowed */
static int look_ed_write(volatile shared_t *sh, u32 id, u32 kind, u32 value)
{
    volatile look_ed_t *e = &sh->ed;
    u32 item, st, ev = 0, p, f, k;
    s32 v = (s32)value, d;

    if (!ed_profile_cell(id) || !(item = ed_item(sh)))
        return 0;
    st = drv_lock();
    if (item == ITEM_ED_VIVID && ed_grain(e)) {   /* the grain item: the writes are steps; commit at the item's redraw */
        if (!e->g_known || e->g_stale || kind) {
            ed_odd(e, id, kind, value);
        } else if (id == CELL_ED_ONE) {
            e->g_one = value;               /* INFO: one cell or all, as the native code keeps it */
        } else if (id == CELL_ED_CURSOR || (id >= CELL_ED_SAT && id < CELL_ED_SAT + 12)) {
            if (!e->g_wr) {
                e->g_old_cursor = e->g_cursor;
                for (k = 0; k < 12; k++)
                    e->g_old_sat[k] = e->g_sat[k];
            }
            if (id == CELL_ED_CURSOR) {
                e->g_cursor = value;
                e->g_wr |= 1u << 12;
            } else {
                e->g_sat[id - CELL_ED_SAT] = v;
                e->g_wr |= 1u << (id - CELL_ED_SAT);
            }
            e->wr_t = drv_ticks();
        } else {
            ed_odd(e, id, kind, value);
        }
        drv_unlock(st);
        return 1;
    }
    if (!e->known || !e->groups || e->stale || kind) {
        if (kind || !e->known)
            ed_odd(e, id, kind, value);
        drv_unlock(st);
        return 1;                           /* nothing to edit yet, or the values are of the entry before */
    }
    e->wr_t = drv_ticks();
    if (item == ITEM_ED_CURVE && id >= CELL_ED_HI && id < CELL_ED_HI + 3) {
        e->sh_tone[id - CELL_ED_HI] = v;
        e->wr_tone |= 1u << (id - CELL_ED_HI);
    } else if (item == ITEM_ED_CURVE && id == CELL_ED_MID) {
        e->sh_mid = value;
    } else if (item == ITEM_ED_VIVID && id >= CELL_ED_SAT && id < CELL_ED_SAT + 12) {
        if (!e->wr_sat)
            for (k = 0; k < 12; k++)
                e->old_sat[k] = e->sh_sat[k];
        e->sh_sat[id - CELL_ED_SAT] = v;
        e->wr_sat |= 1u << (id - CELL_ED_SAT);
    } else if (item == ITEM_ED_VIVID && id == CELL_ED_ONE) {
        e->sh_one = value;                  /* INFO: one hue or all, as the native code keeps it */
    } else if (item == ITEM_ED_VIVID && id == CELL_ED_CURSOR && ed_level_shape(e)) {
        d = ed_ring_step(e->sh_cursor, value);
        e->sh_cursor = value;
        e->fplace = (e->fplace + (u32)(d % FILTERS + FILTERS)) % FILTERS;
        f = filter_at[e->fplace];
        if (f != e->view.filter) {
            e->view.filter = (u8)f;
            if (f && !e->view.level)
                e->view.level = 2;
            e->hold_t[OMK_EDIT_LEVEL] = drv_ticks() | 1;
            ev = (u32)ed_push(e, OMK_EDIT_EVENT(OMK_EDIT_LEVEL, 0, f));
        }
    } else if (item == ITEM_ED_VIVID && id == CELL_ED_CURSOR) {
        /* the cursor: the native code's next place; one the preset does not have is passed by the same way */
        d = ed_ring_step(e->sh_cursor, value);
        p = ed_place(value);
        for (k = 0; p && !(e->mask >> e->ord[p - 1] & 1) && k < 13; k++)
            p = (p + (d < 0 ? 12 : 1)) % 13;
        e->sh_cursor = p ? CELL_ED_SAT + p - 1 : 0;
    } else {
        ed_odd(e, id, kind, value);
    }
    drv_unlock(st);
    if (ev)
        drv_wake();                         /* the core task takes it now rather than at the end of its 250 ms wait */
    return 1;
}

void drv_edp_look_grain(u32 names, u32 tile, u32 count, u32 level);   /* editor.c: the grain item */

/* The grain item's writes since the last commit into the view and events (under drv_lock). All 12 cells written 0, not
 * one step of all from +-1 - the long OK: the strength Off. The cursor - a step of the tile by the places of the native
 * ring (one place on: the next tile, the way the rear dial steps the presets). A saturation cell (the cursor's; all 12 at
 * the "all" place or by INFO: the first of them) - a step of the strength by its change; the same value again (pushed at
 * an end of -5..5) a step that way. 1 - an event queued */
static int ed_commit_grain(volatile look_ed_t *e)
{
    s32 d, v;
    u32 n = 0, wr = e->g_wr, k, zero = 1, pm1 = 1;

    if (!wr)
        return 0;
    e->g_wr = 0;
    e->g_hold = drv_ticks() | 1;
    for (k = 0; k < 12; k++) {
        zero &= e->g_sat[k] == 0;
        pm1 &= e->g_old_sat[k] == e->g_old_sat[0] && (e->g_old_sat[0] == 1 || e->g_old_sat[0] == -1);
    }
    if ((wr & G_ALL) == G_ALL && zero && !pm1) {
        if (e->g_level) {
            e->g_level = 0;
            n += ed_push(e, OMK_EDIT_EVENT(OMK_EDIT_GRAIN, 1, 0));
        }
        return n != 0;
    }
    if (wr >> 12 & 1) {
        d = ed_ring_step(e->g_old_cursor, e->g_cursor);
        d = d > 0 ? 1 : d < 0 ? -1 : 0;
        if (d && e->g_count) {
            v = e->g_tile < 0 ? (d > 0 ? 0 : (s32)e->g_count - 1) : (e->g_tile + d + (s32)e->g_count) % (s32)e->g_count;
            e->g_tile = v;
            n += ed_push(e, OMK_EDIT_EVENT(OMK_EDIT_GRAIN, 0, v));
        }
    }
    if (wr & G_ALL) {
        for (k = 0; k < 12 && !(wr >> k & 1); k++)
            ;
        d = e->g_sat[k] - e->g_old_sat[k];
        if (!d)
            d = e->g_sat[k] >= 5 ? 1 : e->g_sat[k] <= -5 ? -1 : 0;
        v = clamp_s((s32)e->g_level + (d > 0 ? 1 : d < 0 ? -1 : 0), 0, G_LEVEL_MAX);
        if ((u32)v != e->g_level) {
            e->g_level = (u32)v;
            n += ed_push(e, OMK_EDIT_EVENT(OMK_EDIT_GRAIN, 1, v));
        }
    }
    return n != 0;
}

void drv_edp_look_level(u32 filter, u32 level);             /* editor.c: what the look's items draw */
void drv_edp_look_vivid(const signed char *sat, u32 cur, u32 mask, const u8 *angle, const u32 *rgba);
void drv_edp_look_curve(const signed char *tone, u32 mid, const u8 *curve);
void drv_edp_look_none(void);

/* The curve drawn: the module's, of its tone values, moved at the three points by the steps the module has not answered
 * yet (smoothstep between them, as the module's tone edit; the inputs 1/4, 1/2, 3/4 are 63/4, 63/2, 189/4 of the 64
 * points), so a step shows at once and the module's curve takes over when it comes */
/* view, pub: the tone values shown and published; curve: the module's 64 points - the caller's copies (taken under
 * drv_lock; the arithmetic, 64 divisions, runs after it). out may be curve itself */
static void ed_curve_now(const signed char *view, const signed char *pub, const u8 *curve, u8 *out)
{
    static const s32 at4[5] = {0, 63, 126, 189, 252};      /* the points' places, in quarters of a step of k */
    s32 dy[5], q, s, v, j = 0, x4;
    u32 k;

    dy[0] = dy[4] = 0;
    for (k = 0; k < 3; k++)                 /* TONE_STEP 20 of 1023 a step, the curve's 0..255 */
        dy[k + 1] = (view[k] - pub[k]) * 20 * 255 / 1023;
    for (k = 0; k < 64; k++) {
        x4 = 4 * (s32)k;
        while (j < 3 && x4 >= at4[j + 1])
            j++;
        q = (x4 - at4[j]) * 1024 / (at4[j + 1] - at4[j]);
        s = q * q * (3072 - 2 * q) >> 20;
        v = (s32)curve[k] + dy[j] + ((dy[j + 1] - dy[j]) * s >> 10);
        out[k] = (u8)clamp_s(v, 0, 255);
    }
}

/* n words of the editors' block (outside the cache) into the caller's memory */
static void ed_words(void *to, const volatile void *from, u32 n)
{
    const volatile u32 *f = from;
    u32 *t = to, i;

    for (i = 0; i < n; i++)
        t[i] = f[i];
}

/* The look's editor item (editor.c, GUI task, the item handler's trampoline, after the native code's action): the writes
 * into events, then the values for the order about to be drawn. 1 - the group differs from the preset: the Reset hint */
u32 drv_look_item(u32 item)
{
    volatile shared_t *sh = shared();
    volatile look_ed_t *e;
    signed char v[12], pub[3];
    /* u32 arrays: copied from the block by words (ed_words); the bytes are read through the u8 pointers below */
    u32 anglew[3], basew[3], curvew[16], rgba[12], st, i, cur, mask, mid, groups, filter, level, all, ev;
    u32 edited = 0;
    const u8 *angle = (const u8 *)anglew;
    const signed char *base = (const signed char *)basew;

    if (sh && item == ITEM_ED_VIVID && ed_grain(&sh->ed)) {   /* the grain item */
        e = &sh->ed;
        st = drv_lock();
        ev = e->g_known && !e->g_stale ? (u32)ed_commit_grain(e) : 0;
        i = e->g_tile >= 0 && (u32)e->g_tile < e->g_count ? (u32)e->g_tile : 0xff;
        level = e->g_level;
        all = e->g_known && e->g_count;
        drv_unlock(st);
        if (ev)
            drv_wake();
        if (!all) {
            drv_edp_look_none();
            return 0;
        }
        drv_edp_look_grain(e->g_names, i, e->g_count, level);
        return level != 0;                  /* the Reset hint: a long OK turns the strength Off */
    }
    if (!sh || !sh->ed.known || !sh->ed.groups) {
        drv_edp_look_none();
        return 0;
    }
    e = &sh->ed;
    st = drv_lock();
    ev = e->stale ? 0 : (u32)ed_commit(e);
    groups = e->groups;
    mask = e->mask;
    mid = e->sh_mid == 1;
    all = !e->sh_one || !e->sh_cursor;      /* INFO, or the cursor's "all" place: the rear dial moves every vertex */
    i = ed_place(e->sh_cursor);
    cur = i ? e->ord[i - 1] : 12;
    filter = e->view.filter;
    level = e->view.level ? e->view.level : 2;
    /* under the lock only copies; what the drawing needs of them is worked out after it */
    if (item == ITEM_ED_CURVE) {
        for (i = 0; i < 3; i++) {
            v[i] = e->view.tone[i];
            pub[i] = e->pub.tone[i];
        }
        ed_words(curvew, e->curve, 16);
    } else {
        for (i = 0; i < 12; i++)
            v[i] = e->view.vivid[i];
        ed_words(basew, e->base, 3);
        ed_words(anglew, e->angle, 3);
        ed_words(rgba, e->rgba, 12);
    }
    drv_unlock(st);
    if (ev)
        drv_wake();                         /* the core task takes them now rather than at the end of its 250 ms wait */
    if (item == ITEM_ED_CURVE) {
        if (!(groups >> OMK_EDIT_TONE & 1)) {
            drv_edp_look_none();
            return 0;
        }
        for (i = 0; i < 3; i++)
            edited |= v[i] != 0;
        ed_curve_now(v, pub, (const u8 *)curvew, (u8 *)curvew);   /* in place: a point from its own value */
        drv_edp_look_curve(v, mid, (const u8 *)curvew);
        return edited;
    }
    for (i = 0; i < 12; i++) {              /* the vertex at the preset's own + the shift, within the drawing's -5..5 */
        edited |= v[i] != 0;
        v[i] = (signed char)(mask >> i & 1 ? clamp_s(base[i] + v[i], -5, 5) : 0);
    }
    if (groups >> OMK_EDIT_LEVEL & 1) {
        drv_edp_look_level(filter, level);
        return filter != 0;
    }
    if (!(groups >> OMK_EDIT_VIVID & 1)) {
        drv_edp_look_none();
        return 0;
    }
    drv_edp_look_vivid(v, cur | all << 8, mask, angle, rgba);
    return edited;
}

/* Item 5374's picture: the camera's Color Vivid while the look is not active or its preset is a colour one; for a
 * black-and-white preset the module's Color/Level picture (OMK_LOOK_PIC_LEVEL; with the profiles loaded too), without it
 * the profiles' Color Level, and without them the Monotone column's "Filter N". The item's picture record points at our
 * stub number once. CPU 0 */
static u32 ed_pic;                          /* the object item 5374's stub number holds now; 0 - none yet. CPU 0 only */

static void ed_item_picture(void)
{
    volatile u32 *t = (volatile u32 *)IMG_TABLE;
    volatile u16 *rec = (volatile u16 *)REC_VIVID_PIC;
    volatile shared_t *sh = shared();
    u32 want;

    if (drv_cpu() || !look_at || !table_ok())
        return;
    want = look_native[LOOK_PIC_ITEM] ? look_native[LOOK_PIC_ITEM] : t[IMG_COLOR_VIVID];
    if (sh && sh->look && ed_grain(&sh->ed) && look_obj[OMK_LOOK_PIC_GRAIN])
        want = look_obj[OMK_LOOK_PIC_GRAIN];
    else if (sh && sh->look && sh->ed.known && ed_level_shape(&sh->ed))
        want = look_obj[OMK_LOOK_PIC_LEVEL] ? look_obj[OMK_LOOK_PIC_LEVEL] :
               t[IMG_COLOR_LEVEL] != IMG_STUB ? t[IMG_COLOR_LEVEL] : t[IMG_FILTER_N];
    if (want == IMG_STUB || (t[IMG_ED_VIVID] != IMG_STUB && t[IMG_ED_VIVID] != ed_pic))
        return;                             /* no picture to show, or someone else's there */
    t[IMG_ED_VIVID] = ed_pic = want;
    if (*rec == IMG_COLOR_VIVID)
        *rec = IMG_ED_VIVID;
}


#ifdef OMK_EMU
/* OMK_EMU: a build for running core in an emulator, never for a camera. Word 0 = entry + 1 (written by the emulator's
 * test): the next module's values are taken as if the UI were at that entry, so a test sees the dropping of another
 * entry's values without the race the dropping guards against */
u32 drv_emu_look_ui[4] __attribute__((aligned(64), section(".data.cpu_shared")));
#endif

/* What the module shows (core task). Copied; a group's values replace the view's unless a step is newer (its hold).
 * Values of another entry than the one the UI shows now are dropped, not an error: a dial stepped the entry after the
 * module made them (its answer to an edit retried at its next tick, say), and the module sends the new entry's when it
 * gets the step. The events need no entry of their own: the driver makes them only while its values are of the entry
 * the UI shows (a step marks them stale until the module's values of the new entry), and the core loop gives the module
 * the editors' events before the step (look_service: the events, then OMK_LOOK_ENTRY), so an event made before a step
 * reaches the module while its entry is still the one the event was made on */
int drv_look_edits(const struct omk_look_edits *m)
{
    volatile shared_t *sh = shared();
    volatile look_ed_t *e;
    u32 i, g, st, groups, first, full, took, a, rgba[12], curvew[16];
    u8 angle[12], ord[12], pos[12];

    if (!sh || !look_installed)
        return -2;
    if (!m || m->groups & ~(((1u << OMK_EDIT_GROUPS) - 1) | 1u << OMK_EDIT_GRAIN) || m->filter > 8 || m->level < 1 || m->level > 3)
        return -1;
    for (i = 0; i < 3; i++)
        if (m->tone[i] < -7 || m->tone[i] > 7)
            return -1;
    for (i = 0; i < 12; i++)
        if (m->vivid[i] < -5 || m->vivid[i] > 5)
            return -1;
    /* What the drawing needs of the module's values, worked out here once (24 divisions), before the lock: the item
     * handler only copies it at every redraw */
    for (i = 0; i < 12; i++) {
        a = (360 + YELLOW_HUE - m->vivid_hue[i] % 360) % 360;   /* clockwise from the top, as the hue falls */
        angle[i] = (u8)(a * 2 / 5);                             /* 2.5 degrees a step */
        rgba[i] = (u32)m->vivid_rgb[i][0] << 24 | (u32)m->vivid_rgb[i][1] << 16 | (u32)m->vivid_rgb[i][2] << 8 | 0xff;
    }
    ed_order(angle, ord, pos);
    for (i = 0; i < 16; i++)
        curvew[i] = m->curve[4 * i] | m->curve[4 * i + 1] << 8 | m->curve[4 * i + 2] << 16 | (u32)m->curve[4 * i + 3] << 24;
    e = &sh->ed;
    st = drv_lock();
    i = sh->look_index;
#ifdef OMK_EMU                              /* emulator build: the UI entry a test sets (drv_emu_look_ui) */
    if (((volatile u32 *)((u32)drv_emu_look_ui + UNCACHED))[0])
        i = ((volatile u32 *)((u32)drv_emu_look_ui + UNCACHED))[0] - 1;
#endif
    if (m->entry != i) {
        drv_unlock(st);
        log_printf(OMK_LOG_DEBUG, "look: editors' values of entry %u dropped, the UI is at entry %u", m->entry, i);
        return 0;
    }
    groups = e->known ? e->groups : ~0u;
    first = !e->known;
    e->known = 1;
    e->groups = m->groups;
    e->edited = m->edited;
    for (i = 0; i < 3; i++)
        e->pub.tone[i] = m->tone[i];
    for (i = 0; i < 12; i++) {
        e->pub.vivid[i] = m->vivid[i];
        e->base[i] = m->vivid_base[i];
        e->angle[i] = angle[i];
        e->rgba[i] = rgba[i];
    }
    e->pub.filter = m->filter;
    e->pub.level = m->level;
    for (i = 0; i < 16; i++)                /* the curve's bytes in memory order (little endian), by words */
        ((volatile u32 *)e->curve)[i] = curvew[i];
    e->mask = m->vivid_mask & 0x0fff;
    full = first || e->stale || groups != e->groups;
    if (full)
        for (i = 0; i < 12; i++) {
            e->ord[i] = ord[i];
            e->pos[i] = pos[i];
        }
    for (g = 0, took = 0; g < OMK_EDIT_GROUPS; g++) {
        if (!full && ((e->hold_t[g] && drv_ticks_since(e->hold_t[g]) < ED_HOLD_US) || e->wr_sat || e->wr_tone))
            continue;                       /* a step newer than these values, or writes not taken yet: the view keeps it */
        e->hold_t[g] = 0;
        took = 1;
        if (g == OMK_EDIT_TONE)
            for (i = 0; i < 3; i++)
                e->view.tone[i] = e->pub.tone[i];
        if (g == OMK_EDIT_VIVID)
            for (i = 0; i < 12; i++)
                e->view.vivid[i] = e->pub.vivid[i];
        if (g == OMK_EDIT_LEVEL) {
            e->view.filter = e->pub.filter;
            e->view.level = e->pub.level;
        }
    }
    if (took)
        ed_shadow_load(e, full);
    if (full) {                             /* the native code's own state: one hue, Hi and Sh, the first sector */
        e->sh_one = 1;
        e->sh_mid = 0;
        e->sh_cursor = 0;
        for (i = 0; i < 12 && !(e->mask >> e->ord[i] & 1); i++)
            ;
        e->sh_cursor = i < 12 ? CELL_ED_SAT + i : 0;
    }
    e->stale = 0;
    e->since = 0;
    drv_unlock(st);
    if (groups != e->groups)
        ed_item_picture();
    return 0;
}

/* The grain item's tiles (core task, once): names copied into a new arena block. 0, -1 bad, -2 memory */
int drv_look_grain_tiles(const char *const *names, u32 count)
{
    volatile shared_t *sh = shared();
    u8 *blk = 0;
    u32 i, k, st;

    if (!look_installed || !sh || count > OMK_GRAIN_TILES || (count && !names))
        return -1;
    if (count) {
        blk = mem_alloc(count * LOOK_NAME);
        if (!blk)
            return -2;
        mem_set(blk, 0, count * LOOK_NAME);
        for (i = 0; i < count; i++)
            for (k = 0; names[i] && names[i][k] && k < LOOK_NAME - 1; k++)
                blk[i * LOOK_NAME + k] = names[i][k] >= 32 && names[i][k] < 127 ? names[i][k] : '?';
    }
    st = drv_lock();
    sh->ed.g_names = (u32)blk;
    sh->ed.g_count = count;
    drv_unlock(st);
    ed_item_picture();
    return 0;
}

/* The entry's tile and strength from the module (core task): taken unless another entry is shown, or a step is newer
 * than these values (its hold). 0, -1 bad, -2 not available */
int drv_look_grain(u32 entry, s32 tile, u32 level)
{
    volatile shared_t *sh = shared();
    volatile look_ed_t *e;
    u32 st, i, k;

    if (!sh || !look_installed || !sh->ed.g_count)
        return -2;
    if (level > G_LEVEL_MAX || tile >= (s32)sh->ed.g_count || tile < -1)
        return -1;
    e = &sh->ed;
    st = drv_lock();
    i = sh->look_index;
#ifdef OMK_EMU                              /* emulator build: the UI entry a test sets (drv_emu_look_ui) */
    if (((volatile u32 *)((u32)drv_emu_look_ui + UNCACHED))[0])
        i = ((volatile u32 *)((u32)drv_emu_look_ui + UNCACHED))[0] - 1;
#endif
    if (entry != i) {
        drv_unlock(st);
        return 0;
    }
    if (e->g_stale || !e->g_known || !e->g_hold || drv_ticks_since(e->g_hold) >= G_HOLD_US) {
        e->g_tile = tile;
        e->g_level = level;
        e->g_hold = 0;
        if (!e->g_known || e->g_stale) {    /* the native code's cells as it will read them on the item: one cell, the
                                               cursor on the first, all saturations 0 (the strength steps need at most 3
                                               of the 5 each way) */
            e->g_one = 1;
            e->g_cursor = CELL_ED_SAT;
            for (k = 0; k < 12; k++)
                e->g_sat[k] = 0;
            e->g_wr = 0;
        }
    }
    e->g_known = 1;
    e->g_stale = 0;
    drv_unlock(st);
    return 0;
}

/* The editors' events since the last call (core task); the writes not understood go to the log here */
u32 drv_look_edit_take(u32 *ev, u32 max, u32 *lost)
{
    volatile shared_t *sh = shared();
    volatile look_ed_t *e;
    u32 n = 0, st, id, val;

    *lost = 0;
    if (!sh)
        return 0;
    e = &sh->ed;
    while (e->odd_tail != e->odd_head) {
        id = e->odd[e->odd_tail % ED_ODD];
        val = e->odd_val[e->odd_tail % ED_ODD];
        e->odd_tail++;
        log_printf(OMK_LOG_DEBUG, "look: editor write %u = %d (kind %u) swallowed, not an edit", id & 0xffff, (s32)val, id >> 16);
    }
    if (sh->look && !e->known) {            /* an older module never publishes its editors: none at the next list */
        if (!e->since)
            e->since = drv_ticks() | 1;
        else if (drv_ticks_since(e->since) >= ED_WAIT_US) {
            st = drv_lock();
            e->known = 1;
            e->groups = 0;
            drv_unlock(st);
            log_printf(OMK_LOG_INFO, "look: the module shows no editors, the column keeps the look's item only");
        }
    } else if (e->since) {
        e->since = 0;                       /* the look went off meanwhile: the next wait starts from its own time, not
                                               from a stamp the 30-bit counter may have lapped */
    }
    st = drv_lock();
    /* The holds end here, once, when their time is over (this runs at every core loop while the look has an owner):
     * a stamp left in place would look fresh again for ED_HOLD_US every turn of the 30-bit counter (1073 s), and the
     * module's values published just then would not reach the view */
    for (id = 0; id < OMK_EDIT_GROUPS; id++)
        if (e->hold_t[id] && drv_ticks_since(e->hold_t[id]) >= ED_HOLD_US)
            e->hold_t[id] = 0;
    if (e->g_hold && drv_ticks_since(e->g_hold) >= G_HOLD_US)
        e->g_hold = 0;
    if ((e->wr_sat || e->wr_tone) && !e->stale && drv_ticks_since(e->wr_t) >= ED_LATE_US)
        ed_commit(e);                       /* writes no item redraw took (the native code's action ended elsewhere) */
    if (e->g_wr && !e->g_stale && drv_ticks_since(e->wr_t) >= ED_LATE_US)
        ed_commit_grain(e);
    while (n < max && e->tail != e->head)
        ev[n++] = e->ring[e->tail++ % ED_RING];
    *lost = e->lost;
    e->lost = 0;
    drv_unlock(st);
    return n;
}
int drv_look_install(int cache_inv, const char *name)
{
    volatile shared_t *sh;
    u32 i;
    int rc;

    if (look_installed)
        return 0;
    for (i = 0; i < LOOK_SLOTS; i++)
        if (str_ieq(look_slots[i].name, name))
            break;
    if (i == LOOK_SLOTS)
        return -1;
    fw_cache_inv = cache_inv;
    look_at = &look_slots[i];
    rc = install_shared();
    if (rc)
        return rc;
    if (look_column_put() < 0)              /* checked before anything goes in: the slot's column must be the one we know */
        return 4;
    install_editor();
    sh = shared();
    sh->cc_last[0] = hue_base();
    sh->cc_last[1] = 0;
    sh->look_slot = look_at->mode + 1;
    look_installed = 1;
    log_checkpoint("look: UI method and availability pointers, column of %s", look_at->name);
    rc = put_pointers();
    if (rc) {
        look_installed = 0;
        sh->look_slot = 0;
        return rc;
    }
    return 0;
}

u32 drv_look_mode(void)
{
    return look_at ? look_at->mode : 0;
}

int drv_look_active(void)
{
    volatile shared_t *sh = shared();

    return sh && sh->look;
}

/* Core task: the look on or off (the power-on restore); on turns an active profile off, and the profiles' owner is told */
void drv_look_set(int on)
{
    volatile shared_t *sh = shared();

    if (!sh || !look_installed)
        return;
    if (on && sh->prof)
        ui_changed(sh, OMK_PROF_NONE);
    sh->look = on != 0;
    look_item_picture(on != 0);
}

/* The entries: names copied into a new arena block (the renderer may still read the old one: not freed); 0, -1 bad, -2 no
 * memory */
int drv_look_list(const char *const *names, u32 count, u32 current)
{
    volatile shared_t *sh = shared();
    u8 *blk = 0;
    u32 i, k, st;

    if (!look_installed || !sh || count > LOOK_MAX || (count && !names))
        return -1;
    if (count) {
        blk = mem_alloc(count * LOOK_NAME);
        if (!blk)
            return -2;
        mem_set(blk, 0, count * LOOK_NAME);
        for (i = 0; i < count; i++)
            for (k = 0; names[i] && names[i][k] && k < LOOK_NAME - 1; k++)
                blk[i * LOOK_NAME + k] = names[i][k] >= 32 && names[i][k] < 127 ? names[i][k] : '?';
    }
    st = drv_lock();
    sh->look_names = (u32)blk;
    sh->look_count = count;
    sh->look_index = count ? current % count : 0;
    drv_edp_look(sh->look_names, sh->look_index, count);
    drv_unlock(st);
    return 0;
}

/* The test scenario (core/shoot.c), core task: the look's entry chosen as the user chooses it in the picture mode screen.
 * The look on: OMK_LOOK_ON with the entry for the owner at core's next loop (look_changed_ui, as the UI's own choice);
 * already on: the entry as a dial step (look_pick, OMK_LOOK_ENTRY); off: OMK_LOOK_OFF */
int drv_look_choose(int on, u32 index)
{
    volatile shared_t *sh = shared();
    u32 st;

    if (!sh || !look_installed)
        return -1;
    if (!on) {
        if (sh->look)
            look_changed_ui(sh, 0);
        return 0;
    }
    if (index >= sh->look_count)
        return -1;
    if (!drv_prof_base())
        return -4;
    if (sh->prof)
        ui_changed(sh, OMK_PROF_NONE);
    st = drv_lock();
    if (sh->look_index != index) {
        sh->look_index = index;
        if (sh->look) {
            sh->look_pick++;
            sh->ed.stale = 1;               /* the editors' values are the entry's before until the module's next */
            sh->ed.g_stale = 1;
        }
        drv_edp_look(sh->look_names, index, sh->look_count);
    }
    drv_unlock(st);
    if (!sh->look)
        look_changed_ui(sh, 1);
    return 0;
}

static char lower_c(char c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int drv_look_find(const char *name)
{
    volatile shared_t *sh = shared();
    const char *n;
    u32 i, k;

    if (!sh || !look_installed || !sh->look_names || !name || !name[0])
        return -1;
    for (i = 0; i < sh->look_count; i++) {
        n = (const char *)(sh->look_names + i * LOOK_NAME);
        for (k = 0; k < LOOK_NAME && name[k] && n[k] && lower_c(name[k]) == lower_c(n[k]); k++)
            ;
        if (k < LOOK_NAME && !name[k] && !n[k])
            return (int)i;
    }
    return -1;
}

/* An entry's cassette drawing (a CANI section core has checked): copied into a new arena block (the renderer may still
 * read the old one: not freed) and published in the table. 0, -1 bad index, -2 no memory */
int drv_look_art(u32 index, const void *data, u32 size)
{
    u8 *blk;
    u32 st;

    if (!look_installed || index >= LOOK_MAX || !data || !size)
        return -1;
    if (!look_arts) {
        look_arts = mem_alloc(4 * LOOK_MAX);
        if (!look_arts)
            return -2;
        mem_set(look_arts, 0, 4 * LOOK_MAX);
    }
    blk = mem_alloc(size);
    if (!blk)
        return -2;
    mem_copy(blk, data, size);
    st = drv_lock();
    ((volatile u32 *)look_arts)[index] = (u32)blk;
    drv_edp_look_arts((u32)look_arts);
    drv_unlock(st);
    return 0;
}

void drv_look_stats(drv_look_info *st)
{
    volatile shared_t *sh = shared();

    if (!sh) {
        mem_set(st, 0, sizeof(*st));
        return;
    }
    st->seq = sh->look_seq;
    st->ui_on = sh->look_ui;
    st->active = sh->look;
    st->pick = sh->look_pick;
    st->index = sh->look_index;
    st->count = sh->look_count;
    st->swallowed = sh->swallowed;
    st->swallow_kind = sh->swallow_kind;
    st->dial_last = sh->dial_last;
}
