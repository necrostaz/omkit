/* Tests of core parts that do not depend on the camera: cfg, log, store, clock. Run by tests/host/run.sh (make test).
 * The argument is an empty directory for files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "omk_core.h"
#include "omk_log.h"
#include "omk_store.h"
#include "omk_mem.h"
#include "omk_power.h"
#include "host_drv.h"

static int failed;

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);             \
            failed++;                                                           \
        }                                                                       \
    } while (0)

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

static char *read_file(const char *name, long *len)
{
    char p[512];
    FILE *f;
    char *b;

    snprintf(p, sizeof(p), "%s/%s", host_dir, name);
    f = fopen(p, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = calloc(1, *len + 1);
    fread(b, 1, *len, f);
    fclose(f);
    return b;
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

static void test_cfg_template(void)
{
    long n;
    char *t;

    CHECK(cfg_load() == 0);
    t = read_file("OMKIT.INI", &n);
    CHECK(t && strstr(t, "[core]") && strstr(t, "log_level = info"));
    CHECK(t && !strstr(t, "run_logs"));     /* the per-power-on log is gone (core 1.42): not in the sample */
    free(t);
    CHECK(cfg_bool("core", "log", 0) == 1);
    CHECK(!strcmp(cfg_str("CORE", "LOG_LEVEL", "?"), "info"));
}

/* The same cfg module cannot be loaded twice, so parsing is checked in a second process: mode "cfg" */
static void test_cfg_parse(void)
{
    write_file("OMKIT.INI",
               "\xef\xbb\xbf; comment\r\n"
               "# another\n"
               "  [ Core ]  \r\n"
               "  log =  off  \r\n"
               "log_level=debug\r\n"
               "broken line\r\n"
               "[profiles]\n"
               "count = 0x10\n"
               "neg = -7\n"
               "count = 12\n"
               "empty =\n"
               "bad_int = 12a\n"
               "size1 = 256K\n"
               "size2 = 3m\n"
               "size3 = 12\n"
               "size4 = 1x\n"
               "[unterminated\n");
    CHECK(cfg_load() == 1);
    CHECK(cfg_bool("core", "log", 1) == 0);
    CHECK(!strcmp(cfg_str("core", "log_level", "?"), "debug"));
    CHECK(cfg_int("profiles", "count", 0) == 12);      /* the last value */
    CHECK(cfg_int("profiles", "neg", 0) == -7);
    CHECK(cfg_int("profiles", "bad_int", 5) == 5);
    CHECK(!strcmp(cfg_str("profiles", "empty", "?"), ""));
    CHECK(cfg_int("profiles", "missing", 3) == 3);
    {
        u32 v = 0;

        CHECK(cfg_size("profiles", "size1", &v) == 1 && v == 256 * 1024);
        CHECK(cfg_size("profiles", "size2", &v) == 1 && v == 3 << 20);
        CHECK(cfg_size("profiles", "size3", &v) == 1 && v == 12);
        CHECK(cfg_size("profiles", "size4", &v) == -1);
        CHECK(cfg_size("profiles", "nothing", &v) == 0);
    }
    {
        u32 first, bad = cfg_bad_lines(&first);

        CHECK(bad == 2);
        CHECK(first == 6);
    }
}

extern u32 host_written, host_fopens;

static void test_log(void)
{
    long n, m;
    char *t;
    u32 before, removes = host_removes;
    int i;

    /* the log of the runs before is there: this run is appended to it, the file is never removed */
    omk_dir(LOG_DIR);
    write_file("LOG/CORE.LOG", "the run before\r\n");
    log_setup(1, OMK_LOG_INFO);
    log_banner("======== OMKit core %s, run %s ========", "1.0", "12345678");
    log_printf(OMK_LOG_INFO, "hex %08x %X dec %d %u str %s chr %c", 0xab, 0xbeef, -42, 7u, "abc", 'z');
    log_printf(OMK_LOG_DEBUG, "hidden");
    log_printf(OMK_LOG_WARN, "warned");
    log_flush();
    t = read_file("LOG/CORE.LOG", &n);
    CHECK(t && !strncmp(t, "the run before\r\n\r\n", 18) && strstr(t, " I ======== OMKit core 1.0, run 12345678 ========\r\n"));
    CHECK(t && strstr(t, " I hex 000000ab BEEF dec -42 7 str abc chr z\r\n") && !strstr(t, "hidden") &&
          strstr(t, " W warned\r\n"));
    CHECK(host_removes == removes);
    free(t);
    /* a flush appends only what is new: one line */
    before = host_written;
    log_printf(OMK_LOG_INFO, "second");
    log_flush();
    CHECK(host_written - before == 8 + 3 + 6 + 2);
    t = read_file("LOG/CORE.LOG", &m);
    CHECK(t && m == n + 19 && strstr(t, " I second\r\n") && strstr(t, "hex 000000ab"));
    free(t);
    /* nothing new: no file is even opened */
    before = host_fopens;
    log_flush();
    CHECK(host_fopens == before);
    /* the file stays open between portions (an fopen walks its FAT chain on the camera): one open for many; closed after
     * 30 s without a portion, and opened again by the next one */
    before = host_fopens;
    for (i = 0; i < 5; i++) {
        log_printf(OMK_LOG_INFO, "portion %d", i);
        drv_sleep(1000);
        log_service(omk_ms());
    }
    CHECK(host_fopens == before);
    drv_sleep(31000);
    log_service(omk_ms());                  /* nothing to write for 30 s: closed */
    log_printf(OMK_LOG_INFO, "after the pause");
    drv_sleep(600);
    log_service(omk_ms());
    CHECK(host_fopens == before + 1);
    t = read_file("LOG/CORE.LOG", &m);
    CHECK(t && strstr(t, " I portion 4\r\n") && strstr(t, " I after the pause\r\n"));
    free(t);
    /* the camera's PC mode: the open file no longer takes anything (0x9414) - it is closed and the portion goes to a file
     * opened anew, nothing lost and nothing stopped */
    host_files_stale();
    before = host_fopens;
    log_printf(OMK_LOG_INFO, "after the pc mode");
    log_flush();
    CHECK(host_fopens == before + 1);
    log_printf(OMK_LOG_INFO, "and on");
    log_flush();
    CHECK(host_fopens == before + 1);       /* the new file stays open */
    t = read_file("LOG/CORE.LOG", &m);
    CHECK(t && strstr(t, " I after the pc mode\r\n") && strstr(t, " I and on\r\n") && strstr(t, "was closed by the firmware (the card was mounted again), opened again"));
    free(t);
    /* the volume not mounted (USB Storage): nothing is opened or written until it is again */
    host_unmounted = 1;
    before = host_fopens;
    log_close();
    log_printf(OMK_LOG_INFO, "while unmounted");
    log_flush();
    CHECK(host_fopens == before);
    host_unmounted = 0;
    log_retry_now();
    log_flush();
    CHECK(host_fopens == before + 1);
    t = read_file("LOG/CORE.LOG", &n);      /* the length the later checks count from */
    CHECK(t && strstr(t, "while unmounted"));
    free(t);
    /* a long line is cut, '~' at the end */
    {
        char longs[400];

        for (i = 0; i < 399; i++)
            longs[i] = 'a' + i % 26;
        longs[399] = 0;
        log_printf(OMK_LOG_INFO, "%s", longs);
        log_flush();
        t = read_file("LOG/CORE.LOG", &m);
        CHECK(t && strstr(t, "~\r\n") && !strstr(t, longs));
        free(t);
    }
    /* the ring full while nothing is written: whole lines are dropped and counted, the rest arrives intact */
    for (i = 0; i < 700; i++)
        log_printf(OMK_LOG_INFO, "line %04d ..................................................", i);
    log_flush();
    t = read_file("LOG/CORE.LOG", &m);
    CHECK(t && strstr(t, "line 0000 ") && !strstr(t, "line 0699 "));
    free(t);
    log_flush();                            /* the warning itself */
    t = read_file("LOG/CORE.LOG", &m);
    CHECK(t && strstr(t, "lines lost, the buffer was full\r\n"));
    free(t);
    /* after a flush the ring has room again: 700 lines with flushes on the way, nothing lost */
    for (i = 0; i < 700; i++) {
        log_printf(OMK_LOG_INFO, "next %04d ..................................................", i);
        if (i % 100 == 99)
            log_flush();
    }
    log_flush();
    t = read_file("LOG/CORE.LOG", &m);
    CHECK(t && strstr(t, "next 0000 ") && strstr(t, "next 0699 ") && strstr(t, "next 0350 "));
    free(t);
    log_setup(0, OMK_LOG_DEBUG);
    log_printf(OMK_LOG_ERROR, "off");
    log_setup(1, OMK_LOG_INFO);
    log_flush();
    t = read_file("LOG/CORE.LOG", &n);
    CHECK(t && !strstr(t, "off"));
    free(t);
}

