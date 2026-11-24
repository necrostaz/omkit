/* The optics layer (core 1.33): stage 1, the probe - [optics] probe in OMKIT.INI. 0 or absent: nothing is installed and
 * nothing is read or written (no store either); 1..3: the driver's step in the look's still jobs (omk_drv.h,
 * drv_optics_install), its reports in the log through the core task.
 *
 * What may reset the camera runs only when what came before it is on the card - as a trial boot does it, with a state of
 * two copies and a checksum (the store OPTICS) written before the attempt and the previous run's end (core: ended with a
 * power-off or not) read at the next start. The log of a run that resets is lost from its last write to the card.
 *
 * The bisection (core 1.34, [optics] bisect = run number, 0 - off): core 1.33's level 1 reset the camera at the first
 * still of the look with grain, before any report reached the card. The driver's work comes in substages (N, A0, A, B,
 * C1, C2, C3, W0, W1, W2, D, E, F, then the level's own - opticsdrv.c's header), each including the ones before it except
 * the test C3 (DRV_OPTICS_TESTS). From W1 on the work is the worker task's (core 1.37): the core task makes it once,
 * before the first such substage is armed, when the card is not in use - not at start. If it cannot be made, the
 * bisection stops there (logged) and the stills stay at the substage before. [optics] bisect_from: the substage a new run starts at (A: N and A0 passed on
 * the camera with core 1.34):
 *  - the substage under test is armed from the core task when the card is not in use: its line to the log, the log
 *    written to the card, the store's "armed" on the card, and only then the driver's jobs get it (drv_optics_sub);
 *    until then they get the last one passed (none yet: N, no step);
 *  - it passed when a job listed with it ended (the firmware's cleanup), the camera is not busy with the card (the
 *    picture is written) and the application is back in a lasting state (core 1.39: card149's W1 "passed" while the
 *    application stayed in its half-press state 5101 for good - the next substage was blamed for the battery taken out):
 *    the store moves on to the next one, which is armed the same way;
 *  - at start, "armed" with the previous run not ended by a power-off: the substage under test reset the camera - logged,
 *    kept in the store, and the jobs stay at the one before it for this run number; a test that reset the camera is
 *    marked and the bisection goes on with the next one. After the last one: the level.
 *
 * Level 2 then also reads the picture through the window's cached view, which may reset the camera (a write to the cached
 * view of a bus buffer without a window did, core 1.07):
 *  - the cached read is armed for one job: after at least one job's report since the last arming, the log is written to
 *    the card, the store gets "armed" on the card, and only then the driver (drv_optics_arm);
 *  - the job that reads takes the arming; its report clears "armed" and the next arming waits for it to be in the log on
 *    the card;
 *  - at start, "armed" with the previous run not ended by a power-off: counted, logged, and no more arming until
 *    [optics] run is changed (a level-2 card would otherwise reset at every shot).
 * A reset after an arming that no job took yet is counted the same way: the log of that run, written right before the
 * arming, shows whether a still came after it.
 *
 * The crumbs (drv_optics_crumb): the step's last phase, kept past the cache in memory outside the arena - logged at the
 * next start when the memory outlived the reset.
 *
 * The schedule (core feature/optics-layer, after card159: [optics] schedule, without a bisection): a list of substages,
 * one a press - what the jobs of each press get - with the empty loop's length or a sleep instead of it:
 *   schedule = W0, W0, W1/s20, W0, W1/100000, W0, W1, W0
 * NAME - a substage (as bisect_from), NAME/N - with the loop of N iterations (C3, W1, W2), W1/sMS - the worker sleeps MS
 * instead of the loop (the job waits as long, the CPU is free). The first entry is given when the worker task runs (if an
 * entry needs it) and its tests are done; the next one when a job of the press ended, the camera is not busy with the card
 * and the application is in a lasting state - as a substage passes in the bisection, but nothing is armed or stored: the
 * debug service's RAM log is the record. After the last entry the jobs keep it. A hang or a reset on a press is the
 * one before it or its own: the log says which entry each press got.
 *
 * The layer (feature/optics-layer, Bloom and Halation; opticsdrv.c's header, optlayer.h): on when [optics] bloom or
 * halation is 1..5 or the schedule names a layer substage (L0..L4); the jobs then get L4 (without a schedule). Its
 * settings, all in [optics]:
 *   bloom, halation           0..5, the strengths (0 - off)
 *   route                     pair (the firmware's Soft Focus steps, 0x401 + 0x402) or 403 (our last step, 0x403)
 *   layer_width               the small layer's width, 42..400 (324: a cell of 16 pixels of 5184)
 *   layer_bytes               auto, or the bytes of Y, Cb, Cr in a layer word as three digits (012: Y 0, Cb 1, Cr 2)
 *   gamma                     the curve to linear light, 200..300 (x100)
 *   bloom_threshold, halation_threshold    codes 0..255 of Y where the glow starts
 *   bloom_sigma, bloom_sigma2, bloom_mix   the Gaussians in pixels of the picture, the second's weight in percent
 *   halation_sigma, halation_sigma2, halation_mix
 *   bloom_gain, halation_gain              the light added at strength 5, percent of the excess
 *   halation_cb, halation_cr               the halation's colour: Cb, Cr a unit of its Y, percent (signed)
 *   halation_dark                          less halation on bright places: percent
 *   read, read_ms, read_cached             the picture's rows read for the sources, the time a job, the cached view
 * The smallest sigma the transport gives (the ISP's reduction and enlargement) is logged at start; a sigma below it
 * comes out as it (a warning).
 *
 * With the debug service on ([debug] enabled), the moments of the layer - a substage or the cached read armed, a
 * substage passed, the worker task made, a job of ours listed or ended (as the core task sees them) - are events of its
 * RAM log (dbg_event): a mark, the watched tasks' states and the window of snapshots after it. */
