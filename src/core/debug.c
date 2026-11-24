/* The debug service: what happened before a reset, and the camera's state now, read over USB.
 *
 *  - The log and state snapshots go to a RAM ring that outlives a reset (dring.c, the driver's drv_keep_area): two banks,
 *    this power-on and the one before. Lines come from the log's tee (log.c) under the firmware lock; snapshots are made by
 *    the core task: the file tasks and the application state before and after every write of core to the card, in a
 *    window after it, at the edges of the camera's own card work, while the application stays in a passing state, and
 *    on request.
 *  - Three PTP vendor operations (driver ptp.c): commands without data, data to the computer, data from it. Their handlers
 *    run in the firmware's PTP task: they copy at most DBG_STAGE bytes into a buffer of their own and send it, or take the
 *    computer's data into the job buffer, and never wait. Card work is a job the core task does; the computer asks for
 *    its state (STATUS) and then for its result (GET XFER).
 *  - [debug] card_writes = off: neither the log nor the stores are written to the card (they stay in RAM).
 *  - [debug] write = on: files of the OMKit folder written over USB (a temporary file, its CRC-32 read back from the card,
 *    then the rename), the restart, and the test scenario started or stopped (core/shoot.c).
 * Everything here is core's cached data of CPU 0: the core task and the PTP task are both tasks of CPU 0; the state they
 * share changes under drv_lock. */
#include "omk_core.h"
#include "omk_debug.h"
#include "omk_dring.h"
#include "omk_log.h"
#include "omk_power.h"
#include "omk_shoot.h"
#include "omk_store.h"

#define WATCH_MAX 12
#define RESOLVE_GAP_MS 30000                /* tasks not found are looked for again after this, ... */
#define RESOLVE_TRIES 10                    /* ... this many times: a full walk of the tasks takes tens of ms */
#define STUCK_EVERY_MS 500
#define STUCK_SNAPS 20                      /* snapshots per stay in a passing state */
#define STUCK_MEM_MAX 6                     /* [debug] stuck_mem: ranges logged when the application gets stuck ... */
#define STUCK_MEM_LEN 256                   /* ... each at most this long */
#define WRITE_IDLE_MS 30000                 /* an open file with no data for this long is dropped */
#define LIST_SCAN_MAX 1024                  /* entries a LIST job walks (findnext reads the folder from its start) */
#define SNAP_OBJS 64
#define TMP_NAME "OMKDBG.TMP"
#define ALIGN_PTP 128                       /* our PTP buffers: the firmware's own sit on 128-byte steps (0x9702) */
#define RESTART_GRACE_MS 500                /* RESTART: the job's end reaches the computer (it asks every 20 ms) before the
                                               camera leaves USB */
#define RESTART_TAKEN_MS 5000               /* no power-off this long after the request: the firmware did not take it */

enum { T_CARD_BEGIN = 1, T_CARD_END, T_WINDOW, T_CAM_BUSY, T_CAM_IDLE, T_STUCK, T_MARK, T_BOOT, T_JOB, T_EVENT };
enum { J_NONE, J_SNAP, J_READ, J_LIST, J_WBEGIN, J_WDATA, J_WCOMMIT, J_WABORT, J_RESTART, J_SHOOT };

typedef struct {
    char name[DRV_TASK_NAME];
    u16 id, slot;
    u8 cpu, found;
} watch_t;

static int on, write_on, card_on = 1, ptp_on, prev_valid;
static int ring_level = OMK_LOG_DEBUG;
static dring ring;
static dring_bank *cur;                     /* 0: no ring */
static u8 *stage, *xfer;                    /* arena, ALIGN_PTP */
static u32 window_ms = 1000, window_every = 50, stuck_ms = 1000;
static u32 window_until, window_last, cam_busy, writing;
static u32 ui_app, ui_since, ui_snaps, ui_last;
static int ui_seen;
static watch_t watch[WATCH_MAX];
static u32 n_watch, resolve_ms, resolve_tries, core_id;
static u8 snap_buf[24 + 20 * WATCH_MAX], snap_prev[24 + 20 * WATCH_MAX];
static u32 snap_same, snap_prev_len;
static u32 stuck_mem[STUCK_MEM_MAX][2], n_stuck_mem;   /* [debug] stuck_mem: address, length */

/* The job: started by a handler (state QUEUED), done by the core task */
static struct {
    volatile u32 state;
    u32 kind, seq, arg[3], result, bytes, crc, extra;
} job;
static char path[DBG_PATH_MAX + 1];         /* from PUT PATH */
static char text_arg[DBG_TEXT_MAX + 1];     /* from PUT TEXT: the steps of the next SHOOT START */
static char run_name[9];
static char wtarget[OMK_PATH_MAX + 8], wtmp[OMK_PATH_MAX + 8];
static void *wfp;
static u32 wsize, wdone, wcrc, wcrc_want, wlast;
static volatile u32 level_req, mark_req, mark_tag;
static u32 restart_state, restart_t;        /* 0; 1 the request is due at restart_t; 2 posted at restart_t */
#ifdef OMK_EMU
static u32 emu_restart_ms;                  /* the emulator has no USB: [debug] emu_restart starts the job at this time */
#endif

int core_card_ok(void)
{
    return card_on;
}

/* ---- the ring ---- */
static void put(u32 type, const void *a, u32 na, const void *b, u32 nb)
{
    u32 st, ms = omk_ms();                  /* before the lock: omk_ms takes it too */

    if (!cur)
        return;
    st = drv_lock();
    dring_put(cur, type, ms, a, na, b, nb);
    drv_unlock(st);
}

/* The log's tee: the firmware lock is held (no omk_ms here: it takes the lock) */
static void tee(int lvl, u32 ms, const char *text, u32 len)
{
    u8 l = (u8)lvl;

    if (cur)
        dring_put(cur, DR_LOG, ms, &l, 1, text, len);
}

static void mark(const char *what, const char *arg)
{
    char text[96];
    u32 n = 0;

    while (*what && n < sizeof(text) - 2)
        text[n++] = *what++;
    if (arg) {
        text[n++] = ' ';
        while (*arg && n < sizeof(text))
            text[n++] = *arg++;
    }
    put(DR_MARK, text, n, 0, 0);
}

