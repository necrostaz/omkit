/* The test scenario: N pictures per step, each step with its preset, without the user ([shoot] in docs/config.md).
 * A queue of steps and a state machine with a time limit on every state. The shutter button is pressed by the button's
 * own firmware path (drv_shutter); every way out of a state that pressed a key - done, a time limit, a stop, power-off -
 * goes through release_all, which releases what this code pressed (never a press of the user's). Waits are not loops: the
 * core loop calls shoot_service at once, and sooner while a scenario runs. */
#include "omk_core.h"
#include "omk_log.h"
#include "omk_prof.h"
#include "omk_shoot.h"

#define SEC "shoot"
#define TICK_MIN 20                         /* ms between two passes: the key task parses each key change first */
#define HALF_SETTLE 300                     /* ms after the half-press state was seen, before the full press */
#define STUCK_MS 3000                       /* half-press state this long with our keys released: release sent again */
#define FAILS_MAX 3                         /* pictures not seen made in a row: the scenario stops */
#define WAIT_MAX 30000                      /* the camera not ready this long at the start of a picture: it stops */
#define NO_COUNTER_MS 1500                  /* without the picture counter: the least time a picture takes */
#define STEP_NAME 24

enum { K_KEEP, K_STOCK, K_LOOK_NUM, K_LOOK_NAME, K_PROF };
typedef struct {
    u8 kind, num;
    char name[STEP_NAME];
} step_t;

enum { S_IDLE, S_WAIT, S_STEP, S_SETTLE, S_READY, S_HALF, S_FULL, S_FULL_UP, S_WRITE, S_INTERVAL };
static const char *const state_names[] = {"idle", "wait", "step", "settle", "ready", "half", "full", "full_up", "write",
                                          "interval"};

static struct {
    step_t steps[SHOOT_STEPS];
    u32 nsteps, shots, interval, settle, half, hold, write, path;
    u32 state, result;                      /* S_*, SHOOT_* */
    u32 t0, delay;                          /* entry time of the state; the start's delay */
    u32 step, shot, taken, failed, fails_row, total;
    u32 takes0, counter, half_seen, half_ms, full_at, write_t, stuck_t, stuck_sent, started;
    u8 down[2];                             /* our presses: half, full */
} s;

static const char *trim(char *b)
{
    char *e;

    while (*b == ' ' || *b == '\t')
        b++;
    for (e = b + str_len(b); e > b && (e[-1] == ' ' || e[-1] == '\t'); e--)
        ;
    *e = 0;
    return b;
}

static int prefix(const char *s1, const char *p)
{
    char a, b;

    for (; *p; s1++, p++) {
        a = *s1 >= 'A' && *s1 <= 'Z' ? *s1 + 32 : *s1;
        b = *p;
        if (a != b)
            return 0;
    }
    return 1;
}

static int number(const char *t, u32 *out)
{
    u32 v = 0;

    if (!*t)
        return 0;
    for (; *t; t++) {
        if (*t < '0' || *t > '9' || v > 1000)
            return 0;
        v = v * 10 + (u32)(*t - '0');
    }
    *out = v;
    return 1;
}

/* One step: keep | stock | look:<n from 1> | look:<entry name> | prof:c<n> | prof:m<n> */
static int parse_step(const char *t, step_t *st)
{
    u32 n, i;

    st->kind = K_KEEP;
    st->num = 0;
    st->name[0] = 0;
    if (str_ieq(t, "keep"))
        return 0;
    if (str_ieq(t, "stock")) {
        st->kind = K_STOCK;
        return 0;
    }
    if (prefix(t, "look:")) {
        t += 5;
        if (number(t, &n)) {
            if (!n || n > OMK_LOOK_MAX)
                return -1;
            st->kind = K_LOOK_NUM;
            st->num = (u8)(n - 1);
            return 0;
        }
        for (i = 0; t[i] && i < STEP_NAME - 1; i++)
            st->name[i] = t[i];
        st->name[i] = 0;
        st->kind = K_LOOK_NAME;
        return t[0] && !t[i] ? 0 : -1;
    }
    if (prefix(t, "prof:") && (prefix(t + 5, "c") || prefix(t + 5, "m")) && number(t + 6, &n) && n >= 1 && n <= 15) {
        st->kind = K_PROF;
        st->num = (u8)(prefix(t + 5, "m") ? OMK_PROF_MONO(n) : OMK_PROF_COLOR(n));
        return 0;
    }
    return -1;
}