#include "omk_core.h"
#include "omk_debug.h"
#include "omk_log.h"
#include "omk_store.h"

#define OPT_STORE "OPTICS"
#define OPT_STORE_SIZE 128
#define OPT_KEY 1                           /* the cached read (level 2) */
#define BIS_KEY 2                           /* the bisection */
#define OPT_RETRY_MS 2000                   /* a log or store write that failed: tried again after this */
#define LEVEL_MAX 3
#define SUBS DRV_OPTICS_PROBES                 /* the bisection's substages (the layer's are not in it) */
#define BIS_DONE SUBS                       /* bis.index after the last substage */
#define SCHED_MAX 24                        /* entries of [optics] schedule */
#define LOOP_MAX 100000000u                 /* iterations of a schedule's loop: ~2 s at ~10 ns an instruction */
#define SLEEP_MAX 2000                      /* ms of a schedule's sleep (the step gives up on the worker after 10 s) */

/* The cached read's state on the card (store OPTICS, key 1) */
typedef struct {
    u32 run;                                /* [optics] run: another number starts again */
    u32 armed;                              /* 1 - a cached read may run now (written before it is allowed) */
    u32 reads, same;                        /* cached reads reported; with the sum of the read past the cache */
    u32 resets;                             /* runs that ended without a power-off while armed */
    u32 saves;                              /* counts the states written: every one a change the store writes, and
                                               their order across the store's two copies */
} opt_state_t;

/* The bisection's state on the card (store OPTICS, key 2) */
typedef struct {
    u32 run;                                /* [optics] bisect: another number starts again */
    u32 index;                              /* the substage under test, BIS_DONE - all passed */
    u32 armed;                              /* 1 - jobs may get it now (written before they do) */
    u32 failed;                             /* the substage that reset the camera, SUBS - none */
    u32 saves;
    u32 tests;                              /* bits: the tests (DRV_OPTICS_TESTS) that reset the camera */
} bis_state_t;

static struct {
    omk_store *s;
    opt_state_t b;
    u32 level;
    int on;                                 /* the driver's step is installed */
    int worker;                             /* 1 - the worker runs; -1 - it could not be made (not tried again) */
    u32 worker_test;                        /* [optics] worker_test: requests of the test still to hand */
    int can_arm;                            /* level 2, the store on the card, no reset with an arming in this run */
    u32 arm_jobs;                           /* the reports counted at the last arming */
    u32 cached, same;                       /* the driver's counts already in b */
    u32 retry_ms;
    u32 listed, ended;                      /* the driver's counts already told to the debug service */
} op;

static struct {
    bis_state_t b;
    int on;                                 /* a bisection in progress: the store on the card, nothing failed */
    int armed;                              /* this run armed b.index (on the card) */
    int pending;                            /* armed, the driver not given it yet: the worker's tests first */
    u32 ended0;                             /* the driver's count of jobs ended at b.index when it was given */
    u32 retry_ms;
} bis;

/* [optics] schedule */
static struct {
    u32 n, k;                               /* entries; the one given now */
    u8 sub[SCHED_MAX];
    u32 loop[SCHED_MAX], sleep[SCHED_MAX];
    u32 ended0;                             /* the driver's jobs ended (all substages) when entry k was given */
    int on, given, worker;                  /* worker: an entry needs the worker task */
} sch;

/* A state to the card now: 0, or -1 (not written) */
static int opt_put(u16 key, void *b, u32 len, u32 *saves)
{
    u32 keys, used, seq0, seq1;

    ++*saves;
    if (store_set(op.s, key, b, len, OMK_STORE_PERSIST))
        return -1;
    store_info(op.s, &keys, &used, &seq0);
    store_save_now(op.s);
    store_info(op.s, &keys, &used, &seq1);
    return seq1 != seq0 ? 0 : -1;
}

