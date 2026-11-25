/* The optics layer, stage 1 (core 1.33): a CPU step of our own in the look's still jobs - for now a probe that measures
 * and logs. The step is the base of the layer: finding the job's buffers, the window to them, the time limits. It is
 * switched on by [optics] probe in OMKIT.INI (core/optics.c); probe 0 or absent: nothing here is installed, and the job
 * lists are the ones of core 1.32.
 *
 * Where: graindrv.c's W, after the planner, puts the step into the list of our jobs on the mask route (the row live[] of
 * the look's grain) after the development 0x603902a0 (stage 0x901, into the buffer 0x1000) and before the pass to the
 * job's outputs 0x60391b23, where it appends that pass - with room for both and for the selector's own step.
 *
 * The step's contract (firmware code): a list entry h+10984 + 12k is {fn, +4 the pass's callback, +8 the pass not ended},
 * k the u16 h+11312. The step executor 0x60389f71 calls fn by calls with a0 = h; d0 0 - the next step, anything else ends
 * the job with 0x9ED0. The stock CPU step 0x6038c84a does: 0x6038a70c(h) (waits for the pass of the step before - here
 * the development 0x901, whose output the next pass reads), its own work, +8 of its own entry = 0, d0 0. So does this one
 * (thunks.s: drv_tramp_optics_step), always with d0 0: whatever the probe meets, the job ends as without it.
 *
 * Where it runs: the still job runs synchronously in the firmware's Bayer2Ycc handler, CPU 0 (camera logs), in a task of
 * the firmware - not ours. The time is limited (decided for the optics layer): OPT_BUDGET_US of our work a job, and the
 * CPU given away for a tick (drv_sleep(1)) every OPT_YIELD_US. No lock is held across the work; the window's semaphore of
 * the firmware's window manager is (by the manager, for the time of the map).
 *
 * What the step does, by level (the probe's report in the log, a block a job):
 *   1 - the time of the wait for the pass; the job's buffer slots (h+6592 + 88i, 48 of them; a slot is in use when its
 *       allocation +64 is not 0); the slots of the picture 0x1000 and of the small frame 0x1060 by the firmware's search
 *       0x603868ea; an empty loop (the CPU's speed); the window to 0x1000 (map 0x60411572, not waiting for it), 1 MiB read
 *       from it past the cache in words with their sum, the first 64 bytes of the picture; the window released
 *       (0x604116a1); the time of it all.
 *   2 - 1, then the same MiB through the window's cached view (the address - 0x40000000), the CPU's cache written back
 *       and dropped (0x60069fbf) before and after: its time and sum. Not known to work on this camera - a write to the
 *       cached view of a bus buffer without a window reset the camera once (core 1.07). So it runs only when core armed it
 *       (drv_optics_arm), for one job: core arms only after the reports before it are on the card and its store says
 *       "armed" there (core/optics.c) - a reset then leaves the measurements and the attempt on the card.
 *   3 - 1, then stripes in Cb and Cr of the first rows of the picture through the window past the cache (optprobe.h),
 *       the cache written back after them: the JPEG shows which byte is which.
 * On any error (no buffer 0x1000, a format or size not expected, an address out of the ranges, no window) the step
 * reads and writes nothing more, records why and returns 0.
 *
 * The bisection (core 1.34; core/optics.c, [optics] bisect): core 1.33's level 1 reset the camera at the first still
 * of the look with grain, and no report reached the card. So the work is given to the jobs in substages, each one only
 * after the one before passed (a job ended with it, its report on the card), each with "armed" on the card first:
 *   N  - no step (the list as in core 1.32): the base;
 *   A0 - the firmware's own CPU step 0x6038c84a in our place: a step more in the list, none of our code;
 *   A  - our step: the trampoline, the wait 0x6038a70c, +8 cleared, the report (the ring) - nothing else;
 *   B  - and the slots, the firmware's search 0x603868ea and the request C;
 *   C1 - and one tick given away (drv_sleep(1)) (core 1.35: core 1.34's C - the empty loop of 1 000 000 iterations and
 *        a tick - reset the camera inside the loop, so it is split);
 *   C2 - C1 and an empty loop of 10 000 iterations;
 *   C3 - C1 and the empty loop of 1 000 000 iterations at once, as core 1.34's C (a test only: core 1.34's C reset the
 *        camera inside this loop, at the second job of a RAW+JPEG press);
 *   W0 - C1, and an empty request handed to the worker task (core 1.39): the hand-over and the wait alone;
 *   W1 - C1, and the loop at once handed to the worker task (core 1.37, below): the step waits for it;
 *   W2 - C1, the loop in 10 parts in the worker, a tick given away after each;
 *   D  - W2 and the window mapped and released by the worker (no read);
 *   E  - D and the picture's first 64 bytes;
 *   F  - E and the MiB: level 1 as core 1.33 had it;
 * then the level's own work (FULL: F and level 2 or 3). A job keeps the substage it was listed with (its row). The ones
 * after a test (C3) do not include it. From A on the step records at its entry the psw, its sp, the running task and its
 * stack (the kernel's TCB), the request C and the cross-CPU lock bytes, and the lowest sp of its own code - in the report
 * and in the crumbs (all but the lock bytes). From C1 on the work runs only in the task REC_IMAGE (id 0x20, the still
 * job's task): in another task (REC_VIEW runs the executor too) the step stops, the reason recorded.
 *
 * The worker task (core 1.37; the owner's decision: our own task, not the job task's priority changed). A count of
 * milliseconds in the step holds up CPU 0's 93 tasks of priority 55 (the step runs at REC_IMAGE's 54). The work goes to
 * a task of ours, as the firmware's Star Light step hands its rays to the task ImgproCF (0x60393dcc, 0x6036e535,
 * 0x6036e67d) - but one task for good, made by the core task (drv_optics_worker) before the first substage that needs
 * it: CPU 0 (the window is a CPU's state; the core task runs there), priority 56 (below the 93; one other task there,
 * ICTLOG), its stack from the arena (WORKER_STACK, used through the cache of CPU 0 only, its own cache lines). The
 * protocol: the request (the ring record, the stop and abandon words, the sequence words req and done) in the arena
 * block; two RTOS event flags of CPU 0, one waiter each: the worker waits on its "request" flag, the step on its "done"
 * flag (op_delegate). The step waits the firmware's way, as for an ISP pass, and does not return while the worker may
 * touch the picture (op_delegate's comment has the limits).
 * The crumbs: the step and W, H3 write where they are (the job, its substage, the phase, and every phase reached) into
 * the last 128 bytes of the arena's region, 0xa377ff80..0xa3780000 (drv.c's ARENA_TAIL leaves them out of the arena, with
 * the debug service's memory below them; the firmware does not use the region, checked word by word on the camera),
 * through the uncached alias: if the memory outlives a reset, core reads at the
 * next start where the step was (drv_optics_crumb). core writes a heartbeat there too, so a start can tell "memory kept,
 * the step not reached" from "memory lost".
 *
 * The layer (feature/optics-layer: Bloom and Halation; core/optics.c, [optics] bloom, halation and the rest). The
 * firmware's Soft Focus chain is the transport: the ISP reduces the picture 0x1000 into the small frame 0x1060 (pass
 * 0x400, format 0x305, 4 bytes a pixel), the worker draws the glow into it, and the ISP enlarges it and adds it to the
 * picture. Our step runs the reduction itself (0x603937f8, synchronous, in the job task) with the layer's size of ours
 * (the hook at 0x60393879 gives the reduction our {w, h} for the job whose h is in drv_optics_hook[0]), then hands the
 * layer to the worker. Two ways on (route, [optics] route):
 *   PAIR - the firmware's own steps after ours: 0x603932cb (enlarge 0x401 into the picture-sized 0x1061, 40 MB) and
 *          0x603934ba (the pass to the outputs, whose callback 0x6039374d blends 0x1000 and 0x1061: case 11);
 *   403  - our last step (drv_optics_final) instead of the pass to the outputs: 0x6038c50f with the small frame as the
 *          input and the callback 0x6039379e - one pass that enlarges and blends (case 12), no 0x1061.
 * The blend hook (graindrv.c's A, drv_optics_blend) makes the blend additive for our jobs from L2 on: the picture's gain
 * 1024, the layer's 1024 in Y and chroma (the chroma centred at 128 by the blend's own offsets) - the strength is in the
 * layer. If the reduction or the worker fails, the step puts the plain pass to the outputs (and the firmware's CPU step
 * that does nothing but wait, for PAIR's 0x603932cb) into the list in their place: the job ends as without the layer.
 * The substages (as the probe's, in [optics] schedule; without one the jobs get L4):
 *   L0 - the chain with the firmware's weights and size (PAIR: nothing in our step; 403: the reduction in our step) - a
 *        soft focus look: the chain itself on our route, its time and memory;
 *   L1 - and our size, the layer read by the worker: its first words and the bytes' statistics in the log (the order of
 *        Y, Cb, Cr in a word - [optics] layer_bytes = auto takes it from here); the firmware's weights;
 *   L2 - the layer written neutral (nothing added) and the blend additive: the picture as without the layer;
 *   L3 - a pattern in the layer (bands with one byte changed each): which byte is which, on the picture;
 *   L4 - the effect (optlayer.h): the picture's rows read past the cache (or through the cached view), the layer read,
 *        the sources, the Gaussians, the layer written.
 *
 * Memory: the step touches the arena block below (its uncached address), the job's handle (through the cache, on the job's
 * CPU, as the firmware's steps do), the picture through the window, and the stack; the firmware only through the thunks of
 * thunks.s. No core variables, no tables in .rodata - but one: the bench's array (op_bench_core), which only the worker
 * touches, on CPU 0, through its cache. The worker also has its work buffer (an arena block of its own lines, used
 * through the cache of CPU 0 only, as its stack). The core task reports (drv_optics_service).
 *
 * L4's time, a job (the log's lines; optlayer.h has how each part is made cheap): the picture's read (the cover read:
 * every row of cells, to the end - card169), the layer read, the sources, the Gaussians (rows and columns apart, and the share of the values the rows'
 * ranges left to work on), the layer written; one row of the picture timed alone (read; read with the sums; through the
 * cached view too with read_cached) and a bench (a box row on the stack, in the work buffer, in core's memory; a loop
 * with and without a multiplication). Between the parts and the Gaussians' passes the worker gives the CPU away when
 * OPT_YIELD_US passed (op_pace), and stops there if asked. Bloom's tail (optlayer.h's ol_tail_*) has its own time in
 * the calc line; the picture's read logs its rows' mean and longest, and the time of its maps and write-backs.
 *
 * Sets of settings: a job takes the layer's settings in force when it is listed (drv_optics_set, the schedule's L4:X)
 * and keeps them in its row and record; the worker reads that set (op_cfg).
 *
 * Strengths (after card166): a job takes the look's optics rows' strengths in force when it is listed (drv_optics_strength
 * from the core task), or its set's own (a test); both 0 - W lists no step of ours (the job as N). The worker scales the
 * gains by the strengths' steps and by the scene's day weight (optlayer.h's ol_scene, ol_day: from the layer's Y it has
 * read), and Bloom's by the near part's share when there is a tail (ol_tail_add adds the tail at w / (256 - w)). With
 * the strengths comes whether the colour tables in force are black and white (OP_MONO, core's isp_mono): the job's
 * halation then has no colour (Cb, Cr 0 a unit of its Y; its Y as in colour) - a black-and-white film's halo is light,
 * and the layer's chroma is added after the tables made the picture grey.
 *
 * The jobs of a RAW+JPEG press (card169, [optics] jobs): the first (FileType 1) makes the JPEG, the second (FileType 3)
 * the ORF's preview (3200x2400, the whole 4:3 frame) - the camera's pictures of card168 show it. With jobs = first (the
 * default) W lists no step of ours in the second: the press costs one job of the layer, not two; jobs = both - as
 * before. */
#include "omk_drv.h"
#include "omk_core.h"
#include "omk_log.h"
#include "omk_mem.h"
#include "optprobe.h"
#include "optwork.h"
#include "optlayer.h"

#define CPUS 3
#define H_REQ 8                             /* P+4: the record's request C (0x6041e8dc) */
#define H_TYPE 20                           /* u16 P+16, the job type */
#define H_STEPS 10984                       /* 16 x {fn, the pass's callback, the pass not ended} */
#define STEP_PENDING 8
#define STEPS 16
#define H_STEP 11312                        /* u16 the step being run (0x60389f71) */
#define H_NSTEPS 11314
#define H_SLOTS 6592                        /* the job's buffers: 48 x 88 bytes (0x60386415, 0x603868ea) */
#define SLOTS 48
#define SLOT_SIZE 88
#define SL_BUS 0                            /* the data's bus address */
#define SL_FORMAT 8                         /* u16 */
#define SL_PITCH 10                         /* u16 the row's length in pixels, a multiple of 32 */
#define SL_W 12
#define SL_H 14
#define SL_ALLOC 64                         /* the allocation (0 - the slot is free: 0x60386a7a, 0x603867b8) */
#define SL_LEN 72                           /* its length */
#define SL_TYPE 76                          /* u32 the buffer type (0x603868ea compares the word) */
#define SL_CPU 80                           /* u8 the CPU that allocated it ((0x8050) at 0x6038644a) */
#define SL_B81 81
#define SL_B84 84                           /* u8: the slot's release takes another way when it is not 0 (0x603867d8) */
#define BUF_FRAME 0x1000                    /* the picture after the development (0x603902a0 writes it) */
#define BUF_SMALL 0x1060                    /* the Soft Focus chain's small frame: the layer (made by our step, L0 on) */
#define FMT_LAYER 0x305                     /* the small frame's format: 4 bytes a pixel, the row's length in pixels */
#define STEP_SF_UP 0x603932cb               /* PAIR: the firmware's enlarging step (0x401 into 0x1061) */
#define STEP_SF_OUT 0x603934ba              /* PAIR: the pass to the outputs blending 0x1000 and 0x1061 (case 11) */
#define STEP_OUT 0x60391b23                 /* the plain pass to the job's outputs (graindrv.c's) */
#define CB_403 0x6039379e                   /* 403: the callback that blends the small frame into the picture (0x403) */
#define CB_PLAIN 0x60391516                 /* the plain pass's callback (does nothing) */
#define E_NO_BUFFER 0x9ece                  /* the stock steps' "no buffer" (0x603934cf) */
#define LAYER_MIN 42                        /* the reduction at most 16 x after the input's 1/8 (in the emulator even
                                               20 x 15 passed: a margin) */
#define LAYER_MAX 400
#define LAYER_LEN_MAX 0x233000u             /* the allocator's "small" buffers (0x604265ef) */
#define H_B 11364                           /* request B = R of the passes (0x6038b214) */
#define R_STAGE 12
#define R_KIND 16
#define R_CODE 656                          /* u16 the effect code of kind 3 (0x402 / 0x403 by the callbacks) */
#define STAGE_F005 0xf005
#define CODE_402 0x402
#define CODE_403 0x403
#define CASE_402 11                         /* the blend cases of the codes (selector 0x603ecda1, hash 0x605769a8) */
#define CASE_403 12
#define CTX_B 4
#define CTX_IMAGE 8
#define IMG_BLEND 7364                      /* the blend block's image in the register image (0x603d80ec) */
#define BL_GAIN_A 92                        /* input A: Y | chroma << 16; +96 the second chroma */
#define BL_GAIN_A2 96
#define BL_GAIN_B 136                       /* input B: the same */
#define BL_GAIN_B2 140
#define GAIN_ONE 0x04000400u
#define LAYER_GAIN 1024                     /* the layer's gain in the blend: the strength is in the layer */
#define OP_TEST (1u << 16)                  /* a job's strengths (str): a test's, from the INI's set */
#define OP_MONO (1u << 17)                  /* ... the colour tables in force black and white: the halation without colour */
#define GUARD_LO 0x60000200u
#define GUARD_HI 0x60cbe800u
#define FMT_YC 0x302                        /* the picture's format on the mask route: 2 bytes a pixel */
#define C_MODE 194                          /* C: u16 Mode, u8 RawJpeg, u8 FileType (as graindrv.c logs them) */
#define C_RAWJPEG 293
#define C_FILETYPE 298
#define WIN_NOWAIT 1                        /* d1 bit 0 of the map: the manager does not wait for its semaphore (d1 0:
                                               it waits without a limit, 0x604115a5..0x604115b7) */
