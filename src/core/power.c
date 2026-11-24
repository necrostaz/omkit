/* Power states (omk_power.h). Two sides share a few words:
 *  - the driver hooks run in firmware tasks on CPU 0 and call power_hook: they change the state, count a sequence number
 *    at once; the gates then wait (at most POWER_GATE_MS) until the core task acknowledges;
 *  - the core task calls power_service from its loop: on a new sequence number it does the last save
 *    before power-off (the card is still mounted: the card-off gate waits for us) and acknowledges.
 * While the state is not POWER_RUN the core loop does not write to the card. */
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
static volatile u32 hook_ticks[POWER_HOOKS], hook_arg[POWER_HOOKS];
static u32 handled = POWER_RUN, handled_ms, sleep_ticks, sleep_ms, sleep_sec, sleep_frac;
static int sleep_rtc;                       /* 0 - the subprocessor clock was read when sleep began */
static int flag_ignored;                    /* the lever flag stayed 1 although power-off did not happen: ignore until 0 */
static u32 probe[PROBE_WORDS], probe_n;
static u32 cancels;                         /* power-offs and sleeps that did not happen: the task dump watches it */

static const char *const state_names[] = {"running", "power-off", "sleep"};

static u32 ms_since(u32 ticks)
{
    return drv_ticks_since(ticks) / 1000;
}

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
    hook_ticks[kind] = drv_ticks();
    hook_arg[kind] = arg;
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
    log_printf(ok == i ? OMK_LOG_DEBUG : OMK_LOG_WARN, "power: %u of %u hooks installed", ok, i);
    /* After the hooks: the lever flag is set before CameraCommon::shutDown, so nothing slips between check and hooks */
    if (drv_power_pending()) {
        log_printf(OMK_LOG_WARN, "power: power-off already under way at start, modules wait");
        power_hook(POWER_HOOK_OFF, REASON_AT_START);
    }
}

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
        sleep_ms = now;
        sleep_rtc = drv_rtc(&sleep_sec, &sleep_frac);
        log_printf(OMK_LOG_INFO, "sleep: card writes stopped, %u ms after the hook", ms_since(since_ticks));
        modules_power(OMK_POWER_SLEEP);
        log_close();                        /* the card may be away after the sleep */
        break;
    default:
        if (handled == POWER_SLEEP && r != TIMED_OUT) {
            log_printf(OMK_LOG_INFO, "wake: result %x, %u ms by the tick counter since sleep began", r,
                       ((since_ticks - sleep_ticks) & DRV_TICKS_MASK) / 1000);
            correct_clock(now);
        } else {
            log_printf(OMK_LOG_WARN, "%s did not happen, running again", state_names[handled]);
            cancels++;
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

int power_service(u32 now)
{
    u32 st, s, n, r, limit;

    log_probe();
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
    return power_running();
}

u32 power_cancels(void)
{
    return cancels;
}

int power_running(void)
{
    return state == POWER_RUN && acked == seq;
}