static int opt_save(void)
{
    return opt_put(OPT_KEY, &op.b, sizeof(op.b), &op.b.saves);
}

static int bis_save(void)
{
    return opt_put(BIS_KEY, &bis.b, sizeof(bis.b), &bis.b.saves);
}

/* Level 2 at start: the state of [optics] run on the card, a reset with an arming counted */
static void opt_store_start(u32 run)
{
    opt_state_t b;
    u32 saves;

    if (store_get(op.s, OPT_KEY, &b, sizeof(b)) != (int)sizeof(b))
        mem_set(&b, 0, sizeof(b));
    if (b.run != run || !b.saves) {
        saves = b.saves;                    /* counts on: it orders the saves of every run */
        mem_set(&b, 0, sizeof(b));
        b.run = run;
        b.saves = saves;
        log_printf(OMK_LOG_INFO, "optics: cached read test run %u starts", run);
    } else if (b.armed && !core_prev_ended()) {
        b.resets++;
        log_printf(OMK_LOG_WARN, "optics: the run before did not end with a power-off while a cached read was armed (%u "
                   "reads reported before it): counted as a reset by the cached read, no more of them in run %u", b.reads,
                   run);
    }
    b.armed = 0;
    mem_copy(&op.b, &b, sizeof(b));
    if (opt_save()) {
        log_printf(OMK_LOG_ERROR, "optics: the store %s was not written to the card, the cached read stays off", OPT_STORE);
        return;
    }
    op.can_arm = !op.b.resets;
    log_printf(op.b.resets ? OMK_LOG_WARN : OMK_LOG_INFO, "optics: cached read run %u: %u reads reported (%u with the "
               "same sum), %u resets%s", run, op.b.reads, op.b.same, op.b.resets, op.b.resets ?
               " - off; set [optics] run to another number to try again" : "");
}

/* The substage the jobs get while the one under test is not armed: the last one passed before it that is not a test
 * (none: N, no step) */
static u32 bis_safe(void)
{
    u32 k = bis.b.failed < SUBS ? bis.b.failed : bis.b.index;

    while (k > DRV_OPTICS_N && (DRV_OPTICS_TESTS >> (k - 1) & 1))
        k--;
    return k > DRV_OPTICS_N ? k - 1 : DRV_OPTICS_N;
}

/* A substage by its name (bisect_from), N if none */
static u32 bis_named(const char *name)
{
    u32 k;

    for (k = 0; k < SUBS; k++)
        if (str_ieq(name, drv_optics_sub_name(k)))
            return k;
    return DRV_OPTICS_N;
}

/* At start: the bisection's state of [optics] bisect on the card, a reset with a substage armed marked. The substage for
 * the jobs of this run until the next arming */
static u32 bis_start(u32 run, u32 from)
{
    bis_state_t b;
    u32 saves;

    if (store_get(op.s, BIS_KEY, &b, sizeof(b)) != (int)sizeof(b))
        mem_set(&b, 0, sizeof(b));
    if (b.run != run || !b.saves || b.index > BIS_DONE || (b.failed > SUBS)) {
        saves = b.saves;
        mem_set(&b, 0, sizeof(b));
        b.run = run;
        b.index = from < DRV_OPTICS_FULL ? from : DRV_OPTICS_N;
        b.failed = SUBS;
        b.saves = saves;
        log_printf(OMK_LOG_INFO, "optics: bisection run %u starts at %s", run, drv_optics_sub_name(b.index));
    } else if (b.armed && !core_prev_ended() && b.failed == SUBS && b.index < BIS_DONE &&
               (DRV_OPTICS_TESTS >> b.index & 1)) {
        b.tests |= 1u << b.index;
        log_printf(OMK_LOG_WARN, "optics: substage %s RESET THE CAMERA (the run before did not end with a power-off while "
                   "it was armed); a test only - the bisection goes on with %s", drv_optics_sub_name(b.index),
                   drv_optics_sub_name(b.index + 1));
        b.index++;
    } else if (b.armed && !core_prev_ended() && b.failed == SUBS && b.index < BIS_DONE) {
        b.failed = b.index;
        log_printf(OMK_LOG_WARN, "optics: substage %s RESET THE CAMERA (the run before did not end with a power-off while "
                   "it was armed)", drv_optics_sub_name(b.index));
    }
    b.armed = 0;
    mem_copy(&bis.b, &b, sizeof(b));
    if (bis_save()) {
        log_printf(OMK_LOG_ERROR, "optics: the store %s was not written to the card, no bisection: no step", OPT_STORE);
        bis.b.failed = DRV_OPTICS_A0;       /* the jobs stay at N */
        return DRV_OPTICS_N;
    }
    if (bis.b.failed < SUBS)
        log_printf(OMK_LOG_WARN, "optics: bisection run %u: %s reset the camera, the stills stay at %s; set [optics] "
                   "bisect to another number to start again", run, drv_optics_sub_name(bis.b.failed),
                   drv_optics_sub_name(bis_safe()));
    else if (bis.b.index == BIS_DONE)
        log_printf(OMK_LOG_INFO, "optics: bisection run %u: every substage passed, the stills get the level's work", run);
    else
        log_printf(OMK_LOG_INFO, "optics: bisection run %u: %s under test (armed before the next still), the stills get "
                   "%s until then", run, drv_optics_sub_name(bis.b.index), drv_optics_sub_name(bis_safe()));
    for (saves = 0; saves < SUBS; saves++)
        if (bis.b.tests >> saves & 1)
            log_printf(OMK_LOG_WARN, "optics: bisection run %u: the test %s reset the camera", run,
                       drv_optics_sub_name(saves));
    bis.on = bis.b.failed == SUBS && bis.b.index < BIS_DONE;
    return bis.b.index == BIS_DONE ? DRV_OPTICS_FULL : bis_safe();
}