/* ---- snapshots ---- */
static void st16(u8 *p, u32 v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static void st32(u8 *p, u32 v)
{
    st16(p, v);
    st16(p + 2, v >> 16);
}

static int name_is(const char *a, const char *b)
{
    u32 i;

    for (i = 0; i < DRV_TASK_NAME - 1 && a[i] == b[i]; i++)
        if (!a[i])
            return 1;
    return i == DRV_TASK_NAME - 1;
}

/* Every task of the three CPUs once: the slots of the watched names. The core task is found by its id */
static void resolve(u32 now)
{
    drv_task t;
    u32 cpu, slot, i, found = 0;
    u8 map[WATCH_MAX * 24];
    int r;

    resolve_ms = now;
    resolve_tries++;
    for (cpu = 0; cpu < OMK_CPUS; cpu++)
        for (slot = 0; (r = drv_task_read(cpu, slot, &t)) >= 0 && slot < 1024; slot++) {
            if (r != 1 || t.state == DRV_TS_DMT)
                continue;
            for (i = 0; i < n_watch; i++)
                if (!watch[i].found && (watch[i].name[0] ? name_is(watch[i].name, t.name) : cpu == 0 && t.id == core_id)) {
                    watch[i].found = 1;
                    watch[i].id = t.id;
                    watch[i].cpu = cpu;
                    watch[i].slot = slot;
                    if (!watch[i].name[0])
                        mem_copy(watch[i].name, t.name, DRV_TASK_NAME);
                }
        }
    for (i = 0; i < n_watch; i++) {
        st16(map + 24 * found, watch[i].found ? watch[i].id : 0);
        st16(map + 24 * found + 2, watch[i].cpu);
        mem_copy(map + 24 * found + 4, watch[i].name, DRV_TASK_NAME);
        found++;
    }
    put(DR_MAP, map, 24 * found, 0, 0);
}

static u32 bit_index(u32 w)
{
    u32 i;

    for (i = 0; i < 16; i++)
        if (w >> i & 1)
            return i + 1;
    return 0;
}

/* {u16 trigger, u8 tasks, u8 flags, u32 app, u32 keys, u32 same, u32 card, u32 ms} + 20 bytes a task */
static void snapshot(u32 trig, int dedupe)
{
    drv_task t;
    u32 i, n = 0, app = 0, keys = 0, len, st, ms;
    u8 *e;
    int r = drv_ui_probe(&app, &keys);

    if (!cur || !core_in_task())            /* the task reads and the buffer: the core task only */
        return;
    for (i = 0; i < n_watch; i++) {
        e = snap_buf + 24 + 20 * n;
        mem_set(e, 0, 20);
        if (!watch[i].found)
            continue;
        n++;
        st16(e, watch[i].id);
        if (drv_task_read(watch[i].cpu, watch[i].slot, &t) != 1 || t.id != watch[i].id) {
            watch[i].found = 0;             /* gone: looked for again later */
            e[4] = 0xff;
            continue;
        }
        st16(e + 2, t.wobj);
        e[4] = t.state;
        e[5] = bit_index(t.wait);
        e[6] = t.pri;
        e[7] = t.flags;
        st32(e + 8, t.tmo);
        st32(e + 12, t.ret[0]);
        st32(e + 16, t.ret[1]);
    }
    st16(snap_buf, trig);
    snap_buf[2] = n;
    snap_buf[3] = r < 0 ? 0xff : r;
    st32(snap_buf + 4, app);
    st32(snap_buf + 8, keys);
    st32(snap_buf + 16, (u32)drv_card_busy() | writing << 1);
    len = 24 + 20 * n;
    if (dedupe && len == snap_prev_len) {   /* the same as the last one but for the time and the trigger */
        for (i = 2; i < len; i++)           /* not the trigger, the repeat count (12) and the time (20) */
            if ((i < 12 || i >= 16) && (i < 20 || i >= 24) && snap_buf[i] != snap_prev[i])
                break;
        if (i == len) {
            snap_same++;
            return;
        }
    }
    ms = omk_ms();                          /* before the lock: omk_ms takes it too */
    st32(snap_buf + 12, snap_same);
    st32(snap_buf + 20, ms);
    snap_same = 0;
    mem_copy(snap_prev, snap_buf, len);
    snap_prev_len = len;
    st = drv_lock();
    dring_put(cur, DR_SNAP, ms, snap_buf, len, 0, 0);
    cur->h[DH_SNAP_MS] = ms;
    drv_unlock(st);
}

void core_card_write(int begin, const char *what)
{
    if (!on || !cur)
        return;
    writing = begin;
    mark(begin ? "card+" : "card-", what);
    snapshot(begin ? T_CARD_BEGIN : T_CARD_END, 0);
    if (!begin && window_ms) {
        window_until = omk_ms() + window_ms;
        window_last = 0;
    }
}

/* An event of another service of core (optics.c: a substage armed or passed, the worker task made, a job listed or
 * ended): a mark with its text, a snapshot of the watched tasks and the window of snapshots after it, as after a write
 * to the card - a reset soon after the event leaves the tasks' last states in the RAM log. Core task only */
void dbg_event(const char *what, const char *arg)
{
    if (!on || !cur)
        return;
    mark(what, arg);
    snapshot(T_EVENT, 0);
    if (window_ms) {
        window_until = omk_ms() + window_ms;
        window_last = 0;
    }
}

/* [debug] stuck_mem: the words of the ranges given, into the log (the card's and the RAM's) once a stay - what the firmware's
 * state was when the application got stuck, for a camera whose RAM may not outlive what comes next (the battery out).
 * Only memory a read cannot fault on (drv_mem_readable, as GET MEM) */
static void stuck_dump(void)
{
    const volatile u8 *m;
    u32 i, k, a, n;

    for (i = 0; i < n_stuck_mem; i++) {
        a = stuck_mem[i][0];
        n = stuck_mem[i][1];
        m = drv_mem_readable(a, n);
        if (!m) {
            log_printf(OMK_LOG_WARN, "debug: stuck_mem %08x + %u: not readable memory", a, n);
            continue;
        }
        for (k = 0; k + 4 <= n; k += 16)
            log_printf(OMK_LOG_INFO, "debug: mem %08x: %08x %08x %08x %08x", a + k, ((const volatile u32 *)m)[k / 4],
                       k + 8 <= n ? ((const volatile u32 *)m)[k / 4 + 1] : 0, k + 12 <= n ? ((const volatile u32 *)m)[k / 4 + 2] : 0,
                       k + 16 <= n ? ((const volatile u32 *)m)[k / 4 + 3] : 0);
    }
}

/* [debug] stuck_mem = ADDR:LEN, ...: up to STUCK_MEM_MAX ranges, 4-aligned, up to STUCK_MEM_LEN bytes each */
static void stuck_mem_parse(const char *s)
{
    u32 v[2], j, d;

    while (*s && n_stuck_mem < STUCK_MEM_MAX) {
        while (*s == ',' || *s == ' ')
            s++;
        if (!*s)
            break;
        for (j = 0; j < 2; j++) {           /* hex with 0x, or decimal */
            v[j] = 0;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                for (s += 2; (d = *s >= '0' && *s <= '9' ? (u32)(*s - '0') : (*s | 0x20) >= 'a' && (*s | 0x20) <= 'f' ?
                              (u32)((*s | 0x20) - 'a' + 10) : 16) < 16; s++)
                    v[j] = v[j] << 4 | d;
            else
                for (; *s >= '0' && *s <= '9'; s++)
                    v[j] = v[j] * 10 + (u32)(*s - '0');
            if (!j && *s == ':')
                s++;
            else
                break;
        }
        if (j != 1 || (v[0] & 3) || !v[1] || v[1] > STUCK_MEM_LEN || !drv_mem_readable(v[0], v[1])) {
            log_printf(OMK_LOG_WARN, "debug: stuck_mem: a range not ADDR:LEN of readable memory (4-aligned, at most %u "
                       "bytes) - the rest left out", STUCK_MEM_LEN);
            return;
        }
        stuck_mem[n_stuck_mem][0] = v[0];
        stuck_mem[n_stuck_mem++][1] = v[1];
        while (*s && *s != ',')
            s++;
    }
}

/* The application in a passing state (half-press, a transition) longer than stuck_ms: snapshots while it stays */
static void ui_check(u32 now)
{
    u32 app, keys;
    int r = drv_ui_probe(&app, &keys);

    if (r < 0)
        return;
    if (!ui_seen || app != ui_app) {
        ui_seen = 1;
        ui_app = app;
        ui_since = now;
        ui_snaps = 0;
        return;
    }
    if (!r || !stuck_ms || now - ui_since < stuck_ms || ui_snaps >= STUCK_SNAPS || now - ui_last < STUCK_EVERY_MS)
        return;
    if (!ui_snaps) {
        log_printf(OMK_LOG_WARN, "debug: application state %x for %u ms, snapshots to the RAM log", app, now - ui_since);
        stuck_dump();
    }
    ui_snaps++;
    ui_last = now;
    snapshot(T_STUCK, 0);
}

/* ---- jobs (core task) ---- */
static int path_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~';
}