#define OPT_READ (1024 * 1024)              /* bytes read from the picture's start */
#define OPT_CHUNK 16384                     /* bytes between two looks at the clock */
#define OPT_BUDGET_US 300000                /* our work a job, decided for the optics layer: 0.3 s */
#define OPT_YIELD_US 100000                 /* the CPU given away for a tick this often */
#define OPT_LOOP 1000000                    /* iterations of the empty loop (2 instructions each) */
#define RING 8                              /* jobs between two reports (the core task reports every 250 ms) */
#define REC_SLOTS 16                        /* slots recorded a job (the rest counted) */
#define STEP_STOCK 0x6038c84a               /* the firmware's CPU step (substage A0): wait, h+14216 = 0, +8 = 0, d0 0 */
#define LOOP_SHORT 10000                    /* substage C2 */
#define TASK_REC_IMAGE 0x20                 /* the still job's task (CPU 0, priority 54, stack 0x3700) */
#define LOOP_PARTS 10                       /* substage W2 and after: the loop in parts, a tick after each */
#define H_P 4                               /* P = h+4: the cancel check's first argument */
#define H_CANCEL 11318                      /* u16: set by the cancel check (the executor's state word) */
#define WORKER_PRI 56                       /* below CPU 0's 93 tasks of 55; acre_Task changes only 8..11 (0x60051d78) */
#define WORKER_STACK 8192                   /* its code ~0.3 KB deep, the window manager and dly_tsk ~0.4 KB, an
                                               interrupt's frames on top: 4 KB would do (Caduceus' own task has 4 KB,
                                               DMP_RD_MGR 2 KB); twice that, from a 3 MB arena. acre_Task fills it with
                                               0xdeadbeef: the task dump shows how much was used */
#define WORKER_IDLE_MS 1000                 /* the worker's wait for a request, again and again */
#define WORKER_WAIT_MS 50                   /* the step's wait for done, between its checks */
#define WORKER_LATE_US 2000000              /* no done after this: stop asked, logged; the wait goes on */
#define WORKER_GIVE_UP_US 10000000          /* ... and after this the step gives up (abandon), so the camera cannot hang */
#define WORKER_START_MS 300                 /* the core task's wait for the new task to say it runs */
#define WORKER_QUICK_US 2000                /* a wait that returned sooner than this with no request: woken at once */
#define WORKER_QUICK_MAX 3                  /* ... so many in a row: the worker naps WORKER_NAP_MS before the next wait,
                                               so that it can never spin above the tasks below priority 56 (core's 66 among
                                               them: core 1.37 stopped right after the worker was made, card148) */
#define WORKER_NAP_MS 10
#define WS_CANCEL OW_CANCEL                 /* the record's w_state: optwork.h's */
#define WS_LATE OW_LATE
#define WS_ABANDON OW_ABANDON
#define KCB_PTRS 0x8ff00004                 /* kernel.c: (KCB_PTRS)[cpu] the kernel's control block, 0x60041cf4 + 0x18c cpu */
#define KCB_CPU0 0x60041cf4u
#define KCB_STEP 0x18cu
#define KCB_CUR 96                          /* -> the word holding the running TCB */
#define TCB_ID 16                           /* u16 */
#define TCB_PRI 8                           /* u16 the current priority, +10 the base one (kernel.c) */
#define TCB_STKSZ 84
#define TCB_STK 88
#define LOCKS 0x8ff001ccu                   /* the cross-CPU locks' bytes (0x603eac01: lock n - byte LOCKS + (n >> 8), mask
                                               n & 0xff): read only, plain words */
#define ROWS 4                              /* jobs listed and not ended at once (as graindrv.c's rows) */
#define SUBS DRV_OPTICS_SUBS
#define CRUMB 0xa377ff80u                   /* drv.c: the region's last 128 bytes, out of the arena (ARENA_TAIL) */
#define CRUMB_MAGIC 0x4f435242u             /* 'OCRB' */
enum { CR_MAGIC, CR_BOOT, CR_BEAT, CR_STATE, CR_TICKS, CR_PHASES, CR_LISTED, CR_ENDED, CR_PSW, CR_SP, CR_SP_MIN, CR_TASK,
       CR_STK, CR_STKSZ, CR_MODE, CR_WORK, CR_W_LOOPS, CR_W_EMPTY, CR_W_FAST, CR_W_NAPS, CR_W_RC, CR_W_PHASE,
       CR_W_TICKS, CR_W_DONE, CR_W_SP, CR_W_PSW, CR_W_PRI, CR_W_FLAGS, CR_N };
/* The worker's phases (CR_W_PHASE) */
enum { WP_NONE, WP_STARTED, WP_WAITING, WP_WOKEN, WP_WORKING, WP_ANSWERED, WP_NAP };
typedef char op_crumb_words_are_the_api_s[DRV_OPTICS_CRUMB == CR_N ? 1 : -1];
/* Phases of a job, in the crumbs (state: job << 16 | substage << 8 | phase; phases: bit 1 << phase of each reached) */
enum { P_NONE, P_LISTED, P_ENTER, P_WAITED, P_SLOTS, P_SLEPT, P_LOOP, P_BUSY, P_MAP, P_MAPPED, P_HEAD, P_READ, P_READ_END,
       P_CACHED, P_CACHED_END, P_STRIPES, P_STRIPES_END, P_UNMAP, P_UNMAPPED, P_OUT, P_ENDED, P_HANDED, P_WORK, P_WORK_END, P_LATE, P_BACK,
       P_DOWN, P_DOWN_END, P_LAYER, P_LAYER_END, P_FINAL, P_N };

enum { OE_NONE, OE_WAIT, OE_NOBUF, OE_SLOT, OE_ADDR, OE_MAP, OE_WIN, OE_BUDGET, OE_TASK, OE_WORKER, OE_DOWN, OE_LAYER,
       OE_STOPPED, OE_BUFFER, OE_N };
enum { CS_NONE, CS_NOTARMED, CS_NOTWINDOW, CS_BEGUN, CS_DONE };

typedef struct {
    u32 type, bus;
    u32 fmt;                                /* format | pitch << 16 */
    u32 wh;                                 /* width | height << 16 */
    u32 len, alloc;
    u32 bytes;                              /* +80 | +81 << 8 | +84 << 16 */
} op_slot_t;

typedef struct {
    u32 seq;                                /* the job's number + 1, written last; 0 - being written */
    u32 shot;                               /* the look's grain shot (graindrv.c's ring), 0 - not found */
    u32 h, type, mode, level, cpu, step;    /* mode as graindrv.c's; step: k | the list's count << 8 */
    u32 sub, job;                           /* the job's substage (DRV_OPTICS_*), its number in the crumbs */
    u32 set;                                /* the layer's settings: 0 - [optics], 1.. - drv_optics_layer_set's */
    u32 str;                                /* the job's strengths: Bloom | Halation << 8, OP_TEST - a test's, OP_MONO -
                                               black and white */
    u32 sc_mean, sc_share, sc_day;          /* L4: the layer's mean Y, its share at DRV_OPTICS_DAY_CODE (permille), the
                                               day's weight (Q8) */
    u32 gbe, ghe;                           /* L4: the gains used (Q8, the strength, the day and the tail in them) */
    u32 psw, sp, sp_min;                    /* at the step's entry (C); the lowest sp of the step's C code */
    u32 task, stk, stksz;                   /* the running task (its TCB): id, stack base and size; 0 - not read */
    u32 lock[2];                            /* the cross-CPU lock bytes at the entry */
    u32 pitch, width, height;               /* the picture's, for the worker (from W1 on, D and later) */
    u32 w_task, w_state, w_wait;            /* the worker's id; WS_*; the step's wait for it, us */
    u32 w_us, w_yields;                     /* the worker's own time (with the ticks it gave away) */
    u32 err, detail;                        /* OE_*, a value that goes with it */
    u32 t_wait, wait_rc;                    /* the wait for the pass before (0x6038a70c), us and its result */
    u32 nslots, used;                       /* slots recorded, slots in use */
    op_slot_t slot[REC_SLOTS];
    u32 i_frame, i_small;                   /* the firmware's search: index, 48 - none */
    u32 loop_n, loop_us;                    /* the empty loop's iterations (2 instructions each) and time */
    u32 sleep_ms;                           /* W1 with drv_optics_loop's sleep: the worker slept this long instead */
    u32 bus, len, kind, win, t_map, t_unmap, unmap_rc;
    u32 rd_bytes, rd_us, rd_sum;            /* rd_us without the ticks given away */
    u32 head[16];
    u32 c_state, c_bytes, c_us, c_sum, c_fl0, c_fl1;
    u32 s_rows, s_px, s_us, s_fl;
    u32 yields, t_total;                    /* t_total: from the end of the wait, with the ticks given away */
    /* the layer (L substages) */
    u32 route;                              /* DRV_OPTICS_PAIR / _403 */
    u32 t_down, down_rc;                    /* our reduction 0x603937f8: us, its d0 (the slot; 48 or more - none) */
    u32 l_bus, l_wh, l_pitch, l_len, l_kind;    /* the small frame: bus, w | h << 16, row in pixels, bytes, kind */
    u32 l_asked;                            /* the size asked for (w | h << 16), 0 - the firmware's */
    u32 fallback;                           /* 1 - the chain left out (the plain pass put in its place) */
    u32 order;                              /* py | pcb << 8 | pcr << 16 used */
    u32 maps, unmaps_bad;
    u32 t_lread, t_calc, t_lwrite;          /* the worker: the layer read, the sources and Gaussians, the layer written */
    u32 t_src, t_gb, t_gh;                  /* L4, us without the ticks given away: the sources, Bloom's and Halation's
                                               Gaussians */
    u32 t_gt;                               /* ... Bloom's tail (its reduction, Gaussian, enlargement and mix) */
    u32 g_us[2][2];                         /* ... the Gaussians' row and column passes: [Bloom, Halation][OL_ROWS, OL_COLS] */
    u32 g_done, g_all;                      /* values the passes worked on (the rows' ranges), of the whole planes' */
    u32 pr_read, pr_sums, pr_cread, pr_csums;   /* one row of the picture, us: read, read with the sums; the same through
                                                   the cached view (read_cached); pr_err: no row probe */
    u32 pr_err;
    u32 bx_wbuf;                            /* the bench: a box row of OP_BENCH_N values OP_BENCH_REPS times, us */
    u32 lp_plain, lp_mul, lp_udf, lp_hi;    /* loops of OP_TURNS turns, us: plain, mulu, udf01, udf00 + udf15 */
    u32 lp_same;                            /* 1 - the loops of mulu and udf01 gave the same */
    u32 lcached;                            /* 1 - the layer read and written through the window's cached view */
    u32 sig[5];                             /* the Gaussians, layer pixels Q8: Bloom 1, 2, Halation 1, 2; the tail's in
                                               its own plane's pixels */
    u32 f_rows, f_us, f_cells, f_maps, f_err;   /* the picture's read: rows, us, rows of cells read, maps, error */
    u32 f_row_max, f_row_avg, f_maps_us, f_slept;   /* ... its longest row and the rows' running mean (us, with what
                                               the CPU did meanwhile), the maps, write-backs and releases, the ticks
                                               given away */
    u32 lanes[1 + 4 * OL_LANES];            /* L1: ol_lanes_t */
    u32 blend_set;                          /* the blend hook made the blend additive (any CPU; after the report: the
                                               blend's pass comes after the step - the service's count is the one logged) */
} op_rec_t;

typedef struct {
    u32 level;                              /* 1..3, set before the first list */
    u32 armed;                              /* 1 - the next step of level 2 reads through the cached view: the core task
                                               sets it, the step that reads takes it back (drv_lock) */
    u32 jobs;                               /* steps begun: the ring's writer index (drv_lock) */
    u32 listed, noroom;                     /* W: steps put into lists; lists with no room for it */
    u32 seconds;                            /* W: second jobs of RAW+JPEG presses listed without the layer (jobs first) */
    u32 sub;                                /* the substage of new jobs (core: drv_optics_sub) */
    u32 loop, loop_sleep;                   /* drv_optics_loop: the empty loop's iterations (0 - OPT_LOOP); W1 sleeps this
                                               many ms instead of the loop (0 - no) */
    u32 wl;                                 /* W: jobs listed (the crumbs' job number) */
    struct {
        u32 h, sub, job;                    /* h 0 - free: W takes it (CPU 0), H3 frees it (any CPU, h last) */
        u32 layer;                          /* 1 - the layer is ready (the step), so the blend hook makes it additive */
        u32 rec;                            /* the job's ring record (the step), 0 - none */
        u32 set;                            /* the layer's settings in force when it was listed */
        u32 str;                            /* ... and its strengths (as op_rec_t's) */
    } row[ROWS];
    u32 ended[CPUS][SUBS];                  /* H3: our jobs ended, by the substage they had (each CPU its own row) */
    /* The worker: made by the core task; the request from the step, one at a time (the job task runs one job at a time) */
    int w_flag_req, w_flag_done;            /* the event flags (CPU 0) */
    u32 w_tid;                              /* the worker's task id, written by the worker when it runs */
    u32 w_rec;                              /* the request: its ring record */
    ow_chan_t ch;                           /* req, done, stop, abandon (optwork.h) */
    u32 w_count;                            /* requests done */
    u32 w_loops, w_empty, w_fast, w_naps;   /* the worker's waits; woken with no request; at once; naps (spin guard) */
    u32 w_rc;                               /* the result of its last wait (0 - the flag, -2 the time ran out) */
    u32 w_sp, w_psw, w_pri, w_stk;          /* at its start: sp, psw, priority (current | base << 16), stack base */
    op_rec_t test;                          /* drv_optics_worker_test's request */
    /* The layer (drv_optics_layer, core task, before any list) */
    u32 layer_on;
    drv_optics_layer_cfg cfg;               /* the first set ([optics]): also the route, width, bytes and curve of all */
    drv_optics_layer_cfg xcfg[DRV_OPTICS_SETS];    /* drv_optics_layer_set's (core task, before the schedule's first) */
    u32 nsets;                              /* sets 1..nsets given */
    u32 set;                                /* the set of the jobs listed from now on (core: drv_optics_set) */
    u32 str;                                /* the look's strengths of the jobs listed from now on: Bloom | Halation << 8,
                                               OP_MONO (core: drv_optics_strength) */
    u32 order;                              /* py | pcb << 8 | pcr << 16 now (AUTO: the guess of the first L1 dump) */
    u32 order_auto, order_set;
    u16 lin[256];                           /* the curves (ol_luts), copied into the work buffer a job */
    u8 enc[OL_ENC];
    u32 wbuf, wsize;                        /* the worker's buffer: its cached address, bytes */
    u32 fallbacks, blends[CPUS][2];         /* jobs whose chain was left out; blends made additive by the hook, by case */
    op_rec_t ring[RING];
} op_t;

typedef struct {
    u32 t0, ty;                             /* the work's start; the last tick given away */
    u32 slept, yields;
    volatile u32 *stop;                     /* the worker: a word that ends the work, 0 - none */
    u32 budget;                             /* us, 0 - OPT_BUDGET_US; PACE_NONE - no limit (the stop words only) */
} pace_t;
#define PACE_NONE 0xffffffffu

/* Only through slot(): the uncached alias; word 1 - the arena block */
static u32 optics_slot[16] __attribute__((aligned(64), section(".data.cpu_shared")));
static int install_rc = -1;                 /* the result of the one install (core task); -1 not tried */

static volatile u32 *slot(void)
{
    return (volatile u32 *)((u32)optics_slot + DRV_UNCACHED);
}

static volatile op_t *op(void)
{
    return (volatile op_t *)slot()[1];
}

/* The crumbs: words past the arena's end, written past the cache (any CPU; a lost update of the bit set is harmless) */
static volatile u32 *crumb(void)
{
    return (volatile u32 *)CRUMB;
}

static void op_crumb(u32 job, u32 sub, u32 phase)
{
    volatile u32 *c = crumb();

    c[CR_TICKS] = drv_ticks();
    c[CR_PHASES] = (phase == P_LISTED ? 0 : c[CR_PHASES]) | 1u << phase;
    c[CR_STATE] = (job & 0xffff) << 16 | sub << 8 | phase;
}

static u32 op_sp(void)
{
    u32 v;

    __asm__ __volatile__("mov sp,%0" : "=a"(v));
    return v;
}

static u32 op_psw(void)
{
    u32 v;

    __asm__ __volatile__("mov psw,%0" : "=d"(v));
    return v;
}

/* The running task of this CPU from its kernel's words (kernel.c's layout; read only, through the cache - this CPU's
 * kernel writes them so): id, stack base and size; 0 - not the layout expected */
static u32 op_task_pri(u32 *stk, u32 *stksz, u32 *pri)
{
    u32 cpu = drv_cpu(), k, v, p;

    *stk = *stksz = *pri = 0;
    if (cpu >= 3)
        return 0;
    k = ((const volatile u32 *)KCB_PTRS)[cpu];
    if (k != KCB_CPU0 + KCB_STEP * cpu)
        return 0;
    v = *(const volatile u32 *)(k + KCB_CUR);
    if (v < 0x60000000u || v >= 0x64000000u || (v & 3))
        return 0;
    p = *(const volatile u32 *)v;
    if (p < 0x60000000u || p >= 0x64000000u || (p & 3))
        return 0;
    *stksz = *(const volatile u32 *)(p + TCB_STKSZ);
    *stk = *(const volatile u32 *)(p + TCB_STK);
    *pri = *(const volatile u16 *)(p + TCB_PRI) | (u32)*(const volatile u16 *)(p + TCB_PRI + 2) << 16;
    return *(const volatile u16 *)(p + TCB_ID);
}

static u32 op_task(u32 *stk, u32 *stksz)
{
    u32 pri;

    return op_task_pri(stk, stksz, &pri);
}