/* [optics] schedule: the entries, the bad ones left out (logged). Words separated by commas or spaces */
static void sch_parse(const char *s)
{
    char word[24];
    u32 n, k, v, i, slash, sleep;
    int bad;

    while (*s) {
        while (*s == ',' || *s == ' ' || *s == '\t')
            s++;
        for (n = 0; *s && *s != ',' && *s != ' ' && *s != '\t'; s++)
            if (n < sizeof(word) - 1)
                word[n++] = *s;
        word[n] = 0;
        if (!n)
            break;
        for (i = 0; word[i] && word[i] != '/'; i++)
            ;
        slash = word[i] == '/';
        sleep = slash && (word[i + 1] == 's' || word[i + 1] == 'S');
        bad = 0;
        v = 0;
        if (slash) {                        /* /N or /sMS */
            word[i] = 0;
            k = i + 1 + sleep;
            bad = !word[k];
            for (; word[k] && !bad; k++)
                if (word[k] < '0' || word[k] > '9' || v > LOOP_MAX / 10)
                    bad = 1;
                else
                    v = v * 10 + (u32)(word[k] - '0');
        }
        for (k = 0; k < DRV_OPTICS_SUBS && !str_ieq(word, drv_optics_sub_name(k)); k++)
            ;
        if (sleep)
            bad |= k != DRV_OPTICS_W1 || !v || v > SLEEP_MAX;
        else if (slash)
            bad |= (k != DRV_OPTICS_C3 && k != DRV_OPTICS_W1 && k != DRV_OPTICS_W2) || v < 10 || v > LOOP_MAX;
        if (k == DRV_OPTICS_SUBS || bad || sch.n == SCHED_MAX) {
            if (slash)
                word[i] = '/';
            log_printf(OMK_LOG_WARN, "optics: schedule: '%s' %s - left out", word, sch.n == SCHED_MAX ? "is one too many" :
                       "is not a substage, NAME/N (C3, W1, W2; N 10..100000000) or W1/sMS (1..2000)");
            continue;
        }
        sch.sub[sch.n] = (u8)k;
        sch.loop[sch.n] = sleep ? 0 : v;
        sch.sleep[sch.n] = sleep ? v : 0;
        sch.worker |= k >= DRV_OPTICS_WORKER;
        sch.n++;
    }
}

/* At start: what the crumbs of the run before say */
static void crumbs(void)
{
    u32 w[DRV_OPTICS_CRUMB], boot = drv_ticks() | 1;

    if (!drv_optics_crumb(boot, w)) {
        log_printf(OMK_LOG_INFO, "optics: no crumbs of the run before (memory not kept, or no probe then); this run %08x",
                   boot);
        return;
    }
    log_printf(OMK_LOG_WARN, "optics: crumbs of the run before (%08x), the memory outlived its end: heartbeat %u ms, jobs "
               "listed %u, last ended %u; job %u at %s: last phase '%s' at %u us, phases %08x; this run %08x", w[1], w[2],
               w[6], w[7], w[3] >> 16, drv_optics_sub_name(w[3] >> 8 & 0xff), drv_optics_phase(w[3] & 0xff), w[4], w[5],
               boot);
    if (w[9])
        log_printf(OMK_LOG_WARN, "optics: crumbs: at that step's entry psw %08x (IE %u, IM %u), sp %08x, lowest %08x; task %u, "
                   "stack %08x + %u (%d bytes left at the lowest); mode %04x RawJpeg %u FileType %u; priorities %08x",
                   w[8], w[8] >> 11 & 1, w[8] >> 8 & 7, w[9], w[10], w[11], w[12], w[13],
                   w[12] ? (int)(w[10] - w[12]) : 0, w[14] & 0xffff, w[14] >> 16 & 0xff, w[14] >> 24, w[15]);
    if (w[16] | w[17] | w[21])
        log_printf(OMK_LOG_WARN, "optics: crumbs: the worker task: %u waits, %u woken with no request (%u at once), %u naps, "
                   "last wait %d, phase %u at %u us, %u requests done; at its start sp %08x psw %08x priority %u (base %u), "
                   "flags %u %u", w[16], w[17], w[18], w[19], (int)w[20], w[21], w[22], w[23], w[24], w[25], w[26] & 0xffff,
                   w[26] >> 16, w[27] & 0xffff, w[27] >> 16);
}

