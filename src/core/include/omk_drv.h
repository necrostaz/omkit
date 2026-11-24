/* Driver interface: everything core knows about the camera. Implementation: OMKit/src/platform/<platform>/<firmware>/. */
#ifndef OMK_DRV_H
#define OMK_DRV_H

#include "omk_types.h"

#define OMK_CPUS 3

/* NMI handler record. sp, psw and pc of CPU 0 are overwritten by the regular interrupt, they cannot be trusted */
typedef struct {
    u32 task, sp, psw, pc, cause;
    u32 regs[8];
} omk_crash;

const char *drv_name(void);
const char *drv_root(void);                /* OMKit folder on the card, with a trailing separator */
char drv_sep(void);                        /* directory separator */
int drv_check(void);                    /* 0: the firmware is the one the driver was built for */

#define DRV_UNCACHED 0x40000000u           /* cached address + this = the same memory, past the data cache */
#define DRV_TICKS_MASK 0x3fffffffu          /* drv_ticks: microseconds, 30 bits, wraps every ~1073 s */

void *drv_fopen(const char *path, const char *mode);   /* wb does not truncate the file */
/* The same for a path whose last part is a long name (FAT LFN, printable ASCII): the firmware's long-name walk. Read only:
 * the firmware is not known to create long names */
void *drv_fopen_long(const char *path, const char *mode);
u32 drv_fread(void *buf, u32 size, u32 n, void *fp);
u32 drv_fwrite(const void *buf, u32 size, u32 n, void *fp);
void drv_fclose(void *fp);
/* Files of a folder (core task, CPU 0 only): mask "C:\DIR\*.EXT" (a subfolder, not the root), entry 280 bytes zeroed before
 * every call - +0 u32 FAT attributes (0x10 a folder), +16 u32 size, +24 the name (the long one if there is one).
 * findfirst: a handle or -1 (nothing; then no findclose); findnext: 0 or -1 the end; findclose after every good findfirst */
#define DRV_FIND_ENTRY 280
int drv_findfirst(const char *mask, void *entry);
int drv_findnext(int handle, void *entry);
int drv_findclose(int handle);
int drv_remove(const char *path);           /* 0 - removed; a missing file is not an error for the callers */
int drv_mkdir(const char *path);            /* 0 - made */
int drv_exists(const char *path);           /* not 0 - a file or folder of that name is there */
/* Invalidates this CPU's data cache lines (both levels) of a range in the protected firmware range only; 0 - done.
 * For words written through the uncached alias: the next cached read fetches them from memory */
int drv_dcache_inv(u32 addr, u32 len);      /* drop data cache lines of this CPU (firmware code and data range only): 0, -1 out of range */
int drv_fseek(void *fp, u32 offset);        /* from the start of the file; 0 - done. Check with drv_ftell */
u32 drv_ftell(void *fp);
u32 drv_fflush(void *fp);

void drv_sleep(u32 ms);
/* Core task wait that hooks can cut short: drv_wait_init(1) makes the RTOS event flag (core task, once); drv_wait(ms) -
 * 1 if woken; drv_wake() - from a task on CPU 0, not under drv_lock. Without the flag drv_wait is drv_sleep */
int drv_wait_init(int on);
int drv_wait(u32 ms);
void drv_wake(void);
int drv_wait_state(void);                   /* > 0 the flag, 0 not made, < 0 given up */
/* Subprocessor clock, counts during sleep: seconds and 1/1024 s. 0, or -1 if not readable */
int drv_rtc(u32 *sec, u32 *frac1024);
u32 drv_ticks(void);                    /* microseconds, 30 bits, wraps every 1073 s */

/* Microseconds from an earlier drv_ticks() value to now, across the wrap */
static inline u32 drv_ticks_since(u32 then)
{
    return (drv_ticks() - then) & DRV_TICKS_MASK;
}
u32 drv_cpu(void);

/* Firmware lock: interrupts and task switching are disabled. Keep it short, no file calls */
u32 drv_lock(void);
void drv_unlock(u32 state);

/* Memory regions for core and modules: uncached addresses, visible to all CPUs, owned by nobody. 0 - no more regions */
int drv_mem_region(u32 i, void **base, u32 *size);
/* Firmware heap, classes in ascending block size: block size, block count, currently free. 0 - no more classes.
 * Allocation and release only from CPU 0; the memory is visible to all CPUs. core keeps the reserve for the firmware */
