/* OMKit public API for modules. The table only grows; a module checks version and size.
 * The only header a module sees: it has nothing from the driver or the firmware.
 * Almost every function takes the module handle that core passed to module_init: through it core knows whose memory,
 * config section, store and folder are involved. */
#ifndef OMK_API_H
#define OMK_API_H

#include "omk_types.h"
#include "omk_events.h"

#define OMK_API_VERSION 20

#define OMK_LOG_OFF 0
#define OMK_LOG_ERROR 1
#define OMK_LOG_WARN 2
#define OMK_LOG_INFO 3
#define OMK_LOG_DEBUG 4

#define OMK_STORE_PERSIST 1

#define OMK_RES_USER_MAX 0x7fff             /* module resource types: 1..0x7fff */
typedef struct omk_mod omk_mod;
typedef struct omk_store omk_store;
typedef struct omk_file omk_file;
typedef struct omk_ev omk_ev;               /* event invocation: for api->next */

/* Power states (API 5). The handler runs in the core task, short and without sleeping; while the
 * state is not "running" the card is not available (file functions refuse) and core runs no deferred work */
#define OMK_POWER_OFF 1                     /* power-off began: stop, put what must survive into the store (core saves it) */
#define OMK_POWER_RESUME 2                  /* the power-off did not happen: carry on; the card may be gone for a while */
#define OMK_POWER_SLEEP 3                   /* the camera goes to sleep: stop loops and timers, every task freezes soon */
#define OMK_POWER_WAKE 4                    /* back from sleep (or sleep did not happen): carry on; time and card may differ */
typedef void (*omk_power_fn)(u32 event, void *ctx);

/* Picture profiles (API 6): the camera's colour and monochrome profiles. The stored picture mode
 * stays a stock one; core shows the profile in the picture mode screen and applies it to shooting from memory only.
 * A profile: OMK_PROF_COLOR(n) or OMK_PROF_MONO(n), n from 1; OMK_PROF_NONE - a stock picture mode */
#define OMK_PROF_NONE 0
#define OMK_PROF_COLOR(n) (n)
#define OMK_PROF_MONO(n) (0x10 | (n))
#define OMK_PROF_IS_MONO(p) (((p) & 0x10) != 0)
#define OMK_PROF_NUM(p) ((p) & 0x0f)
/* The user picked a profile, or left it for a stock mode (OMK_PROF_NONE); runs in the core task */
typedef void (*omk_prof_fn)(u32 prof, void *ctx);

/* The film look (API 14): events in the core task */
#define OMK_LOOK_OFF 0                      /* the user left the look for another picture mode (arg 0) */
#define OMK_LOOK_ON 1                       /* the user chose the look; arg: the chosen entry */
#define OMK_LOOK_ENTRY 2                    /* a dial chose another entry; arg: its index */
#define OMK_LOOK_EDIT 3                     /* API 16: an editor of the look's column changed a value; arg: OMK_EDIT_EVENT */
#define OMK_LOOK_MAX 64                     /* entries */
#define OMK_NAME_MAX 40                     /* API 15: a long file name of the module folder with its terminator */
typedef void (*omk_look_fn)(u32 event, u32 arg, void *ctx);

/* API 16. The editors of the look's column for the chosen entry (look_edits) and their events (OMK_LOOK_EDIT) */
#define OMK_EDIT_TONE 0                     /* the tone curve: index 0 shadows, 1 middle, 2 highlights; value -7..7 */
#define OMK_EDIT_VIVID 1                    /* Color/Vivid: index 0..11 a sector of the entry; value -5..5, its shift from the
                                               entry's own saturation, 0.05 a step */
#define OMK_EDIT_LEVEL 2                    /* Color/Level: index 0 the filter 0..8 (0 - the entry's own), 1 the level 1..3 */
#define OMK_EDIT_GROUPS 3
#define OMK_EDIT_GRAIN 3                    /* API 17: the grain item. In look_edits' groups: the item is shown. Events: index 0
                                               - the grain tile, value its number in grain_list; index 1 - the strength 0..3
                                               (Off, Low, Med, High); a long OK on the item: index 1, value 0 */
#define OMK_GRAIN_LEVELS 4                  /* Off, Low, Med, High */
#define OMK_GRAIN_TILES 32                  /* grain tile containers at most */
#define OMK_EDIT_OPTICS 4                   /* API 20: the optics rows (Bloom, Halation) after the grain item. In
                                               look_edits' groups: the rows are shown. Events: index OMK_OPTICS_* - the
                                               row, value its strength 0..OMK_OPTICS_MAX; a long OK on a row: value 0 */