void drv_tramp_optics_step(void);           /* thunks.s: the step's entry, called by calls */
void drv_tramp_optics_worker(void);         /* thunks.s: the worker task's entry */
int drv_fw_acre_task(const char *name, void *entry, u32 pri, u32 stksz, void *stk);   /* thunks.s: 0x60051d5c */
u32 drv_fw_job_cancel(u8 *p, u8 *flag);     /* thunks.s: 0x6038a8a5, 1 - cancelled */
int drv_ev_create(const char *name);        /* drv.c, thunks.s: the RTOS event flags of CPU 0 */
int drv_ev_wait(int id, u32 ms);
int drv_ev_set(int id);
int drv_ev_clear(int id);
u32 drv_fw_job_wait(u8 *h);                 /* thunks.s: 0x6038a70c, the result (0 - the pass ended well) */
u32 drv_fw_job_buf(u8 *h, u32 type);        /* thunks.s: 0x603868ea, the slot's index, 48 - none */
u32 drv_fw_win_map(u32 bus, u32 flags);     /* thunks.s: 0x60411572, the window's address, 0 - none */
u32 drv_fw_win_unmap(u32 win);              /* thunks.s: 0x604116a1, 0 - not released */
void drv_fw_cache_flush(void);              /* thunks.s: 0x60069fbf, this CPU's cache written back and dropped */
u32 drv_grain_shot(u32 h);                  /* graindrv.c: the shot of our job h, 0 - none */
u32 drv_fw_sf_down(u8 *h);                  /* thunks.s: 0x603937f8, the reduction 0x400 -> the slot of 0x1060 (u16) */
u32 drv_fw_xfer(u8 *h, u32 slot, u32 cb, u32 *dep);    /* thunks.s: 0x6038c50f, the pass to the outputs -> 0 or a code */
void drv_tramp_optics_final(void);          /* thunks.s: our last step (403), d0 drv_optics_final's */


static u32 op_row(volatile op_t *o, u32 h);
static void op_layer_step(volatile op_t *o, volatile op_rec_t *r, u8 *h, u32 wait_rc);
static void op_fallback(volatile op_t *o, volatile op_rec_t *r, u8 *h, u32 k);
static void op_layer_work(volatile op_t *o, volatile op_rec_t *r, pace_t *p);
static const volatile drv_optics_layer_cfg *op_cfg(volatile op_t *o, u32 set);

/* The empty loop: n iterations of 2 instructions */
static void op_spin(u32 n)
{
    __asm__ __volatile__("1:\n\tadd -1,%0\n\tbne 1b" : "+d"(n) : : "cc");
}

/* 1 - over the budget; a tick given away every OPT_YIELD_US */
static int op_pace(pace_t *p)
{
    u32 t;

    if (drv_ticks_since(p->t0) >= (p->budget ? p->budget : OPT_BUDGET_US) || (p->stop && (p->stop[0] | p->stop[1])))
        return 1;                           /* stop[1]: abandon, right after stop in ow_chan_t */
    if (drv_ticks_since(p->ty) >= OPT_YIELD_US) {
        t = drv_ticks();
        drv_sleep(1);
        p->ty = drv_ticks();
        p->slept += (p->ty - t) & DRV_TICKS_MASK;
        p->yields++;
    }
    return 0;
}

/* Up to `bytes` (a multiple of 4) read from addr in words: the bytes read (less: the budget), their sum in *sum, the time
 * without the ticks given away in *us */
static u32 op_read(pace_t *p, u32 addr, u32 bytes, u32 *sum, u32 *us)
{
    u32 done = 0, s = 0, n, t = drv_ticks(), slept = p->slept;

    while (done < bytes && !op_pace(p)) {
        n = bytes - done < OPT_CHUNK ? bytes - done : OPT_CHUNK;
        s = op_read_sum((const volatile u32 *)(addr + done), n / 4, s);
        done += n;
    }
    *sum = s;
    *us = drv_ticks_since(t) - (p->slept - slept);
    return done;
}

/* The picture's slot checked for the reads (and the stripes): 0, or OE_SLOT with the reason in *detail */
static u32 op_frame_check(const u8 *s, u32 *detail)
{
    u32 bus = *(const u32 *)(s + SL_BUS), alloc = *(const u32 *)(s + SL_ALLOC), len = *(const u32 *)(s + SL_LEN);
    u32 pitch = *(const u16 *)(s + SL_PITCH), w = *(const u16 *)(s + SL_W), hh = *(const u16 *)(s + SL_H);

    *detail = *(const u16 *)(s + SL_FORMAT);
    if (*detail != FMT_YC)
        return OE_SLOT;                     /* detail: the format */
    *detail = pitch | w << 16;
    if (!w || !hh || pitch < w || (bus & 3))
        return OE_SLOT;
    *detail = len;
    if (bus < alloc || bus - alloc >= len || len - (bus - alloc) < OPT_READ ||
        len - (bus - alloc) < (u32)pitch * 2 * hh)
        return OE_SLOT;                     /* the picture and the MiB inside the allocation */
    return 0;
}

/* Level 2: the same bytes through the cached view, the cache written back and dropped before and after. Only when armed:
 * the arming is taken here, right before the read */
static void op_cached(volatile op_t *o, volatile op_rec_t *r, pace_t *p)
{
    u32 st, armed, t, sum, us;

    st = drv_lock();
    armed = o->armed;
    if (armed && r->kind == OP_BUS_WINDOW)
        o->armed = 0;
    drv_unlock(st);
    if (!armed) {
        r->c_state = CS_NOTARMED;
        return;
    }
    if (r->kind != OP_BUS_WINDOW) {
        r->c_state = CS_NOTWINDOW;          /* the arming stays for a job with a window */
        return;
    }
    r->c_state = CS_BEGUN;
    t = drv_ticks();
    drv_fw_cache_flush();
    r->c_fl0 = drv_ticks_since(t);
    r->c_bytes = op_read(p, r->win - OP_UNCACHED, r->rd_bytes, &sum, &us);
    r->c_sum = sum;
    r->c_us = us;
    t = drv_ticks();
    drv_fw_cache_flush();                   /* no line of the picture stays in the cache */
    r->c_fl1 = drv_ticks_since(t);
    r->c_state = CS_DONE;
}

/* Level 3: the stripes in the first rows, past the cache; the cache written back after them (the write buffers before
 * the next pass reads the picture) */
static void op_stripes(volatile op_rec_t *r, u32 base, pace_t *p)
{
    u32 pitch = r->pitch * 2, w = r->width, hh = r->height;
    u32 px = (w < OP_STRIPE_COLS ? w : OP_STRIPE_COLS) & ~1u, rows = hh < OP_STRIPE_ROWS ? hh : OP_STRIPE_ROWS;
    u32 y, t = drv_ticks(), slept = p->slept;

    r->s_px = px;
    for (y = 0; y < rows && !op_pace(p); y++)
        op_stripe_row((volatile u32 *)(base + y * pitch), px);
    r->s_rows = y;
    r->s_us = drv_ticks_since(t) - (p->slept - slept);
    t = drv_ticks();
    drv_fw_cache_flush();
    r->s_fl = drv_ticks_since(t);
}

/* The lowest sp of the step's code, at the places it calls from (record and crumbs) */
static void op_low(volatile op_rec_t *r)
{
    u32 sp = op_sp();

    if (sp < r->sp_min) {
        r->sp_min = sp;
        crumb()[CR_SP_MIN] = sp;
    }
}

/* The work handed to the worker task (op_worker_main): the loop of W1 or W2 and, from D on, the window. The record r
 * carries what the step found (the picture's bus address, kind, sizes); nothing of the job's handle is read here. The
 * stop and abandon words are looked at between the parts (op_pace): the worker touches no buffer after either */
static void op_work(volatile op_t *o, volatile op_rec_t *r)
{
    pace_t p;
    u32 i, t, base, sum, us, sub = r->sub, job = r->job;

    p.t0 = p.ty = drv_ticks();
    p.slept = p.yields = 0;
    p.budget = 0;
    p.stop = &o->ch.stop;
    op_crumb(job, sub, P_WORK);
    if (sub == DRV_OPTICS_W0)
        goto out;                           /* nothing: the answer at once */
    if (sub >= DRV_OPTICS_L0) {
        p.budget = PACE_NONE;               /* the layer's work is bounded by its size; the picture's read has its own */
        op_layer_work(o, r, &p);
        goto out;
    }
    r->loop_n = o->loop ? o->loop : OPT_LOOP;
    if (sub == DRV_OPTICS_W1) {             /* at once: the worker's priority 56 is below every task it could hold up */
        t = drv_ticks();
        if (o->loop_sleep) {                /* the job waits as long, the CPU is free: the wait from the CPU's work */
            r->loop_n = 0;
            r->sleep_ms = o->loop_sleep;
            drv_sleep(o->loop_sleep);
        } else {
            op_spin(r->loop_n);
        }
        r->loop_us = drv_ticks_since(t);
        op_crumb(job, sub, P_LOOP);
        goto out;
    }
    for (i = 0; i < LOOP_PARTS && !(o->ch.stop | o->ch.abandon); i++) {
        t = drv_ticks();
        if (r->loop_n >= LOOP_PARTS)
            op_spin(r->loop_n / LOOP_PARTS);
        r->loop_us += drv_ticks_since(t);
        p.ty -= OPT_YIELD_US;
        op_pace(&p);
    }
    op_crumb(job, sub, P_LOOP);
    if (sub < DRV_OPTICS_D || (o->ch.stop | o->ch.abandon))
        goto out;
    p.t0 = p.ty = drv_ticks();              /* the budget is the picture's: the loop above was a test */
    if (r->kind == OP_BUS_WINDOW) {
        op_crumb(job, sub, P_MAP);
        t = drv_ticks();
        r->win = drv_fw_win_map(r->bus, WIN_NOWAIT);
        r->t_map = drv_ticks_since(t);
        op_crumb(job, sub, P_MAPPED);
        if (!r->win) {
            r->err = OE_MAP;                /* the window is held by another user for another bank, or refused */
            goto out;
        }
        if (!op_win_ok(r->bus, r->win)) {
            r->err = OE_WIN;
            r->detail = r->win;
            goto release;
        }
        base = r->win;
    } else {
        base = op_direct(r->bus);
    }
    if (sub < DRV_OPTICS_E || (o->ch.stop | o->ch.abandon))
        goto release;
    for (i = 0; i < 16; i++)
        r->head[i] = ((const volatile u32 *)base)[i];
    op_crumb(job, sub, P_HEAD);
    if (sub < DRV_OPTICS_F)
        goto release;
    op_crumb(job, sub, P_READ);
    r->rd_bytes = op_read(&p, base, OPT_READ, &sum, &us);
    r->rd_sum = sum;
    r->rd_us = us;
    op_crumb(job, sub, P_READ_END);
    if (sub == DRV_OPTICS_FULL && o->level == 2 && !(o->ch.stop | o->ch.abandon)) {
        op_crumb(job, sub, P_CACHED);
        op_cached(o, r, &p);
        op_crumb(job, sub, P_CACHED_END);
    }
    if (sub == DRV_OPTICS_FULL && o->level == 3 && !(o->ch.stop | o->ch.abandon)) {
        op_crumb(job, sub, P_STRIPES);
        op_stripes(r, base, &p);
        op_crumb(job, sub, P_STRIPES_END);
    }
    if (drv_ticks_since(p.t0) >= OPT_BUDGET_US)
        r->err = OE_BUDGET;
release:
    if (r->kind == OP_BUS_WINDOW && r->win) {
        op_crumb(job, sub, P_UNMAP);
        t = drv_ticks();
        r->unmap_rc = drv_fw_win_unmap(r->win);
        r->t_unmap = drv_ticks_since(t);
        op_crumb(job, sub, P_UNMAPPED);
    }
out:
    r->w_yields = p.yields;
    r->w_us = drv_ticks_since(p.t0);
    op_crumb(job, sub, P_WORK_END);
}

/* The worker task (thunks.s: drv_tramp_optics_worker; created by the core task, drv_optics_worker): CPU 0, priority 56.
 * Waits for a request on its flag, does it, says done on the step's flag. Its sequence words tell a request from a
 * flag left set: a request is new while req != done. Touches the arena block, the picture through the window and its
 * own stack; the firmware only through the thunks */
static void op_wphase(u32 phase)
{
    volatile u32 *c = crumb();

    c[CR_W_TICKS] = drv_ticks();
    c[CR_W_PHASE] = phase;
}

void drv_optics_worker_main(void)
{
    volatile op_t *o = op();
    volatile u32 *c = crumb();
    u32 seq, stk, stksz, pri, t, row = 0, tid;
    int rc;

    if (!o)
        return;
    tid = op_task_pri(&stk, &stksz, &pri);
    c[CR_W_SP] = o->w_sp = op_sp();
    c[CR_W_PSW] = o->w_psw = op_psw();
    c[CR_W_PRI] = o->w_pri = pri;
    c[CR_W_FLAGS] = (o->w_flag_req & 0xffff) | (u32)(o->w_flag_done & 0xffff) << 16;
    o->w_stk = stk;
    o->w_tid = tid;                         /* alive: the core task looks for it - last */
    op_wphase(WP_STARTED);
    for (;;) {
        op_wphase(WP_WAITING);
        t = drv_ticks();
        rc = drv_ev_wait(o->w_flag_req, WORKER_IDLE_MS);
        drv_ev_clear(o->w_flag_req);
        o->w_loops++;
        o->w_rc = (u32)rc;
        c[CR_W_LOOPS] = o->w_loops;
        c[CR_W_RC] = (u32)rc;
        op_wphase(WP_WOKEN);
        if (!ow_take((ow_chan_t *)&o->ch, &seq)) {
            o->w_empty++;
            c[CR_W_EMPTY] = o->w_empty;
            if (drv_ticks_since(t) >= WORKER_QUICK_US) {
                row = 0;
                continue;
            }
            o->w_fast++;
            c[CR_W_FAST] = o->w_fast;
            if (++row >= WORKER_QUICK_MAX) {    /* the wait does not wait: never spin */
                op_wphase(WP_NAP);
                drv_sleep(WORKER_NAP_MS);
                o->w_naps++;
                c[CR_W_NAPS] = o->w_naps;
            }
            continue;
        }
        row = 0;
        op_wphase(WP_WORKING);
        if (!o->ch.abandon)
            op_work(o, (volatile op_rec_t *)o->w_rec);
        ow_answer((ow_chan_t *)&o->ch, seq);    /* after its last access to the picture */
        o->w_count++;
        c[CR_W_DONE] = o->w_count;
        op_wphase(WP_ANSWERED);
        drv_ev_set(o->w_flag_done);
    }
}

/* The step's side (optwork.h): the record r handed to the worker, then the wait for it - the firmware's own way of
 * waiting (an event flag with a timeout, as the step waits for an ISP pass), never a count on the job task's priority
 * 54. Between the waits: the job's cancel check (as the stock waits call it, 0x6038a8a5) - stop asked, the wait goes on;
 * WORKER_LATE_US - stop asked, a crumb, the wait goes on: the step does not return while the worker may touch the
 * picture. Only after WORKER_GIVE_UP_US (the worker stuck in a firmware call, or dead) the step gives up - abandon,
 * which the worker checks before every access to the picture - so that the camera does not hang for good */
typedef struct {
    volatile op_t *o;
    volatile op_rec_t *r;
    u8 *h;
    u32 t0;
} op_wctx_t;

static void op_w_wait(void *x, u32 ms)
{
    volatile op_t *o = ((op_wctx_t *)x)->o;

    drv_ev_wait(o->w_flag_done, ms);
    drv_ev_clear(o->w_flag_done);
}

static int op_w_cancelled(void *x)
{
    u8 *h = ((op_wctx_t *)x)->h;

    return drv_fw_job_cancel(h + H_P, h + H_CANCEL) != 0;
}

static u32 op_w_since(void *x)
{
    return drv_ticks_since(((op_wctx_t *)x)->t0);
}

static void op_w_note(void *x, u32 st)
{
    volatile op_rec_t *r = ((op_wctx_t *)x)->r;

    r->w_state = st;
    op_crumb(r->job, r->sub, P_LATE);
}

static void op_delegate(volatile op_t *o, volatile op_rec_t *r, u8 *h)
{
    op_wctx_t x;
    ow_env_t e;
    u32 seq;

    if (!o->w_tid) {
        r->err = OE_WORKER;
        return;
    }
    r->w_task = o->w_tid;
    o->w_rec = (u32)r;
    seq = ow_post((ow_chan_t *)&o->ch);
    crumb()[CR_WORK] = (seq & 0xffff) | (o->ch.done & 0xffff) << 16;
    op_crumb(r->job, r->sub, P_HANDED);
    x.o = o;
    x.r = r;
    x.h = h;
    x.t0 = drv_ticks();
    e.wait_done = op_w_wait;
    e.cancelled = op_w_cancelled;
    e.since = op_w_since;
    e.note = op_w_note;
    e.ctx = &x;
    drv_ev_set(o->w_flag_req);
    r->w_state = ow_wait((ow_chan_t *)&o->ch, seq, &e, WORKER_WAIT_MS, WORKER_LATE_US, WORKER_GIVE_UP_US);
    r->w_wait = drv_ticks_since(x.t0);
    crumb()[CR_WORK] = (seq & 0xffff) | (o->ch.done & 0xffff) << 16;
    op_crumb(r->job, r->sub, P_BACK);
}

