/* The look's grain: the stills of the active film look take the work list of the firmware's own grain film ART filter
 * (Grainy Film), with our tile as its texture and our values in its blend. core (grain.c) owns the tile containers, loads
 * the chosen tile into one slot in the arena and publishes it here, with the gain of the strength chosen.
 *
 * The request B (R = *(ctx+4), built by 0x6038aa95 and the builders after it): +12 u16 the stage (0 unless a step's
 * callback sets it: 0x100 the texture's development, 0x101 the grain film's pass after it), +14 u16 the job type (P+16),
 * +16 u8 the kind (0 the development builder 0x6038aac0), +20..83 the output descriptor (ctx+24 points there), +28 u16 its
 * format: the class in the high byte (0x60372fdb takes 1, 3, 4 only, otherwise error 0x9E4C), +1838 u16 the effect code
 * (below 260: no case). ctx+52 the job key: below 12 the stock blend case comes from the effect code only.
 *
 * W 0x6038a531 - the planner selector 0x6038a4f0, where it reads the job key for P+8 0 or 2 (a2 = h): for a still of the
 *    look (its key and type, the look active, a gain not 0) W builds the texture through the firmware's builder
 *    0x603a3bdb with the stock parameters and our marker in byte +4 (stock: 1 or 3), puts it at h+14224, writes the grain
 *    film's key 0x01020068 to h+11308 for the time of the ART planner 0x6038c072 -> 0x6038d263 (it reads the key itself at
 *    several places and builds the grain film's still list: 0x6038d248, seed 0x6038c84a, development 0x60390f9a into the
 *    buffer 0x1010 with the callback 0x60390f21, pass 0x60391313 stage 0x101 into the output), then puts the job's key
 *    back: the passes keep the job's key (B+8 comes from P), the stock setup and cleanup gates do nothing for it, H3
 *    releases the texture. Only on CPU 0, where the stills are created (camera census); the slot is read under drv_lock.
 *    The builder waits for its own cross-CPU lock and sleeps: it runs in the creator task, as the stock call does.
 * H2 0x603a5314 - in the tile node builder 0x603a5167, after the firmware tile was read from flash into the buffer at
 *    (72,sp): with our marker in the record (44,sp), the buffer gets our tile through its uncached alias, repeated to
 *    1536 x 1536 with a random cyclic shift, before the node's DMA reads it.
 * H3 0x6038a5d0 - the job's cleanup gate (a0 = h): our texture released (0x603a452a), h+14224 = 0.
 * S 0x603ecda1 - the blend-case selector's entry (called as call ...,[d2,a2],12 from 0x6036fc5c, the pass's blend setup,
 *    and from 0x603d8112, the register image setup; a0 = ctx; the case in d0, returned by ret [d2,a2],12). The selector
 *    only reads (ctx, the request B, the effect code hash), so the trampoline calls the stock selector first and
 *    drv_grain_sel() filters its result: case 1 (the texture blend) for the passes of our jobs at the stage 0x100.
 * A 0x6036fc74 - in the pass's blend setup 0x6036fc58, after the register image was built (the case is not 0): d2 the case,
 *    (a2) = ctx, the blend block's image at (ctx+8)+7364. For our jobs A keeps the colour of input A and writes the tile
 *    mean offset and the gain of input B. The inputs are enabled after this place (0x60372fdb) from the mode word +4.
 * D 0x6036fbbb - the pass setup's check (call ...,[d2],4 from 0x6036f7a1; d0 back, retf [d2],4) whether the detail block
 *    0x1800 (sharpening, noise filter: 0x603dadfd, 0x60376337) is set up: for an output of class 1 or 2, but not at the
 *    stage 0x100 - so the grain film's development goes without it (noisier than Natural on the camera). For the
 *    development of our jobs D answers as for the stage 0: the class decides, the rest stays the grain film's.
 *
 * Every hook runs on any CPU in the firmware's job tasks: it touches only the arena block below (through its uncached
 * address), the job's own handle, ctx, request B and register image (cached, on the job's CPU, as the firmware does) and
 * the stack; the firmware only through the thunks of the calls the stock makes at the same places. No core variables, no
 * tables in .rodata (no switch). The core task reports.
 *
 * The mask route (core 1.21; DRV_GRAIN_LIST, DRV_GRAIN_MASK): W gives the job the monochrome profile's work list instead
 *    (key 12 for the time of the planner: the mask development 0x60390190, stage 0x900, into the 1-byte plane 0x10A0, and
 *    the development 0x603902a0, stage 0x901, whose callback 0x603903dd puts the plane's descriptor at B+532 - as for every
 *    stock monochrome still). The texture is built the same way but not attached by a step: on the 0x901 pass S gives case
 *    1, and A writes, over its image, the mode 0x00100003 (input B from B+468 and input C from B+532), the select word +132
 *    0 (input B from memory, weighted by C) and the texture's descriptor at B+468 (what the grain film's callback 0x60390f21
 *    writes: rec+8.., the address + (h+14216 & rec+92)). The mask's curve replaces the tone of the 0x900 pass (ispdrv.c).
 *    DRV_GRAIN_LIST: the same list without a case - the list alone. The select word: per channel 2 bits (Y bits 0-1, Cb
 *    2-3, Cr 4-5); byte 1 the source (0 memory, 1 the constant +120, 2 the generator), byte 2 the weight (2 the gain, 0 the
 *    gain x C, 1 the gain x (1 - C)) - read from the stock cases (31: generator weighted by its mask; 33, 18: lerp to a
 *    constant; 36: the texture in Y and the generator in Cb/Cr, +132 0x002A2824)
 *    The optics layer's step (core 1.33, opticsdrv.c), when [optics] probe is on, goes into the list right before that
 *    pass to the outputs.
 * Chroma (core 1.21): the generator in Cb and Cr of input B (+132 byte 1 0x28, byte 0 0x24 as case 36), its gain in the high
 *    half of +136 and in +140, a new seed +172 (bit 31 restarts it, as cases 32 and 34) a pass.
 *
 * One tile slot: the core task loads the chosen tile into its buffer and publishes it as a new generation (the entry,
 * DRV_GRAIN_GENS in turn: the marker in the texture parameters differs, so the firmware's texture cache never gives a job
 * a texture of the tile before). While the slot is being loaded (ready 0) a still gets no grain (counted, logged); the
 * core task does not start a load while a job's fill reads the slot (filling, under drv_lock). A job keeps the gain it
 * was created with (its row). The firmware's texture cache shares a texture between the jobs of one shutter press
 * (RAW+JPEG, a series) as for the stock grain film.
 *
 * The fill (H2) reads each row of the tile once: on CPU 0, where W calls the builder, the row is put together in a core
 * buffer reached through the cache (fill_row) and written from there (texfill.h); on another CPU, or with the buffer in
 * use, every word is read from the tile as before (counted, logged).
 * The rows (live[]): a job that never reaches the cleanup gate would keep its row for good, and four of them would end
 * the grain. W takes such a row back: by the handle (a new job at the handle of a row - the job before it there never
 * got to H3: the row is freed and its texture released, as H3 would have) and by age (no free row and one older than
 * TEX_AGE_US: the row is reused; its texture reference stays, see tex_take). Both are counted and logged. */
#include "omk_drv.h"
#include "omk_core.h"
#include "omk_log.h"
#include "omk_mem.h"
#include "texfill.h"

#define SEL_SITE 0x603ecda1
#define SET_SITE 0x6036fc74
#define CPUS 3
#define REPORT_MS 5000

#define CTX_B 4
#define CTX_IMAGE 8                         /* the pass's register image (pool slot 0x603e97d7) */
#define CTX_OUT 24                          /* the output descriptor: B+20 */
#define CTX_KEY 52
#define B_STAGE 12
#define B_TYPE 14
#define B_KIND 16
#define B_OUT 20
#define B_FORMAT 28
#define B_EFFECT 1838
#define EFFECT_CASES 260                    /* effect codes from 0x104 have cases (0x603ecf4c) */
#define IMG_BLEND 7364                      /* the blend block's image in the register image (0x603d80ec) */
#define BL_GAIN_A 92                        /* input A gains: low half Y, high half chroma; +96 the second chroma */
#define BL_GAIN_A2 96
#define BL_OFFSET_B 124                     /* input B offset before the gain: low half Y, high half chroma (case 35:
                                               0x0000FFE6 for its texture, the generators -128 for their centre) */