#define OMK_OPTICS_BLOOM 0
#define OMK_OPTICS_HALATION 1
#define OMK_OPTICS_ROWS 2
#define OMK_OPTICS_MAX 5                    /* strengths 0 (off) .. 5 */
#define OMK_EDIT_RESET 0x80                 /* with a group: the group back to the entry's own values (index, value 0) */
#define OMK_EDIT_EVENT(group, index, value) ((u32)(group) << 24 | (u32)(index) << 16 | ((u32)(value) & 0xffff))
#define OMK_EDIT_GROUP_OF(arg) ((arg) >> 24 & 0x7f)
#define OMK_EDIT_IS_RESET(arg) ((arg) >> 31 & 1)
#define OMK_EDIT_INDEX_OF(arg) ((arg) >> 16 & 0xff)
#define OMK_EDIT_VALUE_OF(arg) ((s32)(s16)((arg) & 0xffff))
typedef struct omk_look_edits {
    u32 groups;                             /* bits 1 << OMK_EDIT_*: the items shown after the look's own; 0 - none */
    u32 edited;                             /* bits 1 << OMK_EDIT_*: changed from the entry's own values */
    u32 entry;                              /* the entry these values are of (its index in look_list): the UI may have moved
                                               on to another one meanwhile, then they are not shown */
    signed char tone[3];                    /* the values shown */
    signed char vivid[12];
    u8 filter, level;
    u8 curve[64];                           /* the curve drawn: output 0..255 at the inputs k / 63 */
    u16 vivid_mask;                         /* bit i: sector i can be edited (the others are drawn at 0, skipped) */
    u16 vivid_hue[12];                      /* a vertex's hue, degrees 0..359 */
    u8 vivid_rgb[12][3];                    /* a vertex's colour, sRGB */
    signed char vivid_base[12];             /* a sector's own saturation in the entry, 0.05 a step (rounded, -127..127):
                                               the vertex is drawn at vivid_base + vivid */
} omk_look_edits;
/* The look's pictures, in meaning values as the profiles' (OMK_ICON_*) */
#define OMK_LOOK_PIC_MODE 0                 /* the picture mode strip, 66 x 50 */
#define OMK_LOOK_PIC_LARGE 1                /* the large one, 80 x 60 */
#define OMK_LOOK_PIC_LABEL 2                /* the menu line "Picture Mode", up to 124 x 39 */
#define OMK_LOOK_PIC_PANEL 3                /* the control panel cell, up to 128 x 39 */
#define OMK_LOOK_PIC_SHOOT 4                /* the shooting screen sign, up to 80 x 36 */
#define OMK_LOOK_PIC_ITEM 5                 /* the editor column's item, 66 x 50 */
#define OMK_LOOK_PIC_PREV 6                 /* the rear dial's label over the editor ("Prev"), up to 56 x 24 */
#define OMK_LOOK_PIC_NEXT 7                 /* the front dial's label ("Next"), up to 56 x 24 */
#define OMK_LOOK_PIC_LEVEL 8                /* API 16: the editor column's Color/Level item of a black-and-white entry, 66 x 50 */
#define OMK_LOOK_PIC_GRAIN 9                /* API 17: the editor column's grain item, 66 x 50 */
#define OMK_LOOK_PIC_TYPE 10                /* API 17: the grain item's label over the dial that changes the tile, up to 56 x 24 */
#define OMK_LOOK_PIC_STRENGTH 11            /* API 17: ... over the dial that changes the strength, up to 56 x 24 (core 1.29;
                                               116 and 104 before) */
#define OMK_LOOK_PIC_BLOOM 12               /* API 20: the optics rows' signs in the editor column's left cell, 46 x 46, */
#define OMK_LOOK_PIC_HALATION 13            /* left of the camera's own number (without them: the camera's "S") */
#define OMK_LOOK_PICTURES 14

/* Event handler: data is the event structure from omk_events.h, ctx is what the module passed at registration */
typedef void (*omk_handler)(omk_ev *ev, void *data, void *ctx);

typedef struct {
    u32 used, peak, allocs, fails, limit, fw_used;
} omk_mem_info;