/* The probe's work after the wait (wait_rc its result), by the job's substage r->sub: in the step up to C3, then handed
 * to the worker (W1 on); the record r filled, the crumbs written phase by phase */
static void op_probe(volatile op_t *o, volatile op_rec_t *r, u8 *h, u32 wait_rc)
{
    const u8 *s;
    volatile op_slot_t *d;
    pace_t p;
    u32 i, n = 0, used = 0, t, span, sub = r->sub, job = r->job;

    p.t0 = p.ty = drv_ticks();
    p.slept = p.yields = 0;
    p.budget = 0;
    p.stop = 0;
    if (sub < DRV_OPTICS_B)
        goto out;
    for (i = 0; i < SLOTS; i++) {
        s = h + H_SLOTS + SLOT_SIZE * i;
        if (!*(const u32 *)(s + SL_ALLOC))
            continue;
        used++;
        if (n == REC_SLOTS)
            continue;
        d = &r->slot[n++];
        d->type = *(const u32 *)(s + SL_TYPE);
        d->bus = *(const u32 *)(s + SL_BUS);
        d->fmt = *(const u16 *)(s + SL_FORMAT) | (u32)*(const u16 *)(s + SL_PITCH) << 16;
        d->wh = *(const u16 *)(s + SL_W) | (u32)*(const u16 *)(s + SL_H) << 16;
        d->len = *(const u32 *)(s + SL_LEN);
        d->alloc = *(const u32 *)(s + SL_ALLOC);
        d->bytes = s[SL_CPU] | s[SL_B81] << 8 | s[SL_B84] << 16;
    }
    r->nslots = n;
    r->used = used;
    r->i_frame = drv_fw_job_buf(h, BUF_FRAME) & 0xffff;   /* a u16 (its callers take the low half) */
    r->i_small = drv_fw_job_buf(h, BUF_SMALL) & 0xffff;
    op_crumb(job, sub, P_SLOTS);
    op_low(r);
    if (sub < DRV_OPTICS_C1)
        goto out;
    if (r->task != TASK_REC_IMAGE) {
        r->err = OE_TASK;                   /* the executor in another task (REC_VIEW: priority 26, under a semaphore) */
        r->detail = r->task;
        if (sub >= DRV_OPTICS_L0)
            op_fallback(o, r, h, r->step & 0xff);
        goto out;
    }
    p.ty -= OPT_YIELD_US;                   /* one tick given away now, whatever the time */
    op_pace(&p);
    op_crumb(job, sub, P_SLEPT);
    op_low(r);
    if (sub >= DRV_OPTICS_L0) {
        op_layer_step(o, r, h, wait_rc);
        goto out;
    }
    if (sub == DRV_OPTICS_C2 || sub == DRV_OPTICS_C3) {
        r->loop_n = sub == DRV_OPTICS_C2 ? LOOP_SHORT : o->loop ? o->loop : OPT_LOOP;
        t = drv_ticks();
        op_spin(r->loop_n);                 /* never 0: op_spin(0) would run 2^32 times */
        r->loop_us = drv_ticks_since(t);
        op_crumb(job, sub, P_LOOP);
    }
    if (sub < DRV_OPTICS_W0)
        goto out;
    if (sub >= DRV_OPTICS_D) {              /* the picture found and checked here, where the handle is: the worker gets
                                               the numbers */
        if (wait_rc) {
            r->err = OE_WAIT;               /* the pass before did not end well: its buffer is not ours to read */
            r->detail = wait_rc;
            goto out;
        }
        if (r->i_frame >= SLOTS) {
            r->err = OE_NOBUF;
            r->detail = r->i_frame;
            goto out;
        }
        s = h + H_SLOTS + SLOT_SIZE * r->i_frame;
        r->bus = *(const u32 *)(s + SL_BUS);
        r->err = op_frame_check(s, &i);
        if (r->err) {
            r->detail = i;
            goto out;
        }
        r->len = *(const u32 *)(s + SL_LEN) - (r->bus - *(const u32 *)(s + SL_ALLOC));
        r->pitch = *(const u16 *)(s + SL_PITCH);
        r->width = *(const u16 *)(s + SL_W);
        r->height = *(const u16 *)(s + SL_H);
        /* The kind by the bytes the probe reaches (all in one bank: one window shows one bank) - op_frame_check made
         * sure they are inside the allocation */
        span = OPT_READ;
        if (sub == DRV_OPTICS_FULL && o->level == 3 && r->pitch * 2 * OP_STRIPE_ROWS > span)
            span = r->pitch * 2 * OP_STRIPE_ROWS;
        r->kind = op_bus_kind(r->bus, span < r->len ? span : r->len);
        if (r->kind == OP_BUS_BAD) {
            r->err = OE_ADDR;
            r->detail = r->bus;
            goto out;
        }
    }
    op_low(r);
    op_delegate(o, r, h);
out:
    r->yields = p.yields;
    r->t_total = drv_ticks_since(p.t0);
}

/* The job's row (W took it), or ROWS */
static u32 op_row(volatile op_t *o, u32 h)
{
    u32 i;

    for (i = 0; i < ROWS && o->row[i].h != h; i++)
        ;
    return i;
}

/* ---- The layer (L substages) ------------------------------------------------------------------------------------ */

/* The hook at 0x60393879 (thunks.s) reads these through their uncached address: [0] the job h whose reduction takes our
 * size, 0 - none; [1] {u16 w, u16 h}, read by the firmware through a0. The step writes them on CPU 0 right before its call
 * of 0x603937f8 and clears [0] right after; the hook runs inside that call, in the same task */
u32 drv_optics_hook[16] __attribute__((aligned(64), section(".data.cpu_shared")));

static volatile u32 *op_hook(void)
{
    return (volatile u32 *)((u32)drv_optics_hook + DRV_UNCACHED);
}

/* The entry at k of the job's list put to fn (the executor reads fn when it gets there) */
static void op_put(u8 *h, u32 k, u32 fn)
{
    if (k < STEPS) {
        *(u32 *)(h + H_STEPS + 12 * k) = fn;
        *(u32 *)(h + H_STEPS + 12 * k + 4) = 0;
        *(u32 *)(h + H_STEPS + 12 * k + STEP_PENDING) = 0;
    }
}

/* The chain left out (step k is ours): PAIR - the firmware's CPU step that only waits in place of 0x603932cb (so that it
 * makes no reduction of its own and no 0x1061), the plain pass to the outputs in place of 0x603934ba; 403 - the plain pass
 * in place of our last step. Only entries that are the ones W put there */
static void op_fallback(volatile op_t *o, volatile op_rec_t *r, u8 *h, u32 k)
{
    if (r->route == DRV_OPTICS_PAIR) {
        if (k + 2 < STEPS && *(const u32 *)(h + H_STEPS + 12 * (k + 1)) == STEP_SF_UP &&
            *(const u32 *)(h + H_STEPS + 12 * (k + 2)) == STEP_SF_OUT) {
            op_put(h, k + 1, STEP_STOCK);
            op_put(h, k + 2, STEP_OUT);
        }
    } else if (k + 1 < STEPS && *(const u32 *)(h + H_STEPS + 12 * (k + 1)) == (u32)drv_tramp_optics_final) {
        op_put(h, k + 1, STEP_OUT);
    }
    r->fallback = 1;
    o->fallbacks++;
    op_crumb(r->job, r->sub, P_FINAL);
}

/* The layer's size for a picture W x H: the configured width, the height by the aspect */
static u32 op_layer_size(volatile op_t *o, u32 W, u32 H)
{
    u32 lw = o->cfg.width, lh;

    lh = W ? (lw * H + W / 2) / W : 0;
    if (lh < LAYER_MIN)
        lh = LAYER_MIN;
    return lw | lh << 16;
}

/* The step's part from L0 on (the job task, after the wait for the development's pass and the checks of the slots and
 * the task): the picture, our reduction, the worker. Any failure: the chain left out */
static void op_layer_step(volatile op_t *o, volatile op_rec_t *r, u8 *h, u32 wait_rc)
{
    const u8 *s;
    volatile u32 *hk = op_hook();
    u32 k = r->step & 0xff, i, d, sub = r->sub, w, hh, pitch, len, ri;

    r->route = o->cfg.route;
    ri = op_row(o, (u32)h);
    if (ri < ROWS) {
        o->row[ri].layer = 0;
        o->row[ri].rec = (u32)r;
    }
    if (wait_rc) {
        r->err = OE_WAIT;
        r->detail = wait_rc;
        goto fail;
    }
    if (r->i_frame >= SLOTS) {
        r->err = OE_NOBUF;
        r->detail = r->i_frame;
        goto fail;
    }
    s = h + H_SLOTS + SLOT_SIZE * r->i_frame;
    r->bus = *(const u32 *)(s + SL_BUS);
    r->err = op_frame_check(s, &d);
    if (r->err) {
        r->detail = d;
        goto fail;
    }
    r->len = *(const u32 *)(s + SL_LEN) - (r->bus - *(const u32 *)(s + SL_ALLOC));
    r->pitch = *(const u16 *)(s + SL_PITCH);
    r->width = *(const u16 *)(s + SL_W);
    r->height = *(const u16 *)(s + SL_H);
    if (sub == DRV_OPTICS_L0 && r->route == DRV_OPTICS_PAIR)
        goto ok;                            /* the firmware's chain as it is: its own reduction in 0x603932cb */
    if (r->i_small < SLOTS) {
        r->err = OE_BUFFER;                 /* a small frame there already: not ours */
        r->detail = r->i_small;
        goto fail;
    }
    if (sub >= DRV_OPTICS_L1) {
        r->l_asked = op_layer_size(o, r->width, r->height);
        hk[1] = r->l_asked;
        hk[0] = (u32)h;
    }
    op_crumb(r->job, sub, P_DOWN);
    i = drv_ticks();
    r->down_rc = drv_fw_sf_down(h) & 0xffff;
    r->t_down = drv_ticks_since(i);
    hk[0] = 0;
    op_crumb(r->job, sub, P_DOWN_END);
    if (r->down_rc >= SLOTS) {
        r->err = OE_DOWN;
        r->detail = r->down_rc;
        goto fail;
    }
    s = h + H_SLOTS + SLOT_SIZE * r->down_rc;
    r->l_bus = *(const u32 *)(s + SL_BUS);
    w = *(const u16 *)(s + SL_W);
    hh = *(const u16 *)(s + SL_H);
    pitch = *(const u16 *)(s + SL_PITCH);
    len = *(const u32 *)(s + SL_LEN);
    r->l_wh = w | hh << 16;
    r->l_pitch = pitch;
    r->l_len = len;
    r->detail = *(const u16 *)(s + SL_FORMAT);
    if (*(const u32 *)(s + SL_TYPE) != BUF_SMALL || r->detail != FMT_LAYER || !w || !hh || pitch < w ||
        (r->l_asked && r->l_wh != r->l_asked) || (r->l_bus & 3) || r->l_bus < *(const u32 *)(s + SL_ALLOC) ||
        len < pitch * 4 * hh || w > LAYER_MAX || hh > LAYER_MAX) {
        r->err = OE_LAYER;                  /* detail: the format */
        goto fail;
    }
    r->l_kind = op_bus_kind(r->l_bus, pitch * 4 * hh);
    if (r->l_kind == OP_BUS_BAD) {
        r->err = OE_ADDR;
        r->detail = r->l_bus;
        goto fail;
    }
    if (sub >= DRV_OPTICS_L1) {
        op_low(r);
        op_delegate(o, r, h);               /* the worker: from here on it may touch the layer and the picture */
        if (r->err || (r->w_state & (OW_LATE | OW_ABANDON | OW_CANCEL))) {
            if (!r->err)
                r->err = OE_STOPPED;
            goto fail;
        }
    }
ok:
    if (ri < ROWS && o->row[ri].h == (u32)h)
        o->row[ri].layer = 1;
    return;
fail:
    op_fallback(o, r, h, k);
}

/* The window (or the direct address) to [bus, bus + len): the base to address bus by, 0 - none (r->err set). *win: the
 * window to release, 0 - none */
static u32 op_map(volatile op_rec_t *r, u32 bus, u32 len, u32 *win)
{
    u32 kind = op_bus_kind(bus, len), w;

    *win = 0;
    if (kind == OP_BUS_DIRECT)
        return op_direct(bus);
    if (kind != OP_BUS_WINDOW) {
        r->err = OE_ADDR;
        r->detail = bus;
        return 0;
    }
    w = drv_fw_win_map(bus, WIN_NOWAIT);
    r->maps++;
    if (!w) {
        r->err = OE_MAP;
        r->detail = bus;
        return 0;
    }
    if (!op_win_ok(bus, w)) {
        drv_fw_win_unmap(w);
        r->err = OE_WIN;
        r->detail = w;
        return 0;
    }
    *win = w;
    return w;
}

static void op_unmap(volatile op_rec_t *r, u32 win)
{
    if (win && !drv_fw_win_unmap(win))
        r->unmaps_bad++;
}

/* The planes and tables in the work buffer (cached, CPU 0 only; ol_layout); 0 - the layer does not fit it */
static int op_planes(volatile op_t *o, ol_t *L, u32 lw, u32 lh)
{
    mem_set(L, 0, sizeof(*L));
    if (!o->wbuf || ol_layout(L, o->wbuf, lw, lh) > o->wbuf + o->wsize)
        return 0;
    return 1;
}

/* The bench (L4, before the read; cheap: ~1 ms each): the row pass of a box (r 1) over OP_BENCH_N values,
 * OP_BENCH_REPS times, in the work buffer (card164: the same time as on the stack and in core's memory - the memory does
 * not slow it); then loops of OP_TURNS turns, b = b x a + 1: plain (an exclusive or for the multiplication), with the
 * CPU's mulu (the compiler's two nops before it), with the extension unit's udf01 (OL_MUL) and with its udf00 + udf15
 * (ol_mulhi, the high word: the means). The loops of mulu and udf01 must end with the same b */
#define OP_BENCH_N 324
#define OP_BENCH_REPS 8
#define OP_BENCH_TMP (OP_BENCH_N + 2 * 1 + 6)
#define OP_TURNS 10000

static u32 op_bench_box(u16 *v, u16 *tmp)
{
    u32 i, ia, ib, t;

    for (i = 0; i < OP_BENCH_N; i++)
        v[i] = (u16)(i * 13 & 4095);
    ol_box_weights(1, 128, &ia, &ib);
    t = drv_ticks();
    for (i = 0; i < OP_BENCH_REPS; i++)
        ol_box_row(v, OP_BENCH_N, 0, OP_BENCH_N, 1, ia, ib, tmp);
    return drv_ticks_since(t);
}

static u32 __attribute__((noipa)) op_loop_plain(u32 n, u32 a)
{
    u32 b = 1;

    do {
        b = (b ^ a) + 1;                    /* as the loop below with an exclusive or for the multiplication */
    } while (--n);
    return b;
}

static u32 __attribute__((noipa)) op_loop_mul(u32 n, u32 a)
{
    u32 b = 1;

    do {
        b = b * a + 1;
    } while (--n);
    return b;
}

static u32 __attribute__((noipa)) op_loop_udf(u32 n, u32 a)
{
    u32 b = 1;

    do {
        b = OL_MUL(b, a) + 1;
    } while (--n);
    return b;
}

static u32 __attribute__((noipa)) op_loop_hi(u32 n, u32 a)
{
    u32 b = 1;

    do {
        b = ol_mulhi(b & 0x7fffffff, a) + n;
    } while (--n);
    return b;
}

static void op_bench(volatile op_rec_t *r, ol_t *L)
{
    volatile u32 sink;                      /* the loops' results kept: the compiler leaves no loop out */
    u32 t, a = r->job | 0x10003, b;

    if (L->lw * L->lh >= OP_BENCH_N + OP_BENCH_TMP)
        r->bx_wbuf = op_bench_box(L->pc, L->pc + OP_BENCH_N);   /* the second Gaussian's plane: not in use yet */
    t = drv_ticks();
    sink = op_loop_plain(OP_TURNS, a);
    r->lp_plain = drv_ticks_since(t);
    t = drv_ticks();
    sink = b = op_loop_mul(OP_TURNS, a);
    r->lp_mul = drv_ticks_since(t);
    t = drv_ticks();
    sink = op_loop_udf(OP_TURNS, a);
    r->lp_udf = drv_ticks_since(t);
    r->lp_same = sink == b;
    t = drv_ticks();
    sink = op_loop_hi(OP_TURNS, a);
    r->lp_hi = drv_ticks_since(t);
    (void)sink;
}

/* One row of the picture timed (L4, before the read, out of its budget): the first row, words read with their sum,
 * then read into the sums (into scratch: the second Gaussian's plane, not in use yet) - past the cache, the packing read
 * loading words; with read_cached also through the cached view, loading bytes, the cache written back and dropped
 * before, between and after (a line kept from the first read would make the second one cheaper) */