int drv_fwheap_class(u32 i, u32 *block, u32 *total, u32 *free);
void *drv_fwheap_alloc(u32 size);
void drv_fwheap_free(void *p);

u32 drv_setting_read(u32 id);           /* read-only access to the working value of a setting */

/* Startup timeline kept by the firmware: name up to 20 characters and time in drv_ticks. 0 - no more entries */
int drv_boot_event(u32 i, char *name, u32 *time);
int drv_crash_read(int cpu, omk_crash *out);   /* 1: a record exists */

/* Module code: core writes the image at an address from drv_mem_region, it executes at drv_exec_addr (on the camera, a
 * cached alias). drv_code_sync: after writing code, before the first call */
void *drv_exec_addr(void *p);
void drv_code_sync(void *exec, u32 size);

/* Events (omk_events.h): whether supported and which phases; install a hook. 0 - installed; 1 - unexpected bytes in place;
 * 2 - crosses a cache line; 3 - no such event */
int drv_ev_supported(u32 event, u32 *phases);
int drv_ev_install(u32 event);

/* jmp in firmware code to target. 0 - installed; 1 - unexpected bytes in place; 2 - crosses a cache line */
int drv_code_jump(u32 site, const u8 *before, void *target);

/* Power hooks (core/power.c): install hook i, its name in *name; the result as drv_code_jump,
 * -1 - no more hooks. The hooks call power_hook (omk_power.h) from firmware tasks.
 * drv_power_reason: name of a firmware power-off reason or 0 */
int drv_power_install(u32 i, const char **name);
const char *drv_power_reason(u32 reason);
/* Power-off already requested by the firmware (power lever accepted): 1 - yes. Read only, any time, CPU 0.
 * drv_power_probe: raw candidate words for the log (order in the driver), how many */
int drv_power_pending(void);
u32 drv_power_probe(u32 *w, u32 max);

/* Picture profiles (core/prof.c). Profiles are OMK_PROF_* of omk_api.h.
 * drv_prof_count: colour and mono profiles, 0 - none in this firmware. drv_prof_install: hooks, lists; 0 or a code.
 * drv_prof_set / drv_prof_get: the active profile for the UI and shooting, any task. drv_prof_base: 1 - the stored
 * picture mode is the base one (a profile may be shown over it). drv_prof_stats: counters and the last UI choice
 * (ui_seq changes when the user picks a profile or leaves it). drv_prof_service: core loop, about once a second:
 * puts back what the firmware restored; 0 ok, 1 put back, -1 foreign values in place (left alone) */
typedef struct {
    u32 ui_seq, ui_prof, reads, writes;
    u32 takes, applied, skip_mode, skip_cam, skip_bkt, other_cpu;
    u32 recalcs, alloc_sub, skip_alloc, skip_block, alloc_mode;
    u32 lv_mode, lv_reads, lv_applied;      /* live view: mode given to it, substitutions of the raw 111, copies */
    u32 ed_items, ed_renders, ed_cpu;       /* graphic editors: their item handler calls, renderer calls, its CPU */
    u32 ed_drawn, ed_failed;                /* frames drawn by core, left to the native code for a fault */
} drv_prof_info;
int drv_prof_count(u32 *colors, u32 *monos);
int drv_prof_install(int cache_inv);        /* cache_inv: drv_dcache_inv instead of the read eviction */
void drv_prof_set(u32 prof);
u32 drv_prof_get(void);
int drv_prof_base(void);
u32 drv_prof_stored_mode(void);            /* the stored picture mode as the driver reads it (for the log) */
void drv_prof_stats(drv_prof_info *st);
int drv_prof_service(void);
/* The film look (core/look.c, platform pictprof.c): a stock picture mode of the strip (the slot,
 * by name - drv_look_slots lists them) shows the look. drv_look_install: 0; -1 no such slot; 1..3 as drv_prof_install;
 * 4 the slot's column is not the one known; above 20 the UI pointers. drv_look_list: the entries' names (copied), the
 * chosen one: 0, -1 bad, -2 no memory. drv_look_stats: seq changes when the user turns the look on or off (ui_on), pick
 * when a dial chooses an entry (index). drv_look_active: any task on CPU 0 */
