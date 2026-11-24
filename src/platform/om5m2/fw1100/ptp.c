/* Vendor operations of the USB picture transfer protocol (PTP) and the memory the debug service keeps across a reset,
 * for OM-5 Mark II 1.1.
 *
 * Registration. The firmware clears its list of operations whenever a USB class is set up and registers the class's own
 * operations (task 0x28, CPU 0); the list of the MTP class and of classes 0, 5 and 6 ends in a common tail that starts at
 * mov 0x970c,d0 (0x614ba920). One jump there (thunks.s) calls drv_ptp_reg_all, which registers every operation of the
 * table with 0x614c0156(d0 code, d1 phase, a0 handler, a1 flag) - a code already in the list is replaced, so the firmware's
 * own codes are never used here. Flag 1: no PTP session needed (Windows keeps the session itself).
 *
 * Dispatch. The firmware's dispatcher 0x614c050f (task PTPCOM 0x41, CPU 0, priority 55, under its semaphore) calls the
 * handler with calls (a2), a0 the request {u16 code; ...; u32 n @12; u32 p1..p5 @16}, a1 the response (the same layout, not
 * filled in before), and reads d2, d3 after it. A C operation is entered through _drv_ptp_entry, which keeps every register
 * and calls drv_ptp_call; that finds the operation by its code, gives it a copy of the parameters and writes its answer.
 * The table is core's cached data: it is read only by tasks of CPU 0 (0x28, PTPCOM), like the import's tables. */
#include "omk_drv.h"
#include "memread.h"

#define PTP_OPS 8
#define PTP_FLAG_NO_SESSION 1
#define SITE_REG 0x614ba920

typedef struct {
    u16 code, phase;
    void *fw;                               /* what the firmware calls */
    drv_ptp_fn fn;                          /* the C handler (fw = _drv_ptp_entry), or 0 */
} ptp_op;

static ptp_op ops[PTP_OPS];
static volatile u32 n_ops;                  /* an entry is whole before the count includes it */
static int hooked;

void drv_ptp_entry(void);                   /* thunks.s */
void drv_tramp_ptp_reg(void);
u32 drv_ptp_reg_fw(u32 code, u32 phase, void *handler, u32 flag);
u32 drv_ptp_send_fw(u32 len, u32 total, const void *buf);
u32 drv_ptp_recv_len_fw(void);
u32 drv_ptp_recv_fw(u32 len, void *buf);

/* Driver parts that have a handler of their own in assembly (import.c: 0x970B) */
int drv_ptp_add_raw(u32 code, u32 phase, void *handler)
{
    u32 i;

    for (i = 0; i < n_ops; i++)
        if (ops[i].code == code)
            return -1;
    if (n_ops >= PTP_OPS || phase > DRV_PTP_PHASE_IN)
        return -1;
    ops[n_ops].code = code;
    ops[n_ops].phase = phase;
    ops[n_ops].fw = handler;
    ops[n_ops].fn = 0;
    n_ops++;
    return 0;
}

int drv_ptp_add(u32 code, u32 phase, drv_ptp_fn fn)
{
    u32 n = n_ops;

    if (!fn || drv_ptp_add_raw(code, phase, (void *)drv_ptp_entry))
        return -1;
    ops[n].fn = fn;
    return 0;
}

/* From the hook in the registration tail (task 0x28, CPU 0) */
void drv_ptp_reg_all(void)
{
    u32 i;

    if (drv_cpu())                          /* never seen: the table is CPU 0's cached data */
        return;
    for (i = 0; i < n_ops; i++)
        drv_ptp_reg_fw(ops[i].code, ops[i].phase, ops[i].fw, PTP_FLAG_NO_SESSION);
}

/* The request and response the dispatcher hands to a handler */
#define REQ_CODE 0
#define REQ_N 12
#define REQ_P 16

void drv_ptp_call(const u8 *req, u8 *resp)
{
    drv_ptp_msg q, r;
    u32 i, code = *(const u16 *)(req + REQ_CODE);
    drv_ptp_fn fn = 0;

    for (i = 0; i < n_ops; i++)
        if (ops[i].code == code)
            fn = ops[i].fn;
    q.code = code;
    q.n = *(const u32 *)(req + REQ_N);
    for (i = 0; i < 5; i++)
        q.p[i] = i < q.n ? *(const u32 *)(req + REQ_P + 4 * i) : 0;
    r.code = 0x2005;                        /* OperationNotSupported: not ours after all */
    r.n = 0;
    if (fn && !drv_cpu())
        fn(&q, &r);
    if (r.n > 5)
        r.n = 5;
    *(u16 *)(resp + REQ_CODE) = r.code;
    *(u32 *)(resp + REQ_N) = r.n;
    for (i = 0; i < r.n; i++)
        *(u32 *)(resp + REQ_P + 4 * i) = r.p[i];
}

