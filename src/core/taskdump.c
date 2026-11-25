/* Task dump: when the camera stops answering, which firmware tasks wait and on what. 
 * The usual tool is a dump of the RTOS task table taken by a watchdog or a debugger. Here the core task is the one that still
 * runs in such a hang (CPU 0, the lowest priority), so it takes the dump itself:
 *  - the snapshot: every task of the three kernels at once into a buffer from the arena (drv_task_read: read only, no locks,
 *    nothing called in the firmware), then the objects the tasks wait for and the mutexes held;
 *  - the log: a few lines per service tick and only while the log ring has room, so a dump never pushes other lines out of
 *    the ring; a dump of 150 tasks is about 15 KB and goes out in 2-4 s;
 *  - triggers: a power-off or sleep that did not happen (power_cancels), the application stuck in a state that lasts a moment
 *    in normal work, every taskdump_every s, once at taskdump_base s for comparison. At most TD_DUMPS per power-on. */
#include "omk_core.h"
#include "omk_log.h"
#include "omk_power.h"
#include "omk_taskdump.h"

#define TD_TASKS 320                        /* tasks kept per snapshot, dormant ones are only counted; the kernels allow 447 */
#define TD_OBJS 64                          /* objects waited for plus mutexes held */
#define TD_DUMPS 8                          /* per power-on: then one line that no more are made */
#define TD_GAP_MS 20000                     /* automatic triggers closer than this make one dump */
#define TD_EVERY_MIN_MS 10000
#define TD_LINES_PER_TICK 12
#define TD_ROOM 8192                        /* write only while the 32 KB log ring has this much free; a line is < 192 bytes */
#define TD_STUCK_SLOW 4                     /* half-press with no key change: fire only after 4 x taskdump_stuck */
#define TD_LINE 176                         /* the log adds 11 characters and cuts at 189 */

enum { WHY_NONE, WHY_POWER, WHY_STUCK, WHY_EVERY, WHY_BASE };
static const char *const why_names[] = {"", "power-off or sleep did not happen", "application stuck", "periodic", "baseline"};

static drv_task *tasks;                     /* arena: TD_TASKS records */
static drv_obj objs[TD_OBJS];
static u32 n_tasks, n_objs, n_held, n_dropped, snap_us, cpu_all[OMK_CPUS], cpu_dmt[OMK_CPUS], cpu_bad[OMK_CPUS], bad_ptr;
static int cpu_run[OMK_CPUS];
static u32 dumps, pending, reason, emitting, pos, last_auto, last_auto_ms, cancels_seen;
static u32 every_ms, base_ms, stuck_ms, next_every;
static int enabled, base_done;
static u32 ui_app, ui_keys, ui_since, ui_changes, ui_fired, ui_seen, ui_fired_ms;

/* ---- a line built piece by piece: log_printf has no field widths for strings ---- */
typedef struct {
    char *p, *end;
} line_t;

static void cat(line_t *l, const char *s)
{
    while (*s && l->p < l->end)
        *l->p++ = *s++;
}