typedef struct {
    u32 version, size;

    /* Log: a line with the module name. Format %s %c %d %u %x %X, zero-padded width: %08x. Callable from any task on CPU 0 */
    void (*log)(omk_mod *m, int level, const char *fmt, ...);

    /* Config: section [MODULE_NAME] in OMKIT.INI */
    const char *(*cfg_str)(omk_mod *m, const char *key, const char *def);
    int (*cfg_int)(omk_mod *m, const char *key, int def);
    int (*cfg_bool)(omk_mod *m, const char *key, int def);
    int (*cfg_size)(omk_mod *m, const char *key, u32 *out);     /* 1 - present, 0 - absent, -1 - not a size; suffixes K, M */

    /* Memory: only through core. 0 - out of memory, reason in the log; the module must survive this */
    void *(*alloc)(omk_mod *m, u32 size);
    void (*free)(omk_mod *m, void *p);
    void (*mem_info)(omk_mod *m, omk_mem_info *out);

    /* Module store: open it once in module_init, size is the space for records */
    omk_store *(*store_open)(omk_mod *m, u32 size);
    int (*store_get)(omk_store *s, u16 key, void *buf, u32 max);
    int (*store_set)(omk_store *s, u16 key, const void *data, u32 len, u32 flags);
    int (*store_del)(omk_store *s, u16 key);

    /* Files in the module folder on the card, 8.3 name without directories. Only from module_init and the core task: the card is slow */
    /* A file of the module folder: an 8.3 name, "r" or "w" (w starts the file empty); from API 15 also a long name as
     * file_list_long gives it (up to OMK_NAME_MAX - 1 printable characters), "r" only */
    omk_file *(*file_open)(omk_mod *m, const char *name, const char *mode);
    u32 (*file_read)(omk_file *f, void *buf, u32 len);
    u32 (*file_write)(omk_file *f, const void *buf, u32 len);
    void (*file_close)(omk_file *f);

    /* Module resources from its file. Types 1..OMK_RES_USER_MAX are the module's own, the rest are reserved by core.
     * res: pointer to the data in memory; 0 - no such resource, or it is on-demand (size is filled in anyway).
     * res_read: an on-demand resource (res/disk/ at build time) is read from the card: bytes read, 0 - error.
     * Only from module_init and the core task: the card is slow */
    const void *(*res)(omk_mod *m, u32 type, u32 id, u32 *size);
    u32 (*res_read)(omk_mod *m, u32 type, u32 id, u32 offset, void *buf, u32 len);

    /* Module strings from res/strings.txt: English only, printable ASCII. Ids are STR_NAME from res_strings.h,
     * which the build generates. No such string - "" */
    const char *(*str)(omk_mod *m, u32 id);

    /* Time */
    void (*sleep)(u32 ms);                  /* only in module_init and the core task */
    u32 (*ms)(void);                        /* milliseconds since camera start */

    /* Events (omk_events.h). Registration only in module_init; the hooks are installed once all modules are loaded.
     * priority: higher runs earlier; OMKIT.INI [NAME] event_<event name> = off | number overrides it or disables the handler.
     * 0 - accepted; -1 - no such event on this firmware; -2 - phase not allowed; -3 - not from module_init; -4 - no free slots.
     * The handler runs in a firmware task: keep it short, no card and no sleep; long work goes through defer */
    int (*on)(omk_mod *m, u32 event, u32 phase, int priority, omk_handler fn, void *ctx);
    void (*next)(omk_ev *ev);               /* REPLACE phase only: the rest of the chain and the original function */
    int (*defer)(omk_mod *m, void (*fn)(void *arg), void *arg);   /* run in the core task; 0, or -1: the queue is full */

    /* Removed in core 0.94: always -1. The slot keeps the table layout */
    int (*overlay_removed)(omk_mod *m, const char *text, u32 ms);

    /* API 4. core version as a string, e.g. "0.55": for display and the log. Compatibility is checked by version only */
    const char *core_version;

    /* API 5. Power states (OMK_POWER_*): one handler per module, only from module_init; a new call replaces it, fn 0 removes.
     * Order: power-off and sleep - the last loaded module first; resume and wake - load order. 0 or -3: not from module_init */
    int (*on_power)(omk_mod *m, omk_power_fn fn, void *ctx);

    /* API 6. Picture profiles (OMK_PROF_*), one module owns them.
     * prof_count: how many colour and monochrome profiles this camera has; 0 - none (no support in this firmware).
     * prof_enable: take the service, only from module_init; fn gets the user's choices. 0; -1 not supported or hooks failed;
     * -2 another module owns it; -3 not from module_init.
     * prof_set: the active profile (OMK_PROF_NONE - stock). 0; -1 no such profile or not the owner; -4 the stored picture
     * mode is not the base one (the user changed it without OMKit): the profile is not applied.
     * prof_get: the active profile */
    int (*prof_count)(u32 *colors, u32 *monos);
    int (*prof_enable)(omk_mod *m, omk_prof_fn fn, void *ctx);
    int (*prof_set)(omk_mod *m, u32 prof);
    u32 (*prof_get)(void);

    /* API 8 (replaces API 7's prof_icon). Pictures of profiles in the UI: OMK_PIC_* ids.
     * The module gives them in meaning values (OMK_ICON_*), 4 bits per pixel, the high nibble first, each row a whole number
     * of bytes; core shows them in the firmware's own style.
     * prof_picture_size: the height and the largest width this camera takes for a picture; a mode icon must be exactly that
     * wide, a label may be narrower. 0; -1 no such picture here.
     * prof_picture: only from the owner; the pixels may be freed after the call. An edited variant that is not set shows the
     * plain one (the large mode icon, the not-edited label, panel and shooting sign). 0; -1 not the owner, bad id or size;
     * -2 no memory; -3 the firmware's table is not as expected */
    int (*prof_picture_size)(u32 pic, u32 *w, u32 *h);
    int (*prof_picture)(omk_mod *m, u32 pic, u32 w, u32 h, const u8 *pixels);

    /* API 10. The colour filter of monochrome profiles: the camera's 3x3 matrix (nine signed 16-bit values in its own fixed
     * point) for a filter 1..8 at a level 1..3, only from the owner; 0 matrix - back to the camera's own. 0; -1 not the owner
     * or bad arguments; -3 the firmware's table is not as expected */
    int (*prof_mono_matrix)(omk_mod *m, u32 filter, u32 level, const s16 *matrix);

    /* API 13. The ISP colour tables: one module owns the service; its first call only from
     * module_init (the hooks go in then). isp_override: the platform's ISP block - for OM5M2 the ISPB section of an .isp
     * file (OMKit/docs/formats/isp.md, section 5), copied, may be freed after the call; 0 - back to the camera's own tables.
     * A change of the gamma or the sector weight tables (another preset) once a second at most; from core 1.02 a change
     * of only the matrix, Cb/Cr, the sector records or the tone (an edit) every 40 ms, at once in live view. isp_census: the jobs of the colour path to the log at debug level (1 on, 0 off).
     * 0; -1 a bad block; -2 too soon; -3 no service (other firmware, hooks failed); -4 the first call not from module_init;
     * -5 another module owns it */
    int (*isp_override)(omk_mod *m, const void *block, u32 size);
    int (*isp_census)(omk_mod *m, int on);
    /* Which jobs get the tables: OMK_ISP_LIVE (live view, also what the camera meters and focuses on) and/or OMK_ISP_STILLS;
     * the default is OMK_ISP_STILLS. Call before isp_override. 0 or as above */
    int (*isp_scope)(omk_mod *m, u32 scope);

    /* API 14. The film look, one module owns it. A stock picture mode of the picture mode
     * screen - the slot: ienhance, vivid, muted, portrait, monotone or underwater - shows the look instead: the module's
     * pictures (OMK_LOOK_PIC_*) and an editor column of one item, drawn where the native Color Creator item draws - a
     * rectangle with the chosen entry's name; the dials step through the module's entries at once. While the look is
     * active the stored picture mode stays Natural and shooting gets Natural: the colour is the module's (API 13).
     * look_enable: only from module_init; fn gets the events in the core task. 0; -1 no such slot (the log lists them) or
     * no service here; -2 another module owns it; -3 not from module_init.
     * look_list: the entries - names of up to 31 printable characters, at most OMK_LOOK_MAX - and the chosen one; copied.
     * 0; -1 not the owner or bad; -2 no memory.
     * look_set: on or off without the UI (the power-on restore). 0; -1 not the owner; -4 the stored picture mode is not
     * Natural (the look is not turned on). look_get: 1 - active.
     * look_picture_size, look_picture: as prof_picture_size and prof_picture, for OMK_LOOK_PIC_* */
    int (*look_enable)(omk_mod *m, const char *slot, omk_look_fn fn, void *ctx);
    int (*look_list)(omk_mod *m, const char *const *names, u32 count, u32 current);
    int (*look_set)(omk_mod *m, int on);
    int (*look_get)(void);
    int (*look_picture_size)(u32 pic, u32 *w, u32 *h);
    int (*look_picture)(omk_mod *m, u32 pic, u32 w, u32 h, const u8 *pixels);

    /* API 14. fn(ctx) in the core task about every ms milliseconds (the core loop's step, 250 ms, at the finest), from the
     * next step on; one per module, a new call replaces it, ms 0 removes it. Not while the camera is off or asleep (the
     * card is not there). 0; -1 bad arguments */
    int (*every)(omk_mod *m, u32 ms, void (*fn)(void *ctx), void *ctx);

    /* API 14. A text for the EXIF UserComment of the pictures taken from now on (JPEG and ORF; not the in-camera RAW
     * development): up to 117 printable ASCII characters, 0 - the camera's own (spaces). Needs the profiles or the film
     * look service in place (their shooting hook writes it). 0; -1 not available */
    int (*exif_comment)(omk_mod *m, const char *text);

    /* API 14. The files of the module folder matching pattern (8.3 with * and ?, e.g. "*.ISP"), at most max names of 13
     * bytes into names, upper case; folders, empty files and names that are not 8.3 (long names, macOS "._" files) are
     * skipped. Only from module_init and the core task. The number of names; 0 also when the folder is missing */
    u32 (*file_list)(omk_mod *m, const char *pattern, char (*names)[13], u32 max);

    /* API 14. The look's name in the UI where the slot's mode name was (the caption over the picture mode strip and every
     * other use of that string), in every language: up to 23 printable ASCII characters. Only the owner of the look.
     * 0; -1 not the owner or bad; -2 no memory; -3 the firmware's string table is not as expected */
    int (*look_name)(omk_mod *m, const char *text);

    /* API 14. The cassette picture of entry `index` (the CANI section of its .isp file, OMKit/docs/formats/isp.md): drawn in
     * the editor instead of the rectangle with the name. Checked (version 1, a drawing of up to 64 records and 6144 bytes,
     * every record's operation, segments and coordinates) and copied. Only the owner of the look. 0; -1 not the owner,
     * bad index or a section that does not pass the checks (the log says why); -2 no memory */
    int (*look_art)(omk_mod *m, u32 index, const void *data, u32 size);

    /* API 15. As file_list, with long names too: at most max names of OMK_NAME_MAX bytes, an 8.3 name in upper case, a
     * long one (FAT LFN) as on the card. Skipped: folders, empty files, names starting with a dot, names longer than
     * OMK_NAME_MAX - 1 or with other than printable ASCII. file_open opens every name it gives */
    u32 (*file_list_long)(omk_mod *m, const char *pattern, char (*names)[OMK_NAME_MAX], u32 max);

    /* API 16. The editors shown after the look's item in its column and their values, for the chosen entry: the tone
     * curve, Color/Vivid, Color/Level, as e->groups says (0 - only the look's item). The UI shows a dial's change at once
     * and sends OMK_LOOK_EDIT with the new value; a long OK on an editor sends its group's reset. The module applies the
     * change and calls this again with what is to be shown (the curve). Only the owner of the look; copied.
     * 0; -1 not the owner or bad; -2 the editors are not available on this firmware */
    int (*look_edits)(omk_mod *m, const omk_look_edits *e);

    /* API 17. Film grain of the look's stills: core owns the grain tile containers (OMKIT\GRAIN\*.GRN, by file name, at
     * most OMK_GRAIN_TILES; checked at boot) and puts the grain on the stills of the look; the owner of the look chooses.
     * grain_list: the containers' 8.3 file names in their order (a tile's number is its index here), the number of them;
     * the first call (only from module_init) takes the grain service: the hooks go in then, the grain item can be shown
     * (OMK_EDIT_GRAIN in look_edits). 0 - no containers or no service here (the log says why).
     * look_grain: the tile (its number; -1 - none, its file is gone) and the strength 0..3 of entry `entry`, the one the UI
     * shows: the grain item shows them, and the stills of the look get them while the strength is not Off (core loads the
     * tile from the card when it changes; a still taken before it is ready has no grain). Only the owner of the look.
     * 0; -1 not the owner or bad; -2 no grain service */
    u32 (*grain_list)(omk_mod *m, char (*files)[13], u32 max);
    int (*look_grain)(omk_mod *m, u32 entry, s32 tile, u32 level);

    /* API 18. The camera's own colour filter matrix of monochrome profiles (as prof_mono_matrix takes it) for a filter
     * 1..8 at a level 1..3 - also while prof_mono_matrix replaces it. Read only, any module.
     * 0; -1 bad arguments; -3 the firmware's table is not as expected */
    int (*prof_mono_matrix_get)(u32 filter, u32 level, s16 *matrix);

    /* API 19. Whether the card may be written now: 0 - free; 1 - the camera is writing a picture (or has just written
     * one), a module defers its own writes as core does; -1 - not available (power-off or sleep: file_open refuses).
     * Any module, from its periodic call or an event handler; cheap (two reads of the firmware's state) */
    int (*card_busy)(void);

    /* API 20. The optics rows of the look's column (OMK_EDIT_OPTICS in look_edits' groups): the strengths 0..OMK_OPTICS_MAX
     * of entry `entry`, the one the UI shows, by row (OMK_OPTICS_BLOOM, OMK_OPTICS_HALATION; count OMK_OPTICS_ROWS). The rows
     * show them, a dial's change comes back as OMK_LOOK_EDIT (OMK_EDIT_OPTICS); core keeps the chosen entry's strengths for
     * the look's stills. Only the owner of the look; copied. 0; -1 not the owner or bad; -2 the rows are not available on
     * this firmware */
    int (*look_optics)(omk_mod *m, u32 entry, const u8 *strength, u32 count);
} omk_api;

