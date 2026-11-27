/* Tests of the debug service (core/dring.c, core/debug.c, the driver's memread.h). Run by tests/host/run.sh:
 *   test_debug <empty dir> ring|memread|proto|write|nocard */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "omk_core.h"
#include "omk_debug.h"
#include "omk_dring.h"
#include "omk_log.h"
#include "omk_shoot.h"
#include "omk_store.h"
#include "memread.h"
#include "host_drv.h"

static int failed;

void modules_power(u32 event)                /* power.c's call into module.c, not in this build */
{
    (void)event;
}

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);             \
            failed++;                                                           \
        }                                                                       \
    } while (0)

/* ---- the ring ---- */
typedef struct {
    u32 n, first, last, bad, types[8];
    char text[64];
} seen_t;

static void count(u32 type, u32 seq, u32 ms, const u8 *data, u32 len, void *ctx)
{
    seen_t *s = ctx;

    (void)ms;
    if (!s->n)
        s->first = seq;
    else if (seq != s->last + 1)
        s->bad++;
    s->last = seq;
    s->n++;
    s->types[type & 7]++;
    if (len < sizeof(s->text)) {
        memcpy(s->text, data, len);
        s->text[len] = 0;
    }
}

static u8 walk_buf[1 << 16];

static seen_t walk(const dring_bank *b)
{
    seen_t s;

    memset(&s, 0, sizeof(s));
    dring_walk(b, walk_buf, b->size / 4, count, &s);
    return s;
}

static void test_ring(void)
{
    dring r;
    seen_t s;
    char line[40];
    u32 i, head, seq, n;
    u8 big[0x5000];

    CHECK(crc32_update(0, "123456789", 9) == 0xcbf43926);   /* the CRC-32 the computer's side checks (zlib's) */
    memset(host_keep, 0x5a, sizeof(host_keep));   /* power-on 1: what RAM holds after the battery was out */
    CHECK(dring_open(&r, host_keep, sizeof(host_keep), "AAAAAAAA", 7, 0) == 0);
    CHECK(r.prev == -1 && r.cur == 0 && r.bank[0].size == 65536);
    CHECK(dring_valid(&r.bank[0]) && r.bank[0].h[DH_BOOT] == 1 && r.bank[0].h[DH_CAUSE] == 7);
    for (i = 0; i < 100; i++) {
        n = snprintf(line, sizeof(line), "line %u", i);
        CHECK(dring_put(&r.bank[0], DR_LOG, i, line, n, 0, 0));
    }
    s = walk(&r.bank[0]);
    CHECK(s.n == 100 && s.first == 0 && s.last == 99 && !s.bad && !strcmp(s.text, "line 99"));

    /* a reset in the middle of a record: the tail and the bytes written, the head not - the record is not read */
    head = r.bank[0].h[DH_HEAD];
    seq = r.bank[0].h[DH_SEQ];
    CHECK(dring_put(&r.bank[0], DR_LOG, 0, "torn", 4, 0, 0));
    r.bank[0].h[DH_HEAD] = head;
    r.bank[0].h[DH_SEQ] = seq;
    s = walk(&r.bank[0]);
    CHECK(s.n == 100 && !strcmp(s.text, "line 99"));

    /* power-on 2: bank 0 is the run before and is kept, bank 1 is new */
    CHECK(dring_open(&r, host_keep, sizeof(host_keep), "BBBBBBBB", 0, 0) == 0);
    CHECK(r.prev == 0 && r.cur == 1 && r.bank[1].h[DH_BOOT] == 2);
    s = walk(&r.bank[0]);
    CHECK(s.n == 100 && !strcmp(s.text, "line 99"));

    /* wrap: many records, the oldest go, the numbers stay in a row; one too long for a quarter of the bank is lost */
    for (i = 0; i < 5000; i++) {
        n = snprintf(line, sizeof(line), "wrap %u", i);
        dring_put(&r.bank[1], i % 3 ? DR_LOG : DR_SNAP, i, line, n, "..", 2);
    }
    s = walk(&r.bank[1]);
    CHECK(s.n > 1000 && s.n < 5000 && s.last == 4999 && !s.bad && !strcmp(s.text, "wrap 4999.."));
    CHECK(r.bank[1].h[DH_HEAD] - r.bank[1].h[DH_TAIL] <= r.bank[1].size);
    memset(big, 1, sizeof(big));
    CHECK(!dring_put(&r.bank[1], DR_SNAP, 0, big, sizeof(big), 0, 0) && r.bank[1].h[DH_LOST] == 1);

    /* power-on 3: the newer bank (1) is the run before; bank 0 starts again */
    CHECK(dring_open(&r, host_keep, sizeof(host_keep), "CCCCCCCC", 0, 0) == 0);
    CHECK(r.prev == 1 && r.cur == 0 && r.bank[0].h[DH_BOOT] == 3);
    CHECK(walk(&r.bank[0]).n == 0 && walk(&r.bank[1]).last == 4999);

    /* a damaged header (a word of the fixed part) is not a run before */
    r.bank[0].h[DH_SIZE] ^= 4;
    r.bank[1].h[DH_RUN0] ^= 1;
    CHECK(!dring_valid(&r.bank[0]) && !dring_valid(&r.bank[1]));
    CHECK(dring_open(&r, host_keep, sizeof(host_keep), "DDDDDDDD", 0, 0) == 0 && r.prev == -1);
    r.bank[1].h[DH_RUN0] ^= 1;
    CHECK(dring_valid(&r.bank[1]));
    CHECK(dring_open(&r, host_keep, 100, "x", 0, 0) == -1);
}