static void cat_num(line_t *l, u32 v, u32 base, int width)
{
    char tmp[11];
    int n = 0;

    do {
        tmp[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (width-- > n && l->p < l->end)
        *l->p++ = '0';
    while (n && l->p < l->end)
        *l->p++ = tmp[--n];
}

static void cat_hex(line_t *l, const char *label, u32 v, int width)
{
    cat(l, label);
    cat_num(l, v, 16, width);
}

static void cat_dec(line_t *l, const char *label, u32 v)
{
    cat(l, label);
    cat_num(l, v, 10, 0);
}

static const char *state_name(u32 s)
{
    switch (s) {
    case DRV_TS_RUN:
        return "RUN";
    case DRV_TS_RDY:
        return "RDY";
    case DRV_TS_WAI:
        return "WAI";
    case DRV_TS_SUS:
        return "SUS";
    case DRV_TS_WAI | DRV_TS_SUS:
        return "WAS";
    case DRV_TS_DMT:
        return "DMT";
    }
    return "?";
}

static const char *wait_name(u32 w)
{
    static const char *const names[] = {"SLP", "DLY", "SEM", "FLG", "SDTQ", "RDTQ", "MBX", "MTX", "SMBF", "RMBF", "CAL",
                                        "ACP", "RDV", "MPF", "MPL", "W15"};
    u32 i;

    for (i = 0; i < 16 && !(w >> i & 1); i++)
        ;
    return i < 16 ? names[i] : "W?";
}

/* ---- snapshot ---- */
static void add_obj(u32 kind, u32 id)
{
    u32 k;
    int r;

    for (k = 0; k < n_objs; k++)
        if (objs[k].id == id && objs[k].kind == kind)
            return;
    if (n_objs >= TD_OBJS)
        return;
    r = drv_obj_read(kind, id, &objs[n_objs]);
    if (r == -1)
        return;                             /* a kind the driver does not read: the task lines say enough */
    if (r == -2)
        objs[n_objs].atr = 0xffff;          /* no such object any more */
    n_objs++;
}

static void snapshot(void)
{
    drv_task spare, *t;
    drv_obj o;
    u32 cpu, slot, i, k, t0 = drv_ticks();
    int r;

    n_tasks = n_objs = n_held = n_dropped = 0;
    for (cpu = 0; cpu < OMK_CPUS; cpu++) {
        cpu_all[cpu] = cpu_dmt[cpu] = cpu_bad[cpu] = 0;
        cpu_run[cpu] = drv_task_current(cpu);
        for (slot = 0;; slot++) {
            t = n_tasks < TD_TASKS ? &tasks[n_tasks] : &spare;
            r = drv_task_read(cpu, slot, t);
            if (r < 0)
                break;
            if (!r)
                continue;
            if (r == 2) {
                if (!cpu_bad[cpu]++)
                    bad_ptr = t->sp;
                continue;
            }
            cpu_all[cpu]++;
            if (t->state == DRV_TS_DMT)
                cpu_dmt[cpu]++;
            else if (t == &spare)
                n_dropped++;
            else
                n_tasks++;
        }
    }
    for (i = 0; i < n_tasks; i++)
        if (tasks[i].wobj)
            add_obj(tasks[i].wait, tasks[i].wobj);
    for (cpu = 0; cpu < OMK_CPUS; cpu++)
        for (i = 0; (r = drv_mutex_read(cpu, i, &o)) >= 0 && n_objs < TD_OBJS; i++) {
            if (r <= 0 || !o.owner)
                continue;
            for (k = 0; k < n_objs && !(objs[k].id == o.id && objs[k].kind == o.kind); k++)
                ;
            if (k == n_objs)
                objs[n_objs++] = o;
            n_held++;
        }
    snap_us = drv_ticks_since(t0);
}

/* ---- lines ---- */
/* taskdump 2 (application stuck): app 5101 keys 14, snapshot 41 ms
 * taskdump 2: cpu0 170 tasks 57 dormant run 009f, cpu1 75 40 run 1023, cpu2 36 4 run 2005; kept 140 dropped 0 */
static void header(void)
{
    u32 app = 0, keys = 0, cpu;
    char text[TD_LINE];
    line_t l = {text, text + sizeof(text) - 1};

    drv_ui_probe(&app, &keys);
    log_printf(reason == WHY_POWER || reason == WHY_STUCK ? OMK_LOG_WARN : OMK_LOG_INFO,
               "taskdump %u (%s): app %x keys %02x, snapshot %u ms", dumps, why_names[reason], app, keys, snap_us / 1000);
    cat_dec(&l, "taskdump ", dumps);
    for (cpu = 0; cpu < OMK_CPUS; cpu++) {
        cat_dec(&l, cpu ? ", cpu" : ": cpu", cpu);
        cat_dec(&l, " ", cpu_all[cpu]);
        cat_dec(&l, " tasks ", cpu_dmt[cpu]);
        cat(&l, " dormant run ");
        if (cpu_run[cpu] < 0)
            cat(&l, "?");
        else
            cat_num(&l, (u32)cpu_run[cpu], 16, 4);
        if (cpu_bad[cpu])
            cat_dec(&l, " bad ", cpu_bad[cpu]);
    }
    cat_dec(&l, "; kept ", n_tasks);
    cat_dec(&l, " dropped ", n_dropped);
    cat_dec(&l, ", objects ", n_objs);
    cat_dec(&l, " mutexes held ", n_held);
    *l.p = 0;
    log_printf(OMK_LOG_INFO, "%s", text);
    for (cpu = 0; cpu < OMK_CPUS && !cpu_bad[cpu]; cpu++)
        ;
    if (cpu < OMK_CPUS)
        log_printf(OMK_LOG_WARN, "taskdump %u: TCB pointers outside RAM were not followed, the first %08x", dumps, bad_ptr);
}

/* td c0 00a7 GUI_TASK WAI FLG 013d ptn 00000001/1 p55/55 sp 704/16384 never 8244 r 6086683f 608a235d ... */
static void task_line(const drv_task *t)
{
    char text[TD_LINE];
    line_t l = {text, text + sizeof(text) - 1};
    u32 i;

    cat_dec(&l, "td c", t->cpu);
    cat_hex(&l, " ", t->id, 4);
    cat(&l, " ");
    cat(&l, t->name);
    cat(&l, " ");
    cat(&l, state_name(t->state));
    if (t->state & DRV_TS_WAI) {
        cat(&l, " ");
        cat(&l, wait_name(t->wait));
        if (t->wobj)
            cat_hex(&l, " ", t->wobj, 4);
        if (t->wait == DRV_TW_FLG) {
            cat_hex(&l, " ptn ", t->wptn, 8);
            cat_dec(&l, "/", t->wmode);
        }
        if (t->tmo)
            cat_dec(&l, " tmo ", t->tmo - 1);
    }
    cat_dec(&l, " p", t->pri);
    cat_dec(&l, "/", t->bpri);
    if (t->wup)
        cat_dec(&l, " wup ", t->wup);
    if (t->sus)
        cat_dec(&l, " sus ", t->sus);
    if (!(t->flags & DRV_TF_BADSTACK) && t->state != DRV_TS_RUN)
        cat_dec(&l, " sp ", t->stk + t->stksz - t->sp);
    else
        cat(&l, " sp -");
    cat_dec(&l, "/", t->stksz);
    if (t->flags & DRV_TF_FILL)
        cat_dec(&l, " never ", t->never);
    if (t->flags & DRV_TF_UNSTABLE)
        cat(&l, " unstable");
    if (t->state != DRV_TS_RUN && !(t->flags & (DRV_TF_CTX | DRV_TF_BADSTACK)))
        cat(&l, " noctx");
    if (t->flags & DRV_TF_BADSTACK)
        cat_hex(&l, " badstack ", t->stk, 8);
    if (t->ret[0])
        cat(&l, " r");
    for (i = 0; i < DRV_TASK_RETS && t->ret[i]; i++)
        cat_hex(&l, " ", t->ret[i], 8);
    if (!t->ret[0] && t->state != DRV_TS_RUN)
        cat_hex(&l, " e ", t->entry, 8);
    *l.p = 0;
    log_printf(OMK_LOG_INFO, "%s", text);
}

/* td obj MTX 0004 owner 00a7 atr 2 ceil 0 waiters 1 */
static void obj_line(const drv_obj *o)
{
    char text[TD_LINE];
    line_t l = {text, text + sizeof(text) - 1};
    u32 i, waiters = 0;

    for (i = 0; i < n_tasks; i++)
        waiters += tasks[i].wobj == o->id && tasks[i].wait == o->kind;
    cat(&l, "td obj ");
    cat(&l, wait_name(o->kind));
    cat_hex(&l, " ", o->id, 4);
    if (o->atr == 0xffff)
        cat(&l, " gone");
    else if (o->kind == DRV_TW_SEM) {
        cat_dec(&l, " cnt ", o->a);
        cat_dec(&l, "/", o->b);
    } else if (o->kind == DRV_TW_FLG)
        cat_hex(&l, " ptn ", o->a, 8);
    else if (o->kind == DRV_TW_MTX) {
        cat_hex(&l, " owner ", o->owner, 4);
        cat_dec(&l, " ceil ", o->a);
    }
    if (o->atr != 0xffff)
        cat_hex(&l, " atr ", o->atr, 0);
    cat_dec(&l, " waiters ", waiters);
    *l.p = 0;
    log_printf(OMK_LOG_INFO, "%s", text);
}

static void emit(void)
{
    u32 lines;

    for (lines = 0; emitting && lines < TD_LINES_PER_TICK && log_room() >= TD_ROOM; lines++, pos++) {
        if (!pos)
            header();
        else if (pos <= n_tasks)
            task_line(&tasks[pos - 1]);
        else if (pos <= n_tasks + n_objs)
            obj_line(&objs[pos - 1 - n_tasks]);
        else {
            log_printf(OMK_LOG_INFO, "taskdump %u end", dumps);
            emitting = 0;
        }
    }
}

/* ---- triggers ---- */
static void start(u32 now)
{
    u32 why = pending;
    int automatic = why == WHY_POWER || why == WHY_STUCK;

    pending = 0;
    if (log_level() < OMK_LOG_INFO)
        return;
    if (dumps >= TD_DUMPS) {
        if (dumps++ == TD_DUMPS)
            log_printf(OMK_LOG_WARN, "taskdump: %u dumps made this power-on, no more", TD_DUMPS);
        return;
    }
    if (automatic && last_auto && now - last_auto_ms < TD_GAP_MS) {
        log_printf(OMK_LOG_INFO, "taskdump: %s, the last dump was %u ms ago, none now", why_names[why], now - last_auto_ms);
        return;
    }
    if (automatic) {
        last_auto = 1;
        last_auto_ms = now;
    }
    dumps++;
    reason = why;
    snapshot();
    pos = 0;
    emitting = 1;
}

static void request(u32 why)
{
    if (!pending || why < pending)          /* the more serious reason wins: power, stuck, periodic, baseline */
        pending = why;
}

/* The application stays in a state that lasts a moment in normal work: a transition, or half-press after a key changed (the
 * user let go or pressed again) - or for a long time with no key change at all. Once per stay in that state */
static void ui_check(u32 now)
{
    u32 app, keys, dur;
    int r = drv_ui_probe(&app, &keys);

    if (r < 0)
        return;
    if (!ui_seen || app != ui_app) {
        ui_seen = 1;
        ui_app = app;
        ui_keys = keys;
        ui_since = now;
        ui_changes = 0;
        ui_fired = 0;
        return;
    }
    if (keys != ui_keys) {
        ui_keys = keys;
        ui_changes++;
    }
    dur = now - ui_since;
    if (ui_fired || !stuck_ms || !r || dur < stuck_ms)
        return;
    if (r == DRV_UI_TRANSIENT || ui_changes || dur >= TD_STUCK_SLOW * stuck_ms) {
        ui_fired = 1;
        ui_fired_ms = now | 1;              /* 0 is "not stuck" */
        log_printf(OMK_LOG_WARN, "taskdump: application state %x for %u ms, keys changed %u times", app, dur, ui_changes);
        request(WHY_STUCK);
    }
}

static u32 seconds(const char *key, int def, u32 max)
{
    int v = cfg_int("core", key, def);

    if (v < 0 || (u32)v > max) {
        log_printf(OMK_LOG_WARN, "config: [core] %s = %d out of 0..%u, using %d", key, v, max, def);
        v = def;
    }
    return (u32)v * 1000;
}

void taskdump_setup(u32 now)
{
    enabled = cfg_bool("core", "taskdump", 1);
    every_ms = seconds("taskdump_every", 0, 3600);
    base_ms = seconds("taskdump_base", 0, 3600);
    stuck_ms = seconds("taskdump_stuck", 6, 600);
    if (!enabled)
        return;
    if (every_ms && every_ms < TD_EVERY_MIN_MS)
        every_ms = TD_EVERY_MIN_MS;
    tasks = mem_alloc(TD_TASKS * sizeof(drv_task));
    if (!tasks) {
        log_printf(OMK_LOG_WARN, "taskdump: no memory for %u tasks, task dumps off", TD_TASKS);
        enabled = 0;
        return;
    }
    next_every = now + every_ms;
    cancels_seen = power_cancels();
    log_printf(OMK_LOG_DEBUG, "taskdump: on, every %u s, baseline at %u s, stuck after %u s, buffer %u bytes", every_ms / 1000,
               base_ms / 1000, stuck_ms / 1000, TD_TASKS * (u32)sizeof(drv_task));
}

u32 taskdump_stuck_since(void)
{
    return enabled && ui_seen && ui_fired ? ui_fired_ms : 0;
}

void taskdump_service(u32 now)
{
    u32 c;

    if (!enabled)
        return;
    c = power_cancels();
    if (c != cancels_seen) {
        cancels_seen = c;
        request(WHY_POWER);
    }
    ui_check(now);
    if (every_ms && (int)(now - next_every) >= 0) {
        next_every = now + every_ms;
        request(WHY_EVERY);
    }
    if (base_ms && !base_done && now >= base_ms) {
        base_done = 1;
        request(WHY_BASE);
    }
    if (!emitting && pending)
        start(now);
    if (emitting)
        emit();
}