static void test_clock(void)
{
    u32 a, b;

    host_ticks = 0x3fffffff - 500000;          /* 0.5 s before the wrap */
    a = omk_ms();
    host_ticks = 1500000;                      /* across the wrap, 1.5 s more */
    b = omk_ms();
    CHECK(b - a == 2000);
}

static void test_store(void)
{
    omk_store *s;
    u32 v, keys, used, seq;
    char big[200], out[200];
    int i;
    u32 t0 = omk_ms(), opens;                  /* the core clock stands still until the test moves host_ticks */

    s = store_open("TEST", 256);
    CHECK(s != 0);
    CHECK(store_open("bad name", 16) == 0);
    v = 7;
    CHECK(store_set(s, 1, &v, 4, OMK_STORE_PERSIST) == 0);
    CHECK(store_set(s, 2, "temp", 4, 0) == 0);
    v = 0;
    CHECK(store_get(s, 1, &v, 4) == 4 && v == 7);
    CHECK(store_get(s, 3, &v, 4) == -1);
    CHECK(store_get(s, 2, out, 2) == 4 && !memcmp(out, "te", 2));

    store_service(t0 + 1000);                       /* 1 s after the change: too early */
    CHECK(file_size("STORE/TEST.STA") == -1);
    store_service(t0 + 1600);
    CHECK(file_size("STORE/TEST.STA") == 512);  /* the fixed length: 20 + 256 rounded up to a sector */

    memset(big, 'x', sizeof(big));
    CHECK(store_set(s, 3, big, 200, OMK_STORE_PERSIST) == 0);
    CHECK(store_set(s, 4, big, 100, 0) == -1);  /* no space */
    CHECK(store_set(s, 3, big, 150, OMK_STORE_PERSIST) == 0);   /* another length: the record is moved */
    CHECK(store_get(s, 3, out, sizeof(out)) == 150);
    store_info(s, &keys, &used, &seq);
    CHECK(keys == 3 && seq == 1);
    store_service(t0 + 5000);
    CHECK(file_size("STORE/TEST.STB") == 512);
    CHECK(store_del(s, 2) == 0 && store_del(s, 2) == -1);

    /* the same history with a new run: version 2 is loaded from TEST.STB; each slot file is opened once */
    opens = host_fopens;
    s = store_open("TEST", 256);
    CHECK(host_fopens == opens + 2);
    CHECK(store_get(s, 1, &v, 4) == 4 && v == 7);
    CHECK(store_get(s, 2, out, 4) == -1);      /* the temporary key is not saved */
    CHECK(store_get(s, 3, out, sizeof(out)) == 150);
    store_info(s, &keys, &used, &seq);
    CHECK(seq == 2);

    /* corrupt the new copy: the old one is used */
    {
        char p[512];
        FILE *f;

        snprintf(p, sizeof(p), "%s/STORE/TEST.STB", host_dir);
        f = fopen(p, "r+b");
        fseek(f, 30, SEEK_SET);
        fputc('!', f);
        fclose(f);
    }
    s = store_open("TEST", 256);
    store_info(s, &keys, &used, &seq);
    CHECK(seq == 1 && keys == 1);
    CHECK(store_get(s, 3, out, 4) == -1);
    for (i = 0; i < 3; i++)                    /* after the rollback writes go to the other file, not to the corrupted one */
        store_set(s, 10 + i, &i, 4, OMK_STORE_PERSIST);
    store_service(t0 + 20000);
    store_info(s, &keys, &used, &seq);
    CHECK(seq == 2 && keys == 4);
    /* the slot held a longer version before (192 bytes): the file keeps its length, zeros after the records */
    CHECK(file_size("STORE/TEST.STB") == 512);

    /* the newer version in the first slot (TEST.STA, version 3), an intact older one in the second: the first slot's
     * records are the ones loaded although the second file is read after it, and neither file is opened twice */
    v = 99;
    CHECK(store_set(s, 1, &v, 4, OMK_STORE_PERSIST) == 0);
    store_service(t0 + 30000);
    store_info(s, &keys, &used, &seq);
    CHECK(seq == 3 && file_size("STORE/TEST.STA") == 512);
    opens = host_fopens;
    s = store_open("TEST", 256);
    CHECK(host_fopens == opens + 2);
    store_info(s, &keys, &used, &seq);
    v = 0;
    CHECK(seq == 3 && keys == 4 && store_get(s, 1, &v, 4) == 4 && v == 99);
    i = -1;
    CHECK(store_get(s, 12, &i, 4) == 4 && i == 2);

    /* both slot files have the fixed length: saves rewrite them in place (r+), nothing is removed or made */
    {
        u32 removes = host_removes, inplace = host_inplace;

        for (i = 0; i < 4; i++) {
            v = 100 + i;
            store_set(s, 1, &v, 4, OMK_STORE_PERSIST);
            store_service(t0 + 40000 + 2000 * i);
        }
        CHECK(host_removes == removes && host_inplace == inplace + 4);
        CHECK(file_size("STORE/TEST.STA") == 512 && file_size("STORE/TEST.STB") == 512);
    }

    /* the files of an older core (the exact length of their records) are read as they are; each is made again once,
     * in its fixed length, at its first save, and rewritten in place after that */
    {
        static const unsigned char rec[8 + 4] = {1, 0, OMK_STORE_PERSIST, 0, 4, 0, 0, 0, 42, 0, 0, 0};
        unsigned char img[20 + sizeof(rec)];
        u32 h[5] = {0x534b4d4f, 1 | 1 << 16, 5, sizeof(rec), 0}, removes, inplace;
        char p[512];
        FILE *f;

        h[4] = crc32_update(0, rec, sizeof(rec));
        memcpy(img, h, 20);
        memcpy(img + 20, rec, sizeof(rec));
        snprintf(p, sizeof(p), "%s/STORE/OLD.STA", host_dir);
        f = fopen(p, "wb");
        fwrite(img, 1, sizeof(img), f);
        fclose(f);
        s = store_open("OLD", 64);
        store_info(s, &keys, &used, &seq);
        v = 0;
        CHECK(seq == 5 && keys == 1 && store_get(s, 1, &v, 4) == 4 && v == 42);
        removes = host_removes;
        inplace = host_inplace;
        for (i = 0; i < 3; i++) {           /* versions 6 (STB, not there), 7 (STA, the old length), 8 (STB) */
            v = 50 + i;
            store_set(s, 1, &v, 4, OMK_STORE_PERSIST);
            store_service(t0 + 60000 + 2000 * i);
        }
        CHECK(host_removes == removes + 2 && host_inplace == inplace + 1);
        CHECK(file_size("STORE/OLD.STA") == 512 && file_size("STORE/OLD.STB") == 512);
        s = store_open("OLD", 64);
        store_info(s, &keys, &used, &seq);
        v = 0;
        CHECK(seq == 8 && store_get(s, 1, &v, 4) == 4 && v == 52);

        /* the firmware refuses the close (its file module failing): the save counts as failed, the version is not
         * taken, and it is tried again */
        host_fclose_rc = 0x0a0f;
        v = 60;
        store_set(s, 1, &v, 4, OMK_STORE_PERSIST);
        store_service(t0 + 70000);
        store_info(s, &keys, &used, &seq);
        CHECK(seq == 8);
        host_fclose_rc = 0;
        store_service(t0 + 70000 + 3000);   /* the first pause of a failing save: 3 s */
        store_info(s, &keys, &used, &seq);
        CHECK(seq == 9);
    }
}