/* An 8.3 path from the card's root, upper case, '\' between the parts; write - the OMKit folder only */
static int path_ok(const char *p, int write)
{
    u32 i = 0, part = 0, name = 0, ext = 0, dot = 0, parts = 0;
    char first[6];

    for (;; i++) {
        char c = p[i];

        if (c == '\\' || !c) {
            if (!name || (dot && !ext))
                return 0;
            if (!parts)
                first[part < 6 ? part : 5] = 0;
            parts++;
            part = name = ext = dot = 0;
            if (!c)
                break;
            continue;
        }
        if (!parts && part < 5)
            first[part] = c;
        part++;
        if (c == '.') {
            if (dot++)
                return 0;
        } else if (!path_char(c) || (dot ? ++ext > 3 : ++name > 8)) {
            return 0;
        }
    }
    if (str_ieq(first, "OMKIT"))
        return !write || parts >= 2;
    return !write && str_ieq(first, "DCIM");
}

static void card_path(char *out, const char *p)
{
    const char *root = drv_root();
    u32 n = 0;

    while (root[n] && root[n] != drv_sep())     /* "C:" */
        out[n] = root[n], n++;
    out[n++] = drv_sep();
    while (*p && n < OMK_PATH_MAX + 6)
        out[n++] = *p++;
    out[n] = 0;
}

static void job_end(u32 result)
{
    u32 st = drv_lock();

    job.result = result;
    job.state = result ? DBG_J_FAILED : DBG_J_DONE;
    drv_unlock(st);
}

static void job_read(void)
{
    char full[OMK_PATH_MAX + 8];
    void *fp;
    u32 n;

    card_path(full, path);
    fp = drv_fopen(full, "r");
    if (!fp) {
        job_end(DBG_E_OPEN);
        return;
    }
    n = 0;                                  /* a seek past the end is not relied on (camera, card152: it left 0) */
    if (!job.arg[0] || (!drv_fseek(fp, job.arg[0]) && drv_ftell(fp) == job.arg[0]))
        n = drv_fread(xfer, 1, job.arg[1], fp);
    drv_fclose(fp);
    job.extra = job.arg[0] + n;             /* read up to here: a piece shorter than asked is the end of the file */
    job.bytes = n;
    job.crc = crc32_update(0, xfer, n);
    job_end(DBG_E_OK);
}

static void job_list(void)
{
    char mask[OMK_PATH_MAX + 8];
    u8 e[DRV_FIND_ENTRY];
    u32 seen = 0, n = 0, k;
    int h, more = 0;

    card_path(mask, path);
    k = str_len(mask);
    if (k + 4 > sizeof(mask)) {
        job_end(DBG_E_PATH);
        return;
    }
    mem_copy(mask + k, "\\*.*", 5);
    mem_set(e, 0, sizeof(e));
    h = drv_findfirst(mask, e);
    while (h != -1) {
        if (seen >= job.arg[0]) {
            if ((n + 1) * DBG_LIST_REC > DBG_XFER_SIZE || seen >= job.arg[0] + LIST_SCAN_MAX) {
                more = 1;
                break;
            }
            mem_set(xfer + n * DBG_LIST_REC, 0, DBG_LIST_REC);
            mem_copy(xfer + n * DBG_LIST_REC, e, 4);            /* attributes */
            mem_copy(xfer + n * DBG_LIST_REC + 4, e + 16, 4);   /* size */
            mem_copy(xfer + n * DBG_LIST_REC + 8, e + 4, 8);    /* DOS date << 16 | time: +4 made, +8 changed */
            for (k = 0; k < DBG_LIST_REC - 17 && e[24 + k]; k++)
                xfer[n * DBG_LIST_REC + 16 + k] = e[24 + k];
            n++;
        }
        seen++;
        mem_set(e, 0, sizeof(e));
        if (drv_findnext(h, e))
            break;
    }
    if (h != -1)
        drv_findclose(h);
    job.bytes = n * DBG_LIST_REC;
    job.crc = crc32_update(0, xfer, job.bytes);
    job.extra = seen | (u32)more << 31;
    job_end(DBG_E_OK);
}

