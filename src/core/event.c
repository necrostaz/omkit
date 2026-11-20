/* Event manager: module handlers by phase and priority, invoked from driver hooks, deferred calls. */
#include "omk_core.h"
#include "omk_ev.h"
#include "omk_log.h"
#include "omk_mem.h"
#include "omk_mod.h"

#define HANDLERS_MAX 64
#define DEFER_MAX 32
#define PHASES 5
#define DATA_MAX 64                         /* copy of the data for notifications */
#define STATS_MS 10000

typedef struct {
    omk_mod *mod;
    u32 event, phase;
    int priority, off;
    omk_handler fn;
    void *ctx;
} handler_t;

/* Event invocation: lives on the ev_dispatch stack, the module gets a pointer */
struct omk_ev {
    u32 event, pos, called;
    void *data;
    ev_orig_fn orig;
    void *ctx;
};

static const struct {
    const char *name;
    u32 size;
} defs[OMK_EV_COUNT] = {
    {"", 0},
    {"pictmode_check", sizeof(omk_ev_pictmode_check)},
    {"s1_api", 0},
};

static handler_t handlers[HANDLERS_MAX];
static u32 nhandlers;
/* after ev_activate: event handlers by phase, in call order */
static handler_t *lists[OMK_EV_COUNT][PHASES][HANDLERS_MAX / 4];
static u32 counts[OMK_EV_COUNT][PHASES];
static int active, installed[OMK_EV_COUNT];
static volatile u32 busy[OMK_EV_COUNT], fires[OMK_EV_COUNT], nested[OMK_EV_COUNT];   /* CPU 0 only */
/* busy[e] is taken and given back under drv_lock, with the task that took it: owner[e] is the id of the task whose
 * handlers run (-1 - the driver could not tell). contended[e]: calls from another task meanwhile (see ev_dispatch) */
static volatile int owner[OMK_EV_COUNT];
static volatile u32 contended[OMK_EV_COUNT];
static u32 logged_fires[OMK_EV_COUNT], logged_skips[OMK_EV_COUNT], logged_contended[OMK_EV_COUNT], next_stats;

static struct {
    omk_mod *mod;
    void (*fn)(void *arg);
    void *arg;
} queue[DEFER_MAX];
static u32 q_head, q_tail, q_lost;

static const char *const phase_names[PHASES] = {"notify_before", "filter_before", "replace", "filter_after", "notify_after"};

const char *ev_name(u32 event)
{
    return event && event < OMK_EV_COUNT ? defs[event].name : "?";
}

static int phase_index(u32 phase)
{
    int i;

    for (i = 0; i < PHASES; i++)
        if (phase == (1u << i))
            return i;
    return -1;
}

int ev_register(omk_mod *m, u32 event, u32 phase, int priority, omk_handler fn, void *ctx)
{
    u32 allowed = 0;
    handler_t *h;

    if (!m || m->state != MOD_LOADING || active) {
        log_printf(OMK_LOG_ERROR, "%s: event %u: register only from module_init", m ? m->name : "?", event);
        return -3;
    }
    if (!event || event >= OMK_EV_COUNT || !drv_ev_supported(event, &allowed)) {
        log_printf(OMK_LOG_WARN, "%s: event %u (%s) is not supported by %s", m->name, event, ev_name(event), drv_name());
        return -1;
    }
    if (phase_index(phase) < 0 || !(allowed & phase) || !fn) {
        log_printf(OMK_LOG_WARN, "%s: event %s: phase %x not allowed (allowed %x)", m->name, ev_name(event), phase, allowed);
        return -2;
    }
    if (nhandlers == HANDLERS_MAX) {
        log_printf(OMK_LOG_ERROR, "%s: event %s: no handler slots", m->name, ev_name(event));
        return -4;
    }
    h = &handlers[nhandlers++];
    h->mod = m;
    h->event = event;
    h->phase = phase;
    h->priority = priority;
    h->off = 0;
    h->fn = fn;
    h->ctx = ctx;
    return 0;
}

void ev_drop_module(omk_mod *m)
{
    u32 i, k = 0;

    for (i = 0; i < nhandlers; i++)
        if (handlers[i].mod != m)
            mem_copy(&handlers[k++], &handlers[i], sizeof(handlers[0]));
    nhandlers = k;
}