/* No card: the log and a store try again with a growing pause, not at every loop; the card back - all is written */
static void test_backoff(void)
{
    omk_store *s, *a, *b;
    u32 before, tries, i, v = 1, keys, used, seq;
    long n;
    char *t;

    log_flush();
    log_close();                            /* the card goes away with the camera's power (sleep): the file is closed */
    log_printf(OMK_LOG_INFO, "kept while the card is away");
    host_no_card = 1;
    before = host_fopens;
    for (i = 0; i < 4 * 600; i++) {         /* ten minutes of core loops, 250 ms apart */
        drv_sleep(250);
        log_service(omk_ms());
    }
    tries = host_fopens - before;           /* at 0.5 s, then 1, 2, 4 ... 32 s apart, then every 60 s: 15 */
    CHECK((tries >= 13 && tries <= 17) || (fprintf(stderr, "log tries %u\n", tries), 0));
    host_no_card = 0;
    before = host_fopens;
    drv_sleep(250);
    log_service(omk_ms());
    CHECK(host_fopens == before);           /* the card is back, but the pause is not over ... */
    log_retry_now();
    log_service(omk_ms());
    CHECK(host_fopens == before + 1);       /* ... unless core says the card may be back */
    log_flush();
    t = read_file("LOG/CORE.LOG", &n);
    CHECK(t && strstr(t, " I kept while the card is away\r\n") && strstr(t, "log: the card takes the log again, after "));
    free(t);
    log_printf(OMK_LOG_INFO, "after the card came back");
    before = host_fopens;
    drv_sleep(600);
    log_service(omk_ms());                  /* no pause left: written when the log goes quiet, as always, to the */
    CHECK(host_fopens == before);           /* file still open */
    t = read_file("LOG/CORE.LOG", &n);
    CHECK(t && strstr(t, "after the card came back"));
    free(t);

    s = store_open("BACK", 64);
    CHECK(s && store_set(s, 1, &v, 4, OMK_STORE_PERSIST) == 0);
    host_no_card = 1;
    before = host_fopens;
    for (i = 0; i < 4 * 600; i++) {
        drv_sleep(250);
        store_service(omk_ms());
    }
    tries = host_fopens - before;           /* at 1.5 s, then 3, 6 ... 48 s apart, then every 60 s: 14 */
    CHECK((tries >= 12 && tries <= 16) || (fprintf(stderr, "store tries %u\n", tries), 0));
    host_no_card = 0;
    drv_sleep(250);
    store_service(omk_ms());
    CHECK(file_size("STORE/BACK.STA") == -1);
    store_retry_now();
    store_service(omk_ms());
    CHECK(file_size("STORE/BACK.STA") == 512);
    v = 2;                                  /* a save that worked ended the pauses: the next change after 1.5 s */
    CHECK(store_set(s, 1, &v, 4, OMK_STORE_PERSIST) == 0);
    drv_sleep(1600);
    store_service(omk_ms());
    CHECK(file_size("STORE/BACK.STB") == 512);

    /* a value that keeps changing, each time within the 1.5 s delay: saved all the same, 12 s after the first change
     * not saved yet, and then every 12 s - not at every change, and not never */
    before = host_fopens;
    for (i = 0; i < 4 * 30; i++) {          /* 30 s, a change every 250 ms */
        v = 100 + i;
        CHECK(store_set(s, 1, &v, 4, OMK_STORE_PERSIST) == 0);
        drv_sleep(250);
        store_service(omk_ms());
        if (i == 4 * 11)
            CHECK(host_fopens == before);   /* 11 s: not yet */
    }
    CHECK(host_fopens - before == 2 || (fprintf(stderr, "store saves %u\n", host_fopens - before), 0));
    drv_sleep(1600);                        /* the changes end: the last value after the usual delay */
    store_service(omk_ms());
    CHECK(host_fopens - before == 3);
    v = 0;
    CHECK(store_get(s, 1, &v, 4) == 4 && v == 100 + 4 * 30 - 1);

    /* the slot of a closed store (a module that failed to start) is reused empty */
    a = store_open("OLDONE", 64);
    CHECK(a && store_set(a, 5, &v, 4, OMK_STORE_PERSIST) == 0);
    store_close(a);
    b = store_open("NEWONE", 64);
    CHECK(b == a);
    store_info(b, &keys, &used, &seq);
    CHECK(keys == 0 && used == 0 && seq == 0 && store_get(b, 5, &v, 4) == -1);
    store_service(omk_ms() + 100000);
    CHECK(file_size("STORE/NEWONE.STA") == -1);   /* nothing changed in it: nothing to save */
}

/* Word-wise copies against a byte reference: every alignment, both directions of overlap; CRC-32 check value */
static void test_copy(void)
{
    u8 a[96], b[96], ref[96];
    int d, sft, n, i, bad = 0;

    for (d = 0; d < 8; d++)
        for (sft = 0; sft < 8; sft++)
            for (n = 0; n < 40; n++) {
                for (i = 0; i < 96; i++)
                    a[i] = ref[i] = (u8)(i * 7 + 1);
                memmove(ref + 16 + d, ref + 16 + sft, n);
                mem_copy(a + 16 + d, a + 16 + sft, n);
                bad += memcmp(a, ref, 96) != 0;
                for (i = 0; i < 96; i++)
                    b[i] = ref[i] = 0x55;
                memset(ref + d, 0xa7, n);
                mem_set(b + d, 0xa7, n);
                bad += memcmp(b, ref, 96) != 0;
            }
    CHECK(bad == 0);
    CHECK(crc32_update(0, "123456789", 9) == 0xcbf43926);
    CHECK(crc32_update(crc32_update(0, "1234", 4), "56789", 5) == 0xcbf43926);
}

static char *log_text(void)
{
    long n;

    log_flush();
    return read_file("LOG/CORE.LOG", &n);
}