static void job_snap(void)
{
    drv_task t;
    drv_obj o;
    u32 cpu, slot, i, k, nt = 0, no = 0, app = 0, keys = 0, at = DBG_SNAP_HEAD;
    u8 *p;
    int r;

    drv_ui_probe(&app, &keys);
    for (cpu = 0; cpu < OMK_CPUS; cpu++)
        for (slot = 0; (r = drv_task_read(cpu, slot, &t)) >= 0 && slot < 1024; slot++) {
            if (r != 1 || at + DBG_SNAP_TASK > DBG_XFER_SIZE - SNAP_OBJS * DBG_SNAP_OBJ)
                continue;
            p = xfer + at;
            st16(p, t.id), st16(p + 2, t.state), st16(p + 4, t.wait), st16(p + 6, t.wobj);
            st16(p + 8, t.pri), st16(p + 10, t.bpri);
            p[12] = t.cpu, p[13] = t.wup, p[14] = t.sus, p[15] = t.flags;
            st32(p + 16, t.tmo), st32(p + 20, t.wptn), st32(p + 24, t.wmode), st32(p + 28, t.entry);
            st32(p + 32, t.stk), st32(p + 36, t.stksz), st32(p + 40, t.sp), st32(p + 44, t.never);
            for (i = 0; i < DRV_TASK_RETS; i++)
                st32(p + 48 + 4 * i, t.ret[i]);
            mem_copy(p + 72, t.name, DRV_TASK_NAME);
            at += DBG_SNAP_TASK;
            nt++;
        }
    for (i = 0; i < nt && no < SNAP_OBJS; i++) {   /* the objects the tasks wait for */
        p = xfer + DBG_SNAP_HEAD + i * DBG_SNAP_TASK;
        u32 kind = p[4] | p[5] << 8, id = p[6] | p[7] << 8;

        if (!id)
            continue;
        for (k = 0; k < no; k++) {
            u8 *q = xfer + at + k * DBG_SNAP_OBJ;

            if ((u32)(q[0] | q[1] << 8) == id && (u32)(q[2] | q[3] << 8) == kind)
                break;
        }
        if (k < no)
            continue;
        r = drv_obj_read(kind, id, &o);
        if (r == -1)
            continue;
        p = xfer + at + no * DBG_SNAP_OBJ;
        st16(p, id), st16(p + 2, kind), st16(p + 4, r ? 0 : o.owner), st16(p + 6, r ? 0xffff : o.atr);
        st32(p + 8, r ? 0 : o.a), st32(p + 12, r ? 0 : o.b);
        no++;
    }
    p = xfer;
    st32(p, 0x534b4d4fu);                   /* 'OMKS' */
    st32(p + 4, 1);
    st32(p + 8, nt);
    st32(p + 12, no);
    st32(p + 16, omk_ms());
    st32(p + 20, app);
    st32(p + 24, keys);
    st32(p + 28, (drv_task_current(0) & 0xffff) | (u32)drv_task_current(1) << 16);
    st32(p + 32, drv_task_current(2) & 0xffff);
    job.bytes = at + no * DBG_SNAP_OBJ;
    job.crc = crc32_update(0, xfer, job.bytes);
    job.extra = nt;
    job_end(DBG_E_OK);
}

static void write_drop(void)
{
    if (wfp)
        drv_fclose(wfp);
    wfp = 0;
    if (wtmp[0])
        drv_remove(wtmp);
    wtmp[0] = 0;
}

/* Missing folders of the path under the OMKit folder are made */
static int make_dirs(const char *full)
{
    char d[OMK_PATH_MAX + 8];
    u32 i, root = str_len(drv_root());

    for (i = 0; full[i]; i++) {
        d[i] = full[i];
        if (full[i] == drv_sep() && i >= root) {
            d[i] = 0;
            if (!drv_exists(d) && drv_mkdir(d))
                return -1;
            d[i] = full[i];
        }
    }
    return 0;
}

static void job_wbegin(void)
{
    u32 k;

    if (wfp) {
        job_end(DBG_E_STATE);
        return;
    }
    card_path(wtarget, path);
    k = str_len(wtarget);
    while (k && wtarget[k - 1] != drv_sep())
        k--;
    if (str_ieq(wtarget + k, TMP_NAME) || k + sizeof(TMP_NAME) > sizeof(wtmp)) {
        job_end(DBG_E_PATH);
        return;
    }
    mem_copy(wtmp, wtarget, k);
    mem_copy(wtmp + k, TMP_NAME, sizeof(TMP_NAME));
    core_card_write(1, wtmp);
    if (make_dirs(wtarget)) {
        core_card_write(0, wtmp);
        wtmp[0] = 0;
        job_end(DBG_E_OPEN);
        return;
    }
    drv_remove(wtmp);                       /* "w" does not truncate here */
    wfp = drv_fopen(wtmp, "wb");
    core_card_write(0, wtmp);
    if (!wfp) {
        wtmp[0] = 0;
        job_end(DBG_E_OPEN);
        return;
    }
    wsize = job.arg[0];
    wcrc_want = job.arg[1];
    wdone = wcrc = 0;
    wlast = omk_ms();
    log_printf(OMK_LOG_INFO, "debug: writing %s, %u bytes", wtarget, wsize);
    job_end(DBG_E_OK);
}

static void job_wdata(void)
{
    u32 n = job.arg[1], w;

    core_card_write(1, wtmp);
    w = wfp ? drv_fwrite(xfer, 1, n, wfp) : 0;
    core_card_write(0, wtmp);
    wlast = omk_ms();
    if (w != n) {
        log_printf(OMK_LOG_WARN, "debug: %s took %u of %u bytes, dropped", wtmp, w, n);
        write_drop();
        job_end(DBG_E_IO);
        return;
    }
    wcrc = crc32_update(wcrc, xfer, n);
    wdone += n;
    job.bytes = wdone;
    job.crc = wcrc;
    job_end(DBG_E_OK);
}

/* Closed, read back from the card, compared, then the old file removed and the new one renamed into its place */
static void job_wcommit(void)
{
    void *fp;
    u32 n, total = 0, crc = 0, result = DBG_E_OK;

    if (!wfp) {
        job_end(DBG_E_STATE);
        return;
    }
    core_card_write(1, wtarget);
    drv_fclose(wfp);
    wfp = 0;
    fp = drv_fopen(wtmp, "r");
    if (fp) {
        while ((n = drv_fread(xfer, 1, DBG_XFER_SIZE, fp)) != 0) {
            crc = crc32_update(crc, xfer, n);
            total += n;
        }
        drv_fclose(fp);
    }
    if (!fp)
        result = DBG_E_OPEN;
    else if (total != wsize || wdone != wsize)
        result = DBG_E_SIZE;
    else if (crc != wcrc_want)
        result = DBG_E_CRC;
    if (!result) {
        drv_remove(wtarget);                /* rename does not replace a file (the firmware's own test removes first) */
        fp = drv_rename(wtmp, wtarget) ? 0 : drv_fopen(wtarget, "r");
        if (!fp)
            result = DBG_E_RENAME;
        else
            drv_fclose(fp);
    }
    core_card_write(0, wtarget);
    job.bytes = total;
    job.crc = crc;
    log_printf(result ? OMK_LOG_WARN : OMK_LOG_INFO, "debug: %s %s, %u bytes, crc %08x", wtarget,
               result ? "NOT replaced" : "replaced", total, crc);
    if (result != DBG_E_RENAME)
        write_drop();                       /* a failed rename keeps the checked temporary file for a look */
    else
        wtmp[0] = 0;
    job_end(result);
}

