/* core: startup in the loader task, config, log, store, service loop. */
#include "omk_api.h"
#include "omk_core.h"
#include "omk_ev.h"
#include "omk_power.h"
#include "omk_prof.h"
#include "omk_isp.h"
#include "omk_look.h"
#include "omk_taskdump.h"
#include "omk_log.h"
#include "omk_store.h"
#include "omk_mod.h"
#include "omk_debug.h"
#include "omk_shoot.h"
#include "omk_handsfree.h"

#define SERVICE_MS 250
#define CORE_STORE_SIZE 256
#define KEY_BOOTS 1                         /* 2 and 3 were used by the reboot loop protection (0.55-0.56, removed) */
#define KEY_ENDED 4                         /* 1 - the last run reached the card-off gate of a power-off */
#define KEY_RUN 5                           /* the last run's name */

#define STACK_FILL 0x57ac57acu
#define STACK_REPORT_MS 10000
#define STACK_LOW 2048                      /* less free than this - warning */

extern u32 __bss_start[], __bss_end[], __stack_bottom[], __stack_top[];

/* core stack: filled with a pattern at startup; the untouched pattern later shows how much of it was used (core.ld, 16 KB) */
static void stack_mark(void)
{
    u32 here, *p;

    for (p = __stack_bottom; (uptr)p < (uptr)&here - 256; p++)
        *p = STACK_FILL;
}

/* 1 - the caller runs in the core task: its stack is core's own (the loader's task, core.ld). No firmware call */
int core_in_task(void)
{
    u32 here;

    return (uptr)&here >= (uptr)__stack_bottom && (uptr)&here < (uptr)__stack_top;
}

static u32 stack_used(void)
{
    u32 *p = __stack_bottom;

    while (p < __stack_top && *p == STACK_FILL)
        p++;
    return (u32)((uptr)__stack_top - (uptr)p);
}

static void stack_service(u32 now)
{
    static u32 last;
    static int warned;
    u32 used, size = (u32)((uptr)__stack_top - (uptr)__stack_bottom);

    if (now - last < STACK_REPORT_MS)
        return;
    last = now;
    used = stack_used();
    log_printf(OMK_LOG_DEBUG, "stack: %u of %u bytes used", used, size);
    if (size - used < STACK_LOW && !warned) {
        warned = 1;
        log_printf(OMK_LOG_WARN, "stack: only %u bytes never used", size - used);
    }
}

static const char *const level_names[] = {"off", "error", "warn", "info", "debug"};
/* run_logs: a log file per power-on, dropped (a file made at every start); still known, so that an old
 * OMKIT.INI gets no warning, and ignored */
static const char *const core_keys[] = {"log", "log_level", "run_logs", "fw_heap", "fw_heap_limit", "core_events",
                                         "fw_cache_inv", "taskdump", "taskdump_every", "taskdump_base", "taskdump_stuck",
                                         "power_rescue"};
static char run_id[9];

static int parse_level(const char *s)
{
    u32 i;

    for (i = 0; i < sizeof(level_names) / sizeof(level_names[0]); i++)
        if (str_ieq(s, level_names[i]))
            return i;
    return -1;
}

/* Power-on name: 8 hex digits from the time counter and the startup timeline, their jitter gives different values */
static void make_run_id(void)
{
    char name[21];
    u32 i, time, h = drv_ticks();

    for (i = 0; drv_boot_event(i, name, &time); i++)
        h = (h << 7 | h >> 25) ^ time;
    for (i = 0; i < 8; i++)
        run_id[i] = "0123456789ABCDEF"[(h >> (28 - 4 * i)) & 15];
    run_id[8] = 0;
}

/* The run's header in CORE.LOG, which holds the runs one after another: the start's date and time by the camera's clock,
 * the build, the run id. The modules' versions follow when they are loaded (modules_load) */