/* Memory manager: 64 KB arena, firmware heap classes 8 x 128, 4 x 2048, 4 x 16384 from host_drv.c */
static void test_mem(void)
{
    u32 total, used, largest, full, i;
    omk_mem_info mi;
    char *t;
    void *a, *b, *c, *d, *big, *filler, *y[4];
    int hello, mod;

    host_region_size = 64 * 1024;
    mem_init();
    log_setup(1, OMK_LOG_DEBUG);
    mem_arena(&total, &used, &full);
    CHECK(total == 65504 && used == 0 && full == 65488);

    a = mem_alloc_for(MEM_OWNER_CORE, 100, 0);
    CHECK(a && ((uptr)a & 15) == 0);
    hello = mem_owner("HELLO", 4096);
    CHECK(hello == 1 && !strcmp(mem_owner_name(hello), "HELLO"));
    b = mem_alloc_for(hello, 1000, 0);
    c = mem_alloc_for(hello, 1000, 0);
    d = mem_alloc_for(hello, 3000, 0);         /* above the 4096 limit */
    CHECK(b && c && !d);
    mem_info(hello, &mi);
    CHECK(mi.used == 2 * 1024 && mi.allocs == 2 && mi.fails == 1 && mi.limit == 4096);
    memset(b, 0xab, 1000);
    memset(c, 0xcd, 1000);

    /* merging: the middle one, then the right and left neighbours */
    mem_free(b);
    mem_free(c);
    mem_free(a);
    mem_arena(&total, &used, &largest);
    CHECK(used == 0 && largest == full);
    mem_info(hello, &mi);
    CHECK(mi.used == 0 && mi.peak == 2048);

    /* MEM_LINES: the data starts and ends on a 64-byte boundary wherever the free block begins (after blocks of 16,
     * 32, 48 and 64 bytes of data), the piece left in front is a free block again, and everything merges back */
    for (i = 1; i <= 4; i++) {
        void *pre = mem_alloc_for(MEM_OWNER_CORE, 16 * i, 0), *after;
        u8 *img = mem_alloc_for(hello, 1000, MEM_IMAGE | MEM_LINES);
        u32 before_used;

        u8 *natural = (u8 *)pre + 16 * i + 16;              /* where the data would be without the flag */

        CHECK(pre && img && ((uptr)img & 63) == 0);
        CHECK(img == natural || (img - natural >= 64 && img - natural < 128));
        mem_info(hello, &mi);
        CHECK(mi.used == 1024 + 16);        /* 1000 up to 16 lines, and the header */
        if (img != natural) {               /* the piece in front is a free block: first fit takes it */
            after = mem_alloc_for(MEM_OWNER_CORE, 16, 0);
            CHECK((u8 *)after == natural);
            mem_free(after);
        }
        after = mem_alloc_for(MEM_OWNER_CORE, 200, 0);      /* too large for the piece: right behind the image */
        CHECK((u8 *)after == img + 1024 + 16);
        memset(img, 0xee, 1024);            /* the whole of its lines is the block's own */
        mem_arena(&total, &before_used, &largest);
        mem_free(after);
        mem_free(img);
        mem_free(pre);
        mem_arena(&total, &used, &largest);
        CHECK(before_used > used && used == 0 && largest == full);
    }
    /* the last free block of the arena, 2048 bytes: with the piece in front (112 bytes at most) 1920 always fit */
    a = mem_alloc_for(MEM_OWNER_CORE, 65488 - 2048 - 16, 0);
    mem_arena(&total, &used, &largest);
    CHECK(a && largest == 2048);
    b = mem_alloc_for(hello, 2048 - 128, MEM_IMAGE | MEM_LINES);
    CHECK(b && ((uptr)b & 63) == 0);
    mem_free_owner(hello, 0);
    mem_free(a);
    mem_arena(&total, &used, &largest);
    CHECK(used == 0 && largest == full);
    mem_info(hello, &mi);
    CHECK(mi.used == 0);

    /* double and foreign frees do not break the arena */
    a = mem_alloc_for(hello, 64, 0);
    mem_free(a);
    mem_free(a);
    b = mem_alloc_for(hello, 64, 0);
    memset(b, 0, 64);
    mem_free((char *)b + 32);
    mem_free(b);
    mem_arena(&total, &used, &largest);
    CHECK(used == 0 && largest == full);

    /* another CPU: refused, the block is left untouched */
    a = mem_alloc_for(hello, 64, 0);
    host_cpu = 1;
    CHECK(mem_alloc_for(hello, 64, 0) == 0);
    mem_free(a);
    host_cpu = 0;
    mem_arena(&total, &used, &largest);
    CHECK(used == 80);
    mem_free(a);

    /* firmware heap: disabled - refused, enabled - data only, with a limit and a reserve for the firmware */
    mod = mem_owner("PROFILES", 0);
    big = mem_alloc_for(mod, 60000, MEM_IMAGE);
    filler = mem_alloc_for(mod, 65504 - 60016 - 16, MEM_IMAGE);     /* the arena is fully used */
    CHECK(big && filler);
    mem_arena(&total, &used, &largest);
    CHECK(used == total && largest == 0);
    CHECK(mem_alloc_for(mod, 8000, 0) == 0);
    mem_setup(1, 8192);
    CHECK(mem_alloc_for(mod, 8000, MEM_IMAGE) == 0);    /* code and resources: arena only */
    for (i = 0; i < 4; i++)
        y[i] = mem_alloc_for(mod, 1000, 0);    /* class 2048: a quarter is left to the firmware */
    CHECK(y[0] && y[1] && y[2] && !y[3] && host_fw_out[1] == 3);
    mem_info(mod, &mi);
    CHECK(mi.fw_used == 3 * 1024);
    mem_setup(1, 3 * 1024 + 32);               /* 40 bytes with the header take 64, they do not fit */
    CHECK(mem_alloc_for(mod, 40, 0) == 0);      /* heap limit */
    mem_setup(1, 8192);
    CHECK(mem_alloc_for(mod, 20000, 0) == 0);   /* larger than a heap block */
    for (i = 0; i < 3; i++)
        mem_free(y[i]);
    CHECK(host_fw_out[1] == 0);
    mem_info(mod, &mi);
    CHECK(mi.fw_used == 0 && mi.used == 65504 && mi.peak == 65504 + 3 * 1024);

    /* service loop: repeated failures, arena fill level, the firmware heap taken by someone else */
    host_fw_out[2] = 3;
    mem_service(100000);
    mem_service(111000);
    host_fw_out[2] = 0;
    t = log_text();
    CHECK(t && strstr(t, " W memory: HELLO wants 3000 bytes, over its limit;"));
    CHECK(t && strstr(t, " W memory: HELLO 1 more failures, last 64 bytes"));   /* the first HELLO failure already happened */
    CHECK(t && strstr(t, " E memory: free ") && strstr(t, "from another cpu, block kept"));
    CHECK(t && strstr(t, "block header damaged or not in use"));
    CHECK(t && strstr(t, " W memory: PROFILES wants 8000 bytes; arena free"));
    CHECK(t && strstr(t, " W memory: PROFILES 4 more failures, last 20000 bytes"));
    CHECK(t && strstr(t, " W memory: arena 100% used, 0 bytes free, largest 0"));
    CHECK(t && strstr(t, " I firmware heap 16384: 1 of 4 blocks free, 0 of them ours in use"));
    CHECK(t && strstr(t, " D memory: PROFILES used 65504 peak 68576 allocs 5 fails 5 firmware heap 0"));
    free(t);
    mem_free(big);
    mem_free(filler);
    mem_arena(&total, &used, &largest);
    CHECK(used == 0 && largest == full);
}

static u32 now_after(u32 ms)
{
    drv_sleep(ms);
    return omk_ms();
}

/* core/module.c is not linked here: record what power.c tells the modules */
static char power_trace[64];
static u32 npower;

void modules_power(u32 event)
{
    if (npower < sizeof(power_trace) - 1)
        power_trace[npower++] = "?ORSW"[event < 5 ? event : 0];
}