/* The test scenario: started (the steps of PUT TEXT, else OMKIT.INI's) or stopped. A start waits for the camera to shoot:
 * in the MTP mode it begins once the cable is out */
static void job_shoot(void)
{
    int rc, delay = cfg_int("shoot", "start_delay", 5000);

    if (job.arg[0] == DBG_SHOOT_STOP) {
        shoot_stop();
        job_end(DBG_E_OK);
        return;
    }
    rc = shoot_start(text_arg[0] ? text_arg : 0, delay < 0 || delay > 600000 ? 5000 : (u32)delay);
    log_printf(rc ? OMK_LOG_WARN : OMK_LOG_INFO, "debug: test scenario %s from USB%s%s", rc == -2 ? "NOT started (one runs)" :
               rc ? "NOT started (a step not understood)" : "started", text_arg[0] ? ": " : "", text_arg);
    text_arg[0] = 0;
    job_end(rc == -2 ? DBG_E_BUSY : rc ? DBG_E_STEPS : DBG_E_OK);
}

/* The restart: refused while a file is being written, a scenario presses keys, or a power-off or sleep is under way; else
 * the job ends at once and the firmware's reboot request follows RESTART_GRACE_MS later (dbg_service). The power-off it
 * leads to is an ordinary one, in the application's task: the power hooks, the card-off gate and the last save as for the
 * lever. The log goes to the card before the request */
static void job_restart(u32 now)
{
    shoot_info si;

    shoot_status(&si);
    if (wfp || restart_state || !power_running() || drv_power_pending() || (si.state == SHOOT_RUNNING && !si.waiting)) {
        log_printf(OMK_LOG_WARN, "debug: restart refused: %s", wfp ? "a file is being written" : restart_state ?
                   "one is under way" : si.state == SHOOT_RUNNING && !si.waiting ? "the test scenario is shooting" :
                   "a power-off or sleep is under way");
        job_end(si.state == SHOOT_RUNNING && !si.waiting ? DBG_E_BUSY : DBG_E_STATE);
        return;
    }
    log_printf(OMK_LOG_INFO, "debug: restart over USB: the firmware's reboot request in %u ms", RESTART_GRACE_MS);
    log_flush();
    restart_state = 1;
    restart_t = now + RESTART_GRACE_MS;
    job.extra = RESTART_GRACE_MS;
    job_end(DBG_E_OK);
}

static void restart_service(u32 now)
{
    if (restart_state == 1 && (s32)(now - restart_t) >= 0) {
        restart_state = 2;
        restart_t = now;
        mark("restart request", 0);
        drv_restart_request();
        log_printf(OMK_LOG_INFO, "debug: the reboot request posted, waiting for the power-off");
    } else if (restart_state == 2 && now - restart_t >= RESTART_TAKEN_MS) {
        restart_state = 0;                  /* the service runs only while the camera does: no power-off came */
        log_printf(OMK_LOG_WARN, "debug: the reboot request was not taken in %u ms (the camera not in the PC mode of USB?), "
                   "running on", RESTART_TAKEN_MS);
    }
}

static void job_run(u32 now)
{
    u32 kind = job.kind;

    if ((kind == J_WBEGIN || kind == J_WDATA || kind == J_WCOMMIT || kind == J_WABORT || kind == J_RESTART) &&
        drv_card_busy())
        return;                             /* the camera writes a picture: as every write of core, later */
    if (kind != J_SNAP)
        log_close();                        /* a file job may read or replace the log file: not while it is open */
    job.state = DBG_J_RUNNING;
    switch (kind) {
    case J_SNAP:
        job_snap();
        break;
    case J_READ:
        job_read();
        break;
    case J_LIST:
        job_list();
        break;
    case J_WBEGIN:
        job_wbegin();
        break;
    case J_WDATA:
        job_wdata();
        break;
    case J_WCOMMIT:
        job_wcommit();
        break;
    case J_WABORT:
        write_drop();
        job_end(DBG_E_OK);
        break;
    case J_SHOOT:
        job_shoot();
        break;
    case J_RESTART:
        job_restart(now);
        break;
    default:
        job_end(DBG_E_STATE);
        break;
    }
}

/* ---- PTP handlers (the firmware's PTP task, CPU 0): no waits, no card, no log ---- */
static void reply(drv_ptp_msg *r, u32 code, u32 n, u32 p0, u32 p1, u32 p2)
{
    r->code = code;
    r->n = n;
    r->p[0] = p0;
    r->p[1] = p1;
    r->p[2] = p2;
}

static int start_job(u32 kind, u32 a0, u32 a1)
{
    u32 st = drv_lock();
    int ok = job.state != DBG_J_QUEUED && job.state != DBG_J_RUNNING;

    if (ok) {
        job.kind = kind;
        job.arg[0] = a0;
        job.arg[1] = a1;
        job.result = job.bytes = job.crc = job.extra = 0;
        job.seq++;
        job.state = DBG_J_QUEUED;
    }
    drv_unlock(st);
    if (ok)
        drv_wake();
    return ok;
}

/* CORE_VERSION "1.42" -> 1 << 16 | 42 */
static u32 version(void)
{
    const char *v = CORE_VERSION;
    u32 major = 0, minor = 0;

    while (*v >= '0' && *v <= '9')
        major = major * 10 + *v++ - '0';
    if (*v == '.')
        v++;
    while (*v >= '0' && *v <= '9')
        minor = minor * 10 + *v++ - '0';
    return major << 16 | minor;
}

static u32 flags(void)
{
    return (cur ? DBG_F_RING : 0) | (prev_valid ? DBG_F_PREV : 0) | (write_on ? DBG_F_WRITE : 0) |
           (card_on ? 0 : DBG_F_NO_CARD) | (ptp_on ? DBG_F_PTP : 0) | (stage && xfer ? DBG_F_BUFFERS : 0) | DBG_F_SHOOT |
           DBG_F_INFO | (write_on ? DBG_F_RESTART : 0);
}

/* GET INFO: key=value lines, plain text */
static u32 info_str(u32 at, const char *v)
{
    while (*v && at < DBG_INFO_MAX - 1)
        stage[at++] = *v++;
    return at;
}