int drv_ptp_install(void)
{
    static const u8 reg_site[5] = {0xfc, 0xcc, 0x0c, 0x97, 0x00};    /* mov 0x970c,d0 */
    static const struct {
        u32 addr;
        u8 bytes[8];
    } fns[] = {                             /* the first bytes of the functions the thunks call */
        {0x614c0156, {0x93, 0xfc, 0xa0, 0x44, 0x21, 0x3a, 0x62, 0x96}},   /* registration */
        {0x614c02d6, {0x93, 0x83, 0xf1, 0xe6, 0xfc, 0xad, 0x9a, 0x11}},   /* data to the computer */
        {0x614c0399, {0xfc, 0xad, 0x9a, 0x11, 0x3a, 0x62, 0x08, 0xa5}},   /* data from it: the header */
        {0x614c040f, {0xfc, 0xa5, 0x94, 0x11, 0x3a, 0x62, 0x92, 0x82}},   /* data from it */
        {0x606aa6ba, {0x47, 0x04, 0x92, 0xdd, 0xa7, 0xe9, 0x00, 0x00}},   /* rename */
    };
    u32 i, k;
    int rc;

    if (hooked)
        return 0;
    for (i = 0; i < sizeof(fns) / sizeof(fns[0]); i++)
        for (k = 0; k < 8; k++)
            if (((const volatile u8 *)fns[i].addr)[k] != fns[i].bytes[k])
                return 1;
    rc = drv_code_jump(SITE_REG, reg_site, (void *)drv_tramp_ptp_reg);
    if (!rc)
        hooked = 1;
    return rc;
}

/* The firmware's check of a data buffer (0x614c0cfc): an even address with bit 30 clear - not a cached alias. Ours are
 * from the arena (uncached), 128-aligned */
int drv_ptp_buffer_ok(const void *buf)
{
    u32 a = (u32)buf;

    return !(a & 1) && !(a & 0x40000000);
}

/* 0x614c02d6(d0 piece, d1 total, a0 buffer): the first call sends the container header with the total; it waits until
 * the transfer of the piece is done (no time limit of its own) */
int drv_ptp_send(const void *buf, u32 len, u32 total)
{
    if (!drv_ptp_buffer_ok(buf))
        return 0;
    return (drv_ptp_send_fw(len, total, buf) & 0xffff) != 0;
}

u32 drv_ptp_recv_len(void)
{
    return drv_ptp_recv_len_fw();
}

int drv_ptp_recv(void *buf, u32 len)
{
    if (!drv_ptp_buffer_ok(buf))
        return 0;
    return (drv_ptp_recv_fw(len, buf) & 0xffff) != 0;
}

/* The firmware's history of PTP operations: a pointer at 0xa2644f98 to {u32 index; u16 flags[2]; 20 entries of 24 bytes:
 * u16 code, response, p1..p5}, written by 0x614b371c and 0x614b37ef in PTPCOM. Copied as it is */
#define PTP_HIST_PTR 0xa2644f98
#define PTP_HIST_SIZE (8 + 20 * 24)

u32 drv_ptp_history(void *out, u32 max)
{
    u32 p = *(const volatile u32 *)PTP_HIST_PTR, n = max < PTP_HIST_SIZE ? max : PTP_HIST_SIZE, i;
    const volatile u8 *at = drv_mem_readable(p, n);

    if (!at)
        return 0;
    for (i = 0; i < n; i++)
        ((u8 *)out)[i] = at[i];
    return n;
}

/* The rule is memread.h's */
const volatile u8 *drv_mem_readable(u32 addr, u32 len)
{
    u32 at;

    return mem_read_ok(addr, len, &at) ? 0 : (const volatile u8 *)at;
}

/* The debug service's memory: the end of the arena's region (drv.c, ARENA_TAIL), below the optics layer's crumbs at
 * 0xa377ff80. The region 0xa3470000..0xa3780000 is not used by the firmware (checked word by word on the camera), and the
 * words at its end outlived resets on the camera */
#define KEEP_BASE 0xa375ff00
#define KEEP_SIZE 0x20080

int drv_keep_area(void **base, u32 *size)
{
    *base = (void *)KEEP_BASE;
    *size = KEEP_SIZE;
    return 1;
}