/* Priority from OMKIT.INI: [NAME] event_<event> = off | number */
static void apply_config(handler_t *h)
{
    static const char prefix[] = "event_";
    char key[40];
    const char *name = ev_name(h->event), *val;
    u32 i;

    for (i = 0; prefix[i]; i++)
        key[i] = prefix[i];
    int prio;

    while (*name && i < sizeof(key) - 1)
        key[i++] = *name++;
    key[i] = 0;
    val = cfg_str(h->mod->name, key, 0);
    if (!val)
        return;
    if (str_ieq(val, "off")) {
        h->off = 1;
        return;
    }
    prio = cfg_int(h->mod->name, key, 0x7fffffff);
    if (prio == 0x7fffffff)
        log_printf(OMK_LOG_WARN, "config: [%s] %s = %s, expected off or a number", h->mod->name, key, val);
    else
        h->priority = prio;
}

/* Sort by priority, ties keep registration order (module load order); then the hooks */
void ev_activate(void)
{
    u32 e, i, k, p, n;
    int rc;

    for (i = 0; i < nhandlers; i++)
        apply_config(&handlers[i]);
    for (e = 1; e < OMK_EV_COUNT; e++) {
        n = 0;
        for (p = 0; p < PHASES; p++) {
            counts[e][p] = 0;
            for (i = 0; i < nhandlers; i++) {
                handler_t *h = &handlers[i];

                if (h->event != e || h->phase != (1u << p) || h->off)
                    continue;
                if (counts[e][p] == HANDLERS_MAX / 4) {
                    log_printf(OMK_LOG_WARN, "event %s: more than %u handlers %s, %s not called", defs[e].name, HANDLERS_MAX / 4,
                               phase_names[p], h->mod->name);
                    continue;
                }
                for (k = counts[e][p]; k && lists[e][p][k - 1]->priority < h->priority; k--)
                    lists[e][p][k] = lists[e][p][k - 1];
                lists[e][p][k] = h;
                counts[e][p]++;
                n++;
            }
            for (k = 0; k < counts[e][p]; k++)
                log_printf(OMK_LOG_INFO, "event %s: %u. %s %s priority %d", defs[e].name, k + 1, lists[e][p][k]->mod->name,
                           phase_names[p], lists[e][p][k]->priority);
        }
        for (i = 0; i < nhandlers; i++)
            if (handlers[i].event == e && handlers[i].off)
                log_printf(OMK_LOG_INFO, "event %s: %s handler off in OMKIT.INI", defs[e].name, handlers[i].mod->name);
        if (!n)
            continue;
        rc = drv_ev_install(e);
        installed[e] = !rc;
        if (rc)
            log_printf(OMK_LOG_ERROR, "event %s: hook not installed, code %d; handlers will not be called", defs[e].name, rc);
        else
            log_printf(OMK_LOG_INFO, "event %s: hook installed, %u handlers", defs[e].name, n);
    }
    active = 1;
}

static void run_phase(omk_ev *ev, u32 p, void *data)
{
    u32 i;

    for (i = 0; i < counts[ev->event][p]; i++) {
        handler_t *h = lists[ev->event][p][i];

        if (p == 0 || p == 4) {             /* notification: own copy, the original data must stay untouched */
            u8 copy[DATA_MAX];

            mem_copy(copy, data, defs[ev->event].size);
            h->fn(ev, copy, h->ctx);
        } else {
            h->fn(ev, data, h->ctx);
        }
    }
}

void ev_next(omk_ev *ev)
{
    u32 e = ev->event;

    if (ev->pos < counts[e][2]) {
        handler_t *h = lists[e][2][ev->pos++];

        h->fn(ev, ev->data, h->ctx);
    } else if (!ev->called) {
        ev->called = 1;
        ev->orig(ev->data, ev->ctx);
    }
}

/* One invocation of an event's handlers at a time (omk_ev lives on this stack, the handlers keep their own state).
 * The hooks call this from firmware tasks of CPU 0, so two things can find the event busy:
 *  - the same task again: a handler, or the original it called, reached the hook once more. The original runs alone -
 *    the usual guard against recursion (counted in nested);
 *  - another task, which preempted the first one inside its handlers. It is not made to wait: the hook sits in a
 *    firmware task that may hold firmware locks or have dispatching disabled, where a sleep fails or blocks what the
 *    first task needs to finish. It gets the original alone too - what the firmware does without OMKit - and the
 *    call is counted apart (contended, a warning in the log): the handlers did not see it.
 * The check and the claim are one step under drv_lock: without it a task switch between them let two tasks run the
 * handlers together, and the first to finish cleared the flag under the other */