/* ---- the memory the computer may read ---- */
static void test_memread(void)
{
    u32 at;

    CHECK(!mem_read_ok(0xa3470000, 16384, &at) && at == 0xa3470000);
    CHECK(!mem_read_ok(0x60041cf4, 4, &at) && at == 0xa0041cf4);       /* cached alias: read past the cache */
    CHECK(!mem_read_ok(0x6ffffffc, 4, &at) && at == 0xaffffffc);
    CHECK(mem_read_ok(0xafffffff, 2, &at) == -1);                      /* past the end of RAM */
    CHECK(mem_read_ok(0x70000000, 4, &at) == -1);
    CHECK(mem_read_ok(0xb0000000, 4, &at) == -1);                      /* the windows */
    CHECK(mem_read_ok(0x8ff00004, 4, &at) == -1);                      /* registers and shared words */
    CHECK(mem_read_ok(0x9890014c, 4, &at) == -1);
    CHECK(mem_read_ok(0x5ffffffc, 8, &at) == -1);
    CHECK(mem_read_ok(0xa0000000, 0, &at) == -1);
    CHECK(mem_read_ok(0xa0000000, 0xffffffff, &at) == -1);
}

/* ---- the USB operations ---- */
static const drv_ptp_fn *op(u32 code)
{
    u32 i;

    for (i = 0; i < host_ptp_n; i++)
        if (host_ptp[i].code == code)
            return &host_ptp[i].fn;
    return 0;
}

static drv_ptp_msg call(u32 code, u32 p1, u32 p2, u32 p3, u32 p4)
{
    drv_ptp_msg q = {code, 4, {p1, p2, p3, p4, 0}}, r = {0, 9, {0}};
    const drv_ptp_fn *f = op(code);

    host_ptp_out_n = host_ptp_sends = host_ptp_total = 0;
    if (f)
        (*f)(&q, &r);
    else
        r.code = 0x2005;
    return r;
}

static drv_ptp_msg put(u32 sub, u32 p2, const void *data, u32 len)
{
    host_ptp_in = data;
    host_ptp_in_n = len;
    return call(DBG_OP_PUT, sub, p2, 0, 0);
}