#define BL_GAIN_B 136                       /* input B gain: low half Y, high half chroma (by input A's layout) */
#define BL_GAIN_B2 140                      /* input B gain, the second chroma */
#define BL_MODE 4                           /* bit 0 input B from memory, bit 1 input C, bit 20 weights by C (0x60372fdb) */
#define BL_SELECT_B 132                     /* input B's select word (see above); case 1: 0x002A0000 */
#define BL_SEED 172                         /* the generator's seed, bit 31: start again (cases 32, 34) */
#define MODE_MASK 0x00100003                /* inputs B and C, weights by C (stock cases 10 and 19) */
#define SELECT_MASK 0x00000000              /* input B from memory, weight gain x C in Y, Cb, Cr (case 31: 0x00002A00) */
#define SELECT_CHROMA 0x00002824            /* byte 1: Y from memory, Cb and Cr from the generator; byte 0 as case 36 */
#define SELECT_BYTES01 0x0000ffff
#define B_TEXTURE 468                       /* input B's descriptor (16 words) for an output of class 1 (0x60372fdb) */
#define REC_DESC 8                          /* the texture record: its descriptor (16 words), +92 the shift mask */
#define REC_SHIFT 92
#define GAIN_COLOUR 0x04000400              /* +92 / +96 of the cases that keep the colour (32, 35) */
#define GUARD_LO 0x60000200u                /* the firmware range the memory guard watches for writes */
#define GUARD_HI 0x60cbe800u

/* The job handle h (job creator 0x6038958c): P = h+4 */
#define H2_SITE 0x603a5314
#define H3_SITE 0x6038a5d0
#define W_SITE 0x6038a531
#define D_SITE 0x6036fbbb
#define H_P 4
#define H_REQ 8                             /* P+4: the record's request C (0x6041e8dc: P+4 = a0) */
#define H_P8 12                             /* P+8: the gates go on only for 0 and 2 */
#define H_TYPE 20                           /* u16 P+16, the job type (= B+14) */
#define H_P18 22                            /* P+18 = C+283 ImgAspect; P+19 the same, 1 for RAW Edit (C+194 = 0x1003) */
#define H_P19 23
#define H_P20 24                            /* P+20, P+21, P+30: the stock texture parameters (0x6038e67c) */
#define H_P21 25
#define H_P22 26                            /* P+22 not 0: multi-frame jobs (0x60394493), not ours */
#define H_P30 34
#define H_SIZE 15484                        /* the handle's size (0x6038958c) */
#define H_STEPS 10984                       /* 16 x {fn, callback, ctx} (0x603a1708 fills it, 0x603895ec zeroes it) */
#define H_NSTEPS 11314                      /* u16 the number of steps */
#define H_KEY 11308
#define H_STATE 11312                       /* u16 h+11312, h+11316, h+11318: the job's state words (0x60389f71 and the
                                               steps write them; h+11318 = 1 from the output pass 0x6038c7aa) - logged
                                               at H3, read only */
#define H_B 11364                           /* request B (the B builders return h+11364) */
#define H_TEX 14224                         /* the texture record the grain film's callback attaches */
#define H_SHIFT 14216                       /* the texture's shift (the seed steps; 0 for a still) */
#define C_MODE 194                          /* C: u16 Mode, u8 RawJpeg, u8 FileType - names from rec_imgpro.c's dump
                                               0x6041dd00..0x6041e2c0; the log only */
#define C_RAWJPEG 293
#define C_FILETYPE 298
#define STEPS 16
#define STEP_ATTACH 0x60390f21              /* the grain film's callback: B+468.. = the texture, B+12 = 0x100 */
#define STEP_GF_DEV 0x60390f9a              /* the grain film's development step (0x6038d7a5 gives it STEP_ATTACH) */
#define STEP_GF_101 0x60391313              /* the grain film's pass after it: 0x1010 -> the output through 0x6038c50f,
                                               kind 3, stage 0x101 (0x60391384); the list of 0x6038d3ec..0x6038d4b6 */
#define STEP_GF_101B 0x603911b5             /* the same pass in the handler 0x6038d7a5's list: 0x1010 -> 0x1000 */
#define GF_KEY 0x01020068                   /* Grainy Film II: its handler 0x6038d7a5 (GF I 0x01010068 the same) */
#define MASK_KEY 12                         /* the monochrome profile: its builder 0x6038db19 gives a still (classes 0..3)
                                               the mask pass and the development 0x901, whatever the grain level */
#define STEP_MASK 0x60390190                /* stage 0x900, the plane 0x10A0 (format 0x304) */
#define STEP_M901 0x603902a0                /* stage 0x901, input C = 0x10A0 (callback 0x603903dd) */
#define STEP_OUT 0x60391b23                 /* the plain pass 0x1000 -> the job's outputs: 0x60391f1f (the buffer 0x1000,
                                               then 0x6038c50f, as the grain film's last step 0x60391313 does for 0x1010)
                                               with the callback 0x60391516, which does nothing. The ART planner's own
                                               last step after 0x6038cfa5 (P+28 not 0, 0x6038dfb6) */
#define STEP_M_OUT 0x60391b8f               /* the same pass with the shading callback 0x60391d48: the stock last step
                                               of the monochrome list, from the job flag 0x80000 (0x6038e0a2) */
#define LIST_DEV 1                          /* the list the planner built (ring detail) */
#define LIST_101 2
#define LIST_MASK 4
#define LIST_901 8
#define LIST_OUT 16                         /* a step that writes the job's outputs follows the development 0x901 */
#define LIST_PROBE 32                       /* the optics layer's step before it (opticsdrv.c) */
#define TEX_STAGE 0x100
#define MASK_STAGE 0x901
#define TEX_MARK 0x80                       /* byte +4 of the parameters: 0x6038c0c1 gives 1 or 3, and the sizes do not
                                               read it (0x603a5167): ours is 0x80 + the generation */
#define CASE_TEX 1
#define F_REC 44                            /* H2: the record, at the node builder's sp */
#define F_OUT 8                             /* H2: the texture's descriptor pointer (record + 8), its +0 the buffer */
#define F_DESC 72                           /* the tile descriptor: +0 the data, +8 the format, +10..+14 the sizes,
                                               +64 the allocation, +72 its length */
#define F_FORMAT 0x304                      /* 1 byte a pixel */
#define TILE DRV_GRAIN_TILE
#define TILE_BYTES (TILE * TILE)
#define DIRECT_LO 0x60000000u               /* the CPU's direct DRAM window, cached; uncached + DRV_UNCACHED (0xa...) */
#define DIRECT_HI 0x70000000u               /* 0xb0000000 up: bus addresses behind a window register (not mapped here) */
#define TEX_LIVE 4                          /* our textures at once: the firmware's texture cache has 4 records */
#define FILL_KEEP 4                         /* fills remembered a generation: one for each record of that cache (the
                                               records at 0xa26ed764, 100 bytes each), so none is ever pushed out */