static u32 info_num(u32 at, u32 v)
{
    char d[11];
    u32 n = 0;

    do
        d[n++] = '0' + v % 10;
    while ((v /= 10) && n < sizeof(d));
    while (n && at < DBG_INFO_MAX - 1)
        stage[at++] = d[--n];
    return at;
}

static u32 info(void)
{
    static const char *const shoot_states[] = {"idle", "running", "done", "failed", "stopped"};
    shoot_info si;
    u32 at;

    shoot_status(&si);
    at = info_str(0, "core=" CORE_VERSION "\nbuild=" CORE_BUILD "\nrun=");
    at = info_str(at, run_name);
    at = info_num(info_str(at, "\npower-on="), cur ? cur->h[DH_BOOT] : 0);
    at = info_str(info_str(at, "\nrestart="), flags() & DBG_F_RESTART ? "yes" : "no");
    at = info_str(info_str(at, "\nshoot="), si.state < 5 ? shoot_states[si.state] : "?");
    at = info_str(at, si.waiting ? ",waiting" : "");
    at = info_num(info_str(at, "\nshoot_taken="), si.taken);
    at = info_num(info_str(at, "\nshoot_failed="), si.failed);
    at = info_num(info_str(at, "\nshoot_total="), si.total);
    at = info_num(info_str(at, "\nshoot_step="), si.state == SHOOT_RUNNING ? si.step + 1 : 0);
    at = info_num(info_str(at, "\nshoot_steps="), si.steps);
    return info_str(at, "\n");
}

static void h_cmd(const drv_ptp_msg *q, drv_ptp_msg *r)
{
    u32 sub = q->p[0], kind = J_NONE;
    int need_path = 0, need_write = 0;

    reply(r, 0x2001, 0, 0, 0, 0);
    switch (sub) {
    case DBG_HELLO:
        reply(r, 0x2001, 5, DBG_MAGIC, DBG_PROTOCOL, version());
        r->p[3] = flags();
        r->p[4] = cur ? cur->h[DH_BOOT] : 0;
        return;
    case DBG_STATUS:
        reply(r, 0x2001, 5, job.kind | job.state << 8 | (job.seq & 0xffff) << 16, job.result, job.bytes);
        r->p[3] = job.crc;
        r->p[4] = job.extra;
        return;
    case DBG_LEVEL:
        if (q->p[1] > OMK_LOG_DEBUG) {
            reply(r, 0x201d, 0, 0, 0, 0);
            return;
        }
        reply(r, 0x2001, 1, ring_level, 0, 0);
        level_req = q->p[1] | 0x100;        /* the core task applies it */
        drv_wake();
        return;
    case DBG_MARK:
        mark_tag = q->p[1];
        mark_req = 1;
        drv_wake();
        return;
    case DBG_SNAP:
        kind = J_SNAP;
        break;
    case DBG_READ:
        kind = J_READ, need_path = 1;
        break;
    case DBG_LIST:
        kind = J_LIST, need_path = 2;
        break;
    case DBG_WBEGIN:
        kind = J_WBEGIN, need_path = 3, need_write = 1;
        break;
    case DBG_WCOMMIT:
        kind = J_WCOMMIT, need_write = 1;
        break;
    case DBG_WABORT:
        kind = J_WABORT, need_write = 1;
        break;
    case DBG_RESTART:
        kind = J_RESTART, need_write = 1;
        if (q->p[1] != DBG_RESTART_KEY) {
            reply(r, 0x201d, 0, 0, 0, 0);
            return;
        }
        break;
    case DBG_SHOOT:
        if (q->p[1] == DBG_SHOOT_STATUS) {  /* the scenario's words, read on CPU 0 as the core task writes them */
            shoot_info si;

            shoot_status(&si);
            reply(r, 0x2001, 5, si.state | si.waiting << 8, si.total, si.taken);
            r->p[3] = si.failed;
            r->p[4] = (si.state == SHOOT_RUNNING ? si.step + 1 : 0) | si.steps << 16;
            return;
        }
        if (q->p[1] > DBG_SHOOT_STOP) {
            reply(r, 0x201d, 0, 0, 0, 0);
            return;
        }
        kind = J_SHOOT, need_write = 1;
        break;
    default:                                /* camera control (0x40..0x4f) and anything else: not done */
        reply(r, 0x2005, 0, 0, 0, 0);
        return;
    }
    if (need_write && !write_on) {
        reply(r, 0x200f, 0, 0, 0, 0);
        return;
    }
    if (!xfer || (need_path && !path_ok(path, need_path == 3)) || (kind == J_READ && q->p[2] > DBG_XFER_SIZE)) {
        reply(r, 0x201d, 0, 0, 0, 0);
        return;
    }
    if (!start_job(kind, q->p[1], q->p[2]))
        reply(r, 0x2019, 0, 0, 0, 0);
    else
        reply(r, 0x2001, 1, job.seq, 0, 0);
}

/* At most n bytes of src to the computer in pieces of 16 KB (even ones until the last) */
static int send(const u8 *src, u32 n)
{
    u32 done = 0, piece;

    while (done < n) {
        piece = n - done < DBG_STAGE ? n - done : DBG_STAGE;
        if (!drv_ptp_send(src + done, piece, n))
            return 0;
        done += piece;
    }
    return 1;
}

static void h_get(const drv_ptp_msg *q, drv_ptp_msg *r)
{
    u32 sub = q->p[0], n = 0, at, i;
    const u8 *src = stage;
    const volatile u8 *mem;
    void *area;
    dring_bank *b;

    reply(r, 0x201d, 0, 0, 0, 0);
    if (!stage)
        return;
    switch (sub) {
    case DBG_BANKS:
        for (i = 0; i < 2 * DH_WORDS; i++)
            st32(stage + 4 * i, cur ? ring.bank[i / DH_WORDS].h[i % DH_WORDS] : 0);
        drv_keep_area(&area, &at);
        st32(stage + 128, (u32)(uptr)area);
        st32(stage + 132, at);
        st32(stage + 136, cur ? ring.cur : 0xff);
        st32(stage + 140, ring.prev < 0 || !cur ? 0xff : ring.prev);
        src = stage;
        n = 144;
        break;
    case DBG_BANK:
        if (!cur || q->p[1] > 1 || (q->p[1] && !prev_valid) || q->p[3] > DBG_STAGE || q->p[3] & 1)
            return;
        b = &ring.bank[q->p[1] ? ring.prev : ring.cur];
        n = q->p[3];
        dring_copy(b, q->p[2], stage, n);
        r->p[1] = b->h[DH_TAIL];            /* after the copy: the computer drops what was overwritten meanwhile */
        r->p[2] = b->h[DH_HEAD];
        break;
    case DBG_MEM:
        mem = q->p[2] > DBG_STAGE ? 0 : drv_mem_readable(q->p[1], q->p[2]);
        if (!mem)
            return;
        n = q->p[2];
        if (!((uptr)mem & 3) && !(n & 3))   /* words where it can: a quarter of the uncached reads */
            for (i = 0; i < n; i += 4)
                *(u32 *)(stage + i) = *(const volatile u32 *)(mem + i);
        else
            for (i = 0; i < n; i++)
                stage[i] = mem[i];
        break;
    case DBG_HIST:
        n = drv_ptp_history(stage, DBG_STAGE);
        break;
    case DBG_INFO:
        n = info();
        break;
    case DBG_XFER:
        if (job.state != DBG_J_DONE || q->p[1] & 1 || q->p[1] > job.bytes || q->p[2] > job.bytes - q->p[1])
            return;
        src = xfer + q->p[1];
        n = q->p[2];
        break;
    default:
        r->code = 0x2005;
        return;
    }
    if (n && !send(src, n)) {
        reply(r, 0x2002, 0, 0, 0, 0);
        return;
    }
    r->code = 0x2001;
    r->n = sub == DBG_BANK ? 3 : 1;
    r->p[0] = n;
}