int shoot_parse(const char *steps)
{
    char buf[STEP_NAME + 8];
    u32 n = 0, k;

    s.nsteps = 0;
    if (!steps)
        steps = "";
    while (*steps) {
        for (k = 0; *steps && *steps != ',' && k < sizeof(buf) - 1; steps++)
            buf[k++] = *steps;
        buf[k] = 0;
        if (*steps && *steps != ',')        /* a step too long for the buffer */
            return -1;
        if (*steps == ',')
            steps++;
        if (!*trim(buf))
            continue;
        if (n == SHOOT_STEPS || parse_step(trim(buf), &s.steps[n])) {
            log_printf(OMK_LOG_ERROR, "shoot: step %u '%s' not understood (keep, stock, look:<n or name>, prof:c<n> or m<n>; "
                       "at most %u steps)", n + 1, trim(buf), SHOOT_STEPS);
            return -1;
        }
        n++;
    }
    if (!n)
        s.steps[n++].kind = K_KEEP;         /* no steps: one without a change of preset */
    s.nsteps = n;
    return 0;
}

static u32 clamp(int v, u32 lo, u32 hi)
{
    return v < (int)lo ? lo : (u32)v > hi ? hi : (u32)v;
}

static void read_keys(void)
{
    const char *tr = cfg_str(SEC, "trigger", "keys");

    s.shots = clamp(cfg_int(SEC, "shots", 1), 1, SHOOT_SHOTS_MAX);
    s.interval = clamp(cfg_int(SEC, "interval", 3000), 500, 3600000);
    s.settle = clamp(cfg_int(SEC, "settle", 1500), 0, 60000);
    s.half = clamp(cfg_int(SEC, "half", 1500), 100, 10000);
    s.hold = clamp(cfg_int(SEC, "hold", 150), 50, 2000);
    s.write = clamp(cfg_int(SEC, "write", 20000), 2000, 120000);
    s.path = str_ieq(tr, "wifi") ? DRV_SHUTTER_WIFI : DRV_SHUTTER_KEYS;
    if (!str_ieq(tr, "wifi") && !str_ieq(tr, "keys"))
        log_printf(OMK_LOG_WARN, "shoot: trigger '%s' unknown, keys used", tr);
}

static void press(u32 key, int down)
{
    int rc;

    if (s.down[key] == (down != 0))
        return;
    rc = drv_shutter(key, down, s.path);
    s.down[key] = down != 0;
    log_printf(OMK_LOG_DEBUG, "shoot: %s %s (%d)", key == DRV_SHUTTER_HALF ? "half-press" : "full press",
               down ? "down" : "up", rc);
}

/* Every way out: what this code pressed is released, the full press first (as the button: the key task holds the
 * half-press's release until the full one's) */
static void release_all(void)
{
    if (s.down[DRV_SHUTTER_FULL]) {
        press(DRV_SHUTTER_FULL, 0);
        if (s.down[DRV_SHUTTER_HALF])
            drv_sleep(TICK_MIN);            /* the key task (above core) parses the first change before the second */
    }
    press(DRV_SHUTTER_HALF, 0);
}

static void go(u32 st, u32 now)
{
    s.state = st;
    s.t0 = now;
}

static void finish(u32 result, const char *why, u32 now)
{
    release_all();
    s.result = result;
    log_printf(result == SHOOT_DONE ? OMK_LOG_INFO : OMK_LOG_WARN, "shoot: %s: %u of %u pictures made, %u not seen made, %u s",
               why, s.taken, s.total, s.failed, (now - s.started) / 1000);
    go(S_IDLE, now);
}

static u32 takes(void)
{
    drv_prof_info p;

    drv_prof_stats(&p);
    return p.takes;
}

/* The picture counter of the driver grows only with the shooting hook in place: the film look or the profiles */
static int counter_ok(void)
{
    drv_look_info l;

    drv_look_stats(&l);
    return l.count || prof_owned();
}

static int app_state(u32 *app)
{
    u32 keys;

    return drv_ui_probe(app, &keys);
}

static int apply_step(const step_t *st)
{
    int rc = 0, idx;

    switch (st->kind) {
    case K_KEEP:
        log_printf(OMK_LOG_INFO, "shoot: step %u: preset kept", s.step + 1);
        return 0;
    case K_STOCK:
        rc = drv_look_choose(0, 0);
        if (rc == -1)                       /* no look here: nothing to turn off */
            rc = 0;
        if (drv_prof_get() != OMK_PROF_NONE)
            drv_prof_choose(OMK_PROF_NONE);
        log_printf(OMK_LOG_INFO, "shoot: step %u: the camera's own picture mode (%d)", s.step + 1, rc);
        return rc;
    case K_LOOK_NUM:
    case K_LOOK_NAME:
        idx = st->kind == K_LOOK_NUM ? st->num : drv_look_find(st->name);
        if (idx < 0) {
            log_printf(OMK_LOG_ERROR, "shoot: step %u: no look entry '%s'", s.step + 1, st->name);
            return -1;
        }
        rc = drv_look_choose(1, (u32)idx);
        log_printf(rc ? OMK_LOG_ERROR : OMK_LOG_INFO, "shoot: step %u: look entry %u (%d)", s.step + 1, idx + 1, rc);
        return rc;
    case K_PROF:
        if (!prof_owned()) {
            log_printf(OMK_LOG_ERROR, "shoot: step %u: no module owns the profiles", s.step + 1);
            return -1;
        }
        drv_prof_choose(st->num);
        log_printf(OMK_LOG_INFO, "shoot: step %u: profile %s%u", s.step + 1, OMK_PROF_IS_MONO(st->num) ? "m" : "c",
                   OMK_PROF_NUM(st->num));
        return 0;
    }
    return -1;
}