typedef struct {
    u32 seq, ui_on, active, pick, index, count, swallowed, swallow_kind, dial_last;
} drv_look_info;
int drv_look_install(int cache_inv, const char *slot);
const char *drv_look_slots(void);
u32 drv_look_mode(void);                    /* the slot's picture mode (the UI's number) */
void drv_look_set(int on);
int drv_look_active(void);
int drv_look_list(const char *const *names, u32 count, u32 current);
int drv_look_name(const char *text);        /* the slot's name in the UI: 0; -1 bad; -2 memory; -3 string table */
int drv_look_art(u32 index, const void *data, u32 size);   /* a checked CANI section: 0; -1 bad; -2 memory */
int drv_look_grain_labels(void);            /* 1 - the grain item's dial label pictures are in place */
int drv_look_labels(void);                  /* 1 - the look's dial label pictures are in place */
void drv_look_stats(drv_look_info *st);
u32 drv_look_trace(u32 *from, u32 *lost, u32 *t, u32 *w, u32 max);    /* the dial writes from *from: ticks, words */
/* The editors of the look's column (API 16): drv_look_edits - what to show (copied; 0, -1 bad, -2 not available);
 * drv_look_edit_take - the editors' events (OMK_EDIT_EVENT) since the last call, oldest first, at most max; *lost - the
 * ones that did not fit the driver's ring. Core task */
struct omk_look_edits;
int drv_look_edits(const struct omk_look_edits *e);
u32 drv_look_edit_take(u32 *ev, u32 max, u32 *lost);
/* The look's grain item (API 17): drv_look_grain_tiles - the tiles' names (copied), how many (0..OMK_GRAIN_TILES);
 * drv_look_grain - entry's tile (-1 none) and strength 0..3 to show; another entry than the UI shows: dropped (0).
 * 0, -1 bad, -2 not available */
int drv_look_grain_tiles(const char *const *names, u32 count);
int drv_look_grain(u32 entry, s32 tile, u32 level);
/* The look's optics rows (API 20, platform pictprof.c): drv_look_optics - entry's strengths (OMK_OPTICS_ROWS of
 * 0..OMK_OPTICS_MAX) to show; another entry than the UI shows: dropped (0). 0, -1 bad, -2 the rows are not available
 * (not installed on this firmware). Core task */
int drv_look_optics(u32 entry, const u8 *strength);
int drv_look_picture_size(u32 pic, u32 *w, u32 *h);
int drv_look_picture(u32 pic, u32 w, u32 h, const u8 *pixels);
/* The EXIF UserComment of the following pictures (S+0xf7c, written in the shooting parameter reader's filter): text up to
 * 117 printable characters, 0 - the camera's own. 0, -1 no shared state (neither profiles nor look installed).
 * drv_exif_count: pictures that got it */
int drv_exif_text(const char *t);
u32 drv_exif_count(void);
/* Graphic profile editors (platform editor.c): install (0 or a code), the core loop's check, counters for drv_prof_stats */
int drv_edp_install(void);
void drv_edp_service(void);
void drv_edp_stats(u32 *items, u32 *renders, u32 *cpu, u32 *drawn, u32 *failed);
void drv_edp_gaps(u32 *min, u32 *last, u32 *count);    /* film look: times between the item's frames (us) */
/* Loading profiles from outside (platform import.c): install (0 or a code); counters: cell lines, profile choices, lines
 * skipped (custom mode slots), the last profile chosen, names refused. drv_prof_choose: from the import */
int drv_imp_install(void);
void drv_imp_stats(u32 *lines, u32 *selects, u32 *skipped, u32 *last_prof, u32 *bad);   /* bad: names refused, the
                                                                                         component 0x106 not as expected */
void drv_prof_choose(u32 prof);
u32 drv_ev_other_cpu(u32 event);            /* event hook calls on CPUs 1 and 2, the original ran without handlers */
/* ISP colour tables (platform ispdrv.c): install the hooks (0 or the failed step); publish
 * an ISPB block (0 - stock again; 0 new tables, 1 a fast change, 2 the same, -1 bad block, -2 too soon, -3 no service);
 * census on/off; counters to the log */
