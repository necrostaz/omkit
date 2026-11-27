/* Power states (omk_power.h). Two sides share a few words:
 *  - the driver hooks run in firmware tasks on CPU 0 and call power_hook: they change the state, count a sequence number
 *    at once; the gates then wait (at most POWER_GATE_MS) until the core task acknowledges;
 *  - the core task calls power_service from its loop: on a new sequence number it does the last save
 *    before power-off (the card is still mounted: the card-off gate waits for us) and acknowledges.
 * While the state is not POWER_RUN the core loop does not write to the card.
 *
 * The lever off without a power-off. The firmware turns the camera off only when its application takes the lever's event;
 * an application stuck in a shooting state (a half-press whose exposure sequence waits for ever for the live view's
 * exposure stream to stop - the task dumps of such hangs, state 0x5101) never does: the lever flag stays set, the camera
 * stays on, and the user takes the battery out. So, while the lever is off and the camera did not go off (power_card_ok):
 *  - no card writes at all (log, stores, the modules' files): the battery may go at any moment - the lines wait in RAM;
 *  - the rescue (a debug build only, on unless [core] power_rescue = off): the lever off RESCUE_MS and the application in one passing state
 *    (drv_ui_probe) all that time, the shutter released and no picture being written - the last save (as at the card-off
 *    gate), then the firmware's own power-off job with a restart (drv_restart_hfw, as at the test scenario's end); the
 *    camera starts again with the lever off, and the firmware turns it off at once. The watchdog is the last resort.
 *
 * A release build (omk_types.h) has the hooks, the lever's flag (one word of RAM, drv_power_pending) and the hold of card
 * writes only: no probe line, no rescue (it reads the application's state at every pass), no read of the subprocessor
 * clock at a sleep. */
#include "omk_core.h"
#include "omk_log.h"
#include "omk_mod.h"
#include "omk_power.h"
#include "omk_store.h"

#define TIMED_OUT 0xffffffffu                /* reason of POWER_RUN set by power_service, not by a hook */
#define REASON_AT_START 0x100               /* power-off reasons found by core itself, not by the firmware hook */
#define REASON_POLLED 0x101
#define PROBE_WORDS 7

static volatile u32 state, seq, acked, reason, since_ticks;
#ifdef OMK_DEBUG
static volatile u32 hook_ticks[POWER_HOOKS], hook_arg[POWER_HOOKS], hook_count[POWER_HOOKS];   /* for the dumps */
#endif
static u32 handled = POWER_RUN, handled_ms, sleep_ticks;
static int flag_ignored;                    /* the lever flag stayed 1 although power-off did not happen: ignore until 0 */
#ifdef OMK_DEBUG
static u32 sleep_ms, sleep_sec, sleep_frac;
static int sleep_rtc;                       /* 0 - the subprocessor clock was read when sleep began */
static u32 probe[PROBE_WORDS], probe_n;
static u32 cancels;                         /* power-offs and sleeps that did not happen: the task dump watches it */
#endif

#ifdef OMK_DEBUG
#define RESCUE_MS 8000                      /* the lever off and the application stuck this long: the rescue (a power-off
                                               takes 3-5 s on the camera, and POWER_CANCEL_MS gives up on it after 5) */
#define RESCUE_OFF_WAIT_MS 1000             /* the power-off job: its hook within this, ... */
#define RESCUE_POSTS 2                      /* ... posted at most this many times */
#define RESCUE_DONE_WAIT_MS 20000           /* the power-off begun: the power goes within this (a settings write takes up
                                               to 10 s) */
#define RESCUE_CAUSE 0x4f4d4b50             /* "OMKP": the cause of the watchdog reset, in (0x8ff000b0) for the next start */

enum { R_WATCH, R_POST, R_OFF, R_GOING };
static int lever_off, stuck;                /* the lever seen off (since lever_since); the application stuck (since
                                               stuck_since, in stuck_app) */
static u32 lever_since, stuck_since, stuck_app, rescue_t, rescue_posts, rescue_offs;
static int rescue_on = 1, rescue_stage;
#endif
static int lever_hold;                      /* the lever is off and the camera did not go off: no card writes */
static u32 hold_ms;

static const char *const state_names[] = {"running", "power-off", "sleep"};

static u32 ms_since(u32 ticks)
{
    return drv_ticks_since(ticks) / 1000;
}

#ifdef OMK_DEBUG
/* After a sleep: the subprocessor clock says how long it really was. The tick counter loses whole turns of 2^30 us in a
 * sleep longer than ~1073 s; only whole turns are added back, so a clock set by the user during the sleep changes nothing */