static void op_row_probe(volatile op_t *o, volatile op_rec_t *r, ol_t *L)
{
    u32 base, win, t, n = r->width / 2, sum;
    u16 *db = L->pc, *dh = db + L->lw;

    base = op_map(r, r->bus, 2 * r->width, &win);
    if (!base) {
        r->pr_err = r->err ? r->err : OE_MAP;
        r->err = 0;                         /* the probe only */
        return;
    }
    t = drv_ticks();
    sum = op_read_sum((const volatile u32 *)base, n, 0);
    r->pr_read = drv_ticks_since(t);
    L->rbytes = 0;
    t = drv_ticks();
    ol_read_row(L, (const volatile u32 *)base, db, dh);
    r->pr_sums = drv_ticks_since(t);
    if (op_cfg(o, r->set)->cached && win) {
        drv_fw_cache_flush();
        t = drv_ticks();
        sum += op_read_sum((const volatile u32 *)(base - OP_UNCACHED), n, 0);
        r->pr_cread = drv_ticks_since(t);
        drv_fw_cache_flush();
        L->rbytes = 1;
        t = drv_ticks();
        ol_read_row(L, (const volatile u32 *)(base - OP_UNCACHED), db, dh);
        r->pr_csums = drv_ticks_since(t);
        drv_fw_cache_flush();               /* no line of the picture stays in the cache */
    }
    r->rd_sum = sum;
    op_unmap(r, win);
}

/* The picture's rows read into the sources (L4, [optics] read; card169): the cover read of optlayer.h - every row of cells
 * from OL_COVER rows of the picture (ol_cover_y: 4 and 12 of its 16), one pixel of each pair, to the end whatever the
 * time, so the sources of a picture are the same from job to job (card168 read in passes until a budget of 70 ms ran out:
 * 56..141 rows of cells of 243 by the load of the CPU, one row of 16 each, the rest from the layer's Y - a wire's glint on
 * the row read made its cells' sources 15 times what they are, and which glints glowed changed from press to press).
 * The rows go down the picture in order, a window a bank (mapped when the next row is in another one); a row across two
 * banks is replaced by the row after it (wholly in the next bank: a row is far shorter than a bank). Through the window
 * past the cache, or through its cached view (the cache written back and dropped after the map and before the release).
 * Between rows op_pace gives the CPU away every OPT_YIELD_US and looks at the stop and abandon words: a stop ends the
 * read, and the rows of cells not read then take the layer's own Y (ol_sources) - as do all of them when no window
 * comes (an error). Only the stop words and an error end it: no budget */
static void op_frame_read(volatile op_t *o, volatile op_rec_t *r, ol_t *L, const ol_par_t *P, pace_t *p)
{
    const volatile drv_optics_layer_cfg *cf = op_cfg(o, r->set);
    u32 W = r->width, H = r->height, rb = r->pitch * 2, lw = L->lw, lh = L->lh, cy, k, y, bus, bank, cur = ~0u, win = 0;
    u32 base = 0, a, t0, ts, tr, sum = 0, s0 = p->slept, cached = cf->cached;

    if (ol_columns(L->cwp, W, lw)) {
        r->f_err = 1;
        return;
    }
    L->shift = ol_read_shift(L->cwp, lw, OL_COVER / 2);   /* OL_COVER rows of one pixel a pair: as one row of both */
    ol_read_table(L, P);
    ol_rows(L, H);
    op_row_probe(o, r, L);                  /* out of the read: a row of both pixels, timed */
    L->rbytes = cached;                     /* through the cache the packing read loads bytes, past it words */
    t0 = drv_ticks();
    for (cy = 0; cy < lh; cy++) {
        for (k = 0; k < OL_COVER; k++) {
            y = ol_cover_y(L, cy, k, H);
            if (y >= H)
                continue;
            bus = r->bus + OL_MUL(y, rb);
            if (bus >> 27 != (bus + 2 * W - 1) >> 27) {   /* across two banks: the next row (the one before at the end) */
                y = y + 1 < H ? y + 1 : y - 1;
                bus = r->bus + OL_MUL(y, rb);
            }
            bank = bus >> 27;
            if (bank != cur) {
                ts = drv_ticks();
                if (cur != ~0u) {
                    if (cached)
                        drv_fw_cache_flush();   /* no line of the picture stays in the cache */
                    op_unmap(r, win);
                    cur = ~0u;
                }
                base = op_map(r, bus, 2 * W, &win);
                r->f_maps++;
                if (!base) {
                    r->f_err = 2;
                    r->err = 0;             /* the read only: the rows of cells not read from the layer's own Y */
                    r->f_maps_us += drv_ticks_since(ts);
                    goto out;
                }
                base = win ? base & ~OP_BANK_MASK : 0;   /* a window shows the whole bank; 0: the CPU's own memory */
                if (cached)
                    drv_fw_cache_flush();
                cur = bank;
                r->f_maps_us += drv_ticks_since(ts);
            }
            if (op_pace(p))                 /* a stop: the rest from the layer's Y */
                goto out;
            tr = drv_ticks();
            a = base ? base | (bus & OP_BANK_MASK) : op_direct(bus);
            ol_cover_cells(L, (const volatile u32 *)(cached ? a - OP_UNCACHED : a), cy, k);
            r->f_rows++;
            tr = drv_ticks_since(tr);
            sum += tr;
            if (tr > r->f_row_max)
                r->f_row_max = tr;
        }
    }
out:
    if (cur != ~0u) {
        ts = drv_ticks();
        if (cached)
            drv_fw_cache_flush();
        op_unmap(r, win);
        r->f_maps_us += drv_ticks_since(ts);
    }
    for (k = 0; k < lh; k++)
        if (L->nrow[k])
            r->f_cells++;
    r->f_us = drv_ticks_since(t0);
    r->f_row_avg = r->f_rows ? sum / r->f_rows : 0;
    r->f_slept = p->slept - s0;
}

/* The Y bytes of n layer words into y: p - the byte of Y in the first word (each byte loaded once) */
static OL_KERNEL void op_layer_y(u8 *y, const volatile u8 *p, u32 n)
{
    u8 *e = y + n;

    do {
        *y++ = *p;
        p += 4;
    } while (y != e);
}

/* The Gaussians' pace (ol_t's callback, after each pass): the pass's time without the ticks given away into us[part],
 * then op_pace - the CPU given away when due, the stop and abandon words looked at */
typedef struct {
    pace_t *p;
    u32 t;                                  /* the end of the last mark (after its tick given away) */
    u32 us[2];                              /* OL_ROWS, OL_COLS */
} op_gpace_t;

static u32 op_gpace(void *ctx, u32 part)
{
    op_gpace_t *g = ctx;
    u32 stop;

    g->us[part & 1] += drv_ticks_since(g->t);
    stop = (u32)op_pace(g->p);
    g->t = drv_ticks();
    return stop;
}

/* The worker's part from L1 on: the layer read (L1: its words' statistics), written neutral (L2), the pattern (L3), or
 * the effect (L4). The stop and abandon words between rows: nothing of the layer or the picture touched after either */
static void op_layer_work(volatile op_t *o, volatile op_rec_t *r, pace_t *p)
{
    ol_t L;
    ol_par_t P;
    ol_lanes_t ln;
    op_gpace_t gp;
    u32 lw = r->l_wh & 0xffff, lh = r->l_wh >> 16, lp = r->l_pitch * 4, sub = r->sub, x, yy, i, w, base, win, t, fl;
    u32 k, t1, s0, s1, stop = 0, lc, sb, sh;
    volatile u32 *row;
    const volatile drv_optics_layer_cfg *cf = op_cfg(o, r->set);

    op_crumb(r->job, sub, P_LAYER);
    P.py = o->order & 0xff;
    P.pcb = o->order >> 8 & 0xff;
    P.pcr = o->order >> 16 & 0xff;
    r->order = o->order;
    if (sub == DRV_OPTICS_L2 || sub == DRV_OPTICS_L3) {
        t = drv_ticks();
        base = op_map(r, r->l_bus, lp * lh, &win);
        if (!base)
            goto out;
        for (yy = 0; yy < lh && !op_pace(p); yy++) {
            row = (volatile u32 *)(base + yy * lp);
            for (x = 0; x < lw; x++)
                row[x] = sub == DRV_OPTICS_L2 ? ol_neutral(&P) : ol_pattern(&P, x, lw);
        }
        drv_fw_cache_flush();               /* the bus's write buffers before the next pass reads the layer */
        op_unmap(r, win);
        r->t_lwrite = drv_ticks_since(t);
        if (yy < lh)
            r->err = OE_STOPPED;
        goto out;
    }
    if (!op_planes(o, &L, lw, lh)) {
        r->err = OE_LAYER;
        r->detail = lw | lh << 16;
        goto out;
    }
    drv_fw_cache_flush();                   /* no old line over the work buffer (its cached view, CPU 0 alone) */
    for (i = 0; i < 128; i++)
        ((u32 *)L.lin)[i] = ((const volatile u32 *)o->lin)[i];
    for (i = 0; i < OL_ENC / 4; i++)
        ((u32 *)L.enc)[i] = ((const volatile u32 *)o->enc)[i];
    t = drv_ticks();
    base = op_map(r, r->l_bus, lp * lh, &win);
    if (!base)
        goto out;
    lc = sub == DRV_OPTICS_L4 && cf->lcached && win;    /* only a window has the cached view */
    r->lcached = lc;
    if (lc) {
        drv_fw_cache_flush();               /* no line left of an earlier map of the window's addresses */
        base -= OP_UNCACHED;
    }
    ol_lanes_init(&ln);
    if (sub == DRV_OPTICS_L4) {
        for (i = 0; i < 16; i++)
            r->head[i] = ((const volatile u32 *)base)[i];
        for (yy = 0, i = 0; yy < lh && !op_pace(p); yy++, i += lw)
            op_layer_y(L.y + i, (const volatile u8 *)(base + OL_MUL(yy, lp)) + P.py, lw);
    } else {
        for (yy = 0, i = 0; yy < lh && !op_pace(p); yy++) {
            row = (volatile u32 *)(base + yy * lp);
            for (x = 0; x < lw; x++, i++) {
                w = row[x];
                if (i < 16)
                    r->head[i] = w;
                if (sub == DRV_OPTICS_L1)
                    ol_lanes_add(&ln, w);
                L.y[i] = (u8)(w >> (8 * P.py));
            }
        }
    }
    if (lc)
        drv_fw_cache_flush();               /* the layer's lines dropped before the window is released */
    op_unmap(r, win);
    r->t_lread = drv_ticks_since(t);
    if (yy < lh) {
        r->err = OE_STOPPED;
        goto out;
    }
    if (sub == DRV_OPTICS_L1) {
        r->lanes[0] = ln.n;
        for (i = 0; i < OL_LANES; i++) {
            r->lanes[1 + i] = ln.min[i];
            r->lanes[1 + OL_LANES + i] = ln.max[i];
            r->lanes[1 + 2 * OL_LANES + i] = ln.sum[i];
            r->lanes[1 + 3 * OL_LANES + i] = ln.sq[i];
        }
        goto out;
    }
    /* L4: the effect */
    fl = ol_floor_sigma(r->width, lw);
    P.tb = L.lin[cf->tb > 255 ? 255 : cf->tb];
    P.th = L.lin[cf->th > 255 ? 255 : cf->th];
    P.sb1 = r->sig[0] = ol_cpu_sigma(cf->sb & 0xffff, fl, r->width, lw);
    P.sb2 = r->sig[1] = ol_cpu_sigma(cf->sb >> 16, fl, r->width, lw);
    P.mixb = cf->mixb;
    P.sh1 = r->sig[2] = ol_cpu_sigma(cf->sh & 0xffff, fl, r->width, lw);
    P.sh2 = r->sig[3] = ol_cpu_sigma(cf->sh >> 16, fl, r->width, lw);
    P.mixh = cf->mixh;
    sb = r->str & 0xff;                     /* the job's strengths: steps of the strongest's gains */
    sh = r->str >> 8 & 0xff;
    P.gb = sb && sb <= 5 ? (cf->gb * cf->steps[0][sb - 1] + 50) / 100 : 0;
    P.gh = sh && sh <= 5 ? (cf->gh * cf->steps[1][sh - 1] + 50) / 100 : 0;
    P.dark = cf->dark;
    P.hcb = r->str & OP_MONO ? 0 : cf->hcb;  /* black and white: a light halo without colour, its light kept */
    P.hcr = r->str & OP_MONO ? 0 : cf->hcr;
    P.em = cf->em;
    P.emb = cf->emb;
    P.bdark = cf->bdark;
    P.hfirst = cf->hfirst;
    P.tmix = P.gb ? (cf->tmix > DRV_OPTICS_TAIL_MAX ? DRV_OPTICS_TAIL_MAX : cf->tmix) : 0;
    P.haze = cf->haze;                      /* in the tail's sources: none without a tail (ol_word_tables) */
    P.btoe = cf->btoe;                      /* the strength's curves after the blur (0: none) */
    P.bmax = cf->bmax;
    P.htoe = cf->htoe;
    P.hmax = cf->hmax;
    P.st = r->sig[4] = P.tmix ? ol_cpu_sigma(cf->st, fl, r->width, lw) / OL_TAIL : 0;   /* in the tail's pixels */
    if (P.st < 64)
        P.tmix = 0;                         /* a tail below 0.25 of its pixel: none (ol_gauss would do nothing) */
    ol_scene(&L, DRV_OPTICS_DAY_CODE, &s0, &s1);   /* the day's weight from the layer's Y */
    r->sc_mean = s0;
    r->sc_share = s1;
    r->sc_day = ol_day(s0, s1, (const u32 *)cf->day);
    P.gb = ol_day_gain(P.gb, r->sc_day, cf->day[4]);
    P.gh = ol_day_gain(P.gh, r->sc_day, cf->day[5]);
    if (P.tmix)                             /* the tail added at w / (256 - w): the near part's share in the gain */
        P.gb = (P.gb * (256 - P.tmix) + 128) >> 8;
    r->gbe = P.gb;
    r->ghe = P.gh;
    mem_set(L.pb, 0, 2 * lw * lh);
    mem_set(L.ph, 0, 2 * lw * lh);
    mem_set(L.nrow, 0, lh);
    ol_word_tables(&L, &P);
    if (cf->bench)
        op_bench(r, &L);
    if (cf->read && (P.gb || P.gh))
        op_frame_read(o, r, &L, &P, p);
    if (op_pace(p)) {
        r->err = OE_STOPPED;
        goto out;
    }
    s0 = p->slept;
    t = drv_ticks();
    ol_sources(&L, &P);
    r->t_src = drv_ticks_since(t);
    gp.p = p;
    L.pace = op_gpace;
    L.ctx = &gp;
    if (P.tmix) {
        t1 = drv_ticks();
        ol_tail_down(&L);                   /* the tail's sources, before Bloom's Gaussian blurs pb: in the tail's time */
        r->t_gt = drv_ticks_since(t1);
    }
    for (k = 0; k < 2 && !stop; k++) {      /* Bloom, Halation; after each pass the CPU given away when it is due */
        if (!(k ? P.gh : P.gb))
            continue;
        gp.us[OL_ROWS] = gp.us[OL_COLS] = 0;
        s1 = p->slept;
        gp.t = t1 = drv_ticks();
        stop = k ? ol_blur2(&L, L.ph, P.sh1, P.sh2, P.mixh) : ol_blur2(&L, L.pb, P.sb1, P.sb2, P.mixb);
        t1 = drv_ticks_since(t1) - (p->slept - s1);
        if (k)
            r->t_gh = t1;
        else
            r->t_gb = t1;
        r->g_us[k][OL_ROWS] = gp.us[OL_ROWS];
        r->g_us[k][OL_COLS] = gp.us[OL_COLS];
        if (!k && P.tmix && !stop) {        /* Bloom's tail: its Gaussian on the small plane, then into pb */
            s1 = p->slept;
            gp.t = t1 = drv_ticks();
            stop = ol_tail_blur(&L, P.st);
            if (!stop)
                ol_tail_add(&L, ol_tail_factor(P.tmix));
            r->t_gt += drv_ticks_since(t1) - (p->slept - s1);
        }
    }
    r->g_done = L.done;
    r->g_all = L.all;
    r->t_calc = drv_ticks_since(t) - (p->slept - s0);
    if (stop || op_pace(p)) {
        r->err = OE_STOPPED;
        goto out;
    }
    t = drv_ticks();
    base = op_map(r, r->l_bus, lp * lh, &win);
    if (!base)
        goto out;
    lc = cf->lcached && win;
    r->lcached = lc;
    if (lc) {
        drv_fw_cache_flush();               /* no line left of an earlier map of the window's addresses */
        base -= OP_UNCACHED;
    }
    for (yy = 0, i = 0; yy < lh && !op_pace(p); yy++, i += lw)
        ol_words(&L, &P, i, (volatile u32 *)(base + OL_MUL(yy, lp)), lw);
    drv_fw_cache_flush();                   /* through the cache: every line written back to memory, and before the
                                               window is released (a line written back later would go to whatever the
                                               window shows then); past it: the bus's write buffers. Both before the
                                               chain's next pass reads the layer */
    op_unmap(r, win);
    r->t_lwrite = drv_ticks_since(t);
    if (yy < lh)
        r->err = OE_STOPPED;
out:
    op_crumb(r->job, sub, P_LAYER_END);
}