int drv_isp_install(void);
int drv_isp_set(const u8 *block, u32 size);
void drv_isp_census(int on);
void drv_isp_scope(u32 scope);             /* OMK_ISP_LIVE | OMK_ISP_STILLS */
void drv_isp_service(u32 now);
/* The grain mask (core 1.21): the weight table of the mask pass (stage 0x900) of jobs with key `key`, built from w - the
 * weight 0..1023 by the picture's 8-bit Y after the look's tone (256 entries) - through the published tone (no tone: the
 * identity); 0 - off. 0, -2 too soon (40 ms between tables; again later), -3 no service. drv_isp_tone_seq changes when the
 * tone the table is built from changes: the core task builds it again then */
int drv_isp_grain_mask(u32 key, const u16 *w);
u32 drv_isp_tone_seq(void);
/* The look's grain (platform graindrv.c): the stills of the active film look get the grain film's work list with our tile
 * (a texture blend, case 1, with our values). Installed once, when the look's owner takes the service; the job key and
 * job type of the look's stills stay as installed. One tile slot: drv_grain_slot_begin - the slot may be written now
 * (0, *gen the generation it will be), -1 a still's fill reads it (later), -2 not installed; drv_grain_slot_ready - the
 * slot holds the tile now (side x side bytes at src in the arena, its integer mean, its name for the log): new stills
 * take generation gen; drv_grain_look_gain - the gain of input B of the next stills, 0 - no grain */
#define DRV_GRAIN_GAIN_MAX 2047             /* the blend's input B gain field, signed 12 bits at least */
#define DRV_GRAIN_GENS 16                   /* slot generations in turn: the low 4 bits of our texture marker */
#define DRV_GRAIN_TILE 1536                 /* the firmware's texture tile: 1536 x 1536, 1 byte a pixel */
#define DRV_GRAIN_TILE_MAX 63               /* tile values as in the firmware's own tile */
typedef struct {
    u32 key;                                /* the job key of the look's stills (ctx+52, h+11308), below 12 */
    u32 type;                               /* their job type (request B+14, P+16) */
} drv_grain_cfg;
int drv_grain_install(const drv_grain_cfg *cfg);   /* 0, or the step that failed */
void drv_grain_service(u32 now);
int drv_grain_slot_begin(u32 *gen);
void drv_grain_slot_ready(u32 gen, const u8 *src, u32 side, u32 mean, const char *name);
void drv_grain_look_gain(u32 gain);
/* The route of the next stills (core 1.21): DRV_GRAIN_GF - the grain film's work list (texture blend on the development,
 * stage 0x100); DRV_GRAIN_LIST - the monochrome profile's list (key 12: mask development 0x900, development 0x901) with the
 * texture built but no blend; DRV_GRAIN_MASK - that list, the texture blended on 0x901 with the mask as its weight. chroma:
 * the gain of the blend generator's noise in Cb and Cr, 0 - none. tag: a number the jobs keep (the bisection's level);
 * drv_grain_route_state - jobs with our texture released at the current tag since install, and alive with it now */
#define DRV_GRAIN_GF 0
#define DRV_GRAIN_LIST 1
#define DRV_GRAIN_MASK 2
#define DRV_GRAIN_CHROMA_MAX 1024
void drv_grain_route(u32 route, u32 chroma, u32 tag);
int drv_grain_route_state(u32 *freed, u32 *live);
/* The image memory allocator in its large layout (mode 1, as for the ART filters) or not: the allocator setup hook
 * (platform pictprof.c) installed if needed; a change asks the GUI task for a picture mode rebuild (at the next UI read of
 * 111 or write). 0, or a code. drv_alloc_mode: the mode now (any task, any CPU) */
int drv_alloc_large(int on);
u32 drv_alloc_mode(void);
void drv_prof_prepare(void);               /* core task: the active profile's values into the block the hooks copy */
int drv_prof_take_recent(void);             /* 1 - shooting parameters were built a moment ago: a picture is being made */
/* The live view context the image module uses now: read only */
typedef struct {
    u16 active, last;                       /* slot numbers 0..4 */
    u32 gen;                                /* A+892 */
    u8 mode, mode2, desc, filter, toning;   /* A+260, +261, +954, +263, +264 */
    u8 cc, mc, level, shading, grain;       /* A+705, +730, +731, +732, +733 */
    u8 sat, sharp, grad, contrast;          /* A+752, +753, +754, +756 */
    u16 s12[2];                             /* A+706, +708: the first two of the 12 hue saturations */
    u8 tone[3];                             /* A+792, +793, +794 */
} drv_lv_view;
int drv_prof_lv_read(drv_lv_view *v);       /* 0, or -1: no valid active slot */
/* Pictures of profiles (OMK_PIC_*): height and the largest width (0, or -1 none here); a picture in meaning values
 * (OMK_ICON_*, 4 bits per pixel) put into the firmware's picture table: 0, -1 bad arguments, -2 no memory, -3 the table is
 * not as expected. Mode icons must have the full width */