/* An [optics] integer clamped to lo..hi (logged when it was out) */
static int opt_int(const char *key, int def, int lo, int hi)
{
    int v = cfg_int("optics", key, def);

    if (v < lo || v > hi) {
        log_printf(OMK_LOG_WARN, "optics: %s = %d out of %d..%d, %d taken", key, v, lo, hi, v < lo ? lo : hi);
        v = v < lo ? lo : hi;
    }
    return v;
}

/* The layer's settings; 1 - the layer is on (a strength, or a layer substage in the schedule) */
static int layer_cfg(drv_optics_layer_cfg *c, const char *schedule)
{
    const char *t;
    u32 sb, sh, fl, i;
    int on;

    sb = (u32)opt_int("bloom", 0, 0, 5);
    sh = (u32)opt_int("halation", 0, 0, 5);
    for (on = sb || sh, i = 0; schedule[i] && !on; i++)
        on = (schedule[i] == 'L' || schedule[i] == 'l') && schedule[i + 1] >= '0' && schedule[i + 1] <= '4' &&
             (!i || schedule[i - 1] == ',' || schedule[i - 1] == ' ');
    if (!on)
        return 0;
    mem_set(c, 0, sizeof(*c));
    t = cfg_str("optics", "route", "pair");
    c->route = str_ieq(t, "403") ? DRV_OPTICS_403 : DRV_OPTICS_PAIR;
    if (!str_ieq(t, "403") && !str_ieq(t, "pair"))
        log_printf(OMK_LOG_WARN, "optics: route = %s unknown (pair, 403), pair taken", t);
    c->width = (u32)opt_int("layer_width", 324, 42, 400);
    t = cfg_str("optics", "layer_bytes", "auto");
    if (t[0] >= '0' && t[0] <= '3' && t[1] >= '0' && t[1] <= '3' && t[2] >= '0' && t[2] <= '3' && !t[3] &&
        t[0] != t[1] && t[0] != t[2] && t[1] != t[2])
        c->order = (u32)(t[0] - '0') | (u32)(t[1] - '0') << 8 | (u32)(t[2] - '0') << 16;
    else {
        if (!str_ieq(t, "auto"))
            log_printf(OMK_LOG_WARN, "optics: layer_bytes = %s: not auto or three different digits 0..3, auto taken", t);
        c->order = DRV_OPTICS_ORDER_AUTO;
    }
    c->gamma = (u32)opt_int("gamma", 220, 200, 300);
    c->tb = (u32)opt_int("bloom_threshold", 200, 0, 254);
    c->th = (u32)opt_int("halation_threshold", 225, 0, 254);
    c->sb = (u32)opt_int("bloom_sigma", 60, 0, 1000) | (u32)opt_int("bloom_sigma2", 0, 0, 1000) << 16;
    c->sh = (u32)opt_int("halation_sigma", 40, 0, 1000) | (u32)opt_int("halation_sigma2", 0, 0, 1000) << 16;
    c->mixb = (u32)opt_int("bloom_mix", 0, 0, 100) * 256 / 100;
    c->mixh = (u32)opt_int("halation_mix", 0, 0, 100) * 256 / 100;
    c->gb = (u32)opt_int("bloom_gain", 150, 0, 1000) * 256 * sb / 500;
    c->gh = (u32)opt_int("halation_gain", 200, 0, 1000) * 256 * sh / 500;
    c->hcb = opt_int("halation_cb", -40, -200, 200) * 256 / 100;
    c->hcr = opt_int("halation_cr", 70, -200, 200) * 256 / 100;
    c->dark = (u32)opt_int("halation_dark", 70, 0, 100) * 256 / 100;
    c->read = (u32)opt_int("read", 1, 0, 1);
    c->read_us = (u32)opt_int("read_ms", 100, 0, 250) * 1000;
    c->cached = (u32)opt_int("read_cached", 0, 0, 1);
    fl = (216 * 5184 + 50 * c->width) / (100 * c->width);   /* optlayer.h's ol_floor_sigma for 5184 */
    log_printf(OMK_LOG_INFO, "optics: layer: route %s, width %u (cells of %u px; the transport alone blurs by sigma ~%u px), "
               "bytes %s; picture read %s, %u ms%s", c->route == DRV_OPTICS_403 ? "403" : "pair", c->width, 5184 / c->width,
               fl, c->order == DRV_OPTICS_ORDER_AUTO ? "auto" : t, c->read ? "on" : "off", c->read_us / 1000, c->cached ?
               " through the CACHED view" : "");
    log_printf(OMK_LOG_INFO, "optics: layer: Bloom %u (threshold %u, sigma %u + %u at %u%%, gain %u%% at 5)", sb, c->tb,
               c->sb & 0xffff, c->sb >> 16, c->mixb * 100 / 256, cfg_int("optics", "bloom_gain", 150));
    log_printf(OMK_LOG_INFO, "optics: layer: Halation %u (threshold %u, sigma %u + %u at %u%%, gain %u%% at 5, colour Cb %d%% "
               "Cr %d%%, dark %u%%)", sh, c->th, c->sh & 0xffff, c->sh >> 16, c->mixh * 100 / 256,
               cfg_int("optics", "halation_gain", 200), c->hcb * 100 / 256, c->hcr * 100 / 256, c->dark * 100 / 256);
    for (i = 0; i < 4; i++) {
        u32 v = i == 0 ? c->sb & 0xffff : i == 1 ? c->sb >> 16 : i == 2 ? c->sh & 0xffff : c->sh >> 16;

        if (v && v < fl && (i < 2 ? sb : sh))
            log_printf(OMK_LOG_WARN, "optics: %s sigma %u px is below the layer's smallest (~%u px at width %u): it comes "
                       "out ~%u px; a wider layer (layer_width) narrows it - ~%u px at %u", i < 2 ? "Bloom" : "Halation",
                       v, fl, c->width, fl, (216 * 5184 + 50 * 400) / (100 * 400), 400);
    }
    return 1;
}