/* The blend hook's part (graindrv.c's A, any CPU, after the image of a pass's blend was built): for the blend of our job's
 * layer from L2 on - case 11 (PAIR: input A the picture, B the enlarged layer) or 12 (403: A the small layer enlarged in
 * the pass, B the picture) - the picture's gains 1024 and the layer's LAYER_GAIN in Y and chroma: the sum. Reads the
 * arena block (uncached), ctx, request R and the image as the grain's part does */
void drv_optics_blend(u8 *ctx, u32 kase)
{
    volatile op_t *o = op();
    volatile op_rec_t *r;
    const u8 *R;
    u8 *img;
    u32 i, h, code, c = kase & 0xff, cpu, gl = LAYER_GAIN | LAYER_GAIN << 16;

    if (!o || !o->layer_on || !ctx || (c != CASE_402 && c != CASE_403))
        return;
    R = *(const u8 **)(ctx + CTX_B);
    img = *(u8 **)(ctx + CTX_IMAGE);
    if (!R || !img || R[R_KIND] != 3 || *(const u16 *)(R + R_STAGE) != STAGE_F005)
        return;
    code = *(const u16 *)(R + R_CODE);
    if (!(c == CASE_402 && code == CODE_402 && o->cfg.route == DRV_OPTICS_PAIR) &&
        !(c == CASE_403 && code == CODE_403 && o->cfg.route == DRV_OPTICS_403))
        return;
    h = (u32)R - H_B;
    i = op_row(o, h);
    if (i == ROWS || o->row[i].sub < DRV_OPTICS_L2 || !o->row[i].layer)
        return;
    img += IMG_BLEND;
    if ((u32)img >= GUARD_LO && (u32)img < GUARD_HI)
        return;                             /* never seen; a cached write there would stop the camera */
    if (c == CASE_402) {
        *(u32 *)(img + BL_GAIN_A) = GAIN_ONE;
        *(u32 *)(img + BL_GAIN_A2) = 1024;
        *(u32 *)(img + BL_GAIN_B) = gl;
        *(u32 *)(img + BL_GAIN_B2) = LAYER_GAIN;
    } else {
        *(u32 *)(img + BL_GAIN_A) = gl;
        *(u32 *)(img + BL_GAIN_A2) = LAYER_GAIN;
        *(u32 *)(img + BL_GAIN_B) = GAIN_ONE;
        *(u32 *)(img + BL_GAIN_B2) = 1024;
    }
    cpu = drv_cpu();
    if (cpu < CPUS)
        o->blends[cpu][c == CASE_403]++;
    r = (volatile op_rec_t *)o->row[i].rec;
    if (r)
        r->blend_set++;
}

/* Our last step for 403 (thunks.s: drv_tramp_optics_final, by the executor's calls; the job task): as the firmware's
 * 0x603934ba - the dependency on the pass of the step before (its +8, then 0), the pass to the outputs 0x6038c50f, our
 * +8 the new pass - but with the small frame as the input and the callback 0x6039379e (0x403: enlarge and blend in one
 * pass). Without a small frame: the plain pass's input and callback (0x60391b23's). d0: 0, or the firmware's code */
u32 drv_optics_final(u8 *h)
{
    u32 k = *(const u16 *)(h + H_STEP), idx, cb = CB_403, dep = 0, rc;

    idx = drv_fw_job_buf(h, BUF_SMALL) & 0xffff;
    if (idx >= SLOTS) {
        idx = drv_fw_job_buf(h, BUF_FRAME) & 0xffff;
        cb = CB_PLAIN;
        if (idx >= SLOTS)
            return E_NO_BUFFER;
    }
    if (k > 0 && k <= STEPS) {
        dep = *(const u32 *)(h + H_STEPS + 12 * (k - 1) + STEP_PENDING);
        *(u32 *)(h + H_STEPS + 12 * (k - 1) + STEP_PENDING) = 0;
    }
    rc = drv_fw_xfer(h, idx, cb, &dep) & 0xffff;
    if (rc)
        return rc;
    if (k < STEPS)
        *(u32 *)(h + H_STEPS + 12 * k + STEP_PENDING) = dep;
    return 0;
}

/* The step (thunks.s: drv_tramp_optics_step, by the step executor's calls; any CPU, the firmware's job task). Its result
 * is not used: the trampoline returns 0 */
u32 drv_optics_step(u8 *h)
{
    volatile op_t *o = op();
    volatile op_rec_t *r;
    u32 k, n, t, rc, seq, st, i, sub = DRV_OPTICS_A, job = 0, psw, sp, stk = 0, stksz = 0, mode = 0, set = 0, str = 0;
    volatile u32 *c;

    if (!h)
        return 0;
    if (o && (i = op_row(o, (u32)h)) < ROWS) {
        sub = o->row[i].sub;
        job = o->row[i].job;
        set = o->row[i].set;
        str = o->row[i].str;
    }
    psw = op_psw();
    sp = op_sp();
    op_crumb(job, sub, P_ENTER);
    if (sub >= DRV_OPTICS_A) {
        c = crumb();
        c[CR_PSW] = psw;
        c[CR_SP] = c[CR_SP_MIN] = sp;
        c[CR_TASK] = op_task(&stk, &stksz);
        c[CR_STK] = stk;
        c[CR_STKSZ] = stksz;
        c[CR_WORK] = 0;
        mode = *(const u32 *)(h + H_REQ);   /* the request C, when it is a DRAM address (as graindrv.c reads it) */
        mode = op_bus_kind(mode, 512) == OP_BUS_DIRECT ? *(const u16 *)(mode + C_MODE) |
               (u32)*(const u8 *)(mode + C_RAWJPEG) << 16 | (u32)*(const u8 *)(mode + C_FILETYPE) << 24 : 0;
        c[CR_MODE] = mode;
    }
    k = *(const u16 *)(h + H_STEP);
    n = *(const u16 *)(h + H_NSTEPS);
    t = drv_ticks();
    rc = drv_fw_job_wait(h);                /* the development 0x901's pass: the picture is complete after it */
    t = drv_ticks_since(t);
    op_crumb(job, sub, P_WAITED);
    if (o) {
        st = drv_lock();
        seq = o->jobs++;
        drv_unlock(st);
        r = &o->ring[seq % RING];
        r->seq = 0;
        for (i = 1; i < sizeof(op_rec_t) / 4; i++)
            ((volatile u32 *)r)[i] = 0;
        r->shot = drv_grain_shot((u32)h);
        r->h = (u32)h;
        r->type = *(const u16 *)(h + H_TYPE);
        r->level = o->level;
        r->sub = sub;
        r->job = job;
        r->set = set <= o->nsets ? set : 0;
        r->str = str;
        if (sub >= DRV_OPTICS_A) {
            c = crumb();
            r->psw = psw;
            r->sp = r->sp_min = sp;
            r->task = c[CR_TASK];
            r->stk = c[CR_STK];
            r->stksz = c[CR_STKSZ];
            r->mode = mode;
            r->lock[0] = ((const volatile u32 *)LOCKS)[0];
            r->lock[1] = ((const volatile u32 *)LOCKS)[1];
            op_low(r);
        }
        r->cpu = drv_cpu();
        r->step = k | n << 8;
        r->t_wait = t;
        r->wait_rc = rc;
        op_probe(o, r, h, rc);
        r->seq = seq + 1;
    }
    if (k < STEPS)
        *(u32 *)(h + H_STEPS + 12 * k + STEP_PENDING) = 0;   /* as the stock step: no pass of its own to wait for */
    op_crumb(job, sub, P_OUT);
    return 0;
}

/* The job h is the second of a RAW+JPEG press (its request C: RawJpeg 1, FileType 3; read as the step reads it). The
 * camera's pictures (card168: the layer's rows read differ between the two jobs of a press, and the JPEG shows the
 * first job's, the ORF's 3200x2400 preview the second's) tell that the JPEG is the first job's picture */
static int op_second(const u8 *h)
{
    u32 c = *(const u32 *)(h + H_REQ);

    return op_bus_kind(c, 512) == OP_BUS_DIRECT && *(const u8 *)(c + C_RAWJPEG) == 1 && *(const u8 *)(c + C_FILETYPE) == 3;
}

/* W (graindrv.c, CPU 0), after the development 0x901 of our job h: the steps after it into out[] - ours, then for the
 * layer's substages the chain (PAIR: 0x603932cb, 0x603934ba; 403: our last step) - their number, 0 - none (no probe or
 * layer, substage N, or no room: `room` entries are free before the selector's step). Without the chain the caller puts
 * the plain pass to the outputs after them. The job gets a row with the substage in force (also at N: H3 counts the
 * job) */
u32 drv_optics_list(u8 *h, u32 room, u32 *out)
{
    volatile op_t *o = op();
    u32 i, sub, n, str = 0, set;
    const volatile drv_optics_layer_cfg *cf;

    if (!o || !h)
        return 0;
    sub = o->sub;
    set = o->set;
    if (sub >= DRV_OPTICS_L0 && !o->layer_on)
        sub = DRV_OPTICS_N;                 /* the layer not set up (drv_optics_layer failed): no step */
    if (sub == DRV_OPTICS_L4) {             /* the strengths: the set's own (a test), else the look's rows' */
        cf = op_cfg(o, set);
        str = (cf->sbv != DRV_OPTICS_ROWS ? cf->sbv : o->str & 0xff) |
              (cf->shv != DRV_OPTICS_ROWS ? cf->shv : o->str >> 8 & 0xff) << 8;
        if (cf->sbv != DRV_OPTICS_ROWS || cf->shv != DRV_OPTICS_ROWS)
            str |= OP_TEST;
        str |= o->str & OP_MONO;            /* black and white: the tables in force, whoever gave the strengths */
        if (!(str & 0xffff))
            sub = DRV_OPTICS_N;             /* Bloom and Halation 0: no step at all (the job as without the layer) */
        else if (cf->jobs == DRV_OPTICS_JOBS_FIRST && op_second(h)) {
            sub = DRV_OPTICS_N;             /* the ORF's preview: no step (the job as without the layer) */
            o->seconds++;
        }
    }
    n = sub < DRV_OPTICS_L0 ? 2 : o->cfg.route == DRV_OPTICS_PAIR ? 3 : 2;   /* with the plain pass after ours */
    if (sub != DRV_OPTICS_N && room < n) {
        o->noroom++;
        return 0;
    }
    i = op_row(o, (u32)h);                  /* a row of a job at this handle that never ended, or a free one, or the
                                               next in turn (a job that never ended: its count is lost) */
    if (i == ROWS)
        i = op_row(o, 0);
    if (i == ROWS)
        i = o->wl % ROWS;
    o->row[i].h = 0;
    o->row[i].sub = sub;
    o->row[i].job = ++o->wl;
    o->row[i].layer = 0;
    o->row[i].rec = 0;
    o->row[i].set = set;
    o->row[i].str = str;
    o->row[i].h = (u32)h;
    crumb()[CR_LISTED] = o->wl;
    op_crumb(o->wl, sub, P_LISTED);
    if (sub == DRV_OPTICS_N)
        return 0;
    o->listed++;
    out[0] = sub == DRV_OPTICS_A0 ? STEP_STOCK : (u32)drv_tramp_optics_step;
    if (sub < DRV_OPTICS_L0)
        return 1;
    if (o->cfg.route == DRV_OPTICS_PAIR) {
        out[1] = STEP_SF_UP;
        out[2] = STEP_SF_OUT;
        return 3;
    }
    out[1] = (u32)drv_tramp_optics_final;
    return 2;
}

/* H3 (graindrv.c, any CPU): our job h ended - counted by its substage, its row freed */
void drv_optics_ended(u8 *h)
{
    volatile op_t *o = op();
    u32 i, cpu = drv_cpu();

    if (!o || (i = op_row(o, (u32)h)) == ROWS)
        return;
    op_crumb(o->row[i].job, o->row[i].sub, P_ENDED);
    crumb()[CR_ENDED] = o->row[i].job;
    if (cpu < CPUS && o->row[i].sub < SUBS)
        o->ended[cpu][o->row[i].sub]++;
    o->row[i].h = 0;
}

/* Core task, once: the firmware's functions the step calls are the ones expected (their first bytes, read past the
 * cache), the arena block, the level. 0, or a code */
static int install(u32 level)
{
    static const struct {
        u32 addr;
        u8 b[4];
    } fn[7] = {
        {0x6038a70c, {0x25, 0x30, 0x2c, 0xf1}},   /* the wait for the pass before: mov 11312,a1; add a0,a1 */
        {0x603868ea, {0x82, 0x00, 0x04, 0xfa}},   /* the buffer search: mov d0,d2; clr d0; clr d1; add 6592,a0 */
        {0x60411572, {0x42, 0x18, 0x00, 0x42}},   /* the window map: mov d0,(24,sp); clr d0; mov d0,(28,sp) */
        {0x604116a1, {0x42, 0x20, 0x00, 0x42}},   /* the window release: mov d0,(32,sp); clr d0; mov d0,(24,sp) */
        {0x60069fbf, {0x00, 0xcd, 0x85, 0x46}},   /* the cache write-back: clr d0; call 0x6006e645 */
        {0x60051d5c, {0x96, 0xf8, 0x92, 0x0c}},   /* acre_Task: mov a1,a2; movbu d0,(12,sp) */
        {0x6038a8a5, {0x96, 0xb0, 0x00, 0xc8}},   /* the cancel check: mov a1,a2; cmp 0,a0; beq */
    };
    volatile op_t *o;
    u32 i, k;

    if (level > 3)
        return 1;                           /* 0: the layer alone (drv_optics_layer), no probe's level */
    for (i = 0; i < 7; i++)
        for (k = 0; k < 4; k++)
            if (*(volatile u8 *)(fn[i].addr + k + DRV_UNCACHED) != fn[i].b[k])
                return 10 + i;
    o = mem_alloc(sizeof(op_t));
    if (!o)
        return 2;
    mem_set((void *)o, 0, sizeof(op_t));
    o->level = level;
    o->sub = DRV_OPTICS_N;                  /* no step until core gives a substage (drv_optics_sub) */
    slot()[1] = (u32)o;                     /* before any list can take the step */
    return 0;
}

int drv_optics_install(u32 level)
{
    if (install_rc < 0)
        install_rc = install(level);
    return install_rc;
}

void drv_tramp_optics_size(void);           /* thunks.s: the hook at 0x60393879 */
#define SIZE_SITE 0x60393879                /* in 0x603937f8: a0 -> {w, h} of the small frame from 0x6037eb7a, then the
                                               format 0x305 into its descriptor (mov 773,d0; movhu d0,(8,a2)) */

/* The high word of x m by halves with the compiler's multiplication: the reference for ol_mulhi */
static u32 op_mulhi_ref(u32 x, u32 m)
{
    u32 xl = x & 0xffff, xh = x >> 16, ml = m & 0xffff, mh = m >> 16, lh = xl * mh, hl = xh * ml;

    return xh * mh + (lh >> 16) + (hl >> 16) + (((xl * ml) >> 16) + (lh & 0xffff) + (hl & 0xffff)) / 65536;
}

/* One extended box along n values (stride step) by its definition, the edges clamped, the compiler's multiplication: the
 * first version of the layer's box (the host test's reference), for the start check */
static void op_box_ref(u16 *v, u32 n, u32 step, u32 r, u32 alpha, u16 *tmp)
{
    u32 i, sum = 0, w = (2 * r + 1) * 256 + 2 * alpha, ia = (65536u * 256 + w / 2) / w, ib = alpha * ia >> 8, e;
    s32 k;

    for (i = 0; i < n; i++)
        tmp[i] = v[i * step];
    for (k = -(s32)r; k <= (s32)r; k++)
        sum += tmp[k < 0 ? 0 : (u32)k < n ? (u32)k : n - 1];
    for (i = 0; i < n; i++) {
        e = tmp[i >= r + 1 ? i - r - 1 : 0] + tmp[i + r + 1 < n ? i + r + 1 : n - 1];
        v[i * step] = (u16)((sum * ia + e * ib + 32768) >> 16);
        sum += tmp[i + r + 1 < n ? i + r + 1 : n - 1];
        sum -= tmp[i >= r ? i - r : 0];
    }
}

/* Core task, at the layer's set-up: the kernels that use the extension unit or assembly against plain C, on scratch at
 * b (uncached, KCHECK_BYTES and a small layer after it; any CPU): OL_MUL against the compiler's mul, ol_mulhi against
 * op_mulhi_ref (64 pairs each), ol_read_pack loading bytes and loading words against ol_read_run (8 cells of a
 * pseudo-random row), and ol_gauss (its row and column kernels with the unit's multiplies: two columns a step and one)
 * against op_box_ref's passes on small sparse planes. In the emulator this runs at every start, on the camera too - a
 * kernel the CPU computes otherwise keeps the layer off. 0, or a bit set: 1 OL_MUL, 2 ol_mulhi, 4 the read by bytes, 8 by
 * words, 16 the Gaussian, 32 the cover read by bytes, 64 by words */