/* Power states (core/power.c): the hooks run "in a firmware task" (here: the same thread), the core task is power_service */
static void test_power(void)
{
    u32 now, t0;
    char *t;
    long len;

    log_setup(1, OMK_LOG_DEBUG);
    power_install();
    now = now_after(0);
    CHECK(power_service(now) == 1 && power_running());

    /* power-off */
    power_hook(POWER_HOOK_OFF, 1);
    CHECK(!power_running());
    CHECK(power_service(now = now_after(250)) == 0);       /* handled: last save, acknowledged, still not running */
    t0 = host_ticks;
    power_hook(POWER_HOOK_CARD_OFF, 0);                     /* gate: acknowledged already, no wait */
    CHECK(((host_ticks - t0) & 0x3fffffff) < 20000);

    /* nothing happened for 5 s: power-off cancelled, running again */
    CHECK(power_service(now = now_after(POWER_CANCEL_MS)) == 0);
    CHECK(power_service(now = now_after(250)) == 1);

    /* card unmount without the power-off hook: it is the signal; the core task does not answer, the gate gives up */
    t0 = host_ticks;
    power_hook(POWER_HOOK_CARD_OFF, 0);
    CHECK(((host_ticks - t0) & 0x3fffffff) >= POWER_GATE_MS * 1000 && ((host_ticks - t0) & 0x3fffffff) < (POWER_GATE_MS + 50) * 1000);
    CHECK(power_service(now = now_after(10)) == 0);
    CHECK(power_service(now = now_after(POWER_CANCEL_MS)) == 0);
    CHECK(power_service(now = now_after(250)) == 1);

    /* sleep and wake; the gate does not wait once the core task has acknowledged */
    power_hook(POWER_HOOK_SLEEP, 0);
    CHECK(!power_running());
    CHECK(power_service(now = now_after(250)) == 0);
    t0 = host_ticks;
    power_hook(POWER_HOOK_SLEEP_GATE, 300000);
    CHECK(((host_ticks - t0) & 0x3fffffff) < 20000);
    host_ticks = (host_ticks + 60000000) & 0x3fffffff;     /* a minute asleep */
    power_hook(POWER_HOOK_WAKE, 0);
    CHECK(power_service(now = now_after(250)) == 1);

    /* a long sleep: the tick counter wrapped 3 times unseen (it shows 60 s), the subprocessor clock 3281 s; the hooks wake
     * the core task */
    {
        extern int host_wakes;
        extern u32 host_rtc_sec;
        int wakes = host_wakes;
        u32 m0;

        host_rtc_sec = 1000;
        power_hook(POWER_HOOK_SLEEP, 0);
        CHECK(host_wakes == wakes + 1);
        CHECK(power_service(now = now_after(250)) == 0);
        host_ticks = (host_ticks + 60000000) & 0x3fffffff;
        host_rtc_sec += 3281;
        power_hook(POWER_HOOK_WAKE, 0);
        CHECK(host_wakes == wakes + 2);
        m0 = omk_ms();
        CHECK(power_service(now = now_after(250)) == 1);
        CHECK(omk_ms() - m0 >= 3 * 1073742 && omk_ms() - m0 < 3 * 1073742 + 1000);
        /* a short sleep with the clock set forward by the user meanwhile: no whole turns, nothing added */
        power_hook(POWER_HOOK_SLEEP, 0);
        CHECK(power_service(now = now_after(250)) == 0);
        host_ticks = (host_ticks + 10000000) & 0x3fffffff;
        host_rtc_sec += 10 + 3600;
        power_hook(POWER_HOOK_WAKE, 0);
        m0 = omk_ms();
        CHECK(power_service(now = now_after(250)) == 1);
        CHECK(omk_ms() - m0 < 1000);
    }

    /* sleep prepared but never entered: running again after POWER_SLEEP_ABORT_MS */
    power_hook(POWER_HOOK_SLEEP, 0);
    CHECK(power_service(now = now_after(250)) == 0);
    CHECK(power_service(now = now_after(POWER_SLEEP_ABORT_MS - 500)) == 0);
    CHECK(power_service(now = now_after(500)) == 0);
    CHECK(power_service(now = now_after(250)) == 1);

    /* the lever flag: found at start (after the hooks), polled as a fallback, ignored while it stays set after a cancel */
    host_power_pending = 1;
    power_install();
    CHECK(!power_running());
    CHECK(power_service(now = now_after(250)) == 0);
    CHECK(power_service(now = now_after(POWER_CANCEL_MS)) == 0);   /* alive after 5 s: cancelled, flag still set */
    CHECK(power_service(now = now_after(250)) == 1);
    CHECK(power_service(now = now_after(250)) == 1);               /* ignored, no new power-off */
    host_power_pending = 0;
    CHECK(power_service(now = now_after(250)) == 1);
    host_power_pending = 1;                                         /* set again: the hook missed it - polled */
    CHECK(power_service(now = now_after(250)) == 0);
    host_power_pending = 0;
    CHECK(power_service(now = now_after(POWER_CANCEL_MS)) == 0);
    CHECK(power_service(now = now_after(250)) == 1);

    /* modules were told: off, resume (cancel), off, resume (card off without the hook), sleep, wake, sleep, wake (sleep
     * did not happen), off, resume (flag at start), off, resume (polled) */
    CHECK(!strcmp(power_trace, "ORORSWSWSWSWOROR") || (fprintf(stderr, "power trace %s\n", power_trace), 0));

    log_flush();
    t = read_file("LOG/CORE.LOG", &len);
    CHECK(t && strstr(t, "power: 1 of 1 hooks installed"));
    CHECK(t && strstr(t, "power: power-off already under way at start, modules wait"));
    CHECK(t && strstr(t, "power-off: power lever before core started, "));
    CHECK(t && strstr(t, "power: lever flag still set, ignored until it clears"));
    CHECK(t && strstr(t, "power-off: power lever flag (polled), "));
    CHECK(t && strstr(t, "power probe: lever 1, "));
    CHECK(t && strstr(t, "power-off: power lever, "));
    CHECK(t && strstr(t, "power: card off, core ready, waited 0 ms"));
    CHECK(t && strstr(t, "power-off did not happen, running again"));
    CHECK(t && strstr(t, "power: card off, core not ready after 1000 ms, going on"));
    CHECK(t && strstr(t, "power-off: reason 0, "));
    CHECK(t && strstr(t, "sleep: card writes stopped"));
    CHECK(t && strstr(t, "power: sleep, core ready, waited 0 ms"));
    CHECK(t && strstr(t, "wake: result 0, 60"));
    CHECK(t && strstr(t, "sleep did not happen, running again"));
    free(t);
}

/* The optics probe's cached read (core/optics.c, [optics] probe = 2), one power-on a phase in the same folder:
 *   1 - run 7 starts; no arming before a job's report; then the arming: the line and the reports before it in the log
 *       file and "armed" in the store, both on the card, when the driver gets it; a read reported - armed again after the
 *       next report; power-off with the arming not taken;
 *   2 - the run before ended without a power-off while armed: counted as a reset, no arming any more in run 7;
 *   3 - [optics] run = 8: starts again, arms. The log file grows over the phases (one log for all power-ons) */
static int opt_arm_ok;
static int opt_store_armed(void);

/* drv_optics_arm: the log on the card has the reports before the arming and its own line, the store says "armed" */
static void opt_on_arm(void)
{
    long n;
    char *t = read_file("LOG/CORE.LOG", &n);

    opt_arm_ok = t && strstr(t, "optics: job report before the arming") &&
                 strstr(t, "optics: arming the cached read for the next job") && opt_store_armed() == 1;
    free(t);
}

/* "armed" of the store OPTICS on the card, -1 none; *reads its read count. Of the two copies the one saved last (the
 * record's own save count; one record, key 1: its value is right after the file header, 20 bytes, and the record
 * header, 8) */
static int opt_store_rec(u32 *reads)
{
    static const char *const name[2] = {"STORE/OPTICS.STA", "STORE/OPTICS.STB"};
    u32 w[6], best = 0;
    int k, armed = -1;
    long n;
    char *t;

    for (k = 0; k < 2; k++) {
        t = read_file(name[k], &n);
        if (t && n >= 28 + 24) {
            memcpy(w, t + 28, sizeof(w));
            if (w[5] > best) {
                best = w[5];
                armed = (int)w[1];
                if (reads)
                    *reads = w[2];
            }
        }
        free(t);
    }
    return armed;
}

static int opt_store_armed(void)
{
    return opt_store_rec(0);
}

static void test_optics(int phase)
{
    long n;
    char *t;
    u32 reads = 0;

    omk_dir(LOG_DIR);
    omk_dir(STORE_DIR);
    write_file("OMKIT.INI", phase < 3 ? "[optics]\nprobe = 2\nrun = 7\n" : "[optics]\nprobe = 2\nrun = 8\n");
    CHECK(cfg_load() == 1);
    log_setup(1, OMK_LOG_INFO);
    host_prev_ended = phase != 2;
    memset(&host_opt, 0, sizeof(host_opt));
    host_opt_on_arm = opt_on_arm;
    optics_setup();
    CHECK(host_opt_level == 2);
    optics_service(1000);
    CHECK(host_opt_arms == 0);              /* no report yet: nothing to have on the card first */
    log_printf(OMK_LOG_INFO, "optics: job report before the arming");
    host_opt.jobs = 1;
    optics_service(1250);
    if (phase == 2) {
        CHECK(host_opt_arms == 0);
        log_flush();
        t = read_file("LOG/CORE.LOG", &n);
        CHECK(t && strstr(t, "counted as a reset by the cached read") && strstr(t, "1 resets - off"));
        free(t);
        CHECK(opt_store_armed() == 0);
        return;
    }
    CHECK(host_opt_arms == 1 && opt_arm_ok);
    CHECK(opt_store_armed() == 1);          /* on the card before the driver got it */
    optics_service(1500);
    CHECK(host_opt_arms == 1);              /* armed, not taken: no second arming */
    /* the arming taken by a job: the read counted and "armed" cleared on the card at once */
    host_opt.armed = 0;
    host_opt.cached = host_opt.cached_same = 1;
    optics_service(1750);
    CHECK(host_opt_arms == 1 && opt_store_rec(&reads) == 0 && reads == 1);
    /* its report: the next arming - not while the log cannot be written, then 2 s later */
    host_opt.jobs = 2;
    opt_arm_ok = 0;
    host_no_card = 1;
    optics_service(2000);
    CHECK(host_opt_arms == 1 && opt_store_armed() == 0);
    host_no_card = 0;
    optics_service(2250);
    CHECK(host_opt_arms == 1);
    optics_service(4100);
    CHECK(host_opt_arms == 2 && opt_arm_ok);
    CHECK(opt_store_rec(&reads) == 1 && reads == 1);
    if (phase == 3) {
        log_flush();
        t = read_file("LOG/CORE.LOG", &n);
        CHECK(t && strstr(t, "cached read test run 8 starts"));
        free(t);
    }
}