#define TEX_AGE_US 300000000u               /* 300 s: a row older than this belongs to a job that is gone - a still's job
                                               ends within seconds of W (the log's "W to H3"). Below the 30-bit wrap of
                                               drv_ticks (1073 s): a row leaked longer ago looks young for part of every
                                               wrap, and is then taken back by a later shot */
#define TEX_RING 16                         /* shots between two reports */

/* What became of a shot (ring), per-CPU counters, the gates of S and A, H2 errors */
enum { T_BUILT = 1, T_SHARED, T_P8, T_P22, T_BUSY, T_GUARD, T_ALLOC, T_FULL, T_BUILDER, T_FILL, T_NOFILL, T_P21,
       T_NOTREADY };
enum { TC_JOBS, TC_TYPE, TC_CPU, TC_BUILT, TC_SHARED, TC_FAILED, TC_SKIPPED, TC_FILLS, TC_FILLERR, TC_FREED, TC_GAVE,
       TC_WROTE, TC_DETAIL, TCN };
enum { TG_KIND, TG_TYPE, TG_EFFECT, TG_CLASS, TG_DESC, TG_LIVE, TG_GUARD, TG_ROUTE, TG_OK };
enum { FE_DESC = 1, FE_ALIGN, FE_BOUNDS, FE_RANGE, FE_NOSRC };

#define DONE_RING 4

/* A fill of a generation's texture (H2) for W, by the firmware's record it filled. One generation can have several
 * textures alive at once - the cache keys them by the job's parameters too (type, P+20, P+21, P+30), so two jobs of one
 * generation with different parameters get two records: each keeps its own fill here (one "last fill" a generation made
 * the first of them look not filled as soon as the second was built: no grain on the jobs that shared the first) */
typedef struct {
    u32 dx, dy, t_fill, err;                /* the shift, the time (us), FE_* */
    u32 addr, alloc, out;                   /* the tile's data and allocation, the texture's buffer (addresses) */
    u32 rec;                                /* the record, written last; 0 - free */
} tex_fill_t;

typedef struct {
    u32 src, side, mean;                    /* the slot's tile of this generation (src 0 - none) */
    u32 builds;                             /* H2: fills of this generation's textures */
    u32 fill_next;                          /* H2: the place of the next record not seen yet */
    tex_fill_t fill[FILL_KEEP];
} tex_entry_t;

typedef struct {
    u32 h, rec, entry;                      /* h 0 - free. Taken by W (CPU 0, drv_lock), freed by H3 or W */
    u32 gain;                               /* the job's gain of input B (+136), set before h */
    u32 key, ring;                          /* the job's own key; its shot number (the ring entry W completes) */
    u32 route, chroma, tag;                 /* DRV_GRAIN_*, the chroma gain and the tag the job was created with */
    u32 t0;                                 /* drv_ticks at W: the job's time to H3 */
} tex_live_t;

typedef struct {
    u32 seq;                                /* shot number + 1, written last; 0 - being written */
    u32 h, type, entry, what, detail, rec, dx, dy, t_build, t_fill, addr, alloc, out;
    u32 mode;                               /* C+194 Mode | C+293 RawJpeg << 16 | C+298 FileType << 24 (0 - no C) */
    u32 pf;                                 /* P+18 | P+19 << 8 | P+21 << 16 */
    u32 gain;
    u32 route, chroma, tag;
} tex_shot_t;

typedef struct {
    u32 key, type;                          /* the look's stills: job key and job type (drv_grain_cfg), set before the
                                               hooks */
    u32 shots, rng;                         /* shots counted (W, CPU 0); H2's random state */
    tex_entry_t e[DRV_GRAIN_GENS];
    tex_live_t live[TEX_LIVE];
    tex_shot_t ring[TEX_RING];
    u32 cnt[CPUS][TCN];
    u32 skip[CPUS][TG_OK];                  /* S and A: case 1 not given / not written, by the gate */
    /* The slot's generation (the entry), whether it is loaded, the gain of new jobs (0 - no grain), fills reading the
     * slot now. The core task writes, W reads, under drv_lock */
    u32 active, ready, gain, filling;
    u32 route, chroma, tag;                 /* of new jobs; the core task writes, W reads under drv_lock */
    u32 seed;                               /* the generator's seed counter (A, any CPU: a lost step is harmless) */
    u32 freed[CPUS];                        /* H3: our textures released with the current tag */
    u32 t_job[CPUS], t_job_max[CPUS];       /* H3: the last and the longest job, W to H3, us */
    struct {
        u32 seq;                            /* the CPU's count of our jobs ended + 1, written last; 0 - being written */
        u32 shot, steps, s12, s16, s18, us;
    } done[CPUS][DONE_RING];                /* H3: our jobs as they end, for the log (the row of each CPU its own) */
    u32 done_n[CPUS];
    u32 row_busy;                           /* H2 on CPU 0: fill_row is in use (taken and given back under drv_lock) */
    u32 direct;                             /* H2: fills that read every word from the tile (not CPU 0, the buffer in
                                               use, a side texfill.h does not take) */
    u32 same_h, aged;                       /* W: rows of jobs that never reached H3 taken back, by the handle and by age */
} tex_t;

/* Only through slot(): the uncached alias; word 1 - the arena block */
static u32 grain_slot[16] __attribute__((aligned(64), section(".data.cpu_shared")));
static int install_rc = -1;                 /* the result of the one install (core task); -1 not tried */
static const char *tex_names[DRV_GRAIN_GENS];   /* core task only */
/* One row of the texture being filled (texfill.h). A core variable, reached through the cache, in a hook - allowed here
 * only because its one user is H2 on CPU 0 (checked there), the CPU whose cache the core image lives in: the fill runs
 * in the job creator's task inside the firmware's texture builder, which W calls on CPU 0, and tasks stay on their CPU.
 * One fill at a time: the builder holds the firmware's cross-CPU texture lock while the node is built, and row_busy
 * makes sure of it. 1536 bytes: 96 of the data cache's 512 lines */
static u32 fill_row[TEX_FILL_ROW];
typedef char tex_fill_tile_is_the_firmware_s[TEX_FILL_TILE == DRV_GRAIN_TILE ? 1 : -1];

static volatile u32 *slot(void)
{
    return (volatile u32 *)((u32)grain_slot + DRV_UNCACHED);
}

static volatile tex_t *tex(void)
{
    return (volatile tex_t *)slot()[1];
}

void drv_tramp_grain_sel(void);             /* thunks.s */
void drv_tramp_grain_set(void);
void drv_tramp_grain_h2(void);
void drv_tramp_grain_h3(void);
void drv_tramp_grain_wl(void);
void drv_tramp_grain_det(void);
u32 drv_optics_list(u8 *h, int room);       /* opticsdrv.c: the optics layer's step for a list, 0 - none */
void drv_optics_ended(u8 *h);               /* opticsdrv.c: our job ended (its substage counted) */
u32 drv_fw_tex_build(u8 *p, const u16 *params);    /* thunks.s: 0x603a3bdb, the record or 0 */
void drv_fw_tex_release(u8 *p, u32 rec);           /* thunks.s: 0x603a452a */

static u32 cpu_now(void)
{
    u32 c = drv_cpu();

    return c < CPUS ? c : 0;
}

/* The texture blend's gates, the same in S and A: -1 - not a pass of our routes (another key or stage, not counted); a
 * TG_* gate that failed; TG_OK - one of our jobs with its texture, *row its row. The stage is the route's: 0x100 on the
 * grain film's list, 0x901 on the mask route; the list alone (DRV_GRAIN_LIST) has none */
static int gate_tex(volatile tex_t *t, const u8 *ctx, const u8 *B, u32 key, u32 *row)
{
    u32 i, v, cls, h, st, route;

    st = *(const u16 *)(B + B_STAGE);
    if (key != t->key || (st != TEX_STAGE && st != MASK_STAGE))
        return -1;
    h = (u32)B - H_B;
    for (i = 0; i < TEX_LIVE && !(t->live[i].h == h && t->live[i].rec); i++)
        ;
    if (i == TEX_LIVE)
        return st == TEX_STAGE ? TG_LIVE : -1;     /* key 2 at 0x901: only ours (W); not one of them now - not counted */
    route = t->live[i].route;
    if (st != (route == DRV_GRAIN_GF ? TEX_STAGE : route == DRV_GRAIN_MASK ? MASK_STAGE : 0))
        return TG_ROUTE;
    if (B[B_KIND] != 0)
        return TG_KIND;
    v = *(const u16 *)(B + B_TYPE);
    if ((v & 0xf0) == 0x40 || (v & 0xf00) == 0x100 || v != t->type)   /* as the job creator drops the ART flags
                                                                        (0x60389872) */
        return TG_TYPE;
    if (*(const u16 *)(B + B_EFFECT) >= EFFECT_CASES)
        return TG_EFFECT;
    cls = *(const u16 *)(B + B_FORMAT) >> 8;
    if (cls != 1 && cls != 3 && cls != 4)
        return TG_CLASS;
    if (route == DRV_GRAIN_MASK && cls != 1)
        return TG_CLASS;                    /* A puts input B at B+468: the place for an output of class 1 */
    if (*(const u32 *)(ctx + CTX_OUT) != (u32)(B + B_OUT))   /* the class above is the one 0x60372fdb reads */
        return TG_DESC;
    *row = i;
    return TG_OK;
}

/* S (thunks.s): the stock case of this call -> the case to return. Case 1 only where the stock gives none */
u32 drv_grain_sel(u8 *ctx, u32 stock)
{
    volatile tex_t *t = tex();
    const u8 *B;
    u32 cpu, row;
    int k;

    if (!t || !ctx || (stock & 0xff) != 0)
        return stock;
    B = *(const u8 **)(ctx + CTX_B);
    if (!B)
        return stock;
    cpu = cpu_now();
    k = gate_tex(t, ctx, B, *(const u32 *)(ctx + CTX_KEY), &row);
    if (k == TG_OK) {
        t->cnt[cpu][TC_GAVE]++;
        return CASE_TEX;
    }
    if (k >= 0)
        t->skip[cpu][k]++;
    return stock;
}

/* A (thunks.s): after the blend setup of a pass with a case */
void drv_grain_set(u8 *ctx, u32 kase)
{
    volatile tex_t *t = tex();
    volatile tex_live_t *l;
    u8 *B, *img;
    const u32 *src;
    u32 cpu, e, gn, row = 0, i, sel, ch, *dst;
    int k;

    if (!t || !ctx || (kase & 0xff) != CASE_TEX)   /* the setup tests the low byte (0x6036fc65) */
        return;
    B = *(u8 **)(ctx + CTX_B);
    img = *(u8 **)(ctx + CTX_IMAGE);
    if (!B || !img)
        return;
    k = gate_tex(t, ctx, B, *(const u32 *)(ctx + CTX_KEY), &row);
    if (k < 0)
        return;                             /* a stock case 1 (the grain film itself): left as it is */
    cpu = cpu_now();
    img += IMG_BLEND;
    if (k == TG_OK && (u32)img >= GUARD_LO && (u32)img < GUARD_HI)
        k = TG_GUARD;                       /* never seen; a cached write there would stop the camera */
    l = &t->live[row];
    e = l->entry;
    if (k != TG_OK || e >= DRV_GRAIN_GENS) {
        t->skip[cpu][k == TG_OK ? TG_LIVE : k]++;
        return;
    }
    gn = l->gain;
    ch = l->chroma;
    /* the handler 0x603d8145 left input A at Y only (+92 1024, +96 0: the grain film is black and white), the offset
     * of input B at 0 (the texture's mean would brighten the picture) and its gain at 1024 */
    *(u32 *)(img + BL_GAIN_A) = GAIN_COLOUR;
    *(u32 *)(img + BL_GAIN_A2) = GAIN_COLOUR & 0xffff;
    if (gn)                                 /* gain 0: the firmware's offset (0xFF800000) stays, input B adds nothing */
        *(u32 *)(img + BL_OFFSET_B) = 0xff800000u | ((0u - t->e[e].mean) & 0xffff);
    *(u32 *)(img + BL_GAIN_B) = gn | ch << 16;
    sel = *(const u32 *)(img + BL_SELECT_B);
    if (l->route == DRV_GRAIN_MASK) {
        /* input B: the texture, as the grain film's callback 0x60390f21 attaches it (B is inside the handle W checked
         * against the guarded range); input C at B+532: the mask plane, put there by the step's callback 0x603903dd */
        src = (const u32 *)(l->rec + REC_DESC);
        dst = (u32 *)(B + B_TEXTURE);
        for (i = 0; i < 16; i++)
            dst[i] = src[i];
        dst[0] = src[0] + (*(const u32 *)(B - H_B + H_SHIFT) & *(const u32 *)(l->rec + REC_SHIFT));
        *(u32 *)(img + BL_MODE) = MODE_MASK;
        sel = SELECT_MASK;
    }
    if (ch) {                               /* the generator in Cb and Cr: offset -128 (+124 high half, +128) as set */
        sel = (sel & ~SELECT_BYTES01) | SELECT_CHROMA;
        *(u32 *)(img + BL_GAIN_B2) = ch;
        *(u32 *)(img + BL_SEED) = 0x80000000u | ++t->seed;
    }
    *(u32 *)(img + BL_SELECT_B) = sel;
    t->cnt[cpu][TC_WROTE]++;
}

/* D (thunks.s), the pass setup's check 0x6036fbbb, any CPU: 0 - the firmware's check; 1 + the answer - the development
 * (kind 0, stage 0x100) of our job: the detail block as for the stage 0, by the output's class (1 or 2) as the check
 * itself. Reads the arena block and the job's ctx and request B */
u32 drv_grain_detail(u8 *ctx)
{
    volatile tex_t *t = tex();
    const u8 *B, *out;
    u32 i, h, cls;

    if (!t || !ctx)
        return 0;
    B = *(const u8 **)(ctx + CTX_B);
    if (!B || *(const u16 *)(B + B_STAGE) != TEX_STAGE || B[B_KIND] != 0 || *(const u32 *)(ctx + CTX_KEY) != t->key)
        return 0;                           /* the grain film's own development and every other pass: the firmware's */
    h = (u32)B - H_B;
    for (i = 0; i < TEX_LIVE && !(t->live[i].h == h && t->live[i].rec); i++)
        ;
    if (i == TEX_LIVE)
        return 0;
    out = *(const u8 **)(ctx + CTX_OUT);
    cls = out ? *(const u16 *)(out + 8) >> 8 : 0;
    t->cnt[cpu_now()][TC_DETAIL]++;
    return 1 + (cls == 1 || cls == 2);
}

/* The job's checks before anything is taken: 0, or the T_* reason. As the firmware's grain film: every still job of the
 * look's type, the two of RAW+JPEG, RAW Edit (P+19 is C+283 ImgAspect, 1 also for RAW Edit) - except P+21 2, whose
 * grain film list blends on a kind 3 pass */
static u32 tex_check(const u8 *h)
{
    if ((u32)h + H_SIZE > GUARD_LO && (u32)h < GUARD_HI)
        return T_GUARD;                     /* never seen; a cached write there would stop the camera */
    if (h[H_P8] != 0 && h[H_P8] != 2)
        return T_P8;
    if (h[H_P22])
        return T_P22;
    if (*(const u32 *)(h + H_TEX))
        return T_BUSY;
    if (h[H_P21] == 2)
        return T_P21;                       /* the firmware's list for it (0x60390f9a, 0x6038ca86, 0x603910c6) attaches the
                                               texture to a kind 3 pass (0x603910d5): case 1 there needs the grain film's key */
    if (drv_alloc_mode() != 1)
        return T_ALLOC;                     /* the large layout is not set up: 40.5 MB might never come */
    return 0;
}

/* W, CPU 0, the job creator's task: a row and our texture for the job h (key and type already checked). The record, its
 * row in *slot_out and its ring entry in *seq_out, or 0 - nothing taken, the ring entry complete. With a record the ring
 * entry stays open: drv_grain_wl_after completes it (r->seq last). Inlined: one C frame a hook */
static inline __attribute__((always_inline)) u32 tex_take(volatile tex_t *t, u8 *h, u32 type, u32 *slot_out,
                                                          u32 *seq_out)
{
    volatile tex_shot_t *r;
    volatile tex_entry_t *en;
    volatile tex_fill_t *f = 0;
    u32 slot, e = 0, seq, st, what, rec, t0, builds, i, mode, gain = 0, fill = 0, route = 0, chroma = 0, tag = 0;
    u32 stale = 0, age, oldest;
    u16 par[3];

    what = tex_check(h);
    i = *(const u32 *)(h + H_REQ);          /* the record's request C: Mode, RawJpeg, FileType (0 - no C) */
    mode = (i >= DIRECT_LO && i < DIRECT_HI - 512) || (i >= DIRECT_LO + DRV_UNCACHED && i < DIRECT_HI + DRV_UNCACHED - 512) ?
           *(const u16 *)(i + C_MODE) | (u32)*(const u8 *)(i + C_RAWJPEG) << 16 | (u32)*(const u8 *)(i + C_FILETYPE) << 24 : 0;
    st = drv_lock();
    /* A row at this handle: the creator makes a new job here, so the job the row was taken for is gone and never came
     * to H3. The row is freed whatever becomes of the new job (the gates find a job's row by its handle: the old row
     * would pass for the new job's), and its texture reference released below, as H3 would have done with this P */
    for (i = 0; i < TEX_LIVE; i++)
        if (t->live[i].h == (u32)h) {
            stale = t->live[i].rec;
            t->live[i].rec = 0;
            t->live[i].h = 0;
            t->same_h++;
            break;                          /* one row a handle: every take frees the handle's row first */
        }
    slot = TEX_LIVE;
    if (!what) {
        for (slot = 0; slot < TEX_LIVE && t->live[slot].h; slot++)
            ;
        if (slot == TEX_LIVE) {
            /* No free row. The oldest complete row past TEX_AGE_US is a job that never ended: its row is used again.
             * Its texture reference is not released here: the release takes the job's P on the last reference
             * (0x603a452a passes it to 0x60386990), and that job's handle may be anything by now. The firmware's
             * record stays referenced; a job of the same generation and parameters shares it as before */
            for (i = 0, oldest = 0; i < TEX_LIVE; i++) {
                age = drv_ticks_since(t->live[i].t0);
                if (t->live[i].rec && age >= TEX_AGE_US && age >= oldest) {
                    oldest = age;
                    slot = i;
                }
            }
            if (slot < TEX_LIVE) {
                t->live[slot].rec = 0;
                t->live[slot].h = 0;
                t->aged++;
            } else {
                what = T_FULL;
            }
        }
    }
    if (!what) {
        e = t->active;                      /* the slot's generation */
        gain = t->gain;
        route = t->route;
        chroma = t->chroma;
        tag = t->tag;
        if (!t->ready || e >= DRV_GRAIN_GENS || !gain)
            what = T_NOTREADY;
        else
            fill = 1;
        t->filling += fill;                 /* the core task leaves the slot alone until the fill below is done */
    }
    if (!what) {
        t->live[slot].entry = e;
        t->live[slot].rec = 0;
        t->live[slot].gain = gain;
        t->live[slot].route = route;
        t->live[slot].chroma = chroma;
        t->live[slot].tag = tag;
        t->live[slot].t0 = drv_ticks();
        t->live[slot].h = (u32)h;
    }
    seq = t->shots++;
    drv_unlock(st);
    if (stale)                              /* outside the lock: the release waits for the firmware's texture lock */
        drv_fw_tex_release(h + H_P, stale);
    r = &t->ring[seq & (TEX_RING - 1)];
    r->seq = 0;
    r->h = (u32)h;
    r->type = type;
    r->entry = e;
    r->pf = h[H_P18] | h[H_P19] << 8 | h[H_P21] << 16;
    r->mode = mode;
    r->gain = gain;
    r->route = route;
    r->chroma = chroma;
    r->tag = tag;
    r->detail = what == T_ALLOC ? drv_alloc_mode() : 0;
    r->rec = r->dx = r->dy = r->t_build = r->t_fill = r->addr = r->alloc = r->out = 0;
    if (what) {
        t->cnt[0][TC_SKIPPED]++;
        r->what = what;
        r->seq = seq + 1;
        return 0;
    }
    en = &t->e[e];
    par[0] = type;                          /* the stock parameters (0x6038e67c..0x6038e6a6), our marker at +4 */
    par[1] = h[H_P21] | h[H_P20] << 8;
    par[2] = (TEX_MARK + e) | h[H_P30] << 8;
    builds = en->builds;
    t0 = drv_ticks();
    rec = drv_fw_tex_build(h + H_P, par);
    r->t_build = drv_ticks_since(t0);
    st = drv_lock();                        /* H2 has run inside the builder (or the cache gave a built texture) */
    t->filling--;
    drv_unlock(st);
    if (rec)                                /* the fill of this record: H2's, just now or for a job before (shared) */
        for (i = 0; i < FILL_KEEP && !f; i++)
            if (en->fill[i].rec == rec)
                f = &en->fill[i];
    if (!rec) {
        what = T_BUILDER;
    } else if (!f || f->err) {
        what = !f ? T_NOFILL : T_FILL;
        r->detail = f ? f->err : 0;
        r->addr = f ? f->addr : 0;
        r->alloc = f ? f->alloc : 0;
        r->out = f ? f->out : 0;
        drv_fw_tex_release(h + H_P, rec);   /* the firmware's tile with our marker: not on a picture */
        rec = 0;
    } else {
        what = en->builds != builds ? T_BUILT : T_SHARED;
        r->dx = f->dx;
        r->dy = f->dy;
        r->t_fill = f->t_fill;
        r->addr = f->addr;
        r->alloc = f->alloc;
        r->out = f->out;
    }
    if (rec) {
        t->live[slot].ring = seq;
        t->live[slot].rec = rec;
        t->cnt[0][what == T_BUILT ? TC_BUILT : TC_SHARED]++;
    } else {
        t->live[slot].h = 0;
        t->cnt[0][TC_FAILED]++;
    }
    r->rec = rec;
    r->what = what;
    if (!rec)
        r->seq = seq + 1;
    *slot_out = slot;
    *seq_out = seq;
    return rec;
}

/* Key, type and CPU of a job for W: the type, or 0 - not ours (counted when it is the look's key) */
static u32 tex_job(volatile tex_t *t, const u8 *h, u32 *cpu)
{
    u32 type;

    if (*(const u32 *)(h + H_KEY) != t->key)
        return 0;
    if (!t->gain || !drv_look_active())
        return 0;                           /* not the look's still, or its strength is Off */
    *cpu = cpu_now();
    type = *(const u16 *)(h + H_TYPE);
    if (!type || type != t->type) {
        t->cnt[*cpu][TC_TYPE]++;
        return 0;
    }
    t->cnt[*cpu][TC_JOBS]++;
    if (*cpu) {
        t->cnt[*cpu][TC_CPU]++;             /* the slot and the ring are kept under drv_lock of CPU 0 */
        return 0;
    }
    return type;
}

/* W (thunks.s), the job creator's task, before the planner: 1 - a still of the look with its texture at h+14224, the
 * grain film's key at h+11308 until drv_grain_wl_after; 0 - the stock planner */
u32 drv_grain_wl(u8 *h)
{
    volatile tex_t *t = tex();
    u32 type, cpu, slot, seq, rec;

    if (!t || !h || !(type = tex_job(t, h, &cpu)))
        return 0;
    rec = tex_take(t, h, type, &slot, &seq);
    if (!rec)
        return 0;                           /* the grain film's callback would read h+14224 = 0: the job's own list */
    t->live[slot].key = *(const u32 *)(h + H_KEY);
    if (t->live[slot].route == DRV_GRAIN_GF) {
        *(u32 *)(h + H_TEX) = rec;          /* what the grain film's setup 0x6038e6b1 does; the seed step clears h+14216 */
        *(u32 *)(h + H_KEY) = GF_KEY;       /* the ART planner picks the grain film's list by it */
    } else {
        *(u32 *)(h + H_KEY) = MASK_KEY;     /* the monochrome profile's list; its steps read neither h+14224 nor the key */
    }
    return 1;
}

/* A step at entry k of the job's list; +4 and +8 are 0 (the creator zeroes the table, 0x603895ec, and writes them so) */
static void put_step(u8 *h, u32 k, u32 fn)
{
    *(u32 *)(h + H_STEPS + 12 * k) = fn;
    *(u32 *)(h + H_STEPS + 12 * k + 4) = 0;
    *(u32 *)(h + H_STEPS + 12 * k + 8) = 0;
}

/* W after the ART planner: the job's key back, the list it built checked (in the ring entry W left open) */
void drv_grain_wl_after(u8 *h)
{
    volatile tex_t *t = tex();
    volatile tex_shot_t *r;
    u32 i, n, k, list = 0;

    if (!t || !h)
        return;
    for (i = 0; i < TEX_LIVE && t->live[i].h != (u32)h; i++)
        ;
    if (i == TEX_LIVE)
        return;                             /* never: W took a row before it returned 1 */
    *(u32 *)(h + H_KEY) = t->live[i].key;
    n = *(const u16 *)(h + H_NSTEPS);
    for (k = 0; k < n && k < STEPS; k++) {
        if (*(const u32 *)(h + H_STEPS + 12 * k) == STEP_GF_DEV && *(const u32 *)(h + H_STEPS + 12 * k + 4) == STEP_ATTACH)
            list |= LIST_DEV;
        if (*(const u32 *)(h + H_STEPS + 12 * k) == STEP_GF_101 || *(const u32 *)(h + H_STEPS + 12 * k) == STEP_GF_101B)
            list |= LIST_101;
        if (*(const u32 *)(h + H_STEPS + 12 * k) == STEP_MASK)
            list |= LIST_MASK;
        if (*(const u32 *)(h + H_STEPS + 12 * k) == STEP_M901)
            list |= LIST_901;
        if (*(const u32 *)(h + H_STEPS + 12 * k) == STEP_OUT || *(const u32 *)(h + H_STEPS + 12 * k) == STEP_M_OUT)
            list |= LIST_OUT;
    }
    /* The monochrome list ends in the buffer 0x1000: both developments get their output from the step (0x6039f787 with
     * an output list of its own writes none of the job's outputs), and the pass from 0x1000 to the outputs is a step the
     * planner adds for the job flag 0x80000 only (0x6038e0a2). The creator gives that flag to key 12 (0x60389926), not
     * to the look's key: without the step the job ends with no picture and no previews (camera: a JPEG of the header
     * alone). The step is added here, after the development 0x901 and before the selector adds its own (0x6038a565 reads
     * the count): the plain one, since the shading callback of the stock one reads the map h+14236 that only the setup
     * of key 12 builds. Room for it and for the selector's step */
    if (t->live[i].route != DRV_GRAIN_GF && n && n + 2 <= STEPS && !(list & LIST_OUT) &&
        *(const u32 *)(h + H_STEPS + 12 * (n - 1)) == STEP_M901) {
        /* The optics layer's step (opticsdrv.c, [optics] probe) goes in first, right after the development 0x901: it
         * waits for that pass and works on the buffer 0x1000 before the pass to the outputs reads it. Room for it too */
        k = drv_optics_list(h, n + 3 <= STEPS);
        if (k) {
            put_step(h, n++, k);
            list |= LIST_PROBE;
        }
        put_step(h, n++, STEP_OUT);
        *(u16 *)(h + H_NSTEPS) = n;
        list |= LIST_OUT;
    }
    r = &t->ring[t->live[i].ring & (TEX_RING - 1)];
    r->detail = list | n << 8;
    r->seq = t->live[i].ring + 1;
}

/* The uncached alias of the tile buffer to write, after the descriptor and its allocation were checked: 0, or FE_*.
 * Only the CPU's direct window: image buffers can have bus addresses (0xb0000000 and up) that the CPU reaches only through
 * a window register; writing their "cached alias" 0x7... reset the camera (core 1.07). Uncached writes need no cache
 * maintenance before the node's DMA */
static u32 fill_check(const u8 *sp, u32 *out)
{
    const u8 *d = sp + F_DESC;
    u32 data = *(const u32 *)d, alloc = *(const u32 *)(d + 64), len = *(const u32 *)(d + 72), u;

    if (*(const u16 *)(d + 8) != F_FORMAT || *(const u16 *)(d + 10) != TILE || *(const u16 *)(d + 12) != TILE ||
        *(const u16 *)(d + 14) != TILE)
        return FE_DESC;
    if (data & 3)
        return FE_ALIGN;
    if (data < alloc || len < TILE_BYTES || data - alloc > len - TILE_BYTES)
        return FE_BOUNDS;                   /* only inside the buffer the firmware allocated for the tile */
    u = data >= DIRECT_LO && data < DIRECT_HI ? data + DRV_UNCACHED : data;
    if (u < DIRECT_LO + DRV_UNCACHED || u > DIRECT_HI + DRV_UNCACHED - TILE_BYTES ||
        (u < GUARD_HI + DRV_UNCACHED && u + TILE_BYTES > GUARD_LO + DRV_UNCACHED))
        return FE_RANGE;
    *out = u;
    return 0;
}

/* H2 (thunks.s): in the node builder 0x603a5167, the firmware tile is in the buffer, the node has not run yet. sp - the
 * builder's sp. Our marker: the generation's tile and a random shift, then the fill's result for W, kept by the record
 * (rec last). side x side bytes of the source (uncached words) -> 1536 x 1536 at dst (uncached), written once and never
 * read back: texfill.h */
void drv_grain_tex_fill(u8 *sp)
{
    volatile tex_t *t = tex();
    volatile tex_entry_t *en;
    volatile tex_fill_t *f;
    const u8 *rec;
    u32 e, err, t0, dst = 0, x = 0, y = 0, rnd, cpu, side, st, rows = 0, i, addr, alloc, out;

    if (!t || !sp)
        return;
    rec = *(const u8 **)(sp + F_REC);
    if (!rec || (rec[4] & 0xf0) != TEX_MARK || (e = rec[4] & 0x0f) >= DRV_GRAIN_GENS)
        return;                             /* not ours: the stock grain film's or the live view's texture */
    en = &t->e[e];
    cpu = cpu_now();
    t0 = drv_ticks();
    addr = *(const u32 *)(sp + F_DESC);
    alloc = *(const u32 *)(sp + F_DESC + 64);
    out = **(const u32 *const *)(sp + F_OUT);   /* written just before the hook site (0x603a5313) */
    side = en->side;
    err = en->src ? fill_check(sp, &dst) : FE_NOSRC;
    if (!err) {
        rnd = t->rng ^ drv_ticks();         /* xorshift32 13/17/5 (as 0x6038c800), stirred by the clock */
        rnd ^= rnd << 13;
        rnd ^= rnd >> 17;
        rnd ^= rnd << 5;
        t->rng = rnd;
        for (x = rnd & 0x3fc; x >= side; x -= side)   /* a multiple of 4 below the side (128..768) */
            ;
        for (y = rnd >> 16 & 0x3ff; y >= side; y -= side)
            ;
        /* The row buffer only on CPU 0 (drv_cpu itself: cpu_now folds an unknown CPU into 0), only for a side the
         * row code takes, and only one fill at a time; otherwise the plain way, with the same result */
        if (!drv_cpu() && side >= 4 && side <= TEX_FILL_TILE && !(side & 3)) {
            st = drv_lock();
            rows = !t->row_busy;
            if (rows)
                t->row_busy = 1;
            drv_unlock(st);
        }
        if (rows) {
            tex_fill_rows((volatile u32 *)dst, (const volatile u32 *)en->src, side, x, y, fill_row);
            t->row_busy = 0;
        } else {
            tex_fill_direct((volatile u32 *)dst, (const volatile u32 *)en->src, side, x, y);
            t->direct++;
        }
    }
    /* The fill's place: the one of this record, or the next in turn (a record not seen in this generation yet). The
     * firmware has FILL_KEEP records, so a place is only ever taken over by its own record */
    for (i = 0; i < FILL_KEEP && en->fill[i].rec != (u32)rec; i++)
        ;
    if (i == FILL_KEEP)
        i = en->fill_next++ % FILL_KEEP;
    f = &en->fill[i];
    f->rec = 0;
    f->dx = x;
    f->dy = y;
    f->err = err;
    f->addr = addr;
    f->alloc = alloc;
    f->out = out;
    f->t_fill = drv_ticks_since(t0);
    en->builds++;
    f->rec = (u32)rec;
    t->cnt[cpu][err ? TC_FILLERR : TC_FILLS]++;
}

/* H3 (thunks.s): the job's cleanup gate, any CPU. Only the row of this job changes: its writer here. Our reference to
 * the texture is released; h+14224 is cleared when it still holds our record */
void drv_grain_tex_clean(u8 *h)
{
    volatile tex_t *t = tex();
    u32 i, rec, us, cpu;

    if (!t || !h)
        return;
    for (i = 0; i < TEX_LIVE && t->live[i].h != (u32)h; i++)
        ;
    if (i == TEX_LIVE)
        return;
    cpu = cpu_now();
    drv_optics_ended(h);                    /* the optics probe's count of the job (nothing without the probe) */
    {
        volatile u32 *d = &t->done[cpu][t->done_n[cpu] % DONE_RING].seq;

        d[0] = 0;
        d[1] = t->live[i].ring + 1;
        d[2] = *(const u16 *)(h + H_NSTEPS);
        d[3] = *(const u16 *)(h + H_STATE);
        d[4] = *(const u16 *)(h + H_STATE + 4);
        d[5] = *(const u16 *)(h + H_STATE + 6);
        d[6] = drv_ticks_since(t->live[i].t0);
        d[0] = ++t->done_n[cpu];
    }
    rec = t->live[i].rec;
    if (rec) {
        drv_fw_tex_release(h + H_P, rec);   /* as the stock cleanup 0x6038e8b2 */
        if (*(const u32 *)(h + H_TEX) == rec)
            *(u32 *)(h + H_TEX) = 0;
        t->cnt[cpu][TC_FREED]++;
        if (t->live[i].tag == t->tag)
            t->freed[cpu]++;
        us = drv_ticks_since(t->live[i].t0);
        t->t_job[cpu] = us;
        if (us > t->t_job_max[cpu])
            t->t_job_max[cpu] = us;
    }
    t->live[i].h = 0;                       /* last */
}

/* The look's grain shot of our job h (its ring entry + 1), 0 - not one of our jobs now. Any CPU: the arena block only
 * (opticsdrv.c's step, for its report) */
u32 drv_grain_shot(u32 h)
{
    volatile tex_t *t = tex();
    u32 i;

    if (!t || !h)
        return 0;
    for (i = 0; i < TEX_LIVE; i++)
        if (t->live[i].h == h)
            return t->live[i].ring + 1;
    return 0;
}

/* Core task: the arena block, then the hooks. W last: it is the one that makes our jobs, so it goes in only with S, A
 * (the blend), H2 (the fill), H3 (the release), D (the detail block) and the allocator's setup hook in place. 0, or the
 * step that failed */
static int install(const drv_grain_cfg *c)
{
    static const u8 sel_site[5] = {0x92, 0x52, 0xf0, 0x06, 0xf8};  /* mov a0,a2; inc4 a2; mov (a2),a1; movbu (16,a1),d0 */
    static const u8 set_site[5] = {0xf0, 0x02, 0xf8, 0x20, 0x18};  /* mov (a2),a0; mov (24,a0),a0 */
    static const u8 h2_site[5] = {0x3c, 0xfa, 0xd0, 0xa4, 0x00};   /* mov sp,a0; add 164,a0 */
    static const u8 h3_site[5] = {0xfa, 0x00, 0x2c, 0x2c, 0xa0};   /* mov (11308,a0),d0; cmp 12,d0 (+ 0x0c) */
    static const u8 w_site[5] = {0xfa, 0x02, 0x2c, 0x2c, 0xa0};    /* mov (11308,a2),d0; cmp 12,d0 (+ 0x0c) */
    static const u8 d_site[5] = {0xf8, 0x24, 0x18, 0x8a, 0x01};    /* mov (24,a0),a1; mov 1,d2 */
    volatile tex_t *t;
    int rc;

    if (!c || c->key >= 12 || !c->type)
        return 1;                           /* keys from 12 up: the planner selector and the cleanup gate take them
                                               another way (cmp 12 at W and H3) */
    /* the selector's 6th and 7th displaced bytes: the rest of movbu (16,a1),d0, re-run by the trampoline; the 6th byte
     * of the cleanup gate and the planner selector: the rest of cmp 12,d0 */
    if (*(volatile u8 *)(SEL_SITE + 5 + DRV_UNCACHED) != 0x41 || *(volatile u8 *)(SEL_SITE + 6 + DRV_UNCACHED) != 0x10)
        return 2;
    if (*(volatile u8 *)(H3_SITE + 5 + DRV_UNCACHED) != 0x0c || *(volatile u8 *)(W_SITE + 5 + DRV_UNCACHED) != 0x0c)
        return 3;
    t = mem_alloc(sizeof(tex_t));
    if (!t)
        return 4;
    mem_set((void *)t, 0, sizeof(tex_t));
    t->key = c->key;
    t->type = c->type;
    t->rng = drv_ticks() | 1;
    slot()[1] = (u32)t;                     /* before any hook can run */
    rc = drv_code_jump(SET_SITE, set_site, (void *)drv_tramp_grain_set);
    if (rc)
        return 10 + rc;
    rc = drv_code_jump(SEL_SITE, sel_site, (void *)drv_tramp_grain_sel);
    if (rc)
        return 20 + rc;
    rc = drv_code_jump(H2_SITE, h2_site, (void *)drv_tramp_grain_h2);
    if (rc)
        return 40 + rc;
    rc = drv_code_jump(H3_SITE, h3_site, (void *)drv_tramp_grain_h3);
    if (rc)
        return 50 + rc;
    rc = drv_alloc_large(0);                /* the setup hook; mode 1 only while the look's grain is on (grain.c) */
    if (rc)
        return 100 + rc;
    rc = drv_code_jump(D_SITE, d_site, (void *)drv_tramp_grain_det);
    if (rc)
        return 80 + rc;
    rc = drv_code_jump(W_SITE, w_site, (void *)drv_tramp_grain_wl);
    if (rc)
        return 70 + rc;
    return 0;
}

/* Once: a second call gives the first one's result (a hook that went in stays) */
int drv_grain_install(const drv_grain_cfg *c)
{
    if (install_rc < 0)
        install_rc = install(c);
    return install_rc;
}

static const char *tex_what(u32 w)
{
    static const char *const name[] = {"?", "built", "shared", "P+8 not 0 or 2", "P+22 set", "texture already there",
                                       "handle in the guarded range", "allocator mode", "4 textures alive",
                                       "builder failed", "fill error", "no fill", "P+21 is 2", "tile not ready (loading)"};

    return w < sizeof(name) / sizeof(name[0]) ? name[w] : "?";
}

/* Core task: a line a shot, the allocator's mode when it changes, the counters when they change */
static void tex_service(volatile tex_t *t)
{
    static u32 seen, mode_seen = 0xffffffffu, sums[CPUS], direct_seen, same_seen, aged_seen;
    static const char *const gate[TG_OK] = {"kind", "type", "effect", "class", "desc", "not ours", "guard",
                                            "stage not the route's"};
    static const char *const route[3] = {"GF", "list", "mask"};
    volatile tex_shot_t *r;
    u32 n, cpu, i, k, sum;

    k = drv_alloc_mode();
    if (k != mode_seen) {
        log_printf(OMK_LOG_INFO, "grain: allocator mode %u", k);
        mode_seen = k;
    }
    if (t->direct != direct_seen) {
        direct_seen = t->direct;
        log_printf(OMK_LOG_INFO, "grain: %u fills read every word from the tile (not on cpu 0, or the row buffer in use)",
                   direct_seen);
    }
    if (t->same_h != same_seen || t->aged != aged_seen) {
        same_seen = t->same_h;
        aged_seen = t->aged;
        log_printf(OMK_LOG_WARN, "grain: rows of jobs that never reached the cleanup taken back: %u by the handle (texture "
                   "released), %u by age (over %u s; the texture reference stays)", same_seen, aged_seen,
                   TEX_AGE_US / 1000000);
    }
    n = t->shots;
    if (n - seen > TEX_RING) {
        log_printf(OMK_LOG_WARN, "grain: %u shots not reported (ring)", n - seen - TEX_RING);
        seen = n - TEX_RING;
    }
    for (; seen != n; seen++) {
        r = &t->ring[seen & (TEX_RING - 1)];
        if (r->seq != seen + 1)
            break;                          /* still being written: the next report */
        k = r->entry < DRV_GRAIN_GENS ? r->entry : 0;
        if (r->what == T_BUILT || r->what == T_SHARED)
            log_printf(OMK_LOG_INFO, "grain: shot %u type %04x: mode %04x rawjpeg %u filetype %u, P+18/19/21 %u/%u/%u, "
                       "slot %u (%s, gain %u, chroma %u), route %s, level %u, %s, list %s (%u steps), shift %u,%u, "
                       "builder %u us, fill %u us, texture %08x, out %08x", seen + 1, r->type, r->mode & 0xffff,
                       r->mode >> 16 & 0xff, r->mode >> 24, r->pf & 0xff, r->pf >> 8 & 0xff, r->pf >> 16 & 0xff, k + 1,
                       tex_names[k] ? tex_names[k] : "?", r->gain, r->chroma, route[r->route < 3 ? r->route : 0], r->tag,
                       tex_what(r->what), (r->detail & (LIST_DEV | LIST_101)) == (LIST_DEV | LIST_101) ? "GF" :
                       (r->detail & (LIST_MASK | LIST_901 | LIST_OUT)) == (LIST_MASK | LIST_901 | LIST_OUT) ? "MONO" :
                       (r->detail & (LIST_MASK | LIST_901)) == (LIST_MASK | LIST_901) ? "MONO without the output step" :
                       "other", r->detail >> 8,
                       r->dx, r->dy, r->t_build, r->t_fill, r->rec, r->out);
        else if (r->what == T_NOTREADY)
            log_printf(OMK_LOG_WARN, "grain: shot %u type %04x: the look's tile %s - no grain", seen + 1, r->type,
                       r->gain ? "is being loaded" : "is not set");
        else if (r->what >= T_BUILDER && r->what <= T_NOFILL)
            log_printf(OMK_LOG_WARN, "grain: shot %u type %04x: slot %u (%s): %s %u, builder %u us, tile %08x (alloc %08x), "
                       "out %08x - no grain", seen + 1, r->type, k + 1, tex_names[k] ? tex_names[k] : "?",
                       tex_what(r->what), r->detail, r->t_build, r->addr, r->alloc, r->out);
        else
            log_printf(OMK_LOG_WARN, "grain: shot %u type %04x h %08x: %s %u - no grain", seen + 1, r->type, r->h,
                       tex_what(r->what), r->detail);
    }
    for (cpu = 0; cpu < CPUS; cpu++) {
        static u32 done_seen[CPUS];
        volatile u32 *d;

        if (t->done_n[cpu] - done_seen[cpu] > DONE_RING)
            done_seen[cpu] = t->done_n[cpu] - DONE_RING;
        for (; done_seen[cpu] != t->done_n[cpu]; done_seen[cpu]++) {
            d = &t->done[cpu][done_seen[cpu] % DONE_RING].seq;
            if (d[0] != done_seen[cpu] + 1)
                break;
            log_printf(OMK_LOG_INFO, "grain: shot %u ended on cpu %u: %u steps, state %04x %04x %04x, W to H3 %u ms", d[1],
                       cpu, d[2], d[3], d[4], d[5], d[6] / 1000);
        }
    }
    for (cpu = 0; cpu < CPUS; cpu++) {
        for (sum = 0, i = 0; i < TCN; i++)
            sum += t->cnt[cpu][i];
        for (i = 0; i < TG_OK; i++)
            sum += t->skip[cpu][i];
        if (sum == sums[cpu])
            continue;
        sums[cpu] = sum;
        log_printf(OMK_LOG_INFO, "grain: texture cpu %u: jobs %u (other types %u, not cpu 0 %u), built %u, shared %u, "
                   "failed %u, skipped %u; fills %u, errors %u; freed %u; case 1 given %u, set %u; detail block %u; job W to H3 "
                   "last %u ms, longest %u ms", cpu,
                   t->cnt[cpu][TC_JOBS], t->cnt[cpu][TC_TYPE], t->cnt[cpu][TC_CPU], t->cnt[cpu][TC_BUILT],
                   t->cnt[cpu][TC_SHARED], t->cnt[cpu][TC_FAILED], t->cnt[cpu][TC_SKIPPED], t->cnt[cpu][TC_FILLS],
                   t->cnt[cpu][TC_FILLERR], t->cnt[cpu][TC_FREED], t->cnt[cpu][TC_GAVE], t->cnt[cpu][TC_WROTE],
                   t->cnt[cpu][TC_DETAIL], t->t_job[cpu] / 1000, t->t_job_max[cpu] / 1000);
        for (i = 0; i < TG_OK; i++)
            if (t->skip[cpu][i])
                log_printf(OMK_LOG_INFO, "grain: texture cpu %u: case 1 not given or set, %s: %u", cpu, gate[i],
                           t->skip[cpu][i]);
    }
}

/* Core task: the reports, every REPORT_MS */
void drv_grain_service(u32 now)
{
    static u32 last_ms;
    volatile tex_t *t = tex();

    if (!t || now - last_ms < REPORT_MS)
        return;
    last_ms = now;
    tex_service(t);
}

/* Core task: see omk_drv.h */
int drv_grain_slot_begin(u32 *gen)
{
    volatile tex_t *t = tex();
    u32 st;

    if (!t)
        return -2;
    st = drv_lock();
    if (t->filling) {
        drv_unlock(st);
        return -1;
    }
    t->ready = 0;
    *gen = (t->active + 1) % DRV_GRAIN_GENS;
    drv_unlock(st);
    return 0;
}

void drv_grain_slot_ready(u32 gen, const u8 *src, u32 side, u32 mean, const char *name)
{
    volatile tex_t *t = tex();
    volatile tex_entry_t *en;
    u32 st, i;

    if (!t || gen >= DRV_GRAIN_GENS)
        return;
    en = &t->e[gen];
    en->src = (u32)src;
    en->side = side;
    en->mean = mean;
    for (i = 0; i < FILL_KEEP; i++) {       /* the generation's number comes round again: no fill of it yet */
        en->fill[i].rec = 0;
        en->fill[i].err = 0;
    }
    en->fill_next = 0;
    tex_names[gen] = name;
    st = drv_lock();
    t->active = gen;
    t->ready = 1;
    drv_unlock(st);
}

void drv_grain_look_gain(u32 gain)
{
    volatile tex_t *t = tex();

    if (t)
        t->gain = gain <= DRV_GRAIN_GAIN_MAX ? gain : DRV_GRAIN_GAIN_MAX;
}

void drv_grain_route(u32 route, u32 chroma, u32 tag)
{
    volatile tex_t *t = tex();
    u32 st;

    if (!t)
        return;
    st = drv_lock();                        /* W takes the three together */
    t->route = route <= DRV_GRAIN_MASK ? route : DRV_GRAIN_GF;
    t->chroma = chroma <= DRV_GRAIN_CHROMA_MAX ? chroma : DRV_GRAIN_CHROMA_MAX;
    t->tag = tag;
    drv_unlock(st);
}

int drv_grain_route_state(u32 *freed, u32 *live)
{
    volatile tex_t *t = tex();
    u32 i, n = 0;

    if (!t)
        return -1;
    for (i = 0; i < CPUS; i++)
        n += t->freed[i];
    *freed = n;
    for (i = 0, n = 0; i < TEX_LIVE; i++)
        if (t->live[i].h && t->live[i].tag == t->tag)
            n++;
    *live = n;
    return 0;
}