#define KCHECK_CELLS 8
#define KCHECK_BYTES (1024 + 4 * OL_READ_RUN * KCHECK_CELLS + 4 * KCHECK_CELLS)
#define KCHECK_LAYER 49152                  /* bytes for ol_layout of the small planes (20 or 21 x 10) and its tables */
static u32 op_kcheck(u8 *b)
{
    u32 *tab = (u32 *)b, *row = tab + 256, *out = row + OL_READ_RUN * KCHECK_CELLS, i, k, j, x = 0x12345678, y, e = 0;
    u32 w, h, sg, r, a;
    ol_t L;

    for (i = 0; i < 64; i++) {
        x = x * 1103515245u + 12345;
        y = x ^ x >> 13;
        if (i & 1)
            y &= 0xffff;                    /* the Gaussians' and the words' factors are small */
        if (OL_MUL(x, y) != x * y)
            e |= 1;
        if (ol_mulhi(x >> 1, y >> 1) != op_mulhi_ref(x >> 1, y >> 1))
            e |= 2;
    }
    for (i = 0; i < 256; i++)
        tab[i] = (i * 37 & 0xfff) | ((i * 91 + 7) & 0xfff) << 16;   /* halves below 4096, as the read's table */
    for (i = 0; i < OL_READ_RUN * KCHECK_CELLS; i++) {
        x = x * 1103515245u + 12345;
        row[i] = x;
    }
    for (k = 0; k < 2; k++) {
        for (i = 0; i < KCHECK_CELLS; i++)
            out[i] = 0;
        ol_read_pack(row, out, out + KCHECK_CELLS, tab, !k);
        for (i = 0; i < KCHECK_CELLS; i++)
            if (out[i] != ol_read_run(row + OL_READ_RUN * i, row + OL_READ_RUN * (i + 1), tab))
                e |= 4 << k;
    }
    for (k = 0; k < 4; k++) {               /* the cover read's loop: bytes and words, stored and added */
        for (i = 0; i < KCHECK_CELLS; i++)
            out[i] = i * 0x00010003u;
        ol_read_pack0(row, out, out + KCHECK_CELLS, tab, !(k & 1), k >> 1);
        for (i = 0; i < KCHECK_CELLS; i++)
            if (out[i] != (k >> 1 ? i * 0x00010003u : 0) + ol_read_run0(row + OL_READ_RUN * i,
                                                                      row + OL_READ_RUN * (i + 1), tab))
                e |= 32 << (k & 1);
    }
    for (k = 0; k < 4; k++) {               /* widths 20 (pairs) and 21; sigma 3 (r 2) and 1.18 layer px (r 0) */
        w = k & 1 ? 21 : 20;
        h = 10;
        sg = k & 2 ? 302 : 768;
        mem_set(&L, 0, sizeof(L));
        ol_layout(&L, (uptr)b + KCHECK_BYTES, w, h);
        for (i = 0; i < w * h; i++) {       /* a few lights, the rest 0: the rows' ranges too */
            x = x * 1103515245u + 12345;
            L.pb[i] = L.pc[i] = x >> 28 ? 0 : (u16)(x >> 8 & 4095);
        }
        ol_gauss(&L, L.pb, sg);
        ol_box_plan(sg, &r, &a);
        for (j = 0; j < OL_BOX_PASSES; j++) {
            for (i = 0; i < h; i++)
                op_box_ref(L.pc + i * w, w, 1, r, a, L.ring);
            for (i = 0; i < w; i++)
                op_box_ref(L.pc + i, h, w, r, a, L.ring);
        }
        for (i = 0; i < w * h; i++)
            if (L.pb[i] != L.pc[i])
                e |= 16;
    }
    return e;
}

/* Core task, once, after drv_optics_install: the layer's settings, curves, work buffer and the size hook. 0, or a code:
 * 1 bad settings or no install, 2 no memory, 3 the site not as expected, 10 + a code of drv_code_jump, 20 + i: the
 * firmware function i not as expected, 30 + op_kcheck's bits: a kernel of the extension unit or of assembly does not give
 * what plain C gives */
int drv_optics_layer(const drv_optics_layer_cfg *c)
{
    static const u8 size_site[5] = {0x2c, 0x05, 0x03, 0xf8, 0x72};    /* mov 773,d0; movhu d0,(8,a2) (+ 0x08) */
    static const struct {
        u32 addr;
        u8 b[4];
    } fn[5] = {
        {0x603937f8, {0xf1, 0xd3, 0x2c, 0x60}},   /* the reduction: mov a0,d3; mov 4192,d0 */
        {0x6038c50f, {0xf1, 0xe6, 0xf1, 0xd6}},   /* the pass to the outputs: mov d1,a2; mov a1,d2 */
        {0x603932cb, {0xcf, 0xb0, 0xf8, 0xfe}},   /* the enlarging step: movm [d2,a2,a3],(sp); add -16,sp */
        {0x603934ba, {0xcf, 0xb0, 0xf8, 0xfe}},   /* the blending pass to the outputs: movm ...; add -8,sp */
        {0x6039379e, {0xcf, 0xb0, 0xf8, 0xfe}},   /* the callback of 0x403: movm ...; add -4,sp */
    };
    static int rc = -1;
    volatile op_t *o = op();
    u32 i, k, lw, sz;
    u8 *b;
    ol_t L;

    if (rc >= 0)
        return rc;
    if (!o || !c || (c->route != DRV_OPTICS_PAIR && c->route != DRV_OPTICS_403) || c->width < LAYER_MIN ||
        c->width > LAYER_MAX)
        return rc = 1;
    for (i = 0; i < 5; i++)
        for (k = 0; k < 4; k++)
            if (*(volatile u8 *)(fn[i].addr + k + DRV_UNCACHED) != fn[i].b[k])
                return rc = 20 + i;
    if (*(volatile u8 *)(SIZE_SITE + 5 + DRV_UNCACHED) != 0x08)
        return rc = 3;
    lw = c->width;
    sz = (u32)ol_layout(&L, 0, lw, lw) + 64;   /* the planes of a square picture (the largest) and the tables */
    b = mem_alloc_for(MEM_OWNER_CORE, sz, MEM_LINES);
    if (!b)
        return rc = 2;
    k = op_kcheck(b);                       /* sz is far above KCHECK_BYTES + KCHECK_LAYER */
    if (k)
        return rc = 30 + (int)k;
    mem_copy((void *)&o->cfg, c, sizeof(*c));
    ol_luts(c->gamma, (u16 *)o->lin, (u8 *)o->enc);
    o->order_auto = c->order == DRV_OPTICS_ORDER_AUTO;
    o->order = o->order_auto ? 0 | 1u << 8 | 2u << 16 : c->order;
    o->wbuf = (u32)b - DRV_UNCACHED;       /* the cached alias: the worker's alone, on CPU 0 */
    o->wsize = sz;
    k = drv_code_jump(SIZE_SITE, size_site, (void *)drv_tramp_optics_size);
    if (k)
        return rc = 10 + (int)k;
    o->layer_on = 1;
    return rc = 0;
}

/* Core task: the layer's state for the log at start: the transport's smallest sigma for a picture W wide, the work
 * buffer's bytes and its (cached) address */
u32 drv_optics_layer_floor(u32 W, u32 *wsize, u32 *wbuf)
{
    volatile op_t *o = op();

    *wsize = o ? o->wsize : 0;
    *wbuf = o ? o->wbuf : 0;
    return o && o->layer_on ? ol_floor_sigma(W, o->cfg.width) : 0;
}

/* Core task, after drv_optics_layer: the further set k (1..DRV_OPTICS_SETS) of the layer's settings - the route,
 * width, bytes and curve stay the first set's. 0, or 1 */
int drv_optics_layer_set(u32 k, const drv_optics_layer_cfg *c)
{
    volatile op_t *o = op();

    if (!o || !o->layer_on || !c || !k || k > DRV_OPTICS_SETS)
        return 1;
    mem_copy((void *)&o->xcfg[k - 1], c, sizeof(*c));
    o->xcfg[k - 1].route = o->cfg.route;
    o->xcfg[k - 1].width = o->cfg.width;
    o->xcfg[k - 1].order = o->cfg.order;
    o->xcfg[k - 1].gamma = o->cfg.gamma;
    if (k > o->nsets)
        o->nsets = k;
    return 0;
}

/* Core task: the layer's settings of the jobs listed from now on (0 - the first set; a set not given - the first) */
void drv_optics_set(u32 k)
{
    volatile op_t *o = op();

    if (o)
        o->set = k <= o->nsets ? k : 0;
}

void drv_optics_strength(u32 bloom, u32 halation, u32 mono)
{
    volatile op_t *o = op();

    if (o)
        o->str = (bloom > 5 ? 5 : bloom) | (halation > 5 ? 5 : halation) << 8 | (mono ? OP_MONO : 0);
}

/* The settings of a job's set (the worker; the arena block, uncached) */
static const volatile drv_optics_layer_cfg *op_cfg(volatile op_t *o, u32 set)
{
    return set && set <= o->nsets ? &o->xcfg[set - 1] : &o->cfg;
}

/* Core task: the substage of the jobs listed from now on */
void drv_optics_sub(u32 sub)
{
    volatile op_t *o = op();

    if (o && sub < SUBS)
        o->sub = sub;
}

/* Core task, between presses (the jobs of the one before ended): the loop of C3, W1 and W2 for the jobs from now on */
void drv_optics_loop(u32 n, u32 sleep_ms)
{
    volatile op_t *o = op();

    if (o) {
        o->loop = n;
        o->loop_sleep = sleep_ms;
    }
}

/* Core task, at start, before anything is listed: the crumbs of the run before into old[CR_N] (as they were: valid only
 * with the magic), then this run's start in their place. 1 - the magic was there */
int drv_optics_crumb(u32 boot, u32 *old)
{
    volatile u32 *c = crumb();
    u32 i;

    for (i = 0; i < CR_N; i++)
        old[i] = c[i];
    for (i = 1; i < CR_N; i++)
        c[i] = 0;
    c[CR_BOOT] = boot;
    c[CR_MAGIC] = CRUMB_MAGIC;
    return old[CR_MAGIC] == CRUMB_MAGIC;
}

/* Core task, every loop: the heartbeat (ms since start) */
void drv_optics_beat(u32 ms)
{
    crumb()[CR_BEAT] = ms;
}

/* Core task, CPU 0, once: the flags, the stack, the task; then a wait for the task to say it runs. acre_Task gives -1
 * also when its name table is full though the task was made, so the answer is the task's own (w_tid) - and a second
 * task is never made, whatever came of the first */
int drv_optics_worker(u32 *tid)
{
    static int rc = -1;
    volatile op_t *o = op();
    u8 *stk;
    u32 i;

    if (rc >= 0) {
        *tid = o ? o->w_tid : 0;
        return o && o->w_tid ? 0 : rc ? rc : 5;
    }
    *tid = 0;
    if (!o)
        return rc = 1;
    o->w_flag_req = drv_ev_create("OMK_OPTQ");
    o->w_flag_done = drv_ev_create("OMK_OPTD");
    if (o->w_flag_req <= 0 || o->w_flag_done <= 0)
        return rc = 3;
    stk = mem_alloc_for(MEM_OWNER_CORE, WORKER_STACK, MEM_LINES);
    if (!stk)
        return rc = 2;
    rc = 0;                                 /* from here on: made, or not known - never again */
    log_checkpoint("optics: worker: flags %d %d, stack %08x; making the task", o->w_flag_req, o->w_flag_done,
                   (u32)stk - DRV_UNCACHED);
    i = drv_fw_acre_task("OMK_OPTX", (void *)drv_tramp_optics_worker, WORKER_PRI, WORKER_STACK,
                         stk - DRV_UNCACHED);   /* the cached alias: the task's stack goes through the cache of CPU 0 */
    log_checkpoint("optics: worker: acre_Task gave %d", (int)i);
    for (i = 0; i < WORKER_START_MS / 10 && !o->w_tid; i++)
        drv_sleep(10);
    log_checkpoint("optics: worker: task %u after %u naps of the core task; at its start sp %08x (stack %08x), psw %08x, "
                   "priority %u (base %u)", o->w_tid, i, o->w_sp, o->w_stk, o->w_psw, o->w_pri & 0xffff, o->w_pri >> 16);
    *tid = o->w_tid;
    if (!o->w_tid)
        rc = 4;
    return rc;
}

/* Core task: the core's own wait for the test's answer: a plain wait on the flag, no job to cancel */
typedef struct {
    volatile op_t *o;
    u32 t0;
} op_tctx_t;

static void op_t_wait(void *x, u32 ms)
{
    volatile op_t *o = ((op_tctx_t *)x)->o;

    drv_ev_wait(o->w_flag_done, ms);
    drv_ev_clear(o->w_flag_done);
}

static int op_t_cancelled(void *x)
{
    (void)x;
    return 0;
}

static u32 op_t_since(void *x)
{
    return drv_ticks_since(((op_tctx_t *)x)->t0);
}

int drv_optics_worker_test(u32 *us, u32 *work, u32 *state)
{
    volatile op_t *o = op();
    volatile op_rec_t *r;
    op_tctx_t x;
    ow_env_t e;
    u32 seq, i;

    if (!o || !o->w_tid)
        return -1;
    r = &o->test;
    for (i = 0; i < sizeof(op_rec_t) / 4; i++)
        ((volatile u32 *)r)[i] = 0;
    r->sub = DRV_OPTICS_W2;                 /* the loop in parts: no picture, nothing of a job */
    o->w_rec = (u32)r;
    seq = ow_post((ow_chan_t *)&o->ch);
    x.o = o;
    x.t0 = drv_ticks();
    e.wait_done = op_t_wait;
    e.cancelled = op_t_cancelled;
    e.since = op_t_since;
    e.note = 0;
    e.ctx = &x;
    drv_ev_set(o->w_flag_req);
    *state = ow_wait((ow_chan_t *)&o->ch, seq, &e, WORKER_WAIT_MS, WORKER_LATE_US, WORKER_GIVE_UP_US);
    *us = drv_ticks_since(x.t0);
    *work = r->w_us;
    return 0;
}

void drv_optics_arm(void)
{
    volatile op_t *o = op();

    if (o)
        o->armed = 1;
}

static const char *op_err(u32 e)
{
    static const char *const name[OE_N] = {"", "the pass before failed", "no picture buffer 0x1000",
                                           "picture buffer not as expected", "address out of the known ranges",
                                           "no window (busy or refused)", "window not the expected one",
                                           "time budget used up", "not the task REC_IMAGE (0x20)",
                                           "no worker task", "no small frame from the reduction",
                                           "small frame not as expected", "the worker stopped or was late",
                                           "a small frame there already"};

    return e < OE_N ? name[e] : "?";
}

/* Core task: names for the log */
const char *drv_optics_sub_name(u32 sub)
{
    static const char *const name[SUBS] = {"N", "A0", "A", "B", "C1", "C2", "C3", "W0", "W1", "W2", "D", "E", "F", "full",
                                           "L0", "L1", "L2", "L3", "L4"};

    return sub < SUBS ? name[sub] : "?";
}

const char *drv_optics_phase(u32 phase)
{
    static const char *const name[P_N] = {"none", "listed", "step entered", "pass waited for", "slots read",
                                          "tick given away", "empty loop", "250 ms busy", "window map called",
                                          "window map returned", "64 bytes read", "1 MiB read begun", "1 MiB read",
                                          "cached read begun", "cached read", "stripes begun", "stripes written",
                                          "window release called", "window released", "step returned", "job ended",
                                          "handed to the worker", "worker began", "worker ended",
                                          "worker late: stop asked", "the step has the worker's answer",
                                          "reduction called", "reduction returned", "layer work began",
                                          "layer work ended", "chain left out"};

    return phase < P_N ? name[phase] : "?";
}

static u32 op_kbs(u32 bytes, u32 us)
{
    return us ? (bytes >> 10) * 1000000u / us : 0;
}

/* sigma Q8 as "u.hh" for the log */
static u32 op_q8i(u32 v)
{
    return v >> 8;
}

static u32 op_q8f(u32 v)
{
    return (v & 0xff) * 100 / 256;
}

