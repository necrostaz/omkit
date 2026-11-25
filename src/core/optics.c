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
 * The layer (feature/optics-layer, Bloom and Halation; opticsdrv.c's header, optlayer.h): on with [optics] layer = on,
 * a strength key (bloom, halation: a test) or a layer substage (L0..L4) in the schedule; the jobs then get L4 (without a
 * schedule). The strengths of a job are the look's optics rows' (the module's preset: look_optics_now, given to the
 * driver when they change), both 0 - no step at all; the gains below are those of the strongest step. Its settings, all
 * in [optics]:
 *   layer                     on - the layer (and the look's optics rows: without the layer they are not installed)
 *   bloom, halation           0..5: strengths of a test in place of the rows' (in a set: that set's presses)
 *   bloom_steps, halation_steps    five percents of the gains, the strengths 1..5 (20, 32, 50, 71, 100 and 16, 25, 40,
 *                                  63, 100: about x1.5 and x1.6 a step; Bloom's 1 about a filter of 1/8 against
 *                                  1/2 at 5, the halation's the host model's sets A .. D of card166)
 *   day_mean, day_share       the day's weight from the layer's Y: its mean from .. to (codes; 95, 125) and the share of
 *                             it at the code 200 or above from .. to (percent; 6, 14) - the larger of the two
 *   day_bloom, day_halation   the gains on a day scene, percent of the night's (100, 6): a blown pixel there is a bright
 *                             surface (metal, a window), not a lamp many times brighter than white - the halation's red
 *                             "neon" on it is less; Bloom glows by day as by night (a diffusion filter; 100 - off)
 *   bench                     1 - the CPU bench of card164/165 in every job (0)
 *   route                     pair (the firmware's Soft Focus steps, 0x401 + 0x402) or 403 (our last step, 0x403)
 *   layer_width               the small layer's width, 42..400 (324: a cell of 16 pixels of 5184)
 *   layer_bytes               auto, or the bytes of Y, Cb, Cr in a layer word as three digits (012: Y 0, Cb 1, Cr 2)
 *   gamma                     the curve to linear light, 200..300 (x100)
 *   bloom_threshold, halation_threshold    codes 0..255 of Y where the glow starts
 *   bloom_sigma, bloom_sigma2, bloom_mix   the Gaussians in pixels of the picture, the second's weight in percent
 *   halation_sigma, halation_sigma2, halation_mix
 *   bloom_gain, halation_gain              the light added at strength 5, percent of the source (0..5000)
 *   halation_cb, halation_cr               the halation's colour: Cb, Cr a unit of its Y, percent (signed)
 *   halation_dark                          less halation on bright places: percent
 *   emission                               the source: 0 - the excess of a pixel's light over the threshold (the
 *                                          first version); 1..64 (16) - the emission of a pixel whose blown light
 *                                          stands for that many whites (the tone curve's shoulder undone): a lamp
 *                                          glows, white paper just under 255 hardly does
 *   bloom_emission, halation_emission      the same for each alone (card168; emission when not given): a diffusion
 *                                          filter glows from bright surfaces too - Bloom with fewer whites (4) and a
 *                                          lower threshold takes them in, the halation keeps to lamps
 *   bloom_dark                             less Bloom on bright places, as halation_dark: percent (80)
 *   halation_first                         1 (default) - the halation's code, and so its colour, from the place's own
 *                                          light, Bloom added after it (its colour does not fade under a strong
 *                                          Bloom); 0 - Bloom first (the first version)
 *   bloom_tail, bloom_tail_sigma           Bloom's long tail (a diffusion filter's glow): the share of Bloom's light,
 *                                          percent (0 - none, the default), in a Gaussian of that sigma, pixels of
 *                                          the picture (300), made on a plane 4 times smaller each way
 *   bloom_haze                             Bloom's haze: percent of all the light (not only over the threshold) in
 *                                          the tail's sources (0 - none; with a tail only) - a diffusion filter
 *                                          scatters a part of everything it passes: bright surfaces glow into the
 *                                          dark around them, the shadows lift, the contrast falls
 *   bloom_toe, bloom_max                   Bloom's strength's curve after the blur, percent of white (0 - none, the
 *   halation_toe, halation_max             default): the toe takes a weak glow away (a glint's), the ceiling holds a
 *                                          strong one under it (a lamp's halo without a flat disc) - optlayer.h's
 *                                          ol_curve
 *   jobs                                   first (default) - only the first job of a RAW+JPEG press gets the layer
 *                                          (it makes the JPEG; the second makes the ORF's preview), both - both
 *   read, read_cached                      the picture's rows read for the sources (every row of cells, two rows of
 *                                          the picture each, whatever the time - card169), the cached view; read_ms
 *                                          (the budget of card168's read) is not taken
 *   layer_cached                           the layer read and written (L4) through the window's cached view, written
 *                                          back before the window is released
 * With emission 0, bloom_dark 0, halation_first 0 and no tail the layer is the first version's, value for value.
 * Further sets of these keys - all but route, layer_width, layer_bytes and gamma - in sections [optics.A] ..
 * [optics.F] (a key not given there is [optics]'s), named by the schedule: L4:A - the jobs of that press get the
 * set A. One session then gives a ladder of settings on one scene: schedule = N, L4:A, L4:B, L4:C.
 * The smallest sigma the transport gives (the ISP's reduction and enlargement) is logged at start; a sigma below it
 * comes out as it (a warning).
 *
 * With the debug service on ([debug] enabled), the moments of the layer - a substage or the cached read armed, a
 * substage passed, the worker task made, a job of ours listed or ended (as the core task sees them) - are events of its
 * RAM log (dbg_event): a mark, the watched tasks' states and the window of snapshots after it. */
#include "omk_core.h"
#include "omk_debug.h"
#include "omk_isp.h"
#include "omk_log.h"
#include "omk_look.h"
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
/* The layer's defaults that the host model chose (the scenes of card162..165; docs of the module) */
#define OPT_EMISSION 16                     /* a blown pixel stands for 16 whites */
#define OPT_BLOOM_DARK 80                   /* Bloom 80 % less on white */
#define OPT_BLOOM_THRESHOLD 200
#define OPT_HAL_THRESHOLD 225
#define OPT_BLOOM_GAIN 150
#define OPT_HAL_GAIN 200
#define OPT_HAZE_MAX 25                     /* bloom_haze, percent of all the light */
#define OPT_Q12 4095                        /* white in linear light (optlayer.h's OL_ONE): the curves' percents */
/* The day's weight (card168: the scenes' logs of card167 - evening rooms mean Y 78..94 and 0.3..5.4 % at the threshold,
 * night 62..71 and 1.3..1.9 %, the day of card162 100 and 14.7 %): the share tells the day, the mean only past 95 */
#define OPT_DAY_MEAN0 95
#define OPT_DAY_MEAN1 125
#define OPT_DAY_SHARE0 6
#define OPT_DAY_SHARE1 14
/* The strengths 1..5 as percents of the gains (the gains: of the strongest step). Geometric steps: Bloom's a diffusion
 * filter's ladder (1/8 : 1/4 : 1/2 about 1 : 1.8 : 3..4.6 - references measured; card168's host model), the halation's
 * the host model's sets of card166 - A (400) at 1, B .. C at 3, D (2500) at 5 (docs of the module) */
static const u8 opt_steps[2][5] = {{20, 32, 50, 71, 100}, {16, 25, 40, 63, 100}};

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
    int layer;                              /* the layer is set up */
    int test_str;                           /* a set gives a test's strength above 0 (in place of the rows') */
    u32 str;                                /* the look's strengths given to the driver: Bloom | Halation << 8, or ~0 */
} op = {.str = ~0u};

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
    u8 set[SCHED_MAX];                      /* the layer's settings: 0 - [optics], k - [optics.X], X = 'A' + k - 1 */
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
    u32 n, k, v, i, slash, sleep, set, colon;
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
        for (colon = 0; word[colon] && word[colon] != ':'; colon++)
            ;
        set = 0;
        if (word[colon]) {                  /* NAME:X - the layer's settings X ([optics.X]) */
            v = (u32)(word[colon + 1] | 0x20) - 'a';
            set = word[colon + 2] || v >= DRV_OPTICS_SETS ? DRV_OPTICS_SETS + 1 : v + 1;
            word[colon] = 0;
        }
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
        if (set)
            bad |= set > DRV_OPTICS_SETS || k < DRV_OPTICS_L0 || k >= DRV_OPTICS_SUBS || slash;
        if (k == DRV_OPTICS_SUBS || bad || sch.n == SCHED_MAX) {
            if (slash)
                word[i] = '/';
            if (set)
                word[colon] = ':';
            log_printf(OMK_LOG_WARN, "optics: schedule: '%s' %s - left out", word, sch.n == SCHED_MAX ? "is one too many" :
                       "is not a substage, NAME/N (C3, W1, W2; N 10..100000000), W1/sMS (1..2000) or L0..L4:X (X A..F)");
            continue;
        }
        sch.set[sch.n] = (u8)set;
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

/* An integer of section sec (else of [optics]: a further set's key not given is the first set's) clamped to lo..hi
 * (logged when it was out) */
static int opt_sint(const char *sec, const char *key, int def, int lo, int hi)
{
    const char *from = cfg_str(sec, key, 0) ? sec : "optics";
    int v = cfg_int(from, key, def);

    if (v < lo || v > hi) {
        log_printf(OMK_LOG_WARN, "%s: %s = %d out of %d..%d, %d taken", from, key, v, lo, hi, v < lo ? lo : hi);
        v = v < lo ? lo : hi;
    }
    return v;
}

static int opt_int(const char *key, int def, int lo, int hi)
{
    return opt_sint("optics", key, def, lo, hi);
}

/* "a, b" (two numbers 0..hi, a below b) into *a, *b; else (absent or bad: logged) the defaults */
static void opt_pair(const char *t, u32 *a, u32 *b, u32 da, u32 db, u32 hi)
{
    u32 v[2], n = 0, x, d;
    const char *t0 = t;

    *a = da;
    *b = db;
    while (t && *t && n < 2) {
        while (*t == ',' || *t == ' ' || *t == '\t')
            t++;
        for (x = d = 0; *t >= '0' && *t <= '9'; t++, d++)
            x = x < 100000 ? x * 10 + (u32)(*t - '0') : x;
        if (!d)
            break;
        v[n++] = x;
    }
    if (!t0)
        return;
    if (n != 2 || *t || v[0] >= v[1] || v[1] > hi) {
        log_printf(OMK_LOG_WARN, "optics: '%s': not two numbers 0..%u, the first below the second; %u, %u taken", t0, hi,
                   da, db);
        return;
    }
    *a = v[0];
    *b = v[1];
}

/* The keys of a set of the layer's settings (sec: [optics] or [optics.X]) that the worker takes a job: the strengths,
 * thresholds, Gaussians, gains, colour, the darkness weights, the source, the tail, the reads; logged as `name` */
static void layer_keys(drv_optics_layer_cfg *c, const char *sec, const char *name)
{
    u32 sb, sh, gb, gh, tail, ts, haze, i, k;
    const char *t;

    /* a strength key of the set (or of [optics]): a test's strength in place of the look's rows */
    c->sbv = cfg_str(sec, "bloom", 0) || cfg_str("optics", "bloom", 0) ? (u32)opt_sint(sec, "bloom", 0, 0, 5) :
             DRV_OPTICS_ROWS;
    c->shv = cfg_str(sec, "halation", 0) || cfg_str("optics", "halation", 0) ? (u32)opt_sint(sec, "halation", 0, 0, 5) :
             DRV_OPTICS_ROWS;
    sb = c->sbv;
    sh = c->shv;
    if ((sb != DRV_OPTICS_ROWS && sb) || (sh != DRV_OPTICS_ROWS && sh))
        op.test_str = 1;
    for (k = 0; k < 2; k++) {               /* bloom_steps, halation_steps: five percents 0..200, else the defaults */
        const char *key = k ? "halation_steps" : "bloom_steps";
        u32 v[5], n = 0, x, d;

        t = cfg_str(cfg_str(sec, key, 0) ? sec : "optics", key, 0);
        while (t && *t && n < 5) {
            while (*t == ',' || *t == ' ' || *t == '\t')
                t++;
            for (x = d = 0; *t >= '0' && *t <= '9'; t++, d++)
                x = x < 1000 ? x * 10 + (u32)(*t - '0') : x;
            if (!d)
                break;
            v[n++] = x;
        }
        if (t && (n != 5 || *t)) {
            log_printf(OMK_LOG_WARN, "optics: %s: not five numbers 0..200, the default taken", key);
            t = 0;
        }
        for (i = 0; i < 5; i++)
            c->steps[k][i] = t ? (v[i] > 200 ? 200 : v[i]) : opt_steps[k][i];
    }
    t = cfg_str(cfg_str(sec, "day_mean", 0) ? sec : "optics", "day_mean", 0);
    opt_pair(t, &c->day[0], &c->day[1], OPT_DAY_MEAN0, OPT_DAY_MEAN1, 255);
    t = cfg_str(cfg_str(sec, "day_share", 0) ? sec : "optics", "day_share", 0);
    opt_pair(t, &c->day[2], &c->day[3], OPT_DAY_SHARE0, OPT_DAY_SHARE1, 100);
    c->day[2] *= 10;                        /* permille */
    c->day[3] *= 10;
    c->day[4] = (u32)opt_sint(sec, "day_bloom", 100, 0, 100);
    c->day[5] = (u32)opt_sint(sec, "day_halation", 6, 0, 100);
    c->bench = (u32)opt_sint(sec, "bench", 0, 0, 1);
    c->tb = (u32)opt_sint(sec, "bloom_threshold", OPT_BLOOM_THRESHOLD, 0, 254);
    c->th = (u32)opt_sint(sec, "halation_threshold", OPT_HAL_THRESHOLD, 0, 254);
    c->sb = (u32)opt_sint(sec, "bloom_sigma", 60, 0, 1000) | (u32)opt_sint(sec, "bloom_sigma2", 0, 0, 1000) << 16;
    c->sh = (u32)opt_sint(sec, "halation_sigma", 40, 0, 1000) | (u32)opt_sint(sec, "halation_sigma2", 0, 0, 1000) << 16;
    c->mixb = (u32)opt_sint(sec, "bloom_mix", 0, 0, 100) * 256 / 100;
    c->mixh = (u32)opt_sint(sec, "halation_mix", 0, 0, 100) * 256 / 100;
    gb = (u32)opt_sint(sec, "bloom_gain", OPT_BLOOM_GAIN, 0, 5000);
    gh = (u32)opt_sint(sec, "halation_gain", OPT_HAL_GAIN, 0, 5000);
    c->gb = gb * 256 / 100;                 /* the strongest step's; a job's strength scales it (steps) */
    c->gh = gh * 256 / 100;
    c->hcb = opt_sint(sec, "halation_cb", -40, -200, 200) * 256 / 100;
    c->hcr = opt_sint(sec, "halation_cr", 70, -200, 200) * 256 / 100;
    c->dark = (u32)opt_sint(sec, "halation_dark", 70, 0, 100) * 256 / 100;
    c->em = (u32)opt_sint(sec, "emission", OPT_EMISSION, 0, 64);
    c->emb = (u32)opt_sint(sec, "bloom_emission", (s32)c->em, 0, 64);   /* Bloom's own; the common one if not given */
    c->em = (u32)opt_sint(sec, "halation_emission", (s32)c->em, 0, 64);
    c->bdark = (u32)opt_sint(sec, "bloom_dark", OPT_BLOOM_DARK, 0, 100) * 256 / 100;
    c->hfirst = (u32)opt_sint(sec, "halation_first", 1, 0, 1);
    tail = (u32)opt_sint(sec, "bloom_tail", 0, 0, (DRV_OPTICS_TAIL_MAX * 100 + 255) / 256);   /* 90 % */
    ts = (u32)opt_sint(sec, "bloom_tail_sigma", 300, 0, 1000);
    c->tmix = ts ? tail * 256 / 100 : 0;
    c->st = c->tmix ? ts : 0;
    haze = (u32)opt_sint(sec, "bloom_haze", 0, 0, OPT_HAZE_MAX);
    c->haze = c->tmix ? haze * 4096 / 100 : 0;
    if (haze && !c->tmix)
        log_printf(OMK_LOG_WARN, "optics: layer%s: bloom_haze %u%% without Bloom's tail (bloom_tail): no haze", name, haze);
    c->read = (u32)opt_sint(sec, "read", 1, 0, 1);
    c->read_us = 0;                         /* card169: the cover read has no budget (read_ms is not taken) */
    if (cfg_str(sec, "read_ms", 0))
        log_printf(OMK_LOG_WARN, "optics: layer%s: read_ms is not taken: the picture's read covers every row of cells "
                   "whatever the time", name);
    c->cached = (u32)opt_sint(sec, "read_cached", 0, 0, 1);
    c->btoe = (u32)opt_sint(sec, "bloom_toe", 0, 0, 50) * OPT_Q12 / 100;
    c->bmax = (u32)opt_sint(sec, "bloom_max", 0, 0, 200) * OPT_Q12 / 100;
    c->htoe = (u32)opt_sint(sec, "halation_toe", 0, 0, 50) * OPT_Q12 / 100;
    c->hmax = (u32)opt_sint(sec, "halation_max", 0, 0, 200) * OPT_Q12 / 100;
    t = cfg_str(cfg_str(sec, "jobs", 0) ? sec : "optics", "jobs", "first");
    c->jobs = str_ieq(t, "both") ? DRV_OPTICS_JOBS_BOTH : DRV_OPTICS_JOBS_FIRST;
    if (!str_ieq(t, "both") && !str_ieq(t, "first"))
        log_printf(OMK_LOG_WARN, "optics: layer%s: jobs = %s unknown (first, both), first taken", name, t);
    c->lcached = (u32)opt_sint(sec, "layer_cached", 0, 0, 1);
    log_printf(OMK_LOG_INFO, "optics: layer%s: Bloom %s (threshold %u, sigma %u + %u at %u%%, gain %u%% at 5, %u%% less on "
               "white; tail %u%% of sigma %u, haze %u%%)", name, sb == DRV_OPTICS_ROWS ? "by the look's rows" : sb == 0 ? "0, a test" :
               sb == 1 ? "1, a test" : sb == 2 ? "2, a test" : sb == 3 ? "3, a test" : sb == 4 ? "4, a test" : "5, a test",
               c->tb, c->sb & 0xffff, c->sb >> 16, c->mixb * 100 / 256, gb, (c->bdark * 100 + 128) / 256,
               (c->tmix * 100 + 128) / 256, c->st, (c->haze * 100 + 2048) / 4096);
    log_printf(OMK_LOG_INFO, "optics: layer%s: Halation %s (threshold %u, sigma %u + %u at %u%%, gain %u%% at 5, colour Cb "
               "%d%% Cr %d%%, dark %u%%, %s)", name, sh == DRV_OPTICS_ROWS ? "by the look's rows" : sh == 0 ? "0, a test" :
               sh == 1 ? "1, a test" : sh == 2 ? "2, a test" : sh == 3 ? "3, a test" : sh == 4 ? "4, a test" : "5, a test",
               c->th, c->sh & 0xffff, c->sh >> 16, c->mixh * 100 / 256, gh, (c->hcb * 100 + (c->hcb < 0 ? -128 : 128)) / 256,
               (c->hcr * 100 + (c->hcr < 0 ? -128 : 128)) / 256,
               c->dark * 100 / 256, c->hfirst ? "before Bloom" : "after Bloom");
    log_printf(OMK_LOG_INFO, "optics: layer%s: strengths 1..5 - Bloom %u %u %u %u %u%%, Halation %u %u %u %u %u%% of the "
               "gains%s", name, c->steps[0][0], c->steps[0][1], c->steps[0][2], c->steps[0][3], c->steps[0][4],
               c->steps[1][0], c->steps[1][1], c->steps[1][2], c->steps[1][3], c->steps[1][4],
               c->bench ? "; the bench on" : "");
    log_printf(OMK_LOG_INFO, "optics: layer%s: a day scene (mean Y %u..%u or %u..%u%% at code 200): Bloom %u%%, "
               "Halation %u%% of the gains", name, c->day[0], c->day[1], c->day[2] / 10, c->day[3] / 10, c->day[4],
               c->day[5]);
    if (c->em)
        log_printf(OMK_LOG_INFO, "optics: layer%s: the source - the emission of a blown pixel as %u whites over the "
                   "threshold; the picture read %s", name, c->em, c->read ? "on" : "off");
    else
        log_printf(OMK_LOG_INFO, "optics: layer%s: the source - the excess over the threshold; the picture read %s",
                   name, c->read ? "on" : "off");
    if (c->emb != c->em)
        log_printf(OMK_LOG_INFO, "optics: layer%s: Bloom's own source - %s %u whites (bloom_emission; Halation's above)",
                   name, c->emb ? "the emission of a blown pixel as" : "the excess over the threshold, not", c->emb);
    log_printf(OMK_LOG_INFO, "optics: layer%s: the strength's curve after the blur (percent of white; 0 - none): Bloom toe "
               "%u, at most %u; Halation toe %u, at most %u", name, (c->btoe * 100 + OPT_Q12 / 2) / OPT_Q12,
               (c->bmax * 100 + OPT_Q12 / 2) / OPT_Q12, (c->htoe * 100 + OPT_Q12 / 2) / OPT_Q12,
               (c->hmax * 100 + OPT_Q12 / 2) / OPT_Q12);
    log_printf(OMK_LOG_INFO, "optics: layer%s: jobs = %s", name, c->jobs == DRV_OPTICS_JOBS_BOTH ? "both - both jobs of a "
               "RAW+JPEG press get the layer (the ORF's preview too)" : "first - the second job of a RAW+JPEG press (the "
               "ORF's preview) without the layer");
}

/* The layer's settings; 1 - the layer is on (a strength, or a layer substage in the schedule) */
static int layer_cfg(drv_optics_layer_cfg *c, const char *schedule)
{
    const char *t;
    u32 sb, sh, fl, i;
    int on;

    sb = (u32)opt_int("bloom", 0, 0, 5);
    sh = (u32)opt_int("halation", 0, 0, 5);
    for (on = sb || sh || cfg_bool("optics", "layer", 0), i = 0; schedule[i] && !on; i++)
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
    c->cached = (u32)opt_int("read_cached", 0, 0, 1);
    c->lcached = (u32)opt_int("layer_cached", 0, 0, 1);
    c->read = (u32)opt_int("read", 1, 0, 1);
    c->read_us = 0;
    fl = (216 * 5184 + 50 * c->width) / (100 * c->width);   /* optlayer.h's ol_floor_sigma for 5184 */
    log_printf(OMK_LOG_INFO, "optics: layer: route %s, width %u (cells of %u px; the transport alone blurs by sigma ~%u px), "
               "bytes %s; picture read %s%s", c->route == DRV_OPTICS_403 ? "403" : "pair", c->width, 5184 / c->width,
               fl, c->order == DRV_OPTICS_ORDER_AUTO ? "auto" : t, c->read ? "on, 2 rows a cell" : "off",
               c->cached ? " through the CACHED view" : "");
    log_printf(OMK_LOG_INFO, "optics: layer: the layer read and written %s", c->lcached ? "through the CACHED view (written "
               "back before the window is released)" : "past the cache");
    layer_keys(c, "optics", "");
    for (i = 0; i < 4; i++) {
        u32 v = i == 0 ? c->sb & 0xffff : i == 1 ? c->sb >> 16 : i == 2 ? c->sh & 0xffff : c->sh >> 16;

        if (v && v < fl && (i < 2 ? sb : sh))
            log_printf(OMK_LOG_WARN, "optics: %s sigma %u px is below the layer's smallest (~%u px at width %u): it comes "
                       "out ~%u px; a wider layer (layer_width) narrows it - ~%u px at %u", i < 2 ? "Bloom" : "Halation",
                       v, fl, c->width, fl, (216 * 5184 + 50 * 400) / (100 * 400), 400);
    }
    return 1;
}

/* The further sets the schedule names (L4:A ...): each from [optics.X] over [optics], to the driver (after
 * drv_optics_layer). A set not given (the driver refused it) - its entries get the first set */
static void layer_sets(const drv_optics_layer_cfg *base)
{
    drv_optics_layer_cfg c;
    char sec[12], name[12];
    const char *se, *ke, *va;
    u32 k, i, used;

    mem_copy(sec, "optics.A", 9);           /* (an initialised array would call memcpy) */
    mem_copy(name, " settings A", 12);

    for (k = 1; k <= DRV_OPTICS_SETS; k++) {
        for (i = 0, used = 0; i < sch.n; i++)
            used |= sch.set[i] == k;
        if (!used)
            continue;
        sec[7] = name[10] = (char)('A' + k - 1);
        for (i = 0; i < cfg_count(); i++) {
            cfg_entry(i, &se, &ke, &va);
            if (str_ieq(se, sec))
                break;
        }
        if (i == cfg_count())
            log_printf(OMK_LOG_WARN, "optics: the schedule names settings %s, but [%s] has no keys: [optics]'s taken",
                       name + 10, sec);
        mem_copy(&c, base, sizeof(c));
        layer_keys(&c, sec, name);
        if (drv_optics_layer_set(k, &c))
            log_printf(OMK_LOG_ERROR, "optics: settings %s not given to the driver: [optics]'s taken", name + 10);
    }
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
        u32 ws, wb;

        rc = drv_optics_layer(&lc);
        if (rc) {
            log_printf(OMK_LOG_ERROR, "optics: the layer not set up, code %d%s: no Bloom and Halation", rc, rc >= 30 &&
                       rc < 158 ? " (a CPU kernel of the layer does not give what plain C gives)" : "");
            if (!lv)
                return;
        } else {
            layer_sets(&lc);
            op.layer = 1;
            drv_look_rows(1);               /* the look's optics rows: only with the layer */
            drv_optics_layer_floor(5184, &ws, &wb);
            log_printf(OMK_LOG_INFO, "optics: layer set up: the size hook in, the worker's buffer %u bytes at %08x; the "
                       "CPU kernels (the extension unit's multiplies, the read's loops) checked against plain C", ws, wb);
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
    char sn[2];

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
    drv_optics_set(sch.set[sch.k]);
    drv_optics_sub(sch.sub[sch.k]);
    sn[0] = (char)(sch.set[sch.k] ? 'A' + sch.set[sch.k] - 1 : 0);
    sn[1] = 0;
    if (sch.sleep[sch.k])
        log_printf(OMK_LOG_INFO, "optics: schedule %u of %u: %s with a sleep of %u ms instead of the loop, given to the next "
                   "press", sch.k + 1, sch.n, drv_optics_sub_name(sch.sub[sch.k]), sch.sleep[sch.k]);
    else if (sch.loop[sch.k])
        log_printf(OMK_LOG_INFO, "optics: schedule %u of %u: %s with a loop of %u iterations, given to the next press",
                   sch.k + 1, sch.n, drv_optics_sub_name(sch.sub[sch.k]), sch.loop[sch.k]);
    else
        log_printf(OMK_LOG_INFO, "optics: schedule %u of %u: %s%s%s given to the next press%s", sch.k + 1, sch.n,
                   drv_optics_sub_name(sch.sub[sch.k]), sch.set[sch.k] ? " with the settings " : "",
                   sn, sch.k + 1 == sch.n ? " (the last entry: kept)" : "");
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

/* The look's strengths (its owner's preset, the optics rows) to the driver when they change, with whether the colour
 * tables in force are black and white (isp_mono, the rule of the look's grain: no colour in the halation then): the next
 * jobs take them. With a test's strengths in the INI's sets the rows' do not reach those jobs: said with the line */
static void opt_strength(void)
{
    u8 v[OMK_OPTICS_ROWS];
    u32 s, mono = isp_mono() ? 1 : 0;

    look_optics_now(v);
    s = v[OMK_OPTICS_BLOOM] | (u32)v[OMK_OPTICS_HALATION] << 8 | mono << 16;
    if (s == op.str)
        return;
    op.str = s;
    drv_optics_strength(v[OMK_OPTICS_BLOOM], v[OMK_OPTICS_HALATION], mono);
    log_printf(OMK_LOG_INFO, "optics: the look's strengths now: Bloom %u, Halation %u%s%s%s", v[OMK_OPTICS_BLOOM],
               v[OMK_OPTICS_HALATION], mono ? ", black and white (the halation without colour)" : "",
               s & 0xffff ? "" : " (no step in its stills)", op.test_str ? "; NOT taken where the INI gives a test's "
               "strengths" : "");
}

int optics_route(void)
{
    return op.layer && ((op.str != ~0u && (op.str & 0xffff)) || op.test_str);
}

void optics_service(u32 now)
{
    drv_optics_info st;
    u32 ended, k;
    int busy;

    if (!op.on)
        return;
    if (op.layer)
        opt_strength();
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