/* Boot, after the store and before the modules: the level, the bisection and the store of level 2, the driver's step */
void optics_setup(void)
{
    int lv = cfg_int("optics", "probe", 0), rc, run = cfg_int("optics", "bisect", 0), layer;
    u32 sub = DRV_OPTICS_FULL;
    drv_optics_layer_cfg lc;

    if (lv < 0 || lv > LEVEL_MAX) {
        log_printf(OMK_LOG_WARN, "optics: probe = %d out of 0..%u, the probe is off", lv, LEVEL_MAX);
        lv = 0;
    }
    layer = layer_cfg(&lc, cfg_str("optics", "schedule", ""));
    if (!lv && !layer)
        return;
    if (!lv)
        sub = DRV_OPTICS_L4;
    op.level = (u32)lv;
    op.worker_test = (u32)cfg_int("optics", "worker_test", 0);
    crumbs();
    if (run || lv == 2) {
        op.s = store_open(OPT_STORE, OPT_STORE_SIZE);
        if (!op.s) {
            log_printf(OMK_LOG_ERROR, "optics: no store %s, the probe is off", OPT_STORE);
            return;
        }
    }
    if (run)
        sub = bis_start((u32)run, bis_named(cfg_str("optics", "bisect_from", "N")));
    else
        sch_parse(cfg_str("optics", "schedule", ""));
    if (sch.n) {                            /* the jobs get nothing of ours until the first entry is given */
        sch.on = 1;
        sub = DRV_OPTICS_N;
        log_printf(OMK_LOG_INFO, "optics: schedule of %u entries, one a press%s", sch.n, sch.worker ?
                   "; the worker task first" : "");
    }
    if (lv == 2)
        opt_store_start((u32)cfg_int("optics", "run", 1));
    rc = drv_optics_install(op.level);
    if (rc) {
        log_printf(OMK_LOG_ERROR, "optics: the probe's step not installed, code %d", rc);
        return;
    }
    if (layer) {
        u32 ws;

        rc = drv_optics_layer(&lc);
        if (rc) {
            log_printf(OMK_LOG_ERROR, "optics: the layer not set up, code %d: no Bloom and Halation", rc);
            if (!lv)
                return;
        } else {
            drv_optics_layer_floor(5184, &ws);
            log_printf(OMK_LOG_INFO, "optics: layer set up: the size hook in, the worker's buffer %u bytes", ws);
            if (!run && !sch.n && lv)
                sub = DRV_OPTICS_L4;        /* a strength with the probe's level: the layer's work */
        }
    }
    drv_optics_sub(sub);
    op.on = 1;
    log_printf(OMK_LOG_INFO, "optics: probe level %u: a step in the look's stills on the mask route%s; the stills get %s",
               op.level, lv <= 1 ? "" : lv == 2 ? (op.can_arm ? ", cached read one job an arming" : ", cached read off") :
               ", stripes in the picture", drv_optics_sub_name(sub));
}