static void log_header(void)
{
    drv_date_t d;
    char when[20];
    u32 v[6], i, k = 0;

    drv_date(&d);
    v[0] = d.year;
    v[1] = d.month;
    v[2] = d.day;
    v[3] = d.hour;
    v[4] = d.minute;
    v[5] = d.second;
    for (i = 0; i < 6; i++) {               /* yyyy-mm-dd hh:mm:ss */
        u32 width = i ? 2 : 4, x = v[i], p = width;

        while (p--) {
            when[k + p] = '0' + x % 10;
            x /= 10;
        }
        k += width;
        when[k++] = i == 2 ? ' ' : i == 5 ? 0 : i < 2 ? '-' : ':';
    }
    log_banner("======== OMKit core %s (%s), %s, run %s, started %s ========", CORE_VERSION, CORE_BUILD, drv_name(), run_id,
               d.year >= 2000 && d.year < 2100 && d.month >= 1 && d.month <= 12 ? when : "?");
}

static void setup_log(int cfg_state)
{
    const char *lvl_text = cfg_str("core", "log_level", "info"), *sec, *key, *val;
    int lvl = parse_level(lvl_text), bad_level = lvl < 0;
    u32 i, k, bad, first;

    if (bad_level)
        lvl = OMK_LOG_INFO;
    log_setup(cfg_bool("core", "log", 1), lvl);
    log_header();
    log_printf(OMK_LOG_INFO, cfg_state > 0 ? "config OMKIT.INI" :
               cfg_state == 0 ? "config: no OMKIT.INI, template written, defaults used" :
                                "config: no OMKIT.INI and cannot write template, defaults used");
    if (bad_level)
        log_printf(OMK_LOG_WARN, "config: log_level '%s' unknown, using info", lvl_text);
    bad = cfg_bad_lines(&first);
    if (bad)
        log_printf(OMK_LOG_WARN, "config: %u lines not understood, first at line %u", bad, first);
    for (i = 0; i < cfg_count(); i++) {
        cfg_entry(i, &sec, &key, &val);
        if (!str_ieq(sec, "core"))
            continue;
        for (k = 0; k < sizeof(core_keys) / sizeof(core_keys[0]) && !str_ieq(key, core_keys[k]); k++)
            ;
        if (k == sizeof(core_keys) / sizeof(core_keys[0]))
            log_printf(OMK_LOG_WARN, "config: [core] %s unknown", key);
    }
}

/* Firmware heap as a reserve for module data: only with fw_heap = on and a limit set */
static void setup_memory(void)
{
    u32 limit = 0;
    int on = cfg_bool("core", "fw_heap", 0), r = cfg_size("core", "fw_heap_limit", &limit);

    if (r < 0)
        log_printf(OMK_LOG_WARN, "config: fw_heap_limit '%s' is not a size", cfg_str("core", "fw_heap_limit", ""));
    if (on && (r <= 0 || !limit)) {
        log_printf(OMK_LOG_WARN, "config: fw_heap = on needs fw_heap_limit, firmware heap stays off");
        on = 0;
    }
    mem_setup(on, limit);
    if (on)
        log_printf(OMK_LOG_INFO, "memory: firmware heap allowed, limit %u", limit);
}

static void log_diagnostics(void)
{
    omk_crash c;
    char name[21];
    u32 i, time, block, total, free, used, largest;

    for (i = 0; i < OMK_CPUS; i++)
        if (drv_crash_read(i, &c))
            log_printf(OMK_LOG_WARN, "NMI record cpu %u: task %u cause %08x regs %08x %08x %08x %08x", i, c.task, c.cause,
                       c.regs[0], c.regs[1], c.regs[4], c.regs[5]);
    if (log_level() < OMK_LOG_DEBUG)
        return;
    mem_arena(&total, &used, &largest);
    log_printf(OMK_LOG_DEBUG, "cpu %u, arena %u of %u used, largest free %u", drv_cpu(), used, total, largest);
    for (i = 0; drv_boot_event(i, name, &time); i++)
        if (str_ieq(name, "FastEndCb") || str_ieq(name, "MainStart"))
            log_printf(OMK_LOG_DEBUG, "boot %s at %u ms", name, time / 1000);
    for (i = 0; drv_fwheap_class(i, &block, &total, &free); i++)
        log_printf(OMK_LOG_DEBUG, "firmware heap %u: %u of %u blocks free", block, free, total);
}