/* The optics probe's bisection ([optics] bisect, core 1.34), one power-on a phase in the same folder:
 *   1 - run 1 starts at N (no step until armed); every substage is armed only with its line in the log and "armed" in the
 *       store on the card; a job ended with it - passed, the next one armed: N, A0, then A armed and the run "resets";
 *   2 - the run before ended without a power-off with A armed: "A RESET THE CAMERA", the stills stay at A0, no arming;
 *       the crumbs of the run before are logged (the memory kept them);
 *   3 - [optics] bisect = 2: starts again at N;
 *   4..6 - a run from C3: C3 (a test) resets, the bisection goes on; the worker made before W1 is armed, never at start;
 *       W1 resets - the end, the stills at C2; 7 - from W1 with no worker: stopped before it */
static void test_bisect(int phase)
{
    long n;
    char *t;
    u32 k;

    omk_dir(LOG_DIR);
    omk_dir(STORE_DIR);
    if (phase >= 4) {                       /* bisect_from; a test (C3) that resets: the bisection goes on; the worker */
        write_file("OMKIT.INI", phase < 7 ? "[optics]\nprobe = 1\nbisect = 3\nbisect_from = C3\n" :
                                            "[optics]\nprobe = 1\nbisect = 4\nbisect_from = W0\n");
        CHECK(cfg_load() == 1);
        log_setup(1, OMK_LOG_INFO);
        host_prev_ended = phase == 4 || phase == 7;
        host_opt_worker_rc = phase == 7 ? 4 : 0;
        host_opt_workers = 0;
        memset(&host_opt, 0, sizeof(host_opt));
        optics_setup();
        CHECK(host_opt_sub == DRV_OPTICS_C2);   /* until the arming: the one before C3 (or before W1, C3 being a test) */
        CHECK(host_opt_workers == 0);       /* not at start */
        optics_service(1000);
        if (phase == 4) {
            CHECK(host_opt_sub == DRV_OPTICS_C3 && host_opt_workers == 0);
            return;                         /* the "reset" with C3 armed */
        }
        log_flush();
        t = read_file("LOG/CORE.LOG", &n);
        if (phase == 5) {                   /* C3 marked; the worker made, then W0 armed; then the "reset" with W0 armed */
            CHECK(host_opt_sub == DRV_OPTICS_W0 && host_opt_workers == 1);
            CHECK(t && strstr(t, "bisection run 3 starts at C3") && strstr(t, "substage C3 RESET THE CAMERA") &&
                  strstr(t, "the test C3 reset the camera") && strstr(t, "substage W0 armed for the next still (the worker "
                  "task made first)") && strstr(t, "the worker task runs: id 119"));
            CHECK(t && strstr(t, "substage W0 armed") < strstr(t, "the worker task runs"));   /* armed on the card first */
        } else if (phase == 6) {            /* W0 is no test: the bisection ends there, the stills at C2 */
            CHECK(host_opt_sub == DRV_OPTICS_C2 && host_opt_workers == 0);
            CHECK(t && strstr(t, "substage W0 RESET THE CAMERA") && strstr(t, "W0 reset the camera, the stills stay at C2"));
        } else {                            /* no worker: stopped before W0, "armed" taken back, asked once */
            optics_service(4000);
            CHECK(host_opt_sub == DRV_OPTICS_C2 && host_opt_workers == 1);
            free(t);
            log_flush();
            t = read_file("LOG/CORE.LOG", &n);
            CHECK(t && strstr(t, "the worker task not running, code 4") && strstr(t, "bisection stopped before W0"));
        }
        free(t);
        return;
    }
    write_file("OMKIT.INI", phase < 3 ? "[optics]\nprobe = 1\nbisect = 1\n" : "[optics]\nprobe = 1\nbisect = 2\n");
    CHECK(cfg_load() == 1);
    log_setup(1, OMK_LOG_INFO);
    host_prev_ended = phase != 2;
    memset(&host_opt, 0, sizeof(host_opt));
    if (phase == 2) {                       /* the step's crumbs left by the run before */
        host_opt_crumb[0] = 0x4f435242u;
        host_opt_crumb[3] = 3 << 16 | DRV_OPTICS_A << 8 | 7;
    }
    optics_setup();
    CHECK(host_opt_level == 1);
    if (phase == 2) {
        CHECK(host_opt_sub == DRV_OPTICS_A0);
        for (k = 0; k < 4; k++)
            optics_service(1000 + 250 * k);
        CHECK(host_opt_sub == DRV_OPTICS_A0);
        log_flush();
        t = read_file("LOG/CORE.LOG", &n);
        CHECK(t && strstr(t, "substage A RESET THE CAMERA") && strstr(t, "the stills stay at A0"));
        CHECK(t && strstr(t, "crumbs of the run before") && strstr(t, "job 3 at A: last phase '250 ms busy'"));
        free(t);
        return;
    }
    CHECK(host_opt_sub == DRV_OPTICS_N);
    for (k = DRV_OPTICS_N; k <= DRV_OPTICS_A; k++) {
        optics_service(1000 + 1000 * k);    /* armed: its line on the card, "armed" in the store, then the driver */
        CHECK(host_opt_sub == k);
        t = read_file("LOG/CORE.LOG", &n);
        CHECK(t && strstr(t, k == DRV_OPTICS_N ? "substage N armed" : k == DRV_OPTICS_A0 ? "substage A0 armed" :
                                                                                         "substage A armed"));
        free(t);
        if (k == DRV_OPTICS_A)
            break;                          /* the "reset" with A armed: the process ends here */
        optics_service(1250 + 1000 * k);
        CHECK(host_opt_sub == k);           /* no job of it ended yet: no move */
        host_opt.ended[k]++;
        host_ui_state = DRV_UI_HALF;        /* the job ended, the application still in its half-press state */
        optics_service(1400 + 1000 * k);
        optics_service(1450 + 1000 * k);
        CHECK(host_opt_sub == k);           /* not passed: else the second call would have armed the next one */
        host_ui_state = 0;
        optics_service(1500 + 1000 * k);    /* passed; the next one armed at the next loop */
    }
    if (phase == 3) {
        log_flush();
        t = read_file("LOG/CORE.LOG", &n);
        CHECK(t && strstr(t, "bisection run 2 starts") && strstr(t, "substage A0 passed"));
        free(t);
    }
}

/* The optics schedule ([optics] schedule, no bisection): nothing of ours until the worker runs and its test is done;
 * then one entry a press - the next one only when a job ended and the application is in a lasting state; the loop's
 * length or W1's sleep given with each; bad entries left out; the last entry kept */