int shoot_start(const char *steps, u32 delay_ms)
{
    u32 now = omk_ms(), i;

    if (s.state != S_IDLE)
        return -2;
    if (shoot_parse(steps ? steps : cfg_str(SEC, "steps", "")))
        return -1;
    read_keys();
    s.total = s.nsteps * s.shots;
    if (s.total > SHOOT_TOTAL_MAX) {
        log_printf(OMK_LOG_ERROR, "shoot: %u pictures asked for, at most %u", s.total, SHOOT_TOTAL_MAX);
        return -1;
    }
    s.step = s.shot = s.taken = s.failed = s.fails_row = 0;
    s.down[0] = s.down[1] = 0;
    s.delay = delay_ms;
    s.started = now;
    s.result = SHOOT_RUNNING;
    log_printf(OMK_LOG_INFO, "shoot: %u steps, %u pictures each, every %u ms, by %s; starts in %u ms", s.nsteps, s.shots,
               s.interval, s.path == DRV_SHUTTER_WIFI ? "wifi events" : "keys", delay_ms);
    for (i = 0; i < s.nsteps; i++)
        if (s.steps[i].kind == K_LOOK_NAME)
            log_printf(OMK_LOG_DEBUG, "shoot: step %u: look '%s'", i + 1, s.steps[i].name);
    go(S_WAIT, now);
    return 0;
}

void shoot_stop(void)
{
    if (s.state != S_IDLE)
        finish(SHOOT_STOPPED, "stopped", omk_ms());
}

void shoot_power_lost(void)
{
    if (s.state != S_IDLE)
        finish(SHOOT_FAILED, "power-off or sleep", omk_ms());
}

void shoot_status(shoot_info *out)
{
    out->state = s.state == S_IDLE ? (s.result == SHOOT_RUNNING ? SHOOT_IDLE : s.result) : SHOOT_RUNNING;
    out->total = s.total;
    out->taken = s.taken;
    out->failed = s.failed;
    out->step = s.step;
    out->steps = s.nsteps;
}

void shoot_setup(u32 now)
{
    const char *run = cfg_str(SEC, "run", "off");
    static const char *const keys[] = {"run", "start_delay", "steps", "shots", "interval", "settle", "trigger", "half",
                                       "hold", "write"};
    const char *sec, *key, *val;
    u32 i, k;

    (void)now;
    for (i = 0; i < cfg_count(); i++) {
        cfg_entry(i, &sec, &key, &val);
        if (!str_ieq(sec, SEC))
            continue;
        for (k = 0; k < sizeof(keys) / sizeof(keys[0]) && !str_ieq(key, keys[k]); k++)
            ;
        if (k == sizeof(keys) / sizeof(keys[0]))
            log_printf(OMK_LOG_WARN, "config: [shoot] %s unknown", key);
    }
    if (str_ieq(run, "boot"))
        shoot_start(0, clamp(cfg_int(SEC, "start_delay", 5000), 0, 600000));
    else if (!str_ieq(run, "off"))
        log_printf(OMK_LOG_WARN, "config: [shoot] run '%s' unknown (off, boot)", run);
}

/* After a picture: the next one, the next step or the end */
static void next(u32 now)
{
    if (++s.shot < s.shots) {
        go(S_INTERVAL, now);
        return;
    }
    s.shot = 0;
    if (++s.step < s.nsteps) {
        go(S_INTERVAL, now);
        return;
    }
    finish(SHOOT_DONE, "done", now);
}

static void picture_failed(const char *why, u32 now)
{
    u32 app = 0;

    app_state(&app);
    release_all();
    s.failed++;
    s.fails_row++;
    log_printf(OMK_LOG_WARN, "shoot: picture %u of step %u: %s (state %s, app %04x)", s.shot + 1, s.step + 1, why,
               state_names[s.state], app);
    if (s.fails_row >= FAILS_MAX)
        finish(SHOOT_FAILED, "stopped after pictures not made in a row", now);
    else
        next(now);
}