static omk_store *core_store;
static int prev_ended = 1;                 /* count_boot: the previous run ended with a power-off */

/* Boot count; and whether the previous run ended with a power-off - if not, it was a reboot, a crash or the battery */
static void count_boot(omk_store *s)
{
    u32 boots = 0, ended = 1, zero = 0;
    char last[9] = {0};

    core_store = s;
    if (!s) {
        log_printf(OMK_LOG_ERROR, "store CORE: cannot open");
        return;
    }
    store_get(s, KEY_BOOTS, &boots, sizeof(boots));
    store_get(s, KEY_ENDED, &ended, sizeof(ended));
    store_get(s, KEY_RUN, last, 8);
    boots++;
    store_set(s, KEY_BOOTS, &boots, sizeof(boots), OMK_STORE_PERSIST);
    store_set(s, KEY_ENDED, &zero, sizeof(zero), OMK_STORE_PERSIST);
    store_set(s, KEY_RUN, run_id, 8, OMK_STORE_PERSIST);
    store_save_now(s);                      /* "not ended" on the card now: a crash while modules load or hooks go in counts */
    prev_ended = ended != 0;
    log_printf(OMK_LOG_INFO, "boot %u since the store was created", boots);
    if (!ended)
        log_printf(OMK_LOG_WARN, "the previous run %s did not end with a power-off: a reboot, a crash or the battery taken out",
                   last[0] ? last : "?");
}

/* Core's card writes (the log, the stores) wait while the camera is in a passing state of shooting - a half-press, whose
 * exposure sequence stops and starts the live view's exposure stream, or a transition - and UI_SETTLE_MS after it: the
 * firmware itself does no file work then, and a background writer keeps out of the camera's way as it keeps out of a
 * picture being written (drv_card_busy). Bounded: a passing state longer than UI_HOLD_MAX_MS is a stuck camera, and its
 * lines (the task dump) go to the card again */
#define UI_HOLD_MAX_MS 6000
#define UI_SETTLE_MS 500

static int ui_hold(u32 now)
{
    static u32 since, left;
    static int in, after;                   /* in a passing state since `since`; out of it since `left` */
    u32 app, keys;
    int r = drv_ui_probe(&app, &keys);

    if (r == DRV_UI_HALF || r == DRV_UI_TRANSIENT) {
        if (!in) {
            in = 1;
            since = now;
        }
        after = 0;
        return now - since < UI_HOLD_MAX_MS;
    }
    if (in) {
        in = 0;
        after = 1;
        left = now;
    }
    if (after && now - left < UI_SETTLE_MS)
        return 1;
    after = 0;
    return 0;
}

int core_prev_ended(void)
{
    return prev_ended;
}

const char *core_run(void)
{
    return run_id;
}

/* Power-off, at the card-off gate, just before the last save */
void core_mark_ended(void)
{
    u32 one = 1;

    if (core_store)
        store_set(core_store, KEY_ENDED, &one, sizeof(one), OMK_STORE_PERSIST);
    dbg_ended();
}

