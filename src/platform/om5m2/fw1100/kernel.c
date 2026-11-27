/* The firmware's uITRON 4.0 kernels (one per CPU, the same code): tasks and objects for the task dump (core/taskdump.c).
 * Layout: static analysis of the kernel 0x6002f000..0x6003a000 of fw 1.1, checked in the emulator on CPU 0 (it does not
 * run the kernels of CPUs 1 and 2; they run the same code on their own control blocks).
 *
 * Read only, no locks, nothing is called in the firmware. The words of this CPU's kernel are read through the cache, as the
 * kernel itself writes them; the words of CPUs 1 and 2 through the uncached alias: their write-back data caches are not
 * shared, so a word written there a moment ago may still be old in memory (DRV_TF_UNSTABLE marks a task whose words changed
 * between two reads). Every pointer is range-checked before it is followed: a read outside memory restarts the camera.
 *
 * A release build has drv_task_current alone (the event manager names the caller's task with it); the walk of the tasks
 * and objects is the task dump's, a debug build's. */
#include "omk_drv.h"

/* Kernel control blocks: (KCB_PTRS)[cpu], set by each CPU at start (get_tid 0x600317b4 reads it). They are static data of the
 * image; a different value means a different firmware layout, and nothing is read then */
#define KCB_PTRS 0x8ff00004
static const u32 kcb_expected[OMK_CPUS] = {0x60041cf4, 0x60041e80, 0x6004200c};

#define KCB_CUR 96                          /* -> word holding the running TCB (dispatcher 0x60035a07) */
#define KCB_LIMITS 60                       /* -> u16 pairs {static, max} per kind: task, sem, flg, dtq, mbx, mtx, mbf, ... */
#define KCB_STATIC 124                      /* -> static task table: 28-byte rows {id, atr, exinf, task, pri, stksz, stk}, ends at id 0 */
#define KCB_STATIC_END 128                  /* -> past that table */
#define KCB_TCBS 204                        /* -> TCB pointers, [index - 1] (ref_tsk 0x60034bab) */
#define KCB_SEMS 208                        /* sig_sem 0x60035c2a */
#define KCB_FLGS 212                        /* clr_flg 0x60030a47, wai_flg 0x600374ae */
#define KCB_MTXS 224                        /* loc_mtx 0x60037ee3, unl_mtx 0x600384cf */
#define LIM_TSK 2                           /* byte offset of the max count of each kind in the KCB_LIMITS pairs */
#define LIM_SEM 6
#define LIM_FLG 10
#define LIM_MTX 22

/* TCB, 120 bytes: ref_tsk 0x60034bab, make-wait 0x6003449f, release 0x600344f4, create 0x60030b02, acre_tsk 0x6003061d */
#define TCB_PRI 8                           /* u16 current priority */
#define TCB_BPRI 10                         /* u16 base priority */
#define TCB_STATE 12                        /* u16 TTS_*: 1 run, 2 ready, 4 wait, 8 suspended, 12 both, 16 dormant */
#define TCB_WAIT 14                         /* u16 TTW_* bit, copied from the object's +2 when the wait begins */
#define TCB_ID 16                           /* u16; 0 - slot not in use */
#define TCB_WOBJ 18                         /* u16 id of the object waited for */
#define TCB_WUP 21                          /* u8 queued wake-ups */
#define TCB_SUS 22                          /* u8 suspend count */
#define TCB_SP 24                           /* saved sp of a task that is not running */
#define TCB_TMO 36                          /* ticks left + 1; 0 - no timeout */
#define TCB_ENTRY 76                        /* T_CTSK copy from +68: atr, exinf, task, itskpri, stksz, stk */
#define TCB_STKSZ 84
#define TCB_STK 88                          /* filled in by the kernel when it allocated the stack itself */
#define TCB_WPTN 96                         /* event flag wait: pattern */
#define TCB_WMODE 100                       /* event flag wait: mode byte, 1 - OR */

/* Object control blocks */
#define SEM_ID 0
#define SEM_ATR 12
#define SEM_MAX 20
#define SEM_CNT 24
#define FLG_ID 0
#define FLG_ATR 12
#define FLG_PTN 20
#define MTX_ID 8                            /* a mutex block starts with the link in its owner's list */
#define MTX_ATR 20                          /* u8: 2 - priority inheritance, 3 - ceiling */
#define MTX_CEIL 22
#define MTX_OWNER 24                        /* u16 task id, 0 - free */