static void test_schedule(void)
{
    long n;
    char *t;
    u32 k;
    static const u32 want_sub[] = {DRV_OPTICS_W0, DRV_OPTICS_W1, DRV_OPTICS_W0, DRV_OPTICS_W1, DRV_OPTICS_C3};
    static const u32 want_loop[] = {0, 0, 0, 100000, 20000};
    static const u32 want_sleep[] = {0, 20, 0, 0, 0};

    omk_dir(LOG_DIR);
    write_file("OMKIT.INI", "[optics]\nprobe = 1\nworker_test = 1\n"
                            "schedule = W0, W1/s20, w0 bogus W1/5 W2/s5, W1/100000,C3/20000\n");
    CHECK(cfg_load() == 1);
    log_setup(1, OMK_LOG_INFO);
    host_prev_ended = 1;
    host_opt_worker_rc = 0;
    host_opt_workers = 0;
    host_ui_state = 0;
    memset(&host_opt, 0, sizeof(host_opt));
    optics_setup();
    CHECK(host_opt_level == 1 && host_opt_sub == DRV_OPTICS_N && host_opt_workers == 0);
    optics_service(1000);                   /* the worker made; its test still to do: nothing given */
    CHECK(host_opt_workers == 1 && host_opt_sub == DRV_OPTICS_N);
    optics_service(1250);                   /* the test (no worker in the host driver: "no worker") */
    optics_service(1500);
    CHECK(host_opt_sub == want_sub[0] && host_opt_loop == 0 && host_opt_sleep == 0);
    for (k = 1; k < 5; k++) {
        optics_service(2000 * k);
        CHECK(host_opt_sub == want_sub[k - 1]);  /* no job ended: no move */
        host_opt.ended[want_sub[k - 1]]++;
        host_ui_state = DRV_UI_HALF;        /* ended, the application still in its half-press state: no move */
        optics_service(2000 * k + 100);
        CHECK(host_opt_sub == want_sub[k - 1]);
        host_ui_state = 0;
        optics_service(2000 * k + 200);
        CHECK(host_opt_sub == want_sub[k] && host_opt_loop == want_loop[k] && host_opt_sleep == want_sleep[k]);
    }
    host_opt.ended[DRV_OPTICS_C3]++;
    optics_service(11000);
    CHECK(host_opt_sub == DRV_OPTICS_C3 && host_opt_loop == 20000);   /* the last one kept */
    log_flush();
    t = read_file("LOG/CORE.LOG", &n);
    CHECK(t && strstr(t, "optics: schedule of 5 entries, one a press; the worker task first"));
    CHECK(t && strstr(t, "'bogus' is not a substage") && strstr(t, "'W1/5' is not") && strstr(t, "'W2/s5' is not"));
    CHECK(t && strstr(t, "schedule 2 of 5: W1 with a sleep of 20 ms instead of the loop, given to the next press"));
    CHECK(t && strstr(t, "schedule 4 of 5: W1 with a loop of 100000 iterations"));
    CHECK(t && strstr(t, "schedule 5 of 5: C3 with a loop of 20000 iterations"));
    free(t);
}

/* The optics layer's settings ([optics] bloom, halation, ...; core/optics.c): 1 - strengths without a probe or a
 * schedule: the jobs get L4, the settings as given and as defaults, the log line; 2 - a schedule of layer substages
 * without strengths: the layer on, route 403, the byte positions given, a sigma below the transport's warned of */
extern drv_optics_layer_cfg host_opt_layer;
extern u8 host_look_str[2];
extern int host_look_str_on, host_look_rows;
extern u32 host_opt_str, host_opt_strs;

static void test_layer(int phase)
{
    long n;
    char *t;

    omk_dir(LOG_DIR);
    write_file("OMKIT.INI", phase == 1 ? "[optics]\nbloom = 5\nhalation = 2\nbloom_sigma = 80\nbloom_sigma2 = 200\n"
                                         "bloom_mix = 25\nhalation_cb = -50\n" :
                                         "[optics]\nroute = 403\nlayer_bytes = 120\nlayer_width = 648\n"
                                         "halation = 1\nhalation_sigma = 10\nlayer_cached = 1\n"
                                         "schedule = L0, L1, l2, L4\n");
    CHECK(cfg_load() == 1);
    log_setup(1, OMK_LOG_INFO);
    host_prev_ended = 1;
    host_opt_worker_rc = 0;
    host_opt_sub = 99;
    memset(&host_opt_layer, 0, sizeof(host_opt_layer));
    memset(&host_opt, 0, sizeof(host_opt));
    optics_setup();
    CHECK(host_opt_level == 0);
    if (phase == 1) {
        CHECK(host_opt_sub == DRV_OPTICS_L4);
        CHECK(host_opt_layer.route == DRV_OPTICS_PAIR && host_opt_layer.width == 324);
        CHECK(host_opt_layer.order == DRV_OPTICS_ORDER_AUTO && host_opt_layer.gamma == 220);
        CHECK(host_opt_layer.tb == 200 && host_opt_layer.th == 225);
        CHECK(host_opt_layer.sb == (80u | 200u << 16) && host_opt_layer.mixb == 64 && host_opt_layer.sh == 40);
        CHECK(host_opt_layer.gb == 150 * 256 / 100 && host_opt_layer.gh == 200 * 256 / 100);   /* the strongest step's */
        CHECK(host_opt_layer.sbv == 5 && host_opt_layer.shv == 2);     /* a test's strengths in place of the rows' */
        CHECK(host_opt_layer.steps[0][0] == 25 && host_opt_layer.steps[0][4] == 100 && host_opt_layer.steps[1][0] == 16);
        CHECK(host_opt_layer.day[0] == 80 && host_opt_layer.day[1] == 110 && host_opt_layer.day[2] == 50 &&
              host_opt_layer.day[3] == 150 && host_opt_layer.day[4] == 40 && host_opt_layer.day[5] == 6);
        CHECK(host_opt_layer.bench == 0 && host_look_rows == 1);
        CHECK(host_opt_layer.hcb == -50 * 256 / 100 && host_opt_layer.hcr == 70 * 256 / 100);
        CHECK(host_opt_layer.read == 1 && host_opt_layer.read_us == 100000 && host_opt_layer.cached == 0);
        CHECK(host_opt_layer.lcached == 0);
        CHECK(host_opt_layer.em == 16 && host_opt_layer.bdark == 80 * 256 / 100 && host_opt_layer.hfirst == 1);
        CHECK(host_opt_layer.tmix == 0 && host_opt_layer.st == 0);     /* the tail off by default */
    } else {
        CHECK(host_opt_sub == DRV_OPTICS_N);    /* the schedule's first entry later */
        CHECK(host_opt_layer.route == DRV_OPTICS_403 && host_opt_layer.width == 400);   /* 648 clamped to 400 */
        CHECK(host_opt_layer.order == (1u | 2u << 8 | 0u << 16) && host_opt_layer.gb == 150 * 256 / 100);
        CHECK(host_opt_layer.sbv == DRV_OPTICS_ROWS && host_opt_layer.shv == 1);
        CHECK(host_opt_layer.lcached == 1);
    }
    log_flush();
    t = read_file("LOG/CORE.LOG", &n);
    CHECK(t && strstr(t, "optics: layer: route"));
    CHECK(t && strstr(t, phase == 1 ? "the layer read and written past the cache" :
                                      "the layer read and written through the CACHED view"));
    if (phase == 1)
        CHECK(t && strstr(t, "optics: probe level 0: a step in the look's stills on the mask route; the stills get L4"));
    else
        CHECK(t && strstr(t, "layer_width = 648 out of 42..400") && strstr(t, "Halation sigma 10 px is below the layer's "
                                                                           "smallest") && strstr(t, "schedule of 4 entries"));
    free(t);
}

/* The field's INI: [optics] layer = on without strengths - the strengths are the look's rows' (given to the driver
 * when they change), the steps and the day's keys read, bad ones logged and the defaults taken */