#define TURN_MS 1073742                     /* 2^30 us */

static void correct_clock(u32 now)
{
    u32 sec, frac, real, seen, turns;

    if (sleep_rtc || drv_rtc(&sec, &frac))
        return;
    real = (sec - sleep_sec) * 1000 + frac * 1000 / 1024 - sleep_frac * 1000 / 1024;
    seen = now - sleep_ms;
    if (real <= seen + 2000 || real - seen > 400 * TURN_MS)   /* nothing missing, or a clock change: leave it */
        return;
    turns = (real - seen + TURN_MS / 2) / TURN_MS;
    if (!turns || (real - seen > turns * TURN_MS ? real - seen - turns * TURN_MS : turns * TURN_MS - (real - seen)) > 5000)
        return;
    omk_ms_add(turns * TURN_MS);
    log_printf(OMK_LOG_INFO, "wake: slept %u s by the subprocessor clock, the tick counter wrapped %u times, clock corrected",
               real / 1000, turns);
}
#endif

/* Under drv_lock */
static void set_state(u32 s, u32 r)
{
    state = s;
    reason = r;
    since_ticks = drv_ticks();
    seq++;
}

/* Gate in a firmware task: wait until the core task has acknowledged the current state */
static int gate(const char *what)
{
    u32 st, s, t0 = drv_ticks();

    st = drv_lock();
    s = seq;
    drv_unlock(st);
    while (acked != s && ms_since(t0) < POWER_GATE_MS)
        drv_sleep(10);
    if (acked == s) {
        log_printf(OMK_LOG_INFO, "power: %s, core ready, waited %u ms", what, ms_since(t0));
        return 1;
    }
    log_printf(OMK_LOG_WARN, "power: %s, core not ready after %u ms, going on", what, ms_since(t0));
    return 0;
}

void power_hook(u32 kind, u32 arg)
{
    u32 st, s, n;

    if (!kind || kind >= POWER_HOOKS)
        return;
#ifdef OMK_DEBUG
    hook_ticks[kind] = drv_ticks();
    hook_arg[kind] = arg;
    hook_count[kind]++;
#endif
    st = drv_lock();
    s = state;
    n = seq;
    switch (kind) {
    case POWER_HOOK_OFF:
        if (s != POWER_OFF)
            set_state(POWER_OFF, arg);
        break;
    case POWER_HOOK_CARD_OFF:
        if (s != POWER_OFF)
            set_state(POWER_OFF, 0);        /* a path past the power-off hook */
        break;
    case POWER_HOOK_SLEEP:
    case POWER_HOOK_SLEEP_GATE:
        if (s == POWER_RUN)
            set_state(POWER_SLEEP, 0);
        break;
    case POWER_HOOK_WAKE:
        if (s == POWER_SLEEP)
            set_state(POWER_RUN, arg);
        break;
    }
    n = n != seq;
    drv_unlock(st);
    if (n)
        drv_wake();                         /* the core task acknowledges now, not after its 250 ms wait */
    switch (kind) {
    case POWER_HOOK_OFF:
    case POWER_HOOK_SLEEP:
        break;
    case POWER_HOOK_CARD_OFF:
        /* The last save happens here, not when power-off began: by now the firmware has finished its own card work
         * (a picture being written), and the core task is idle once it acknowledged */
        if (gate("card off") && state == POWER_OFF) {
            core_mark_ended();
            store_save_all();
            log_flush();
            log_close();
        }
        break;
    case POWER_HOOK_SLEEP_GATE:
        gate("sleep");
        break;
    }
}

void power_install(void)
{
    const char *name;
    u32 i, ok = 0;
    int rc;

    for (i = 0; (rc = drv_power_install(i, &name)) >= 0; i++) {
        if (!rc)
            ok++;
        else
            log_printf(OMK_LOG_ERROR, "power: hook %s not installed, code %d", name, rc);
    }
    if (ok != i)
        log_printf(OMK_LOG_WARN, "power: %u of %u hooks installed", ok, i);
    else
        log_printf(OMK_LOG_DEBUG, "power: %u of %u hooks installed", ok, i);
#ifdef OMK_DEBUG
    rescue_on = cfg_bool("core", "power_rescue", 1);
#endif
    /* After the hooks: the lever flag is set before CameraCommon::shutDown, so nothing slips between check and hooks */
    if (drv_power_pending()) {
        log_printf(OMK_LOG_WARN, "power: power-off already under way at start, modules wait");
        power_hook(POWER_HOOK_OFF, REASON_AT_START);
    }
}