/* Core task: the layer's lines of a job's block */
static void op_report_layer(volatile op_rec_t *r, u32 n)
{
    u32 k, m, v, sd[OL_LANES], mean[OL_LANES];
    char set[2] = {(char)(r->set ? 'A' + r->set - 1 : 0), 0};

    log_printf(OMK_LOG_INFO, "optics: job %u: layer %s%s%s, route %s: reduction %u us (d0 %x), small frame %ux%u (asked "
               "%ux%u) pitch %u, %u bytes at %08x (%s)", n, drv_optics_sub_name(r->sub), r->set ? ", settings " : "", set,
               r->route == DRV_OPTICS_403 ? "403" :
               "pair", r->t_down, r->down_rc, r->l_wh & 0xffff, r->l_wh >> 16, r->l_asked & 0xffff, r->l_asked >> 16,
               r->l_pitch, r->l_len, r->l_bus, r->l_kind == OP_BUS_WINDOW ? "window" : r->l_kind == OP_BUS_DIRECT ?
               "direct" : "-");
    if (r->fallback)
        log_printf(OMK_LOG_WARN, "optics: job %u: the chain LEFT OUT (%s): the plain pass to the outputs", n, op_err(r->err));
    if (r->sub >= DRV_OPTICS_L1 && r->w_task)
        log_printf(OMK_LOG_INFO, "optics: job %u: layer read %u us, sources and Gaussians %u us, written %u us%s; window maps "
                   "%u (%u not released); bytes Y %u Cb %u Cr %u", n, r->t_lread, r->t_calc, r->t_lwrite, r->lcached ?
                   " (cached view)" : "", r->maps, r->unmaps_bad, r->order & 0xff, r->order >> 8 & 0xff,
                   r->order >> 16 & 0xff);
    if (r->sub == DRV_OPTICS_L4 && r->w_task) {
        log_printf(OMK_LOG_INFO, "optics: job %u: Gaussians in layer pixels: Bloom %u.%02u + %u.%02u, Halation %u.%02u + "
                   "%u.%02u (after the transport's own); Bloom's tail %u.%02u in its plane's (x%u)", n, op_q8i(r->sig[0]),
                   op_q8f(r->sig[0]), op_q8i(r->sig[1]), op_q8f(r->sig[1]), op_q8i(r->sig[2]), op_q8f(r->sig[2]),
                   op_q8i(r->sig[3]), op_q8f(r->sig[3]), op_q8i(r->sig[4]), op_q8f(r->sig[4]), OL_TAIL);
        log_printf(r->f_err || r->f_cells < (r->l_wh >> 16) ? OMK_LOG_WARN : OMK_LOG_INFO, "optics: job %u: picture read: "
                   "%u rows (%u a row of cells, one pixel a pair), %u of %u rows of cells%s, %u us, %u maps%s", n,
                   r->f_rows, OL_COVER, r->f_cells, r->l_wh >> 16, r->f_cells < (r->l_wh >> 16) ? " - the REST FROM THE "
                   "LAYER'S Y" : " (all)", r->f_us, r->f_maps, r->f_err == 1 ? " - layer too small for the picture, not "
                   "read" : r->f_err == 2 ? " - no window" : "");
        log_printf(OMK_LOG_INFO, "optics: job %u: picture read's time: rows' mean %u us, longest %u us (with what the CPU "
                   "did meanwhile); maps, write-backs and releases %u us; ticks given away %u us", n, r->f_row_avg,
                   r->f_row_max, r->f_maps_us, r->f_slept);
        log_printf(OMK_LOG_INFO, "optics: job %u: strengths Bloom %u, Halation %u (%s%s); scene: mean Y %u, %u.%u%% at the "
                   "threshold, day %u%%; gains Bloom %u%%, Halation %u%%", n, r->str & 0xff, r->str >> 8 & 0xff,
                   r->str & OP_TEST ? "a test's" : "the look's rows", r->str & OP_MONO ? "; black and white: the "
                   "halation without colour" : "", r->sc_mean, r->sc_share / 10, r->sc_share % 10,
                   r->sc_day * 100 / 256, r->gbe * 100 / 256, r->ghe * 100 / 256);
        log_printf(OMK_LOG_INFO, "optics: job %u: calc: sources %u us, Bloom %u us (rows %u, columns %u), Halation %u us "
                   "(rows %u, columns %u), Bloom's tail %u us; the passes on %u of %u values (%u%%)", n, r->t_src, r->t_gb,
                   r->g_us[0][0], r->g_us[0][1], r->t_gh, r->g_us[1][0], r->g_us[1][1], r->t_gt, r->g_done, r->g_all,
                   r->g_all >= 100 ? r->g_done / (r->g_all / 100) : 0);
        if (r->pr_err)
            log_printf(OMK_LOG_WARN, "optics: job %u: picture row not timed: %s", n, op_err(r->pr_err));
        else if (r->pr_read || r->pr_sums)
            log_printf(OMK_LOG_INFO, "optics: job %u: picture row: read %u us, read and sums %u us (%u words, past the cache)",
                       n, r->pr_read, r->pr_sums, r->width / 2);
        if (r->pr_cread || r->pr_csums)
            log_printf(OMK_LOG_INFO, "optics: job %u: picture row through the cached view: read %u us, read and sums %u us", n,
                       r->pr_cread, r->pr_csums);
        if (r->lp_plain)                    /* the bench ran ([optics] bench) */
            log_printf(r->lp_same ? OMK_LOG_INFO : OMK_LOG_WARN, "optics: job %u: bench: a box row of %u values x %u %u us; "
                       "%u turns: plain %u us, mulu %u us, udf01 %u us (%s), udf00 + udf15 %u us", n, OP_BENCH_N,
                       OP_BENCH_REPS, r->bx_wbuf, OP_TURNS, r->lp_plain, r->lp_mul, r->lp_udf, r->lp_same ? "the same" :
                       "DIFFERENT", r->lp_hi);
    }
    if (r->sub != DRV_OPTICS_L1 || !r->lanes[0])
        return;
    for (k = 0; k < 16; k += 8)
        log_printf(OMK_LOG_INFO, "optics: job %u: layer words %u..%u: %08x %08x %08x %08x %08x %08x %08x %08x", n, k, k + 7,
                   r->head[k], r->head[k + 1], r->head[k + 2], r->head[k + 3], r->head[k + 4], r->head[k + 5],
                   r->head[k + 6], r->head[k + 7]);
    for (k = 0; k < OL_LANES; k++) {
        m = r->lanes[1 + 2 * OL_LANES + k] / r->lanes[0];
        v = r->lanes[1 + 3 * OL_LANES + k] / r->lanes[0] * 256;
        mean[k] = m;
        sd[k] = ol_isqrt(v > m * m ? v - m * m : 0);
    }
    log_printf(OMK_LOG_INFO, "optics: job %u: layer bytes over %u pixels (min/mean/max, sd): 0 %u/%u/%u %u; 1 %u/%u/%u %u; "
               "2 %u/%u/%u %u; 3 %u/%u/%u %u", n, r->lanes[0], r->lanes[1], mean[0], r->lanes[5], sd[0], r->lanes[2],
               mean[1], r->lanes[6], sd[1], r->lanes[3], mean[2], r->lanes[7], sd[2], r->lanes[4], mean[3], r->lanes[8],
               sd[3]);
}

/* Core task: [optics] layer_bytes = auto - the byte positions from the first L1 dump (ol_lane_order), for the jobs after */
static void op_auto_order(volatile op_t *o, volatile op_rec_t *r)
{
    ol_lanes_t ln;
    u32 k, py, pcb, pcr;

    if (!o->order_auto || o->order_set || r->sub != DRV_OPTICS_L1 || !r->lanes[0])
        return;
    ln.n = r->lanes[0];
    for (k = 0; k < OL_LANES; k++) {
        ln.min[k] = r->lanes[1 + k];
        ln.max[k] = r->lanes[1 + OL_LANES + k];
        ln.sum[k] = r->lanes[1 + 2 * OL_LANES + k];
        ln.sq[k] = r->lanes[1 + 3 * OL_LANES + k];
    }
    if (ol_lane_order(&ln, &py, &pcb, &pcr)) {
        log_printf(OMK_LOG_WARN, "optics: layer_bytes = auto: the dump does not tell the bytes apart; Y %u, Cb %u, Cr %u kept",
                   o->order & 0xff, o->order >> 8 & 0xff, o->order >> 16 & 0xff);
        return;
    }
    o->order = py | pcb << 8 | pcr << 16;
    o->order_set = 1;
    log_printf(OMK_LOG_INFO, "optics: layer_bytes = auto: Y in byte %u, Cb %u, Cr %u (the fourth: the smallest range) - "
               "for the jobs from now on", py, pcb, pcr);
}

/* Core task: a job's block in the log */
static void op_report(volatile op_rec_t *r, u32 n)
{
    volatile op_slot_t *d;
    u32 i, sh = r->shot;

    log_printf(r->err && r->err != OE_BUDGET ? OMK_LOG_WARN : OMK_LOG_INFO, "optics: job %u (shot %u) type %04x, mode "
               "%04x/%u/%u, substage %s, cpu %u, step %u of %u: wait %u us (rc %x), probe %u us, %u yields%s%s", n, sh,
               r->type, r->mode & 0xffff, r->mode >> 16 & 0xff, r->mode >> 24, drv_optics_sub_name(r->sub), r->cpu,
               (r->step & 0xff) + 1, r->step >> 8, r->t_wait, r->wait_rc, r->t_total, r->yields, r->err ? "; stopped: " : "",
               op_err(r->err));
    if (r->sub >= DRV_OPTICS_A)
        log_printf(OMK_LOG_INFO, "optics: job %u: %s", n, !(r->mode >> 16 & 0xff) ? "a single picture (RawJpeg 0)" :
                   (r->mode >> 24) == 1 ? "RAW+JPEG, the first job of the press (FileType 1)" :
                   (r->mode >> 24) == 3 ? "RAW+JPEG, the second job of the press (FileType 3)" : "RAW+JPEG, FileType other");
    if (r->sub >= DRV_OPTICS_WORKER && r->w_task)
        log_printf(r->w_state ? OMK_LOG_WARN : OMK_LOG_INFO, "optics: job %u: worker task %u: %u us of its own (%u yields), "
                   "the step waited %u us%s%s%s", n, r->w_task, r->w_us, r->w_yields, r->w_wait,
                   r->w_state & WS_CANCEL ? "; the job was cancelled: stop asked" : "",
                   r->w_state & WS_LATE ? "; LATE: stop asked" : "",
                   r->w_state & WS_ABANDON ? "; GIVEN UP: the step returned without it" : "");
    if (r->sub >= DRV_OPTICS_A)
        log_printf(OMK_LOG_INFO, "optics: job %u: psw %08x (IE %u, IM %u), sp %08x, lowest %08x; task %u, stack %08x + %u "
                   "(%d bytes left at the lowest); cross-CPU lock bytes %08x %08x", n, r->psw, r->psw >> 11 & 1,
                   r->psw >> 8 & 7, r->sp, r->sp_min, r->task, r->stk, r->stksz, r->stk ? (int)(r->sp_min - r->stk) : 0,
                   r->lock[0], r->lock[1]);
    if (r->sub < DRV_OPTICS_B)
        return;
    if (r->err && r->err != OE_BUDGET)
        log_printf(OMK_LOG_WARN, "optics: job %u: %s, value %08x", n, op_err(r->err), r->detail);
    for (i = 0; i < r->nslots && i < REC_SLOTS; i++) {
        d = &r->slot[i];
        log_printf(OMK_LOG_INFO, "optics: job %u buffer %04x: bus %08x format %03x pitch %u, %ux%u, %u bytes at %08x, cpu %u, "
                   "+81 %u, +84 %u", n, d->type, d->bus, d->fmt & 0xffff, d->fmt >> 16, d->wh & 0xffff, d->wh >> 16, d->len,
                   d->alloc, d->bytes & 0xff, d->bytes >> 8 & 0xff, d->bytes >> 16 & 0xff);
    }
    log_printf(OMK_LOG_INFO, "optics: job %u: %u buffers in use (%u listed); search: 0x1000 slot %u, 0x1060 %s; loop "
               "%u x 2 instructions in %u us", n, r->used, r->nslots, r->i_frame, r->i_small < SLOTS ? "present" :
               "none (as expected)", r->loop_n, r->loop_us);
    if (r->sleep_ms)
        log_printf(OMK_LOG_INFO, "optics: job %u: the worker slept %u ms instead of the loop (%u us)", n, r->sleep_ms,
                   r->loop_us);
    if (r->sub >= DRV_OPTICS_L0) {
        op_report_layer(r, n);
        return;
    }
    if (!r->kind)
        return;
    log_printf(OMK_LOG_INFO, "optics: job %u: picture %08x (%u bytes) %s %08x, map %u us; uncached read %u bytes in %u us "
               "(%u KB/s), sum %08x; release %u us (%s)", n, r->bus, r->len, r->kind == OP_BUS_WINDOW ? "window" : "direct",
               r->kind == OP_BUS_WINDOW ? r->win : op_direct(r->bus), r->t_map, r->rd_bytes, r->rd_us,
               op_kbs(r->rd_bytes, r->rd_us), r->rd_sum, r->t_unmap, r->kind != OP_BUS_WINDOW ? "none" :
               r->unmap_rc ? "ok" : "FAILED");
    for (i = 0; i < 16 && r->sub >= DRV_OPTICS_E; i += 8)
        log_printf(OMK_LOG_INFO, "optics: job %u: bytes %u..%u: %08x %08x %08x %08x %08x %08x %08x %08x", n, 4 * i,
                   4 * i + 31, r->head[i], r->head[i + 1], r->head[i + 2], r->head[i + 3], r->head[i + 4], r->head[i + 5],
                   r->head[i + 6], r->head[i + 7]);
    if (r->sub == DRV_OPTICS_FULL && r->level == 2) {
        if (r->c_state == CS_DONE)
            log_printf(r->c_bytes == r->rd_bytes && r->c_sum != r->rd_sum ? OMK_LOG_WARN : OMK_LOG_INFO,
                       "optics: job %u: cached view %08x: write-back %u us, read %u bytes in %u us (%u KB/s), sum %08x - %s; "
                       "write-back %u us", n, r->win - OP_UNCACHED, r->c_fl0, r->c_bytes, r->c_us,
                       op_kbs(r->c_bytes, r->c_us), r->c_sum, r->c_bytes != r->rd_bytes ? "not the same bytes (budget)" :
                       r->c_sum == r->rd_sum ? "the same as past the cache" : "DIFFERENT from past the cache", r->c_fl1);
        else
            log_printf(OMK_LOG_INFO, "optics: job %u: cached view not read: %s", n, r->c_state == CS_NOTARMED ?
                       "not armed (one job an arming, after the log is on the card)" : r->c_state == CS_NOTWINDOW ?
                       "the picture is not behind a window" : "begun, not ended");
    }
    if (r->sub == DRV_OPTICS_FULL && r->level == 3)
        log_printf(OMK_LOG_INFO, "optics: job %u: stripes %u rows x %u pixels in %u us (%u KB/s), write-back %u us", n, r->s_rows,
                   r->s_px, r->s_us, op_kbs(r->s_rows * r->s_px * 2, r->s_us), r->s_fl);
}

/* Core task: the reports of the steps that ended; the counts since install in *st (core/optics.c) */
void drv_optics_service(drv_optics_info *st)
{
    static u32 seen, noroom_seen, w_seen[3];
    static drv_optics_info c;
    volatile op_t *o = op();
    volatile op_rec_t *r;
    u32 n, i, k;

    if (!o) {
        mem_set(st, 0, sizeof(*st));
        return;
    }
    n = o->jobs;
    if (n - seen > RING) {
        c.lost += n - seen - RING;
        log_printf(OMK_LOG_WARN, "optics: %u jobs not reported (ring)", n - seen - RING);
        seen = n - RING;
    }
    for (; seen != n; seen++) {
        r = &o->ring[seen % RING];
        if (r->seq != seen + 1)
            break;                          /* still being written: the next report */
        op_report(r, seen + 1);
        op_auto_order(o, r);
        c.jobs++;
        if (r->c_state == CS_BEGUN || r->c_state == CS_DONE)
            c.cached++;
        if (r->c_state == CS_DONE && r->c_bytes == r->rd_bytes && r->c_sum == r->rd_sum)
            c.cached_same++;
    }
    if (o->noroom != noroom_seen) {
        noroom_seen = o->noroom;
        log_printf(OMK_LOG_WARN, "optics: %u job lists had no room for the probe's step", noroom_seen);
    }
    for (i = 0; i < SUBS; i++)
        for (c.ended[i] = 0, k = 0; k < CPUS; k++)
            c.ended[i] += o->ended[k][i];
    if (o->w_tid && (o->w_empty != w_seen[0] || o->w_naps != w_seen[1] || o->w_count != w_seen[2])) {
        w_seen[0] = o->w_empty;
        w_seen[1] = o->w_naps;
        w_seen[2] = o->w_count;
        log_printf(o->w_naps ? OMK_LOG_WARN : OMK_LOG_DEBUG, "optics: worker: %u waits, %u woken with no request (%u at "
                   "once), %u naps, %u requests done, last wait %d", o->w_loops, o->w_empty, o->w_fast, o->w_naps,
                   o->w_count, (int)o->w_rc);
    }
    if (o->layer_on) {
        static u32 blends_seen, fallbacks_seen;
        u32 b = 0;

        for (k = 0; k < CPUS; k++)
            b += o->blends[k][0] + o->blends[k][1];
        if (b != blends_seen || o->fallbacks != fallbacks_seen) {
            blends_seen = b;
            fallbacks_seen = o->fallbacks;
            log_printf(o->fallbacks ? OMK_LOG_WARN : OMK_LOG_DEBUG, "optics: layer: blends made additive %u (cpu 0/1/2 case "
                       "11: %u %u %u, case 12: %u %u %u); chains left out %u", b, o->blends[0][0], o->blends[1][0],
                       o->blends[2][0], o->blends[0][1], o->blends[1][1], o->blends[2][1], o->fallbacks);
        }
    }
    if (o->seconds != c.seconds) {
        c.seconds = o->seconds;
        log_printf(OMK_LOG_INFO, "optics: layer: %u second jobs of RAW+JPEG presses without the layer (jobs = first: the "
                   "JPEG is the first job's picture, the second makes the ORF's preview)", c.seconds);
    }
    c.armed = o->armed;
    c.listed = o->listed;
    c.noroom = noroom_seen;
    mem_copy(st, &c, sizeof(*st));
}