/* The worker task, once, before a substage that needs it (core task, the card not in use): 1 running, -1 not */
static int opt_worker(void)
{
    u32 tid;
    int rc;

    if (op.worker)
        return op.worker;
    rc = drv_optics_worker(&tid);
    op.worker = rc ? -1 : 1;
    dbg_event(rc ? "optics: worker task not made" : "optics: worker task made", 0);
    if (rc)
        log_printf(OMK_LOG_ERROR, "optics: the worker task not running, code %d (task %u)", rc, tid);
    else
        log_printf(OMK_LOG_INFO, "optics: the worker task runs: id %u, CPU 0, priority 56", tid);
    return op.worker;
}

/* The schedule, core loop: the first entry once the worker runs (if needed); then one entry a press */
static void sch_service(u32 now, int busy, u32 ended)
{
    u32 app, keys;
    int first;

    (void)now;
    if (busy)
        return;
    if (!sch.given) {
        if (sch.worker && opt_worker() < 0) {
            sch.on = 0;                     /* the jobs stay at N */
            log_printf(OMK_LOG_ERROR, "optics: schedule stopped: no worker task");
            return;
        }
        if (op.worker_test)
            return;                         /* its tests first */
    } else {
        if (ended == sch.ended0 || drv_ui_probe(&app, &keys) > 0)
            return;                         /* no job of the press ended yet, or the application not settled */
        if (sch.k + 1 == sch.n) {
            sch.ended0 = ended;
            return;                         /* the last entry: kept */
        }
        sch.k++;
    }
    first = !sch.given;
    sch.given = 1;
    sch.ended0 = ended;
    drv_optics_loop(sch.loop[sch.k], sch.sleep[sch.k]);
    drv_optics_sub(sch.sub[sch.k]);
    if (sch.sleep[sch.k])
        log_printf(OMK_LOG_INFO, "optics: schedule %u of %u: %s with a sleep of %u ms instead of the loop, given to the next "
                   "press", sch.k + 1, sch.n, drv_optics_sub_name(sch.sub[sch.k]), sch.sleep[sch.k]);
    else if (sch.loop[sch.k])
        log_printf(OMK_LOG_INFO, "optics: schedule %u of %u: %s with a loop of %u iterations, given to the next press",
                   sch.k + 1, sch.n, drv_optics_sub_name(sch.sub[sch.k]), sch.loop[sch.k]);
    else
        log_printf(OMK_LOG_INFO, "optics: schedule %u of %u: %s given to the next press%s", sch.k + 1, sch.n,
                   drv_optics_sub_name(sch.sub[sch.k]), sch.k + 1 == sch.n ? " (the last entry: kept)" : "");
    dbg_event("optics: schedule", drv_optics_sub_name(sch.sub[sch.k]));
    dbg_mem_dump(first ? "before the first press of the schedule" : "a press of the schedule ended, the camera settled");
}

/* The bisection, core loop: the substage under test armed; passed - the next one saved, then armed */
static void bis_service(const drv_optics_info *st, u32 now, int busy)
{
    bis_state_t prev;
    u32 app, keys;

    if (!bis.on || busy || (bis.retry_ms && now - bis.retry_ms < OPT_RETRY_MS))
        return;
    if (bis.pending) {                      /* armed, the worker made: its tests first, then the jobs get it */
        if (op.worker_test)
            return;
        bis.pending = 0;
        drv_optics_sub(bis.b.index);
        log_printf(OMK_LOG_INFO, "optics: substage %s given to the next still", drv_optics_sub_name(bis.b.index));
        return;
    }
    if (!bis.armed) {
        /* "armed" on the card before anything of the substage happens - for one that needs the worker task, before the
         * task is made: card148 (core 1.37) stopped right after it was made, before any still. A reset from here on is
         * the substage's, and the next start makes no task */
        log_printf(OMK_LOG_INFO, "optics: substage %s armed for the next still%s", drv_optics_sub_name(bis.b.index),
                   bis.b.index >= DRV_OPTICS_WORKER && !op.worker ? " (the worker task made first)" : "");
        bis.b.armed = 1;
        dbg_event("optics: armed", drv_optics_sub_name(bis.b.index));
        if (!log_flush() || bis_save()) {   /* the log up to here on the card, then "armed" */
            bis.b.armed = 0;
            store_set(op.s, BIS_KEY, &bis.b, sizeof(bis.b), OMK_STORE_PERSIST);
            bis.retry_ms = now | 1;
            return;
        }
        bis.retry_ms = 0;
        bis.armed = 1;
        bis.ended0 = st->ended[bis.b.index < SUBS ? bis.b.index : 0];
        if (bis.b.index >= DRV_OPTICS_WORKER && opt_worker() < 0) {
            bis.on = 0;                     /* the jobs keep the substage before (given at start); not armed on the card */
            bis.b.armed = 0;
            store_set(op.s, BIS_KEY, &bis.b, sizeof(bis.b), OMK_STORE_PERSIST);
            store_save_now(op.s);
            log_printf(OMK_LOG_ERROR, "optics: bisection stopped before %s: no worker task", drv_optics_sub_name(bis.b.index));
            return;
        }
        if (bis.b.index >= DRV_OPTICS_WORKER && op.worker_test) {
            bis.pending = 1;                /* [optics] worker_test: its requests first */
            return;
        }
        drv_optics_sub(bis.b.index);
        return;
    }
    if (st->ended[bis.b.index] == bis.ended0)
        return;                             /* no job of it ended yet */
    if (drv_ui_probe(&app, &keys) > 0)
        return;                             /* the application in a state that should last a moment only: not yet */
    mem_copy(&prev, &bis.b, sizeof(prev));
    bis.b.index++;
    bis.b.armed = 0;
    if (bis_save()) {
        mem_copy(&bis.b, &prev, sizeof(prev));
        store_set(op.s, BIS_KEY, &bis.b, sizeof(bis.b), OMK_STORE_PERSIST);
        bis.retry_ms = now | 1;
        return;
    }
    bis.armed = 0;
    log_printf(OMK_LOG_INFO, "optics: substage %s passed (%u jobs ended with it)%s", drv_optics_sub_name(prev.index),
               st->ended[prev.index] - bis.ended0, bis.b.index == BIS_DONE ? "; bisection done, the level's work from now"
               " on" : "");
    dbg_event("optics: passed", drv_optics_sub_name(prev.index));
    if (bis.b.index == BIS_DONE)
        bis.on = 0;                         /* the driver keeps FULL, the last one given */
}