#ifdef OMK_DEBUG
/* Candidate words for the power-off flag, debug: at start and on every change */
static void log_probe(void)
{
    u32 w[PROBE_WORDS], n, i, same;

    if (log_level() < OMK_LOG_DEBUG)
        return;
    n = drv_power_probe(w, PROBE_WORDS);
    for (i = 0, same = n == probe_n; same && i < n; i++)
        same = w[i] == probe[i];
    if (same)
        return;
    for (i = 0; i < n; i++)
        probe[i] = w[i];
    probe_n = n;
    log_printf(OMK_LOG_DEBUG, "power probe: lever %x, hfw %x, ui %08x, keys %02x, %08x, app %x, %s", n > 0 ? w[0] : 0,
               n > 1 ? w[1] : 0, n > 2 ? w[2] : 0, n > 3 ? w[3] : 0, n > 4 ? w[4] : 0, n > 5 ? w[5] : 0,
               n > 6 && w[6] ? "viewfinder" : "screen");
}
#endif

static void handle(u32 s, u32 r, u32 now)
{
    const char *why;

    switch (s) {
    case POWER_OFF:
        why = r == REASON_AT_START ? "power lever before core started" : r == REASON_POLLED ? "power lever flag (polled)" :
              drv_power_reason(r);
        if (why)
            log_printf(OMK_LOG_INFO, "power-off: %s, %u ms after the hook", why, ms_since(since_ticks));
        else
            log_printf(OMK_LOG_INFO, "power-off: reason %u, %u ms after the hook", r, ms_since(since_ticks));
        modules_power(OMK_POWER_OFF);       /* they update their stores in memory: the card-off gate saves them */
        log_close();                        /* no card writes until the card-off gate, which opens it once more */
        break;
    case POWER_SLEEP:
        sleep_ticks = since_ticks;
#ifdef OMK_DEBUG
        sleep_ms = now;
        sleep_rtc = drv_rtc(&sleep_sec, &sleep_frac);
#endif
        log_printf(OMK_LOG_INFO, "sleep: card writes stopped, %u ms after the hook", ms_since(since_ticks));
        modules_power(OMK_POWER_SLEEP);
        log_close();                        /* the card may be away after the sleep */
        break;
    default:
        if (handled == POWER_SLEEP && r != TIMED_OUT) {
            log_printf(OMK_LOG_INFO, "wake: result %x, %u ms by the tick counter since sleep began", r,
                       ((since_ticks - sleep_ticks) & DRV_TICKS_MASK) / 1000);
#ifdef OMK_DEBUG
            correct_clock(now);
#endif
        } else {
            log_printf(OMK_LOG_WARN, "%s did not happen, running again", state_names[handled]);
#ifdef OMK_DEBUG
            cancels++;
#endif
        }
        log_retry_now();                    /* running again: the card may be back, writes that failed are tried at
                                               once rather than after their pause (log.c, store.c) */
        store_retry_now();
        modules_power(handled == POWER_SLEEP ? OMK_POWER_WAKE : OMK_POWER_RESUME);
        break;
    }
    handled = s;
    handled_ms = now;
}

#ifdef OMK_DEBUG
/* The last resort: the watchdog. Nothing more is written: the file task may be the one that hangs */
static void rescue_reset(const char *why)
{
    log_printf(OMK_LOG_ERROR, "power: rescue: %s: reset by the watchdog (cause %08x)", why, RESCUE_CAUSE);
    rescue_stage = R_GOING;                 /* drv_reset does not return on the camera */
    rescue_t = 0;
    drv_reset(RESCUE_CAUSE);
}

/* The lever off: is the application stuck? One passing state all the while, the shutter released (a half-press held keeps
 * the firmware's power-off waiting by design) and no picture being written */