int ev_dispatch(u32 event, void *data, ev_orig_fn orig, void *ctx)
{
    omk_ev ev;
    u32 st;
    int me, taken;

    if (!active || event >= OMK_EV_COUNT) {
        orig(data, ctx);
        return 1;
    }
    me = drv_task_current(0);               /* the caller: hooks call handlers on CPU 0 only */
    st = drv_lock();
    taken = !busy[event];
    if (taken) {
        busy[event] = 1;
        owner[event] = me;
        fires[event]++;
    } else if (owner[event] == me) {        /* also when the task is unknown both times: taken as the same one */
        nested[event]++;
    } else {
        contended[event]++;
    }
    drv_unlock(st);
    if (!taken) {
        orig(data, ctx);
        return 1;
    }
    ev.event = event;
    ev.pos = 0;
    ev.called = 0;
    ev.data = data;
    ev.orig = orig;
    ev.ctx = ctx;
    run_phase(&ev, 0, data);
    run_phase(&ev, 1, data);
    ev_next(&ev);                           /* REPLACE chain; with no handlers, the original right away */
    run_phase(&ev, 3, data);
    run_phase(&ev, 4, data);
    busy[event] = 0;                        /* one word, by the owner: no lock needed to give it back */
    return ev.called;
}

int ev_defer(omk_mod *m, void (*fn)(void *arg), void *arg)
{
    u32 st, n;

    if (!fn || drv_cpu() != 0)
        return -1;
    st = drv_lock();
    n = (q_tail + 1) % DEFER_MAX;
    if (n == q_head) {
        q_lost++;
        drv_unlock(st);
        return -1;
    }
    queue[q_tail].mod = m;
    queue[q_tail].fn = fn;
    queue[q_tail].arg = arg;
    q_tail = n;
    drv_unlock(st);
    if (!core_in_task())                    /* the lock is released: the core task runs the call now, not at the end of
                                               its wait (up to 250 ms). Not from the core task itself (a periodic call
                                               that defers): the call runs at its next pass anyway, and a wake at
                                               every pass would turn the loop into a spin */
        drv_wake();
    return 0;
}

/* Deferred calls: only those queued when the pass begins. A call queued meanwhile - by one of these calls or by
 * another task - runs at the next pass (ev_defer wakes the core task for it unless the core task itself deferred; then
 * the pass after the loop's wait, at most 250 ms). Draining until the queue is empty would never end with a call that
 * defers itself again, and the core task would do nothing else: no power acknowledgement, no log, no store */
void ev_service(u32 now)
{
    u32 st, e, lost, left;
    void (*fn)(void *arg);
    void *arg;

    st = drv_lock();
    left = (q_tail + DEFER_MAX - q_head) % DEFER_MAX;
    drv_unlock(st);
    for (; left; left--) {
        st = drv_lock();
        fn = queue[q_head].fn;              /* only this function moves q_head: the entries counted are still there */
        arg = queue[q_head].arg;
        q_head = (q_head + 1) % DEFER_MAX;
        drv_unlock(st);
        fn(arg);
    }
    st = drv_lock();
    lost = q_lost;
    q_lost = 0;
    drv_unlock(st);
    if (lost)
        log_printf(OMK_LOG_WARN, "events: defer queue full, %u calls lost", lost);
    if ((int)(now - next_stats) < 0)
        return;
    next_stats = now + STATS_MS;
    for (e = 1; e < OMK_EV_COUNT; e++) {
        u32 other = drv_ev_other_cpu(e);

        if (installed[e] && (fires[e] != logged_fires[e] || other + nested[e] != logged_skips[e])) {
            logged_fires[e] = fires[e];
            logged_skips[e] = other + nested[e];
            log_printf(OMK_LOG_DEBUG, "event %s: %u calls, %u without handlers on another cpu, %u nested", defs[e].name, fires[e],
                       other, nested[e]);
        }
        if (contended[e] != logged_contended[e]) {
            logged_contended[e] = contended[e];
            log_printf(OMK_LOG_WARN, "event %s: %u calls came from another task while the handlers were running, "
                       "the handlers did not see them", defs[e].name, contended[e]);
        }
    }
}