/* A task switched out by the dispatcher 0x60035a07 (`call 0x600386cb,[d2,d3,a2,a3],16`) has the return 0x60035ae3 at
 * saved sp + 16; the frames of the wait service, its wrapper and the firmware caller follow above */
#define CTX_RET_OFF 16
#define CTX_RET 0x60035ae3
#define KERNEL_LO 0x6002f000                /* kernel code: its return addresses say nothing about the caller */
#define KERNEL_HI 0x6003a000
#define SCAN_BYTES 512                      /* how far above the saved sp return addresses are looked for */
#define CODE_LO 0x60000200                  /* main image, code and data */
#define CODE_HI 0x617e0000
#define OMK_CODE_LO 0x6345a000              /* core and the modules, cached alias: a task stopped in a hook */
#define OMK_CODE_HI 0x63780000

/* acre_Task 0x60051f15 fills the stack of the tasks it creates with this word, all but the top 32 bytes. The kernel's own
 * create does not: static tasks have no fill, their high-water mark is unknown */
#define STACK_FILL 0xdeadbeefu
#define STACK_MAX 0x40000

/* Names. Static tasks: the kernel configuration table 0x6002f064 holds one pointer per CPU to 26-byte records
 * {char name[24]; u16 index}. Tasks made by acre_Task: 64 records {char name[16]; u16 id} per CPU at 0xa26acd5e +
 * 1152 * cpu (acre_Task 0x60051e6b, uncached, as the firmware writes them) */
#define NAMES_STATIC 0x6002f064
#define NAMES_STATIC_REC 26
#define NAMES_STATIC_LEN 24
#define NAMES_DYN 0xa26acd5e
#define NAMES_DYN_STEP 1152
#define NAMES_DYN_N 64
#define NAMES_DYN_REC 18
#define NAMES_DYN_LEN 16

static int kptr(u32 p)
{
    return p >= 0x60000000 && p < 0x64000000 && !(p & 3);
}

/* The address to read a word of cpu's kernel at: this CPU through the cache, the others past it */
static u32 at(u32 cpu, u32 a)
{
    return cpu == drv_cpu() ? a : a + DRV_UNCACHED;
}

static u32 rd32(u32 cpu, u32 a)
{
    return *(const volatile u32 *)at(cpu, a);
}

static u32 rd16(u32 cpu, u32 a)
{
    return *(const volatile u16 *)at(cpu, a);
}

#ifdef OMK_DEBUG
static u32 rd8(u32 cpu, u32 a)
{
    return *(const volatile u8 *)at(cpu, a);
}
#endif

static u32 kcb_of(u32 cpu)
{
    u32 k;

    if (cpu >= OMK_CPUS)
        return 0;
    k = ((const volatile u32 *)KCB_PTRS)[cpu];
    return k == kcb_expected[cpu] ? k : 0;
}

#ifdef OMK_DEBUG
/* Max count and the pointer array of one kind of cpu's objects; 0 - none */
static u32 table(u32 cpu, u32 off, u32 lim, u32 *max)
{
    u32 k = kcb_of(cpu), l, t;

    *max = 0;
    if (!k)
        return 0;
    l = rd32(cpu, k + KCB_LIMITS);
    t = rd32(cpu, k + off);
    if (!kptr(l) || !kptr(t))
        return 0;
    *max = rd16(cpu, l + lim);
    return t;
}

/* Block of object id of the kind at off; 0 - no such object */
static u32 block(u32 id, u32 off, u32 lim, u32 *cpu)
{
    u32 max, t, i = id & 0xfff, p;

    *cpu = id >> 12 & 7;
    t = table(*cpu, off, lim, &max);
    if (!t || !i || i > max)
        return 0;
    p = rd32(*cpu, t + 4 * (i - 1));
    return kptr(p) ? p : 0;
}

/* Names are read as the firmware keeps them: the static tables never change, the acre_Task table is uncached memory */
static void copy_name(char *out, u32 a, u32 len)
{
    u32 i, c;

    for (i = 0; i < len && i < DRV_TASK_NAME - 1; i++) {
        c = *(const volatile u8 *)(a + i);
        if (!c)
            break;
        out[i] = c >= 0x20 && c < 0x7f ? (char)c : '?';
    }
    out[i] = 0;
}