int drv_prof_picture_size(u32 pic, u32 *w, u32 *h);
int drv_prof_picture(u32 pic, u32 w, u32 h, const u8 *pixels);
int drv_prof_picture_set(u32 pic);          /* 1 - the module's picture is in place */
/* Monochrome profile filter matrix for filter 1..8, level 1..3 (9 x s16; 0 - the camera's own): 0, -1 no room, -3 table */
int drv_prof_mono_matrix(u32 filter, u32 level, const s16 *matrix);
/* The matrices set since the last call take effect for CPU 0 too (one cache push for all of them): core task, at every
 * loop (prof_service); nothing owed - nothing done */
void drv_prof_flush(void);
int drv_prof_mono_matrix_get(u32 filter, u32 level, s16 *matrix);   /* the camera's own, also while replaced */
/* The camera itself is using the card (the media busy flags its own power code waits for): core postpones its writes */
int drv_card_busy(void);

/* Kernel tasks and objects of the three CPUs for the task dump (core/taskdump.c). Read only, no
 * locks, core task only. The tasks of other CPUs are read past the cache: a word they wrote a moment ago may still be old.
 * drv_task_read: slot of cpu - 1 a task, 0 an empty slot, 2 the slot's TCB pointer is not RAM (t->sp has it), -1 no more
 * slots (or not the expected layout).
 * drv_task_current: id of the task cpu runs now, -1 unknown; for cpu 0 also from any task of CPU 0 (a few reads of that
 * CPU's kernel words, no state of the driver: the event manager names the caller with it). drv_obj_read: an object waited for, by its DRV_TW_* kind and id
 * (semaphore, event flag, mutex): 0, -1 kind not known to the driver, -2 no such object. drv_mutex_read: mutex slot i of
 * cpu: 1 exists, 0 empty, -1 no more.
 * drv_ui_probe: the application state and the raw input byte; DRV_UI_HALF or DRV_UI_TRANSIENT for the states that should
 * last a moment only, 0 another state, -1 not readable */
#define DRV_TASK_RETS 6
#define DRV_TASK_NAME 20
enum { DRV_TS_RUN = 1, DRV_TS_RDY = 2, DRV_TS_WAI = 4, DRV_TS_SUS = 8, DRV_TS_DMT = 16 };   /* uITRON 4.0 TTS_*: WAI | SUS both */
enum {                                      /* uITRON 4.0 TTW_*, as the kernel keeps them */
    DRV_TW_SLP = 1, DRV_TW_DLY = 2, DRV_TW_SEM = 4, DRV_TW_FLG = 8, DRV_TW_SDTQ = 0x10, DRV_TW_RDTQ = 0x20, DRV_TW_MBX = 0x40,
    DRV_TW_MTX = 0x80, DRV_TW_SMBF = 0x100, DRV_TW_RMBF = 0x200, DRV_TW_MPF = 0x2000, DRV_TW_MPL = 0x4000
};
enum {
    DRV_TF_UNSTABLE = 1,                    /* the words changed while they were read */
    DRV_TF_CTX = 2,                         /* the saved context is the dispatcher's: the return addresses are real frames */
    DRV_TF_FILL = 4,                        /* the stack has the fill pattern: never is known */
    DRV_TF_BADSTACK = 8                     /* stack base, size or saved sp out of range: not scanned */
};
typedef struct {
    u16 id, state, wait, wobj;              /* wait and wobj only while waiting; wobj 0 for sleep and delay */
    u16 pri, bpri;
    u8 cpu, wup, sus, flags;
    u32 tmo;                                /* ticks left + 1, 0 - no timeout */
    u32 wptn, wmode;                        /* event flag wait */
    u32 entry, stk, stksz, sp;
    u32 never;                              /* bytes at the stack bottom never written (DRV_TF_FILL) */
    u32 ret[DRV_TASK_RETS];                 /* return addresses above the saved sp, innermost first, kernel ones left out */
    char name[DRV_TASK_NAME];
} drv_task;
typedef struct {
    u16 id, kind;
    u16 owner, atr;                         /* mutex: owner task id (0 free); attribute byte of the object */
    u32 a, b;                               /* semaphore: count, max; flag: pattern; mutex: ceiling priority */
} drv_obj;
int drv_task_read(u32 cpu, u32 slot, drv_task *t);
int drv_task_current(u32 cpu);
int drv_obj_read(u32 kind, u32 id, drv_obj *o);
int drv_mutex_read(u32 cpu, u32 i, drv_obj *o);
enum { DRV_UI_HALF = 1, DRV_UI_TRANSIENT = 2 };
int drv_ui_probe(u32 *app, u32 *keys);