static void h_put(const drv_ptp_msg *q, drv_ptp_msg *r)
{
    u32 sub = q->p[0], len, i;
    int busy = job.state == DBG_J_QUEUED || job.state == DBG_J_RUNNING;

    reply(r, 0x201d, 0, 0, 0, 0);
    if (!stage || !xfer)
        return;
    if (busy) {
        r->code = 0x2019;                   /* the data is drained by the firmware */
        return;
    }
    if (sub == DBG_PATH) {
        len = drv_ptp_recv_len();
        if (!len || len > DBG_PATH_MAX || !drv_ptp_recv(stage, len))
            return;
        for (i = 0; i < len && stage[i]; i++)
            path[i] = stage[i] == '/' ? '\\' : stage[i] >= 'a' && stage[i] <= 'z' ? stage[i] - 32 : stage[i];
        path[i] = 0;
        reply(r, 0x2001, 0, 0, 0, 0);
        return;
    }
    if (sub == DBG_TEXT) {                  /* printable ASCII, up to DBG_TEXT_MAX; empty clears it */
        len = drv_ptp_recv_len();
        if (len > DBG_TEXT_MAX || (len && !drv_ptp_recv(stage, len)))
            return;
        for (i = 0; i < len && stage[i] >= 0x20 && stage[i] < 0x7f; i++)
            text_arg[i] = stage[i];
        if (i < len && stage[i]) {
            text_arg[0] = 0;
            return;
        }
        text_arg[i] = 0;
        reply(r, 0x2001, 0, 0, 0, 0);
        return;
    }
    if (sub != DBG_WDATA) {
        r->code = 0x2005;
        return;
    }
    if (!write_on) {
        r->code = 0x200f;
        return;
    }
    len = drv_ptp_recv_len();
    if (!wfp || q->p[1] != wdone || !len || len > DBG_XFER_SIZE || len > wsize - wdone || !drv_ptp_recv(xfer, len))
        return;
    if (start_job(J_WDATA, q->p[1], len))
        reply(r, 0x2001, 1, job.seq, 0, 0);
}

/* ---- setup and service ---- */
static int parse_level(const char *s, int def)
{
    static const char *const names[] = {"off", "error", "warn", "info", "debug"};
    u32 i;

    for (i = 0; i < 5; i++)
        if (str_ieq(s, names[i]))
            return i;
    return def;
}

static u32 cfg_ms(const char *key, int def, u32 max)
{
    int v = cfg_int("debug", key, def);

    return v < 0 || (u32)v > max ? (u32)def : (u32)v;
}

/* [debug] tasks: names separated by commas or spaces */
static void watch_list(void)
{
    const char *s = cfg_str("debug", "tasks", "FILECLBK,FILEFGND,REC_FILE,MOVE_FILEWRITE,DCFPROC_SUB,REC_IMAGE,REC_MAIN,OMK_OPTX");
    u32 n;

    n_watch = 1;                            /* entry 0: the core task, by its id */
    while (*s && n_watch < WATCH_MAX) {
        while (*s == ',' || *s == ' ')
            s++;
        for (n = 0; *s && *s != ',' && *s != ' '; s++)
            if (n < DRV_TASK_NAME - 1)
                watch[n_watch].name[n++] = *s;
        if (n)
            watch[n_watch++].name[n] = 0;
    }
}

void dbg_setup(const char *run)
{
    void *area;
    u32 size;

    on = cfg_bool("debug", "enabled", 0);
    if (!on)
        return;
    mem_copy(run_name, run, 8);
    write_on = cfg_bool("debug", "write", 0);
    card_on = cfg_bool("debug", "card_writes", 1);
    ring_level = parse_level(cfg_str("debug", "ring_level", "debug"), OMK_LOG_DEBUG);
    if (drv_keep_area(&area, &size) && !dring_open(&ring, area, size, run, 0,
                                                   (card_on ? 0 : DF_NO_CARD) | (write_on ? DF_WRITE : 0))) {
        cur = &ring.bank[ring.cur];
        prev_valid = ring.prev >= 0;
        log_tee(tee, ring_level);
    }
    if (!card_on) {
        log_card(0);
        store_card(0);
    }
}

/* The run before, from its bank: counts and the last line of its log */
typedef struct {
    u32 records, lines, snaps, last_ms, gaps, skipped;
    char last[100];
} walk_t;

static void walk_fn(u32 type, u32 seq, u32 ms, const u8 *data, u32 len, void *ctx)
{
    walk_t *w = ctx;
    u32 i;

    (void)seq;
    w->records++;
    w->last_ms = ms;
    if (type == DR_SNAP)
        w->snaps++;
    if (type != DR_LOG || !len)
        return;
    w->lines++;
    for (i = 0; i + 1 < len && i < sizeof(w->last) - 1; i++)
        w->last[i] = data[i + 1] >= 0x20 && data[i + 1] < 0x7f ? data[i + 1] : '?';
    w->last[i] = 0;
}