static void test_layer_rows(void)
{
    long n;
    char *t;

    omk_dir(LOG_DIR);
    write_file("OMKIT.INI", "[optics]\nlayer = on\nbloom_steps = 10, 20, 30, 40, 250\nhalation_steps = 1, 2\n"
                            "day_mean = 60, 120\nday_share = 9, 3\nday_halation = 35\nbench = 1\nbloom_tail = 95\n");
    CHECK(cfg_load() == 1);
    log_setup(1, OMK_LOG_INFO);
    host_prev_ended = 1;
    host_opt_worker_rc = 0;
    host_opt_sub = 99;
    host_look_rows = 0;
    host_look_str_on = 0;
    host_opt_str = 0xffffffffu;
    host_opt_strs = 0;
    memset(&host_opt_layer, 0, sizeof(host_opt_layer));
    memset(&host_opt, 0, sizeof(host_opt));
    optics_setup();
    CHECK(host_opt_sub == DRV_OPTICS_L4 && host_look_rows == 1);
    CHECK(host_opt_layer.sbv == DRV_OPTICS_ROWS && host_opt_layer.shv == DRV_OPTICS_ROWS);
    CHECK(host_opt_layer.steps[0][0] == 10 && host_opt_layer.steps[0][3] == 40 && host_opt_layer.steps[0][4] == 200);
    CHECK(host_opt_layer.steps[1][0] == 16 && host_opt_layer.steps[1][4] == 100);    /* not five: the default */
    CHECK(host_opt_layer.day[0] == 60 && host_opt_layer.day[1] == 120 && host_opt_layer.day[2] == 50 &&
          host_opt_layer.day[3] == 150 && host_opt_layer.day[4] == 40 && host_opt_layer.day[5] == 35);
    CHECK(host_opt_layer.bench == 1 && host_opt_layer.tmix == 90 * 256 / 100);   /* the tail at most 90 % */
    CHECK(!optics_route());
    optics_service(1000);                   /* the look gives nothing yet: 0, 0 */
    CHECK(host_opt_str == 0 && host_opt_strs == 1 && !optics_route());
    host_look_str_on = 1;
    host_look_str[0] = 3;
    host_look_str[1] = 1;
    optics_service(1250);
    optics_service(1500);                   /* given once */
    CHECK(host_opt_str == (3u | 1u << 8) && host_opt_strs == 2 && optics_route());
    host_look_str[0] = 0;
    host_look_str[1] = 0;
    optics_service(1750);
    CHECK(host_opt_str == 0 && host_opt_strs == 3 && !optics_route());
    log_flush();
    t = read_file("LOG/CORE.LOG", &n);
    CHECK(t && strstr(t, "optics: layer: Bloom by the look's rows (threshold 200"));
    CHECK(t && strstr(t, "optics: layer: strengths 1..5 - Bloom 10 20 30 40 200%, Halation 16 25 40 63 100% of the gains; "
                         "the bench on"));
    CHECK(t && strstr(t, "optics: layer: a day scene (mean Y 60..120 or 5..15% at the Bloom threshold): Bloom 40%, "
                         "Halation 35% of the gains"));
    CHECK(t && strstr(t, "halation_steps: not five numbers") && strstr(t, "'9, 3': not two numbers"));
    CHECK(t && strstr(t, "optics: the look's strengths now: Bloom 3, Halation 1"));
    CHECK(t && strstr(t, "optics: the look's strengths now: Bloom 0, Halation 0 (no step in its stills)"));
    free(t);
}

/* The layer's further sets: schedule entries L4:X take [optics.X] over [optics] (a key not given there is [optics]'s),
 * the press of each entry gets its set; bad set entries left out */
extern drv_optics_layer_cfg host_opt_sets[DRV_OPTICS_SETS + 1];
extern u32 host_opt_nsets, host_opt_set;

static void test_layer_sets(void)
{
    long n;
    char *t;
    u32 k;
    static const u32 want_sub[] = {DRV_OPTICS_N, DRV_OPTICS_L4, DRV_OPTICS_L4, DRV_OPTICS_L4, DRV_OPTICS_L2};
    static const u32 want_set[] = {0, 0, 1, 2, 1};

    omk_dir(LOG_DIR);
    write_file("OMKIT.INI", "[optics]\nbloom = 3\nhalation = 3\nemission = 0\nbloom_dark = 0\nhalation_first = 0\n"
                            "schedule = N, L4, L4:A, L4:b, L4:Z, W1:A, L2:A\n"
                            "[optics.A]\nbloom = 5\nemission = 16\nbloom_tail = 30\nbloom_tail_sigma = 250\n"
                            "bloom_dark = 80\n[optics.B]\nhalation_gain = 3000\nhalation_first = 1\n");
    CHECK(cfg_load() == 1);
    log_setup(1, OMK_LOG_INFO);
    host_prev_ended = 1;
    host_opt_worker_rc = 0;
    host_ui_state = 0;
    host_opt_nsets = 0;
    host_opt_set = 99;
    memset(host_opt_sets, 0, sizeof(host_opt_sets));
    memset(&host_opt_layer, 0, sizeof(host_opt_layer));
    memset(&host_opt, 0, sizeof(host_opt));
    optics_setup();
    CHECK(host_opt_nsets == 2);
    CHECK(host_opt_layer.em == 0 && host_opt_layer.bdark == 0 && host_opt_layer.hfirst == 0 && host_opt_layer.tmix == 0);
    CHECK(host_opt_layer.gb == 150 * 256 / 100 && host_opt_layer.sbv == 3 && host_opt_layer.shv == 3);
    CHECK(host_opt_sets[1].sbv == 5 && host_opt_sets[1].shv == 3 && host_opt_sets[1].gh == 200 * 256 / 100);
    CHECK(host_opt_sets[1].em == 16 && host_opt_sets[1].tmix == 30 * 256 / 100 && host_opt_sets[1].st == 250);
    CHECK(host_opt_sets[1].bdark == 80 * 256 / 100 && host_opt_sets[1].hfirst == 0 && host_opt_sets[1].th == 225);
    CHECK(host_opt_sets[2].gh == 3000 * 256 / 100 && host_opt_sets[2].hfirst == 1 && host_opt_sets[2].em == 0);
    CHECK(host_opt_sets[2].tmix == 0 && host_opt_sets[2].st == 0);
    optics_service(1000);                   /* the worker made, the first entry given */
    CHECK(host_opt_sub == want_sub[0] && host_opt_set == want_set[0]);
    for (k = 1; k < 5; k++) {
        host_opt.ended[want_sub[k - 1]]++;
        optics_service(2000 * k);
        CHECK(host_opt_sub == want_sub[k] && host_opt_set == want_set[k]);
    }
    log_flush();
    t = read_file("LOG/CORE.LOG", &n);
    CHECK(t && strstr(t, "optics: schedule of 5 entries"));
    CHECK(t && strstr(t, "'L4:Z' is not a substage") && strstr(t, "'W1:A' is not a substage"));
    CHECK(t && strstr(t, "optics: layer settings A: Bloom 5, a test (threshold 200, sigma 60 + 0 at 0%, gain 150% at 5, "
                         "80% less on white; tail 30% of sigma 250)"));
    CHECK(t && strstr(t, "optics: layer settings B: Halation 3, a test") && strstr(t, "before Bloom"));
    CHECK(t && strstr(t, "optics: layer settings A: the source - the emission of a blown pixel as 16 whites"));
    CHECK(t && strstr(t, "schedule 3 of 5: L4 with the settings A given to the next press"));
    CHECK(t && strstr(t, "schedule 4 of 5: L4 with the settings B given"));
    free(t);
}

int main(int argc, char **argv)
{
    host_dir = argc > 1 ? argv[1] : ".";
    mem_init();
    if (argc > 2 && !strcmp(argv[2], "cfg"))
        test_cfg_parse();
    else if (argc > 2 && !strcmp(argv[2], "mem"))
        test_mem();
    else if (argc > 2 && !strcmp(argv[2], "power"))
        test_power();
    else if (argc > 3 && !strcmp(argv[2], "optics"))
        test_optics(atoi(argv[3]));
    else if (argc > 3 && !strcmp(argv[2], "bisect"))
        test_bisect(atoi(argv[3]));
    else if (argc > 2 && !strcmp(argv[2], "schedule"))
        test_schedule();
    else if (argc > 3 && !strcmp(argv[2], "layer"))
        test_layer(atoi(argv[3]));
    else if (argc > 2 && !strcmp(argv[2], "layer_sets"))
        test_layer_sets();
    else if (argc > 2 && !strcmp(argv[2], "layer_rows"))
        test_layer_rows();
    else {
        test_cfg_template();
        test_log();
        test_clock();
        test_store();
        test_backoff();
        test_copy();
    }
    if (failed)
        fprintf(stderr, "%d checks failed\n", failed);
    else
        printf("ok\n");
    return failed != 0;
}