u32 shoot_service(u32 now, u32 most)
{
    u32 dt = now - s.t0, app = 0;
    int ui;

    if (s.state == S_IDLE)
        return most;
    ui = app_state(&app);
    switch (s.state) {
    case S_WAIT:                            /* the start's delay */
        if (dt < s.delay)
            return most;
        go(S_STEP, now);
        /* fall through */
    case S_STEP:
        if (s.shot == 0 && apply_step(&s.steps[s.step])) {
            s.failed += s.shots;            /* a step whose preset is not there: its pictures are not taken */
            s.shot = s.shots - 1;
            next(now);
            return TICK_MIN;
        }
        go(s.shot == 0 && s.steps[s.step].kind != K_KEEP ? S_SETTLE : S_READY, now);
        return TICK_MIN;
    case S_SETTLE:                          /* the owner applies the preset at its own tick; the ISP tables once a second */
        if (dt < s.settle)
            return most;
        go(S_READY, now);
        /* fall through */
    case S_READY:                           /* the camera idle in shooting, the card free */
        if (ui != 0 || drv_card_busy() || drv_prof_take_recent()) {
            if (now - s.t0 >= WAIT_MAX)
                finish(SHOOT_FAILED, "the camera not ready for a picture", now);
            return 100;
        }
        s.counter = counter_ok();
        s.takes0 = takes();
        s.half_seen = 0;
        s.stuck_sent = 0;
        log_printf(OMK_LOG_INFO, "shoot: picture %u of step %u (%u of %u), app %04x", s.shot + 1, s.step + 1,
                   s.taken + s.failed + 1, s.total, app);
        press(DRV_SHUTTER_HALF, 1);
        go(S_HALF, now);
        return TICK_MIN;
    case S_HALF:                            /* focus: the half-press state, or the time limit - then the full press anyway */
        if (ui == DRV_UI_HALF && !s.half_seen) {
            s.half_seen = 1;
            s.half_ms = dt;
            s.full_at = now + HALF_SETTLE;
            log_printf(OMK_LOG_DEBUG, "shoot: half-press state after %u ms", dt);
        }
        if (s.half_seen ? (s32)(now - s.full_at) < 0 : dt < s.half)
            return TICK_MIN;
        if (!s.half_seen)
            log_printf(OMK_LOG_WARN, "shoot: no half-press state in %u ms (app %04x): full press anyway", s.half, app);
        press(DRV_SHUTTER_FULL, 1);
        go(S_FULL, now);
        return TICK_MIN;
    case S_FULL:
        if (dt < s.hold)
            return TICK_MIN;
        press(DRV_SHUTTER_FULL, 0);
        go(S_FULL_UP, now);
        return TICK_MIN;
    case S_FULL_UP:
        press(DRV_SHUTTER_HALF, 0);
        s.write_t = now;
        s.stuck_t = 0;
        go(S_WRITE, now);
        return TICK_MIN;
    case S_WRITE: {                         /* made: the counter grew (or the least time passed), the camera idle again */
        int made = s.counter ? takes() != s.takes0 : dt >= NO_COUNTER_MS;

        if (ui == DRV_UI_HALF) {            /* the watchdog: half-press with our keys released */
            if (!s.stuck_t)
                s.stuck_t = now;
            else if (now - s.stuck_t >= STUCK_MS && !s.stuck_sent) {
                s.stuck_sent = 1;
                log_printf(OMK_LOG_WARN, "shoot: half-press state for %u ms after the release: released again",
                           now - s.stuck_t);
                s.down[DRV_SHUTTER_HALF] = 1;   /* so that press() sends it */
                press(DRV_SHUTTER_HALF, 0);
            }
        } else
            s.stuck_t = 0;
        if (made && ui == 0 && !drv_card_busy()) {
            s.taken++;
            s.fails_row = 0;
            log_printf(OMK_LOG_INFO, "shoot: picture made: half-press %s %u ms, written %u ms after the release, app %04x, "
                       "counter %u", s.half_seen ? "in" : "not seen,", s.half_seen ? s.half_ms : s.half, now - s.write_t,
                       app, s.counter ? takes() : 0);
            next(now);
            return TICK_MIN;
        }
        if (dt >= s.write)
            picture_failed(s.counter && !made ? "no picture in time" : "the camera did not come back in time", now);
        return 50;
    }
    case S_INTERVAL:
        if (dt < s.interval)
            return dt + most < s.interval ? most : s.interval - dt;
        go(S_STEP, now);
        return TICK_MIN;
    }
    return most;
}