/* Level 2, core loop: the reports of cached reads into the store; the next arming when the log is on the card */
static void opt_arm(const drv_optics_info *st, u32 now, int busy)
{
    if (st->cached != op.cached || st->cached_same != op.same) {
        op.b.reads += st->cached - op.cached;
        op.b.same += st->cached_same - op.same;
        op.cached = st->cached;
        op.same = st->cached_same;
        op.b.armed = st->armed;             /* the arming was taken: the read ended without a reset */
        op.b.saves++;
        store_set(op.s, OPT_KEY, &op.b, sizeof(op.b), OMK_STORE_PERSIST);
        if (!busy)
            store_save_now(op.s);           /* not armed on the card as soon as it may be written (or by the store) */
    }
    if (st->armed || st->jobs == op.arm_jobs || busy || (op.retry_ms && now - op.retry_ms < OPT_RETRY_MS))
        return;
    log_printf(OMK_LOG_INFO, "optics: arming the cached read for the next job (%u reads before)", op.b.reads);
    dbg_event("optics: cached read armed", 0);
    if (!log_flush()) {                     /* the reports before it are not all on the card */
        op.retry_ms = now | 1;
        return;
    }
    op.b.armed = 1;
    if (opt_save()) {
        op.b.armed = 0;
        store_set(op.s, OPT_KEY, &op.b, sizeof(op.b), OMK_STORE_PERSIST);
        op.retry_ms = now | 1;
        log_printf(OMK_LOG_WARN, "optics: the store did not take the arming, tried again later");
        return;
    }
    op.retry_ms = 0;
    op.arm_jobs = st->jobs;
    drv_optics_arm();
}

void optics_service(u32 now)
{
    drv_optics_info st;
    u32 ended, k;
    int busy;

    if (!op.on)
        return;
    drv_optics_beat(now);
    drv_optics_service(&st);
    for (ended = k = 0; k < DRV_OPTICS_SUBS; k++)
        ended += st.ended[k];
    if (st.listed != op.listed) {           /* the debug service's events: the tasks while a job of ours is under way */
        op.listed = st.listed;
        dbg_event("optics: job listed", 0);
    }
    if (ended != op.ended) {
        op.ended = ended;
        dbg_event("optics: job ended", 0);
    }
    busy = drv_card_busy() || drv_prof_take_recent();
    if (op.worker_test && op.worker > 0 && !busy) {
        u32 us, work, state;

        op.worker_test--;
        if (drv_optics_worker_test(&us, &work, &state))
            log_printf(OMK_LOG_WARN, "optics: worker test: no worker");
        else
            log_printf(state ? OMK_LOG_WARN : OMK_LOG_INFO, "optics: worker test: answered in %u us (the worker's own %u us), "
                       "state %u", us, work, state);
        if (!op.worker_test)
            log_flush();                    /* the tests' lines on the card before anything is armed */
        return;
    }
    if (bis.on) {
        bis_service(&st, now, busy);
        return;                             /* the cached read only after the bisection */
    }
    if (sch.on) {
        sch_service(now, busy, ended);
        return;                             /* no cached read with a schedule */
    }
    if (!op.worker && !busy && (!bis.b.run || bis.b.index == BIS_DONE))
        opt_worker();                       /* without a bisection the stills get the level's work: it needs the worker */
    if (op.can_arm && (!bis.b.run || bis.b.index == BIS_DONE))
        opt_arm(&st, now, busy);
}
