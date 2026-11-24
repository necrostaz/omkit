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
 * Memory: the step touches the arena block below (its uncached address), the job's handle (through the cache, on the job's
 * CPU, as the firmware's steps do), the picture through the window, and the stack; the firmware only through the thunks of
 * thunks.s. No core variables, no tables in .rodata. The core task reports (drv_optics_service). */
#include "omk_drv.h"
#include "omk_core.h"
#include "omk_log.h"
#include "omk_mem.h"
#include "optprobe.h"
#include "optwork.h"

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
#define BUF_SMALL 0x1060                    /* the Soft Focus chain's small frame: not expected on this route */
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
       P_CACHED, P_CACHED_END, P_STRIPES, P_STRIPES_END, P_UNMAP, P_UNMAPPED, P_OUT, P_ENDED, P_HANDED, P_WORK, P_WORK_END, P_LATE, P_BACK, P_N };

enum { OE_NONE, OE_WAIT, OE_NOBUF, OE_SLOT, OE_ADDR, OE_MAP, OE_WIN, OE_BUDGET, OE_TASK, OE_WORKER, OE_N };
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
    u32 bus, len, kind, win, t_map, t_unmap, unmap_rc;
    u32 rd_bytes, rd_us, rd_sum;            /* rd_us without the ticks given away */
    u32 head[16];
    u32 c_state, c_bytes, c_us, c_sum, c_fl0, c_fl1;
    u32 s_rows, s_px, s_us, s_fl;
    u32 yields, t_total;                    /* t_total: from the end of the wait, with the ticks given away */
} op_rec_t;

typedef struct {
    u32 level;                              /* 1..3, set before the first list */
    u32 armed;                              /* 1 - the next step of level 2 reads through the cached view: the core task
                                               sets it, the step that reads takes it back (drv_lock) */
    u32 jobs;                               /* steps begun: the ring's writer index (drv_lock) */
    u32 listed, noroom;                     /* W: steps put into lists; lists with no room for it */
    u32 sub;                                /* the substage of new jobs (core: drv_optics_sub) */
    u32 wl;                                 /* W: jobs listed (the crumbs' job number) */
    struct {
        u32 h, sub, job;                    /* h 0 - free: W takes it (CPU 0), H3 frees it (any CPU, h last) */
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
    op_rec_t ring[RING];
} op_t;

typedef struct {
    u32 t0, ty;                             /* the work's start; the last tick given away */
    u32 slept, yields;
    volatile u32 *stop;                     /* the worker: a word that ends the work, 0 - none */
} pace_t;

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

/* The empty loop: n iterations of 2 instructions */
static void op_spin(u32 n)
{
    __asm__ __volatile__("1:\n\tadd -1,%0\n\tbne 1b" : "+d"(n) : : "cc");
}

/* 1 - over the budget; a tick given away every OPT_YIELD_US */
static int op_pace(pace_t *p)
{
    u32 t;

    if (drv_ticks_since(p->t0) >= OPT_BUDGET_US || (p->stop && (p->stop[0] | p->stop[1])))
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
    p.stop = &o->ch.stop;
    op_crumb(job, sub, P_WORK);
    if (sub == DRV_OPTICS_W0)
        goto out;                           /* nothing: the answer at once */
    r->loop_n = OPT_LOOP;
    if (sub == DRV_OPTICS_W1) {             /* at once: the worker's priority 56 is below every task it could hold up */
        t = drv_ticks();
        op_spin(OPT_LOOP);
        r->loop_us = drv_ticks_since(t);
        op_crumb(job, sub, P_LOOP);
        goto out;
    }
    for (i = 0; i < LOOP_PARTS && !(o->ch.stop | o->ch.abandon); i++) {
        t = drv_ticks();
        op_spin(OPT_LOOP / LOOP_PARTS);
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
        goto out;
    }
    p.ty -= OPT_YIELD_US;                   /* one tick given away now, whatever the time */
    op_pace(&p);
    op_crumb(job, sub, P_SLEPT);
    op_low(r);
    if (sub == DRV_OPTICS_C2 || sub == DRV_OPTICS_C3) {
        r->loop_n = sub == DRV_OPTICS_C2 ? LOOP_SHORT : OPT_LOOP;
        t = drv_ticks();
        op_spin(r->loop_n);
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

/* The step (thunks.s: drv_tramp_optics_step, by the step executor's calls; any CPU, the firmware's job task). Its result
 * is not used: the trampoline returns 0 */
u32 drv_optics_step(u8 *h)
{
    volatile op_t *o = op();
    volatile op_rec_t *r;
    u32 k, n, t, rc, seq, st, i, sub = DRV_OPTICS_A, job = 0, psw, sp, stk = 0, stksz = 0, mode = 0;
    volatile u32 *c;

    if (!h)
        return 0;
    if (o && (i = op_row(o, (u32)h)) < ROWS) {
        sub = o->row[i].sub;
        job = o->row[i].job;
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

/* W (graindrv.c, CPU 0), after the development 0x901 of our job h: the step to put into the list, or 0 - none (no probe,
 * substage N, or no room - `room` 0 - for a step, the pass to the outputs and the selector's step). The job gets a row
 * with the substage in force (also at N: H3 counts the job) */
u32 drv_optics_list(u8 *h, int room)
{
    volatile op_t *o = op();
    u32 i, sub;

    if (!o || !h)
        return 0;
    sub = o->sub;
    if (sub != DRV_OPTICS_N && !room) {
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
    o->row[i].h = (u32)h;
    crumb()[CR_LISTED] = o->wl;
    op_crumb(o->wl, sub, P_LISTED);
    if (sub == DRV_OPTICS_N)
        return 0;
    o->listed++;
    return sub == DRV_OPTICS_A0 ? STEP_STOCK : (u32)drv_tramp_optics_step;
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

    if (level < 1 || level > 3)
        return 1;
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

/* Core task: the substage of the jobs listed from now on */
void drv_optics_sub(u32 sub)
{
    volatile op_t *o = op();

    if (o && sub < SUBS)
        o->sub = sub;
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
                                           "no worker task"};

    return e < OE_N ? name[e] : "?";
}

/* Core task: names for the log */
const char *drv_optics_sub_name(u32 sub)
{
    static const char *const name[SUBS] = {"N", "A0", "A", "B", "C1", "C2", "C3", "W0", "W1", "W2", "D", "E", "F", "full"};

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
                                          "worker late: stop asked", "the step has the worker's answer"};

    return phase < P_N ? name[phase] : "?";
}

static u32 op_kbs(u32 bytes, u32 us)
{
    return us ? (bytes >> 10) * 1000000u / us : 0;
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
    c.armed = o->armed;
    c.listed = o->listed;
    c.noroom = noroom_seen;
    mem_copy(st, &c, sizeof(*st));
}