static void rescue_service(u32 now)
{
    u32 app, keys, dt = now - rescue_t;
    int r, rc;

    switch (rescue_stage) {
    case R_WATCH:
        r = drv_ui_probe(&app, &keys);
        if ((r != DRV_UI_HALF && r != DRV_UI_TRANSIENT) || !(keys & DRV_KEYS_S1_UP) || drv_card_busy() ||
            (stuck && app != stuck_app)) {
            stuck = 0;
            return;
        }
        if (!stuck) {
            stuck = 1;
            stuck_since = now;
            stuck_app = app;
        }
        if (now - lever_since < RESCUE_MS || now - stuck_since < RESCUE_MS)
            return;
        log_printf(OMK_LOG_WARN, "power: the lever off %u ms, the application in state %x for %u ms: the camera is stuck; "
                   "rescue - the last save, then the firmware's power-off with a restart", now - lever_since, app,
                   now - stuck_since);
        store_save_all();                   /* as at the card-off gate: the camera is still and the card idle */
        log_flush();
        log_close();
        rescue_posts = 0;
        rescue_stage = R_POST;
        /* fall through */
    case R_POST:
        rescue_offs = power_hook_count(POWER_HOOK_OFF);
        rc = drv_restart_hfw();
        rescue_posts++;
        log_printf(rc ? OMK_LOG_WARN : OMK_LOG_INFO, "power: rescue: the power-off job %s (%d), try %u of %u",
                   rc ? "NOT posted" : "posted", rc, rescue_posts, RESCUE_POSTS);
        rescue_stage = R_OFF;
        rescue_t = now;
        return;
    case R_OFF:
        if (power_hook_count(POWER_HOOK_OFF) != rescue_offs) {
            rescue_stage = R_GOING;
            rescue_t = now;
            return;
        }
        if (dt < RESCUE_OFF_WAIT_MS)
            return;
        if (rescue_posts < RESCUE_POSTS) {
            rescue_stage = R_POST;
            return;
        }
        rescue_reset("the power-off job did not run");
        return;
    case R_GOING:
        if (rescue_t && dt >= RESCUE_DONE_WAIT_MS)
            rescue_reset("the power-off began but the camera stays on");
        return;
    }
}

#endif

/* Every loop of the core task, after the state's own handling: the hold of card writes while the lever is off and the
 * camera runs. It reads the lever's flag only (one word of RAM); the rescue of a stuck camera is a debug build's */
static void lever_service(u32 now)
{
    if (!drv_power_pending()) {
        if (lever_hold)
            log_printf(OMK_LOG_INFO, "power: the lever is on again, card writes again after %u ms without them", now - hold_ms);
        lever_hold = 0;
#ifdef OMK_DEBUG
        lever_off = stuck = 0;
        if (rescue_stage == R_POST)
            rescue_stage = R_WATCH;
#endif
        return;
    }
#ifdef OMK_DEBUG
    if (!lever_off) {
        lever_off = 1;
        lever_since = now;
    }
#endif
    if (!lever_hold && power_running()) {   /* a power-off that did not happen (or never began), the lever still off */
        lever_hold = 1;
        hold_ms = now;
        log_close();
#ifdef OMK_DEBUG
        log_printf(OMK_LOG_WARN, "power: the lever is off but the camera did not go off: no card writes until the lever is on "
                   "again%s", rescue_on ? "; a stuck camera is restarted" : "");
#else
        log_printf(OMK_LOG_WARN, "power: the lever is off but the camera did not go off: no card writes until the lever is on "
                   "again");
#endif
    }
#ifdef OMK_DEBUG
    if (rescue_on)
        rescue_service(now);
#endif
}

int power_service(u32 now)
{
    u32 st, s, n, r, limit;

#ifdef OMK_DEBUG
    log_probe();
#endif
    if (state == POWER_RUN && acked == seq) {   /* fallback for the lever, should the hook miss it */
        if (!drv_power_pending())
            flag_ignored = 0;
        else if (!flag_ignored)
            power_hook(POWER_HOOK_OFF, REASON_POLLED);
    }
    st = drv_lock();
    s = state;
    n = seq;
    r = reason;
    drv_unlock(st);
    if (n != acked) {
        handle(s, r, now);
        acked = n;
    }
    limit = handled == POWER_OFF ? POWER_CANCEL_MS : POWER_SLEEP_ABORT_MS;
    if (handled != POWER_RUN && now - handled_ms >= limit) {
        st = drv_lock();
        if (seq == n)                       /* still the same: power-off cancelled or sleep never entered */
            set_state(POWER_RUN, TIMED_OUT);
        drv_unlock(st);
        if (handled == POWER_OFF && drv_power_pending()) {
            flag_ignored = 1;
            log_printf(OMK_LOG_WARN, "power: lever flag still set, ignored until it clears");
        }
    }
    lever_service(now);
    return power_running();
}

#ifdef OMK_DEBUG
u32 power_hook_count(u32 kind)
{
    return kind < POWER_HOOKS ? hook_count[kind] : 0;
}

u32 power_cancels(void)
{
    return cancels;
}
#endif

int power_running(void)
{
    return state == POWER_RUN && acked == seq;
}

int power_card_ok(void)
{
#ifdef OMK_DEBUG
    if (rescue_stage != R_WATCH)
        return 0;
#endif
    return power_running() && !lever_hold;
}