static u32 static_rows[OMK_CPUS];           /* rows of each static task table, counted once (core task only) */

static void task_name(u32 cpu, u32 id, char *out)
{
    u32 a = NAMES_DYN + NAMES_DYN_STEP * cpu, i, k, tbl, rows, end;

    for (i = 0; i < NAMES_DYN_N; i++, a += NAMES_DYN_REC)
        if ((u32)(*(const volatile u8 *)(a + 16) | *(const volatile u8 *)(a + 17) << 8) == id) {
            copy_name(out, a, NAMES_DYN_LEN);
            return;
        }
    k = kcb_of(cpu);
    tbl = ((const volatile u32 *)NAMES_STATIC)[cpu];
    if (k && !static_rows[cpu]) {
        a = rd32(cpu, k + KCB_STATIC);
        end = rd32(cpu, k + KCB_STATIC_END);
        for (rows = 0; kptr(a) && a + 28 <= end && rows < 1024 && rd32(cpu, a); a += 28)
            rows++;
        static_rows[cpu] = rows;
    }
    if (tbl >= 0x60000000 && tbl < 0x64000000)
        for (i = 0, a = tbl; i < static_rows[cpu]; i++, a += NAMES_STATIC_REC)
            if ((u32)(*(const volatile u8 *)(a + NAMES_STATIC_LEN) | *(const volatile u8 *)(a + NAMES_STATIC_LEN + 1) << 8) ==
                (id & 0xfff)) {
                copy_name(out, a, NAMES_STATIC_LEN);
                return;
            }
    out[0] = '?';
    out[1] = 0;
}

/* A word on a stack that is a return address: in code, and the instruction before it is a call (MN10300: call d16 cd,
 * call d32 dd, calls d16 fa ff, calls d32 fc ff, calls (an) f0 f0..f3). Code bytes are read past the cache */
static int is_return(u32 v)
{
    const volatile u8 *p;

    if (!(v >= CODE_LO + 8 && v < CODE_HI) && !(v >= OMK_CODE_LO + 8 && v < OMK_CODE_HI))
        return 0;
    p = (const volatile u8 *)(v + DRV_UNCACHED);
    return p[-7] == 0xdd || p[-5] == 0xcd || (p[-6] == 0xfc && p[-5] == 0xff) || (p[-4] == 0xfa && p[-3] == 0xff) ||
           (p[-2] == 0xf0 && (p[-1] & 0xfc) == 0xf0);
}

static void stack_scan(u32 cpu, drv_task *t)
{
    u32 top = t->stk + t->stksz, a, v, end, n = 0;

    if (!kptr(t->stk) || t->stksz < 64 || t->stksz > STACK_MAX || !kptr(top - 4)) {
        t->flags |= DRV_TF_BADSTACK;
        return;
    }
    if (rd32(cpu, t->stk) == STACK_FILL) {
        for (a = t->stk; a < top && rd32(cpu, a) == STACK_FILL; a += 4)
            ;
        t->never = a - t->stk;
        t->flags |= DRV_TF_FILL;
    }
    if (t->state == DRV_TS_RUN || t->state == DRV_TS_DMT)
        return;                             /* the saved sp of a running task is old; a dormant one has no frames */
    if (t->sp < t->stk || t->sp + CTX_RET_OFF + 4 > top || (t->sp & 3)) {
        t->flags |= DRV_TF_BADSTACK;
        return;
    }
    if (rd32(cpu, t->sp + CTX_RET_OFF) == CTX_RET)
        t->flags |= DRV_TF_CTX;
    end = t->sp + SCAN_BYTES < top ? t->sp + SCAN_BYTES : top;
    for (a = t->sp + CTX_RET_OFF + 4; a < end && n < DRV_TASK_RETS; a += 4) {
        v = rd32(cpu, a);
        if ((v < KERNEL_LO || v >= KERNEL_HI) && is_return(v))
            t->ret[n++] = v;
    }
}