/* The debug service (core/debug.c, core 1.40).
 * drv_keep_area: memory that outlives a reset of the camera (not a power-off or the battery taken out), outside the
 * arena: uncached, written by tasks of CPU 0 only. 0 - none on this platform.
 * drv_ptp_add: a vendor operation of the USB picture transfer protocol (PTP) - code, data phase (0 none, 1 to the
 * computer, 2 from it), the handler - registered at every USB connection from then on, without a session; 0, -1 the table
 * is full or the code is there already. drv_ptp_install: the registration hook, once (0, or a code as drv_code_jump).
 * The handlers run in the firmware's PTP task (CPU 0, priority 55, about 1 KB of stack for them): short, no waits, no
 * card. drv_ptp_send: data to the computer, in pieces (the first one with the total length; pieces of odd length only
 * last), from a buffer that drv_ptp_buffer_ok accepts: 1 sent, 0 refused. drv_ptp_recv_len: the length of the data the
 * computer sends (once per operation of phase 2); drv_ptp_recv: len bytes of it into the buffer, 1 or 0.
 * drv_ptp_history: the firmware's own record of the last PTP operations, copied (bytes, 0 - none).
 * drv_mem_readable: where to read len bytes of addr, if that is memory a read cannot fault on (RAM; the cached alias is
 * turned into the uncached one), or 0 - not allowed.
 * drv_rename: a file in the same folder gets the new name (the target must not exist): 0 or the firmware's error */
#define DRV_PTP_PHASE_NONE 0
#define DRV_PTP_PHASE_OUT 1                 /* data to the computer */
#define DRV_PTP_PHASE_IN 2                  /* data from the computer */
typedef struct {
    u32 code, n, p[5];                      /* operation or response code, parameters used, parameters */
} drv_ptp_msg;
typedef void (*drv_ptp_fn)(const drv_ptp_msg *req, drv_ptp_msg *resp);
int drv_keep_area(void **base, u32 *size);
int drv_ptp_add(u32 code, u32 phase, drv_ptp_fn fn);
int drv_ptp_install(void);
int drv_ptp_buffer_ok(const void *buf);
int drv_ptp_send(const void *buf, u32 len, u32 total);
u32 drv_ptp_recv_len(void);
int drv_ptp_recv(void *buf, u32 len);
u32 drv_ptp_history(void *out, u32 max);
const volatile u8 *drv_mem_readable(u32 addr, u32 len);
int drv_rename(const char *from, const char *to);

/* The test scenario (core/shoot.c), core task only, never under drv_lock.
 * drv_shutter: the shutter button half (DRV_SHUTTER_HALF) or fully (DRV_SHUTTER_FULL) pressed (down 1) or released (0) -
 * by the button's own path (DRV_SHUTTER_KEYS: the key image of the key task, as a real press) or by the events of the
 * Wi-Fi remote (DRV_SHUTTER_WIFI); the firmware's answer (keys), 0 (wifi), -1 bad arguments.
 * drv_look_choose: as the user in the picture mode screen - entry `index` and the look on (on 1), or the look off (on 0);
 * core's look service tells the owner at its next loop. 0; -1 no look or a bad index; -4 the stored picture mode is not
 * the base one (as look_set). drv_look_find: the index of the entry of that name (case not minded), -1 none */
enum { DRV_SHUTTER_HALF = 0, DRV_SHUTTER_FULL = 1 };
enum { DRV_SHUTTER_KEYS = 0, DRV_SHUTTER_WIFI = 1 };
int drv_shutter(u32 key, int down, u32 path);
int drv_look_choose(int on, u32 index);
int drv_look_find(const char *name);

#endif