#define OMK_ISP_LIVE 1
#define OMK_ISP_STILLS 2

/* Pictures of profiles (API 8). prof: OMK_PROF_COLOR(n) or OMK_PROF_MONO(n) */
#define OMK_PROF_ICON_NORMAL 0              /* mode icon: the picture mode strip */
#define OMK_PROF_ICON_LARGE 1
#define OMK_PROF_ICON_LARGE_EDITED 2        /* the large one for a profile changed from its defaults */
#define OMK_PROF_ICON_VARIANTS 3
#define OMK_PIC_MODE(prof, variant) (0x100 | (prof) << 2 | (variant))
#define OMK_PIC_LABEL(prof, edited) (0x200 | (prof) << 1 | (edited))   /* the menu line "Picture Mode" */
#define OMK_PIC_PANEL(prof, edited) (0x300 | (prof) << 1 | (edited))   /* API 9: the picture mode cell of the control panel */
#define OMK_PIC_SHOOT(prof, edited) (0x400 | (prof) << 1 | (edited))   /* API 9: the picture mode sign of the shooting screen */
/* API 11: pictures of the profile editor that the camera does not have (the rest are its own) */
#define OMK_PIC_EDITOR(what) (0x500 | (what))
#define OMK_EDITOR_COLOR_LEVEL 0            /* the item "Color / Level" */
#define OMK_EDITOR_GRAIN(level) (1 + (level))   /* the item "Grain" at Off, Low, Medium, High (0..3) */
#define OMK_EDITOR_GRAIN_BAR(level) (5 + (level))   /* the same in the bar of choices */
#define OMK_EDITOR_LABEL_FILTER 9           /* API 12: the dial labels of the item "Color / Level": "Color Filter" (<= 116x24;
                                               not shown since core 1.29: the camera's own "Color" stands there) */
#define OMK_EDITOR_LABEL_LEVEL 10           /* and "Level" (<= 56x24) */
#define OMK_EDITOR_PICTURES 11
/* Pixel meanings: the sign, its outline and the shades between them and around */
#define OMK_ICON_CLEAR 0
#define OMK_ICON_SIGN 1
#define OMK_ICON_OUTLINE 2
#define OMK_ICON_EDGE_IN 3                  /* between the sign and the outline, nearer the sign */
#define OMK_ICON_EDGE_OUT 4                 /* nearer the outline */
#define OMK_ICON_OUTLINE_EDGE 5             /* outside the outline */
#define OMK_ICON_HALO 6                     /* the faint rim around everything */

/* Module entry. 0 - the module runs; otherwise core unloads it and logs the reason */
int module_init(const omk_api *api, omk_mod *self);

#endif