int drv_task_read(u32 cpu, u32 slot, drv_task *t)
{
    u32 max, tbl = table(cpu, KCB_TCBS, LIM_TSK, &max), p, i, again;

    if (!tbl || slot >= max)
        return -1;
    p = rd32(cpu, tbl + 4 * slot);
    if (!p)
        return 0;
    if (!kptr(p)) {
        t->id = slot + 1;                   /* the slot's index, for the log */
        t->sp = p;
        return 2;                           /* the pointer is not RAM: not followed */
    }
    for (i = 0; i < 2; i++) {
        t->id = rd16(cpu, p + TCB_ID);
        t->state = rd16(cpu, p + TCB_STATE);
        t->wait = rd16(cpu, p + TCB_WAIT);
        t->wobj = rd16(cpu, p + TCB_WOBJ);
        t->pri = rd16(cpu, p + TCB_PRI);
        t->bpri = rd16(cpu, p + TCB_BPRI);
        t->wup = rd8(cpu, p + TCB_WUP);
        t->sus = rd8(cpu, p + TCB_SUS);
        t->tmo = rd32(cpu, p + TCB_TMO);
        t->wptn = rd32(cpu, p + TCB_WPTN);
        t->wmode = rd8(cpu, p + TCB_WMODE);
        t->sp = rd32(cpu, p + TCB_SP);
        t->entry = rd32(cpu, p + TCB_ENTRY);
        t->stksz = rd32(cpu, p + TCB_STKSZ);
        t->stk = rd32(cpu, p + TCB_STK);
        again = rd16(cpu, p + TCB_STATE) != t->state || rd16(cpu, p + TCB_WOBJ) != t->wobj || rd32(cpu, p + TCB_SP) != t->sp;
        if (!again)
            break;
    }
    if (!t->id)
        return 0;
    t->cpu = cpu;
    t->flags = again ? DRV_TF_UNSTABLE : 0;
    t->never = 0;
    for (i = 0; i < DRV_TASK_RETS; i++)
        t->ret[i] = 0;
    if (!(t->state & DRV_TS_WAI))
        t->wait = t->wobj = 0;
    else if (t->wait <= 2)
        t->wobj = 0;                        /* sleep and delay have no object */
    t->name[0] = 0;
    if (t->state == DRV_TS_DMT)
        return 1;                           /* the dump only counts dormant tasks: no name, no stack */
    task_name(cpu, t->id, t->name);
    stack_scan(cpu, t);
    return 1;
}
#endif

int drv_task_current(u32 cpu)
{
    u32 k = kcb_of(cpu), v, p;

    if (!k)
        return -1;
    v = rd32(cpu, k + KCB_CUR);
    if (!kptr(v))
        return -1;
    p = rd32(cpu, v);
    return kptr(p) ? (int)rd16(cpu, p + TCB_ID) : -1;
}

#ifdef OMK_DEBUG
int drv_obj_read(u32 kind, u32 id, drv_obj *o)
{
    u32 cpu, p;

    o->id = id;
    o->kind = kind;
    o->owner = o->atr = 0;
    o->a = o->b = 0;
    switch (kind) {
    case DRV_TW_SEM:
        if (!(p = block(id, KCB_SEMS, LIM_SEM, &cpu)) || rd16(cpu, p + SEM_ID) != id)
            return -2;
        o->atr = rd8(cpu, p + SEM_ATR);
        o->a = rd32(cpu, p + SEM_CNT);
        o->b = rd32(cpu, p + SEM_MAX);
        return 0;
    case DRV_TW_FLG:
        if (!(p = block(id, KCB_FLGS, LIM_FLG, &cpu)) || rd16(cpu, p + FLG_ID) != id)
            return -2;
        o->atr = rd8(cpu, p + FLG_ATR);
        o->a = rd32(cpu, p + FLG_PTN);
        return 0;
    case DRV_TW_MTX:
        if (!(p = block(id, KCB_MTXS, LIM_MTX, &cpu)) || rd16(cpu, p + MTX_ID) != id)
            return -2;
        o->atr = rd8(cpu, p + MTX_ATR);
        o->owner = rd16(cpu, p + MTX_OWNER);
        o->a = rd16(cpu, p + MTX_CEIL);
        return 0;
    }
    return -1;
}

int drv_mutex_read(u32 cpu, u32 i, drv_obj *o)
{
    u32 max, t = table(cpu, KCB_MTXS, LIM_MTX, &max), p, id;

    if (!t || i >= max)
        return -1;
    p = rd32(cpu, t + 4 * i);
    if (!kptr(p) || !(id = rd16(cpu, p + MTX_ID)))
        return 0;
    return drv_obj_read(DRV_TW_MTX, id, o) ? 0 : 1;
}
#endif