static void report_prev(void)
{
    dring_bank *b = &ring.bank[ring.prev];
    walk_t w;
    char run[9];
    u32 i;

    if (!prev_valid) {
        log_printf(OMK_LOG_INFO, "debug: no RAM log of a run before (a power-off or the battery cleared the memory)");
        return;
    }
    mem_set(&w, 0, sizeof(w));
    dring_walk(b, xfer, b->size / 4, walk_fn, &w, &w.gaps, &w.skipped);
    for (i = 0; i < 8; i++)
        run[i] = b->h[DH_RUN0 + i / 4] >> (8 * (i % 4));
    run[8] = 0;
    log_printf(prev_valid && !(b->h[DH_FLAGS] & DF_ENDED) ? OMK_LOG_WARN : OMK_LOG_INFO,
               "debug: the run before (%s, power-on %u) %s; heartbeat %u ms, %u records (%u lines, %u snapshots), the last "
               "at %u ms, %u lost; %u damaged places, %u bytes skipped", run, b->h[DH_BOOT], b->h[DH_FLAGS] & DF_ENDED ?
               "ended with a power-off" : "did NOT end with a power-off", b->h[DH_BEAT], w.records, w.lines, w.snaps,
               w.last_ms, b->h[DH_LOST], w.gaps, w.skipped);
    if (w.lines)
        log_printf(OMK_LOG_INFO, "debug: its last line: %s", w.last);
}

static u8 *alloc_aligned(u32 size)
{
    u8 *p = mem_alloc(size + ALIGN_PTP);

    return p ? (u8 *)(((uptr)p + ALIGN_PTP - 1) & ~(uptr)(ALIGN_PTP - 1)) : 0;
}

void dbg_start(void)
{
    static const char *const keys[] = {"enabled", "write", "card_writes", "ring_level", "tasks", "window_ms",
                                       "window_every", "stuck_ms", "stuck_mem", "emu_restart"};
    const char *sec, *key, *val;
    u32 i, k, n;
    int rc = -1;

    for (i = 0; i < cfg_count(); i++) {
        cfg_entry(i, &sec, &key, &val);
        if (!str_ieq(sec, "debug"))
            continue;
        for (k = 0; k < sizeof(keys) / sizeof(keys[0]) && !str_ieq(key, keys[k]); k++)
            ;
        if (k == sizeof(keys) / sizeof(keys[0]))
            log_printf(OMK_LOG_WARN, "config: [debug] %s unknown", key);
    }
    if (!on)
        return;
    window_ms = cfg_ms("window_ms", 1000, 10000);
    window_every = cfg_ms("window_every", 50, 1000);
    if (window_every < 10)
        window_every = 10;
    stuck_ms = cfg_ms("stuck_ms", 1000, 600000);
    stuck_mem_parse(cfg_str("debug", "stuck_mem", ""));
#ifdef OMK_EMU
    emu_restart_ms = write_on ? cfg_ms("emu_restart", 0, 600000) : 0;
#endif
    watch_list();
    core_id = drv_task_current(0);
    stage = alloc_aligned(DBG_STAGE);
    xfer = alloc_aligned(DBG_XFER_SIZE);
    if (!cur) {
        log_printf(OMK_LOG_WARN, "debug: no RAM log on this platform");
    } else {
        log_printf(OMK_LOG_INFO, "debug: on, RAM log bank %u (power-on %u) of %u bytes, level %u%s%s", ring.cur,
                   cur->h[DH_BOOT], cur->size, ring_level, card_on ? "" : ", NO card writes (log and stores in RAM)",
                   write_on ? ", files and restart over USB allowed" : "");
        if (xfer)
            report_prev();
    }
    if (!stage || !xfer) {
        log_printf(OMK_LOG_WARN, "debug: no memory for its buffers, no USB operations");
        return;
    }
    n = !drv_ptp_add(DBG_OP_CMD, DRV_PTP_PHASE_NONE, h_cmd) + !drv_ptp_add(DBG_OP_GET, DRV_PTP_PHASE_OUT, h_get) +
        !drv_ptp_add(DBG_OP_PUT, DRV_PTP_PHASE_IN, h_put);
    if (n == 3)
        rc = drv_ptp_install();
    ptp_on = !rc;
    log_printf(ptp_on ? OMK_LOG_INFO : OMK_LOG_WARN, ptp_on ? "debug: USB operations %x..%x ready (MTP mode)" :
               "debug: USB operations not installed (%x, %u of 3)", ptp_on ? DBG_OP_CMD : (u32)rc, ptp_on ? DBG_OP_PUT : n);
}

void dbg_service(u32 now)
{
    u32 busy;

    if (!on)
        return;
    if (cur)
        cur->h[DH_BEAT] = now;
    if (level_req) {
        ring_level = level_req & 0xff;
        level_req = 0;
        log_tee(tee, ring_level);
        log_printf(OMK_LOG_INFO, "debug: RAM log level %u (from USB)", ring_level);
    }
    if (n_watch && (!resolve_ms || (resolve_tries < RESOLVE_TRIES && now - resolve_ms >= RESOLVE_GAP_MS))) {
        u32 i, missing = 0;

        for (i = 0; i < n_watch; i++)
            missing += !watch[i].found;
        if (missing || !resolve_ms)
            resolve(now);
    }
    if (mark_req) {
        mark_req = 0;
        mark("mark from USB", 0);
        snapshot(T_MARK, 0);
        log_printf(OMK_LOG_INFO, "debug: mark %u from USB", mark_tag);
    }
    busy = drv_card_busy() != 0;
    if (busy != cam_busy) {
        cam_busy = busy;
        snapshot(busy ? T_CAM_BUSY : T_CAM_IDLE, 0);
    }
    if (window_until && (int)(window_until - now) > 0 && now - window_last >= window_every) {
        window_last = now;
        snapshot(T_WINDOW, 1);
    } else if (window_until && (int)(window_until - now) <= 0) {
        window_until = 0;
    }
    ui_check(now);
#ifdef OMK_EMU
    if (emu_restart_ms && (s32)(now - emu_restart_ms) >= 0 && start_job(J_RESTART, DBG_RESTART_KEY, 0))
        emu_restart_ms = 0;
#endif
    if (job.state == DBG_J_QUEUED)
        job_run(now);
    restart_service(now);
    /* signed: the job just run took its time after `now` (omk_ms at its end), so wlast may be later than now - unsigned,
     * that dropped the file at once after every WBEGIN and WDATA on the camera (card155) */
    if (wfp && (s32)(now - wlast) >= WRITE_IDLE_MS && job.state != DBG_J_QUEUED) {
        log_printf(OMK_LOG_WARN, "debug: no data for %s for %u s, the temporary file dropped", wtarget, WRITE_IDLE_MS / 1000);
        write_drop();
    }
}

u32 dbg_tick(u32 tick)
{
    if (on && restart_state == 1)
        return RESTART_GRACE_MS / 5;
    if (on && (window_until || job.state == DBG_J_QUEUED) && window_every < tick)
        return window_every;
    return tick;
}

void dbg_ended(void)
{
    if (cur)
        cur->h[DH_FLAGS] |= DF_ENDED;
}