static void write_file(const char *name, const char *text)
{
    char p[512];
    FILE *f;

    snprintf(p, sizeof(p), "%s/%s", host_dir, name);
    f = fopen(p, "wb");
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static long file_size(const char *name)
{
    char p[512];
    FILE *f;
    long n;

    snprintf(p, sizeof(p), "%s/%s", host_dir, name);
    f = fopen(p, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

static u32 now_ms;

static void service(void)
{
    now_ms += 50;
    host_ticks = (host_ticks + 50000) & 0x3fffffff;
    dbg_service(omk_ms());
}

static void start(const char *ini)
{
    host_in_task = 1;
    write_file("OMKIT.INI", ini);
    CHECK(cfg_load() == 1);
    dbg_setup("RUN00001");
    log_setup(1, OMK_LOG_INFO);
    dbg_start();
}

static u32 job_wait(void)
{
    drv_ptp_msg r;
    u32 i;

    for (i = 0; i < 10; i++) {
        service();
        r = call(DBG_OP_CMD, DBG_STATUS, 0, 0, 0);
        if ((r.p[0] >> 8 & 0xff) >= DBG_J_DONE)
            return r.p[1] | ((r.p[0] >> 8 & 0xff) == DBG_J_FAILED ? 0x100 : 0);
    }
    return 0xdead;
}

static void test_proto(void)
{
    drv_ptp_msg r;
    seen_t s;
    dring_bank b;

    memset(host_keep, 0, sizeof(host_keep));
    host_tasks[0].id = 7, host_tasks[0].state = DRV_TS_RUN, strcpy(host_tasks[0].name, "OMK_LOAD");
    host_tasks[1].id = 0x51, host_tasks[1].state = DRV_TS_WAI, host_tasks[1].wait = DRV_TW_RDTQ;
    host_tasks[1].wobj = 0x30, strcpy(host_tasks[1].name, "FILECLBK");
    host_ntasks = 2;
    start("[debug]\r\nenabled = on\r\nring_level = info\r\nbogus = 1\r\n");
    CHECK(host_ptp_n == 3 && host_ptp_installs == 1);
    CHECK(host_ptp[0].phase == 0 && host_ptp[1].phase == 1 && host_ptp[2].phase == 2);
    r = call(DBG_OP_CMD, DBG_HELLO, 0, 0, 0);
    CHECK(r.code == 0x2001 && r.n == 5 && r.p[0] == DBG_MAGIC && r.p[1] == DBG_PROTOCOL && r.p[2] == ((u32)atoi(CORE_VERSION) << 16 | (u32)atoi(strchr(CORE_VERSION, '.') + 1)));
    CHECK((r.p[3] & (DBG_F_RING | DBG_F_PTP | DBG_F_BUFFERS)) == (DBG_F_RING | DBG_F_PTP | DBG_F_BUFFERS));
    CHECK(!(r.p[3] & (DBG_F_WRITE | DBG_F_NO_CARD)) && r.p[4] == 1);
    CHECK(call(DBG_OP_CMD, 0x41, 0, 0, 0).code == 0x2005);              /* camera control: kept free */
    CHECK(call(DBG_OP_CMD, 99, 0, 0, 0).code == 0x2005);
    CHECK(call(DBG_OP_CMD, DBG_WBEGIN, 10, 0, 0).code == 0x200f);        /* no write key */
    CHECK(call(DBG_OP_CMD, DBG_RESTART, DBG_RESTART_KEY, 0, 0).code == 0x200f);
    CHECK(put(DBG_WDATA, 0, "x", 1).code == 0x200f);
    /* the test scenario: its state for anyone, a start or a stop only with the write key; the build and the run */
    r = call(DBG_OP_CMD, DBG_SHOOT, DBG_SHOOT_STATUS, 0, 0);
    CHECK(r.code == 0x2001 && r.n == 5 && r.p[0] == SHOOT_IDLE);
    CHECK(call(DBG_OP_CMD, DBG_SHOOT, DBG_SHOOT_START, 0, 0).code == 0x200f);
    CHECK(call(DBG_OP_CMD, DBG_SHOOT, 7, 0, 0).code == 0x201d);
    CHECK(call(DBG_OP_CMD, DBG_HELLO, 0, 0, 0).p[3] & (DBG_F_SHOOT | DBG_F_INFO));
    r = call(DBG_OP_GET, DBG_INFO, 0, 0, 0);
    host_ptp_out[host_ptp_out_n] = 0;
    CHECK(r.code == 0x2001 && r.p[0] == host_ptp_out_n && host_ptp_out_n < DBG_INFO_MAX);
    {
        static const char want[] = "core=" CORE_VERSION "\nbuild=" CORE_BUILD "\nrun=RUN00001\npower-on=1\n";

        CHECK(!strncmp((char *)host_ptp_out, want, sizeof(want) - 1));
    }
    CHECK(strstr((char *)host_ptp_out, "\nrestart=no\nshoot=idle\n") != 0);
    CHECK(strstr((char *)host_ptp_out, "\nkind=debug\n") != 0);          /* the build's kind */

    /* the log reaches the ring; the snapshots name the watched tasks; the core task's write of the log is marked */
    log_printf(OMK_LOG_INFO, "hello ring");
    log_printf(OMK_LOG_DEBUG, "not kept: below the ring's level");
    service();
    log_flush();
    r = call(DBG_OP_GET, DBG_BANKS, 0, 0, 0);
    CHECK(r.code == 0x2001 && host_ptp_out_n == 144 && host_ptp_total == 144);
    CHECK(!memcmp(host_ptp_out, "OMKD", 4) && host_ptp_out[136] == 0 && host_ptp_out[140] == 0xff);
    b.h = (volatile u32 *)host_keep;
    b.d = host_keep + 128;
    b.size = 65536;
    s = walk(&b);
    CHECK(s.n > 5 && s.types[DR_LOG] && s.types[DR_MAP] == 1 && s.types[DR_SNAP] >= 2 && s.types[DR_MARK] >= 2 && !s.bad);
    r = call(DBG_OP_GET, DBG_BANK, 0, 0, 4096);
    CHECK(r.code == 0x2001 && r.n == 3 && r.p[0] == 4096 && host_ptp_out_n == 4096 && r.p[1] == 0 && r.p[2] > 100);
    CHECK(host_ptp_out[0] == DRING_SYNC);
    CHECK(call(DBG_OP_GET, DBG_BANK, 1, 0, 64).code == 0x201d);          /* no run before */
    CHECK(call(DBG_OP_GET, DBG_BANK, 0, 0, DBG_STAGE + 2).code == 0x201d);

    /* memory: the white list, the copy */
    memset(host_mem, 0xab, sizeof(host_mem));
    r = call(DBG_OP_GET, DBG_MEM, 0xa0001000, 256, 0);
    CHECK(r.code == 0x2001 && host_ptp_out_n == 256 && host_ptp_out[255] == 0xab);
    CHECK(call(DBG_OP_GET, DBG_MEM, 0x90000000, 4, 0).code == 0x201d && host_ptp_out_n == 0);
    CHECK(call(DBG_OP_GET, DBG_MEM, 0xa0001000, DBG_STAGE + 4, 0).code == 0x201d);
    r = call(DBG_OP_GET, DBG_HIST, 0, 0, 0);
    CHECK(r.code == 0x2001 && host_ptp_out_n == 488);
    CHECK(call(DBG_OP_GET, 77, 0, 0, 0).code == 0x2005);

    /* jobs: the path, a read, the task snapshot; paths outside OMKIT and DCIM are refused */
    CHECK(put(DBG_PATH, 0, "omkit/omkit.ini", 15).code == 0x2001);
    CHECK(call(DBG_OP_CMD, DBG_READ, 0, 1000, 0).code == 0x2001);
    CHECK(call(DBG_OP_CMD, DBG_SNAP, 0, 0, 0).code == 0x2019);           /* one job at a time */
    CHECK(job_wait() == 0);
    r = call(DBG_OP_CMD, DBG_STATUS, 0, 0, 0);
    CHECK(r.p[2] == (u32)file_size("OMKIT.INI") && r.p[4] == r.p[2]);
    r = call(DBG_OP_GET, DBG_XFER, 0, r.p[2], 0);
    CHECK(r.code == 0x2001 && !memcmp(host_ptp_out, "[debug]", 7));
    CHECK(call(DBG_OP_GET, DBG_XFER, 0, 100000, 0).code == 0x201d);
    /* the next piece from an offset; past the end - nothing, no error (the tool stops at a short piece) */
    CHECK(call(DBG_OP_CMD, DBG_READ, 1, 6, 0).code == 0x2001 && job_wait() == 0);
    r = call(DBG_OP_CMD, DBG_STATUS, 0, 0, 0);
    CHECK(r.p[2] == 6 && r.p[4] == 7);
    r = call(DBG_OP_GET, DBG_XFER, 0, 6, 0);
    CHECK(r.code == 0x2001 && !memcmp(host_ptp_out, "debug]", 6));
    CHECK(call(DBG_OP_CMD, DBG_READ, 100000, 10, 0).code == 0x2001 && job_wait() == 0);
    CHECK(call(DBG_OP_CMD, DBG_STATUS, 0, 0, 0).p[2] == 0);
    /* a folder: attributes, size (the firmware's entry +16), times, name */
    host_find = 1;
    CHECK(put(DBG_PATH, 0, "OMKIT", 5).code == 0x2001 && call(DBG_OP_CMD, DBG_LIST, 0, 0, 0).code == 0x2001);
    CHECK(job_wait() == 0);
    r = call(DBG_OP_CMD, DBG_STATUS, 0, 0, 0);
    CHECK(r.p[2] >= DBG_LIST_REC && r.p[2] % DBG_LIST_REC == 0 && !(r.p[4] >> 31));
    r = call(DBG_OP_GET, DBG_XFER, 0, r.p[2], 0);
    {
        u32 i, found = 0;

        for (i = 0; i < host_ptp_out_n; i += DBG_LIST_REC)
            if (!strcmp((char *)host_ptp_out + i + 16, "OMKIT.INI"))
                found = *(u32 *)(host_ptp_out + i) == 0x20 && *(u32 *)(host_ptp_out + i + 4) == (u32)file_size("OMKIT.INI") &&
                        *(u32 *)(host_ptp_out + i + 12) == 0x5d787263;
        CHECK(found);
    }
    host_find = 0;
    CHECK(call(DBG_OP_CMD, DBG_SNAP, 0, 0, 0).code == 0x2001 && job_wait() == 0);
    r = call(DBG_OP_CMD, DBG_STATUS, 0, 0, 0);
    CHECK(r.p[4] == 2 && r.p[2] == DBG_SNAP_HEAD + 2 * DBG_SNAP_TASK + DBG_SNAP_OBJ);
    r = call(DBG_OP_GET, DBG_XFER, 0, r.p[2], 0);
    CHECK(!memcmp(host_ptp_out, "OMKS", 4) && !strcmp((char *)host_ptp_out + DBG_SNAP_HEAD + DBG_SNAP_TASK + 72, "FILECLBK"));
    CHECK(put(DBG_PATH, 0, "DCIM\\..\\X", 9).code == 0x2001 && call(DBG_OP_CMD, DBG_READ, 0, 10, 0).code == 0x201d);
    CHECK(put(DBG_PATH, 0, "MISC\\A.TXT", 10).code == 0x2001 && call(DBG_OP_CMD, DBG_READ, 0, 10, 0).code == 0x201d);
    CHECK(put(DBG_PATH, 0, "OMKIT\\TOOLONGNAME.TXT", 21).code == 0x2001 && call(DBG_OP_CMD, DBG_READ, 0, 10, 0).code == 0x201d);
    CHECK(put(DBG_PATH, 0, "DCIM\\100OLYMP", 13).code == 0x2001 && call(DBG_OP_CMD, DBG_LIST, 0, 0, 0).code == 0x2001);
    CHECK(job_wait() == 0);
    CHECK(put(DBG_PATH, 0, "OMKIT\\NONE.TXT", 14).code == 0x2001 && call(DBG_OP_CMD, DBG_READ, 0, 10, 0).code == 0x2001);
    CHECK(job_wait() == (0x100 | DBG_E_OPEN));
    /* level and mark */
    r = call(DBG_OP_CMD, DBG_LEVEL, OMK_LOG_DEBUG, 0, 0);
    CHECK(r.code == 0x2001 && r.p[0] == OMK_LOG_INFO);
    CHECK(call(DBG_OP_CMD, DBG_LEVEL, 9, 0, 0).code == 0x201d);
    CHECK(call(DBG_OP_CMD, DBG_MARK, 5, 0, 0).code == 0x2001);
    service();
    log_printf(OMK_LOG_DEBUG, "kept now");
    s = walk(&b);
    CHECK(strstr(s.text + 1, " D kept now") && s.text[0] == OMK_LOG_DEBUG);
}

static u32 crc_of(const char *s, u32 n)
{
    return crc32_update(0, s, n);
}

static void test_write(void)
{
    static const char body[] = "new module image";
    drv_ptp_msg r;
    long n;

    memset(host_keep, 0, sizeof(host_keep));
    write_file("MODULES_OLD", "x");
    start("[debug]\r\nenabled = on\r\nwrite = on\r\n");
    CHECK(call(DBG_OP_CMD, DBG_HELLO, 0, 0, 0).p[3] & DBG_F_WRITE);
    /* writes: the OMKit folder only */
    CHECK(put(DBG_PATH, 0, "DCIM\\100OLYMP\\P1.JPG", 20).code == 0x2001);
    CHECK(call(DBG_OP_CMD, DBG_WBEGIN, 4, 0, 0).code == 0x201d);
    CHECK(put(DBG_PATH, 0, "OMKIT", 5).code == 0x2001 && call(DBG_OP_CMD, DBG_WBEGIN, 4, 0, 0).code == 0x201d);
    CHECK(put(DBG_PATH, 0, "OMKIT\\OMKDBG.TMP", 16).code == 0x2001 && call(DBG_OP_CMD, DBG_WBEGIN, 4, 0, 0).code == 0x2001);
    CHECK(job_wait() == (0x100 | DBG_E_PATH));
    CHECK(put(DBG_WDATA, 0, "abcd", 4).code == 0x201d);                 /* no file open */
    /* a file in a folder that is not there yet, in two pieces, then replaced; every card operation takes 5 ms (the job ends
     * later than the service pass began: the open file must not look idle) */
    host_fopen_us = 5000;
    CHECK(put(DBG_PATH, 0, "OMKIT\\MODULES\\HELLO.OMM", 23).code == 0x2001);
    CHECK(call(DBG_OP_CMD, DBG_WBEGIN, sizeof(body) - 1, crc_of(body, sizeof(body) - 1), 0).code == 0x2001);
    CHECK(job_wait() == 0);
    CHECK(put(DBG_WDATA, 4, body, 6).code == 0x201d);                   /* not where the file ends */
    CHECK(put(DBG_WDATA, 0, body, 6).code == 0x2001 && job_wait() == 0);
    CHECK(put(DBG_WDATA, 6, body + 6, 100).code == 0x201d);             /* past the size given */
    CHECK(put(DBG_WDATA, 6, body + 6, sizeof(body) - 7).code == 0x2001 && job_wait() == 0);
    CHECK(file_size("MODULES/HELLO.OMM") == -1 && file_size("MODULES/OMKDBG.TMP") >= 0);
    CHECK(call(DBG_OP_CMD, DBG_WCOMMIT, 0, 0, 0).code == 0x2001 && job_wait() == 0);
    n = file_size("MODULES/HELLO.OMM");
    CHECK(n == (long)sizeof(body) - 1 && file_size("MODULES/OMKDBG.TMP") == -1);
    host_fopen_us = 0;
    /* a wrong sum: the old file stays */
    CHECK(call(DBG_OP_CMD, DBG_WBEGIN, 3, 12345, 0).code == 0x2001 && job_wait() == 0);
    CHECK(put(DBG_WDATA, 0, "xyz", 3).code == 0x2001 && job_wait() == 0);
    CHECK(call(DBG_OP_CMD, DBG_WCOMMIT, 0, 0, 0).code == 0x2001 && job_wait() == (0x100 | DBG_E_CRC));
    CHECK(file_size("MODULES/HELLO.OMM") == n && file_size("MODULES/OMKDBG.TMP") == -1);
    /* too short: the old file stays; abort drops the temporary file */
    CHECK(call(DBG_OP_CMD, DBG_WBEGIN, 10, 0, 0).code == 0x2001 && job_wait() == 0);
    CHECK(call(DBG_OP_CMD, DBG_WCOMMIT, 0, 0, 0).code == 0x2001 && job_wait() == (0x100 | DBG_E_SIZE));
    CHECK(call(DBG_OP_CMD, DBG_WBEGIN, 10, 0, 0).code == 0x2001 && job_wait() == 0);
    CHECK(call(DBG_OP_CMD, DBG_WABORT, 0, 0, 0).code == 0x2001 && job_wait() == 0 && file_size("MODULES/OMKDBG.TMP") == -1);
    /* the restart: the key word; refused while a file is open; else the job ends first, the firmware's reboot request
     * follows half a second later, once; no power-off in 5 s - a warning, and a new restart may be asked for */
    CHECK(call(DBG_OP_CMD, DBG_HELLO, 0, 0, 0).p[3] & DBG_F_RESTART);
    CHECK(call(DBG_OP_CMD, DBG_RESTART, 1, 0, 0).code == 0x201d);
    CHECK(put(DBG_PATH, 0, "OMKIT/A.TXT", 11).code == 0x2001);
    CHECK(call(DBG_OP_CMD, DBG_WBEGIN, 10, 0, 0).code == 0x2001 && job_wait() == 0);
    CHECK(call(DBG_OP_CMD, DBG_RESTART, DBG_RESTART_KEY, 0, 0).code == 0x2001 && job_wait() == (0x100 | DBG_E_STATE));
    CHECK(call(DBG_OP_CMD, DBG_WABORT, 0, 0, 0).code == 0x2001 && job_wait() == 0);
    host_restarts = 0;
    CHECK(call(DBG_OP_CMD, DBG_RESTART, DBG_RESTART_KEY, 0, 0).code == 0x2001 && job_wait() == 0);
    CHECK(call(DBG_OP_CMD, DBG_STATUS, 0, 0, 0).p[4] == 500 && host_restarts == 0);
    CHECK(call(DBG_OP_CMD, DBG_RESTART, DBG_RESTART_KEY, 0, 0).code == 0x2001 && job_wait() == (0x100 | DBG_E_STATE));
    {
        u32 i;

        for (i = 0; i < 12; i++)                /* 600 ms */
            service();
        CHECK(host_restarts == 1);
        for (i = 0; i < 100; i++)               /* 5 s more: not taken */
            service();
        CHECK(host_restarts == 1);
    }
    CHECK(call(DBG_OP_CMD, DBG_RESTART, DBG_RESTART_KEY, 0, 0).code == 0x2001 && job_wait() == 0);

    /* the test scenario over USB: the steps of PUT TEXT; it waits while the camera does not shoot (host_app 0: not the
     * shooting state, as in the MTP mode), one at a time, stopped; a step not understood */
    CHECK(put(DBG_TEXT, 0, "keep\x01", 5).code == 0x201d);
    CHECK(put(DBG_TEXT, 0, "keep", 4).code == 0x2001);
    CHECK(call(DBG_OP_CMD, DBG_SHOOT, DBG_SHOOT_START, 0, 0).code == 0x2001 && job_wait() == 0);
    r = call(DBG_OP_CMD, DBG_SHOOT, DBG_SHOOT_STATUS, 0, 0);
    CHECK(r.p[0] == (SHOOT_RUNNING | 1 << 8) && r.p[2] == 0 && r.p[4] >> 16 == 1);
    CHECK(call(DBG_OP_CMD, DBG_SHOOT, DBG_SHOOT_START, 0, 0).code == 0x2001 && job_wait() == (0x100 | DBG_E_BUSY));
    CHECK(call(DBG_OP_CMD, DBG_SHOOT, DBG_SHOOT_STOP, 0, 0).code == 0x2001 && job_wait() == 0);
    CHECK(call(DBG_OP_CMD, DBG_SHOOT, DBG_SHOOT_STATUS, 0, 0).p[0] == SHOOT_STOPPED);
    CHECK(put(DBG_TEXT, 0, "look:", 5).code == 0x2001);
    CHECK(call(DBG_OP_CMD, DBG_SHOOT, DBG_SHOOT_START, 0, 0).code == 0x2001 && job_wait() == (0x100 | DBG_E_STEPS));
    r = call(DBG_OP_GET, DBG_INFO, 0, 0, 0);
    host_ptp_out[host_ptp_out_n] = 0;
    CHECK(strstr((char *)host_ptp_out, "\nshoot=stopped\n") != 0);
}

static void test_nocard(void)
{
    omk_store *s;
    u32 v = 5;

    memset(host_keep, 0, sizeof(host_keep));
    start("[debug]\r\nenabled = on\r\ncard_writes = off\r\n");
    CHECK(!core_card_ok() && (call(DBG_OP_CMD, DBG_HELLO, 0, 0, 0).p[3] & DBG_F_NO_CARD));
    s = store_open("TEST", 64);
    CHECK(s && store_set(s, 1, &v, 4, OMK_STORE_PERSIST) == 0);
    log_printf(OMK_LOG_INFO, "not on the card");
    host_ticks = (host_ticks + 20000000) & 0x3fffffff;
    store_service(omk_ms());
    store_save_all();
    log_flush();
    CHECK(file_size("STORE/TEST.STA") == -1 && file_size("STORE/TEST.STB") == -1 && file_size("LOG/CORE.LOG") == -1);
    CHECK(store_get(s, 1, &v, 4) == 4 && v == 5);
    CHECK(log_level() >= OMK_LOG_INFO);
}

int main(int argc, char **argv)
{
    const char *what = argc > 2 ? argv[2] : "ring";

    host_dir = argc > 1 ? argv[1] : ".";
    mem_init();
    if (!strcmp(what, "ring"))
        test_ring();
    else if (!strcmp(what, "memread"))
        test_memread();
    else if (!strcmp(what, "proto"))
        test_proto();
    else if (!strcmp(what, "write"))
        test_write();
    else if (!strcmp(what, "nocard"))
        test_nocard();
    else
        failed = 1;
    if (failed)
        fprintf(stderr, "%d checks failed\n", failed);
    else
        printf("ok\n");
    return failed != 0;
}