/* Entry from the loader, in the OMK_LOAD task. Does not return: continues as the low-priority service loop */
void core_main(void)
{
    u32 *p, tick, deferred = 0, deferred_ms = 0;
    int cfg_state, loaded = 1, busy, wait_told = 0, dirs_ok;

    for (p = __bss_start; p < __bss_end; p++)
        *p = 0;
    stack_mark();
    if (drv_check())                        /* another firmware: every address of the driver is wrong there, so no
                                               firmware function is called - no log either (it takes the firmware
                                               lock and opens a file). Back to the loader, silently */
        return;
    omk_ms();
    mem_init();
    make_run_id();
    cfg_state = cfg_load();
    dbg_setup(run_id);                      /* before the log's first line: its RAM ring gets the start too */
    dirs_ok = !core_card_ok() || (!omk_dir(LOG_DIR) & !omk_dir(STORE_DIR));   /* before the first log write and store */
    setup_log(cfg_state);
    if (!dirs_ok)
        log_printf(OMK_LOG_WARN, "cannot make the folders %s and %s", LOG_DIR, STORE_DIR);
    setup_memory();
    log_diagnostics();
    count_boot(store_open("CORE", CORE_STORE_SIZE));
    log_checkpoint("boot: config, log and store ready");
    /* SHOOT.RUN now, not after the modules: a scenario without hands must hold USB before the firmware applies its USB
     * choice - in the start with the cable in that is the card's mount, about when core starts (handsfree.c) */
    hf_setup(shoot_once());
    if (drv_wait_init(cfg_bool("core", "core_events", 1)) > 0)
        log_printf(OMK_LOG_DEBUG, "boot: the core task waits on an event flag");
    else if (cfg_bool("core", "core_events", 1))
        log_printf(OMK_LOG_WARN, "boot: no event flag for the core task, plain sleep");
    log_checkpoint("boot: installing power hooks");
    power_install();
    taskdump_setup(omk_ms());               /* before the modules: its buffer is taken from the arena first */
    dbg_start();                            /* its buffers too; the run before; the USB operations */
#ifdef OMK_EMU
    {
        /* OMK_EMU: a build for running core in an emulator, never for a camera - a check of the thunks to the
         * firmware heap at boot */
        u32 block, total, before, after, back;
        void *p;

        drv_fwheap_class(1, &block, &total, &before);
        p = drv_fwheap_alloc(1000);
        drv_fwheap_class(1, &block, &total, &after);
        if (p)
            drv_fwheap_free(p);
        drv_fwheap_class(1, &block, &total, &back);
        log_printf(OMK_LOG_INFO, "emu: firmware heap %u: block %x, free %u -> %u -> %u", block, (u32)(uptr)p, before, after, back);
    }
#endif
    if (power_running())
        loaded = modules_load();            /* 0 - a power-off began meanwhile: the rest as below */
    else
        loaded = 0;                         /* power-off already under way: load only if it does not happen */
    log_printf(OMK_LOG_INFO, "ready, api %u", omk_api_get()->version);
    if (loaded)
        shoot_setup(omk_ms());              /* the test scenario after the modules: its presets are theirs */
    log_checkpoint("boot: entering the service loop");
    if (power_running())
        log_flush();
    for (tick = SERVICE_MS;;) {
        u32 now;

        drv_wait(tick);                     /* the power hooks cut it short */
        now = omk_ms();
        tick = SERVICE_MS;
        if (drv_wait_state() < 0 && !wait_told) {
            wait_told = 1;
            log_printf(OMK_LOG_WARN, "core: the event flag wait fails or returns at once again and again, plain sleep from now on");
        }
        if (!power_service(now)) {
            shoot_power_lost();             /* keys released, the scenario waits for the camera to shoot again */
            hf_service(now);                /* a restart of its own under way: its stages */
            continue;                       /* power-off or sleep: no card, no module work */
        }
        if (!loaded) {
            loaded = modules_load();
            if (loaded)
                shoot_setup(now);
        }
        prof_service(now);                  /* first: a new profile's live view block is published right after a wake-up by
                                               the profile change, before the store's writes */
        tick = shoot_service(now, SERVICE_MS);   /* sooner while a test scenario runs */
        hf_service(now);
        busy = drv_card_busy() || drv_prof_take_recent() || ui_hold(now) || !power_card_ok();
        if (busy && !deferred)
            deferred_ms = now;
        deferred += busy;
        if (!busy && deferred) {
            log_printf(OMK_LOG_DEBUG, "card: core writes waited %u ms, %u ticks, while the camera used the card, shot or the "
                       "lever was off", now - deferred_ms, deferred);
            deferred = 0;
        }
        if (!busy)
            store_service(now);
        mem_service(now);
        stack_service(now);
        ev_service(now);
        look_service(now);
        isp_service(now);
        grain_service(now);
        modules_tick(now);
        taskdump_service(now);
        dbg_service(now);
        tick = dbg_tick(tick);
        if (!busy)
            log_service(now);
    }
}
