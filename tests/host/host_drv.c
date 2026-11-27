/* Driver for testing core on the host: files C:\OMKIT\X live in the HOST_DIR directory, the test sets the time.
 * As on the camera, wb mode does not truncate an existing file. Firmware heap: three classes on top of malloc. */
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <stdlib.h>
#include <string.h>
#include "omk_drv.h"
#include "omk_events.h"
#include "host_drv.h"
#include "omk_look.h"
#include "omk_core.h"

u32 host_ticks;
u32 host_cpu;
u32 host_region_size = 1 << 20;
const char *host_dir = ".";
static unsigned char heap[1 << 20] __attribute__((aligned(16)));

static const u32 fw_block[3] = {128, 2048, 16384};
u32 host_fw_total[3] = {8, 4, 4};
u32 host_fw_out[3];

/* C:\OMKIT\A\B -> host_dir/A/B */
static void host_path(char *out, const char *path)
{
    const char *root = drv_root();
    size_t n = strlen(root), i;

    if (!strncmp(path, root, n))
        path += n;
    snprintf(out, 512, "%s/%s", host_dir, path);
    for (i = strlen(host_dir); out[i]; i++)
        if (out[i] == 0x5c)
            out[i] = '/';
}

u32 host_written, host_fopens, host_removes, host_inplace;
int host_no_card;                       /* 1 - no file opens, as without a card */

u32 host_fopen_us;

/* Files open now, and whether the firmware no longer takes them (the camera's PC mode: every call gives 0x9414) */
static struct {
    FILE *f;
    int stale;
} open_files[32];

static FILE *opened(FILE *f)
{
    unsigned i;

    for (i = 0; f && i < sizeof(open_files) / sizeof(open_files[0]); i++)
        if (!open_files[i].f) {
            open_files[i].f = f;
            open_files[i].stale = 0;
            break;
        }
    return f;
}

static int stale(void *fp, int forget)
{
    unsigned i;
    int r = 0;

    for (i = 0; i < sizeof(open_files) / sizeof(open_files[0]); i++)
        if (open_files[i].f == fp) {
            r = open_files[i].stale;
            if (forget)
                open_files[i].f = 0;
        }
    return r;
}

void host_files_stale(void)
{
    unsigned i;

    for (i = 0; i < sizeof(open_files) / sizeof(open_files[0]); i++)
        if (open_files[i].f)
            open_files[i].stale = 1;
}

#define HOST_STALE_RC 0x9414                /* the firmware's answer on such a file (camera, card157) */

static FILE *host_fopen(const char *path, const char *mode);

void *drv_fopen(const char *path, const char *mode)
{
    return opened(host_fopen(path, mode));
}

static FILE *host_fopen(const char *path, const char *mode)
{
    char p[512];
    FILE *f;

    host_path(p, path);
    host_fopens++;
    host_ticks = (host_ticks + host_fopen_us) & 0x3fffffff;   /* a card operation takes time, as on the camera */
    if (host_no_card)
        return 0;
    if (mode[0] == 'r' && mode[1] == '+') {   /* the firmware: no truncation, made if not there */
        host_inplace++;
        f = fopen(p, "r+b");
        return f ? f : fopen(p, "w+b");
    }
    if (mode[0] == 'r')
        return fopen(p, "rb");
    if (mode[0] == 'a')                     /* the firmware: open or create, at the end */
        return fopen(p, "ab");
    f = fopen(p, "r+b");                    /* the firmware's w does not truncate */
    return f ? f : fopen(p, "wb");
}

int drv_remove(const char *path)
{
    char p[512];

    host_path(p, path);
    host_removes++;
    return remove(p);
}

int drv_mkdir(const char *path)
{
    char p[512];

    host_path(p, path);
#ifdef _WIN32
    return _mkdir(p);
#else
    return mkdir(p, 0777);
#endif
}

int drv_exists(const char *path)
{
    char p[512];
    struct stat st;

    host_path(p, path);
    return stat(p, &st) == 0;
}

int drv_fseek(void *fp, u32 offset)
{
    long end;

    if (fseek(fp, 0, SEEK_END))
        return -1;
    end = ftell(fp);                        /* the firmware stops at the end of the file */
    return fseek(fp, (long)offset < end ? (long)offset : end, SEEK_SET);
}

u32 drv_ftell(void *fp)
{
    return (u32)ftell(fp);
}

u32 drv_fread(void *buf, u32 size, u32 n, void *fp)
{
    return fread(buf, size, n, fp);
}

u32 drv_fwrite(const void *buf, u32 size, u32 n, void *fp)
{
    if (stale(fp, 0))
        return 0;
    host_written += size * n;               /* the tests check how much a flush writes */
    return fwrite(buf, size, n, fp);
}

int host_fclose_rc;                         /* what drv_fclose answers (0 - closed), the file is closed anyway */

int drv_fclose(void *fp)
{
    int was = stale(fp, 1);

    fclose(fp);
    return was ? HOST_STALE_RC : host_fclose_rc;
}

u32 drv_fflush(void *fp)
{
    return stale(fp, 0) ? HOST_STALE_RC : (u32)fflush(fp);
}

void drv_sleep(u32 ms)
{
    host_ticks = (host_ticks + ms * 1000) & 0x3fffffff;
}

u32 drv_ticks(void)
{
    return host_ticks;
}

u32 drv_cpu(void)
{
    return host_cpu;
}

int host_task = 7;

int drv_task_current(u32 cpu)
{
    return cpu ? -1 : host_task;
}

u32 drv_lock(void)
{
    return 0x5a;
}

void drv_unlock(u32 state)
{
    if (state != 0x5a)
        fprintf(stderr, "unlock with a foreign state\n");
}

int drv_mem_region(u32 i, void **base, u32 *size)
{
    if (i)
        return 0;
    *base = heap + 8;                       /* unaligned start: core aligns it itself */
    *size = host_region_size - 8;
    return 1;
}

int drv_fwheap_class(u32 i, u32 *block, u32 *total, u32 *free)
{
    if (i >= 3)
        return 0;
    *block = fw_block[i];
    *total = host_fw_total[i];
    *free = host_fw_total[i] - host_fw_out[i];
    return 1;
}

void *drv_fwheap_alloc(u32 size)
{
    u32 c;
    unsigned char *p;

    if (host_cpu)
        fprintf(stderr, "firmware heap alloc from cpu %u\n", host_cpu);
    for (c = 0; c < 3 && fw_block[c] < size; c++)
        ;
    if (c == 3 || host_fw_out[c] == host_fw_total[c])
        return 0;
    p = malloc(16 + fw_block[c]);              /* glibc aligns to 16 */
    p[0] = c;
    host_fw_out[c]++;
    return p + 16;
}

void drv_fwheap_free(void *p)
{
    unsigned char *b = (unsigned char *)p - 16;

    host_fw_out[b[0]]--;
    free(b);
}

const char *drv_name(void)
{
    return "host";
}

const char *drv_root(void)
{
    static const char root[] = {'C', ':', 0x5c, 'O', 'M', 'K', 'I', 'T', 0x5c, 0};

    return root;
}

char drv_sep(void)
{
    return 0x5c;
}

void *drv_exec_addr(void *p)
{
    return p;
}

void drv_code_sync(void *exec, u32 size)
{
    (void)exec;
    (void)size;
}

int drv_check(void)
{
    return 0;
}

int drv_boot_event(u32 i, char *name, u32 *time)
{
    (void)i;
    (void)name;
    (void)time;
    return 0;
}

int host_install_rc;
u32 host_installs[8];

int drv_ev_supported(u32 event, u32 *phases)
{
    if (event == OMK_EV_PICTMODE_CHECK)
        *phases = OMK_PH_NOTIFY_BEFORE | OMK_PH_REPLACE | OMK_PH_FILTER_AFTER | OMK_PH_NOTIFY_AFTER;
    else if (event == OMK_EV_S1_API)
        *phases = OMK_PH_NOTIFY_BEFORE;
    else
        return 0;
    return 1;
}

int drv_ev_install(u32 event)
{
    host_installs[event]++;
    return host_install_rc;
}

/* Power hooks: one pretend hook, always installed (core/power.c) */
int drv_power_install(u32 i, const char **name)
{
    if (i)
        return -1;
    *name = "test";
    return 0;
}

const char *drv_power_reason(u32 reason)
{
    return reason == 1 ? "power lever" : 0;
}

int host_power_pending;                 /* what drv_power_pending reports: the lever flag */

int drv_power_pending(void)
{
    return host_power_pending;
}

u32 host_k3;                                /* K3 of the power probe: the firmware's power-off or sleep began */

u32 host_probes, host_rtc_reads;            /* reads of the camera's state words and of the subprocessor clock */

u32 drv_power_probe(u32 *w, u32 max)
{
    u32 n;

    host_probes++;
    for (n = 0; n < max && n < 6; n++)
        w[n] = n == 0 ? (u32)host_power_pending : n == 1 ? host_k3 : 0;
    return n;
}

/* Picture profiles: the driver state as the host test sets it (core/prof.c) */
u32 host_prof, host_prof_seq, host_prof_ui, host_prof_base = 1, host_prof_installs;
u32 host_prof_takes;
int host_prof_install_rc;

int drv_prof_count(u32 *colors, u32 *monos)
{
    *colors = *monos = 3;
    return 1;
}

int drv_prof_install(int cache_inv)
{
    (void)cache_inv;
    host_prof_installs++;
    return host_prof_install_rc;
}

void drv_prof_set(u32 prof)
{
    host_prof = prof;
}

u32 drv_prof_get(void)
{
    return host_prof;
}

int drv_prof_base(void)
{
    return host_prof_base;
}

u32 drv_prof_stored_mode(void)
{
    return host_prof_base ? 1 : 5;
}

void drv_prof_stats(drv_prof_info *st)
{
    memset(st, 0, sizeof(*st));
    st->ui_seq = host_prof_seq;
    st->ui_prof = host_prof_ui;
    st->takes = host_prof_takes;
}

int drv_prof_service(void)
{
    return 0;
}

int drv_edp_install(void)
{
    return 0;
}

void drv_edp_service(void)
{
}

void drv_edp_stats(u32 *items, u32 *renders, u32 *cpu, u32 *drawn, u32 *failed)
{
    *items = *renders = *cpu = *drawn = *failed = 0;
}

void drv_imp_stats(u32 *lines, u32 *selects, u32 *skipped, u32 *last_prof, u32 *bad)
{
    *lines = *selects = *skipped = *last_prof = *bad = 0;
}

void drv_prof_prepare(void)
{
}

int drv_prof_take_recent(void)
{
    return 0;
}

u32 drv_file_id(void *fp)
{
    return fp && !stale(fp, 0) ? 0x10000u | ((u32)(uintptr_t)fp & 0xfff0u) | 1u : 0;
}

int host_unmounted;                         /* 1 - the volume is not mounted (USB Storage) */

int drv_card_mounted(void)
{
    return !host_unmounted;
}

int drv_card_busy(void)
{
    return 0;
}

int host_ended;                             /* core_mark_ended calls (main.c is not in the host build) */

void core_mark_ended(void)
{
    host_ended++;
}

u32 host_ev_other_cpu[16];

u32 drv_ev_other_cpu(u32 event)
{
    return event < 16 ? host_ev_other_cpu[event] : 0;
}

int host_wakes, host_rtc_ok = 1;
u32 host_rtc_sec, host_rtc_frac;

int drv_wait_init(int on)
{
    return on ? 1 : 0;
}

int drv_wait(u32 ms)
{
    drv_sleep(ms);
    return 0;
}

void drv_wake(void)
{
    host_wakes++;
}

int drv_wait_state(void)
{
    return 1;
}

drv_date_t host_date = {2026, 11, 24, 15, 2, 11, 0, {0}};

void drv_date(drv_date_t *d)
{
    *d = host_date;
}

int drv_rtc(u32 *sec, u32 *frac1024)
{
    host_rtc_reads++;
    if (!host_rtc_ok)
        return -1;
    *sec = host_rtc_sec;
    *frac1024 = host_rtc_frac;
    return 0;
}

int drv_prof_lv_read(drv_lv_view *v)
{
    (void)v;
    return -1;
}

u32 host_icons;

int drv_prof_picture_size(u32 pic, u32 *w, u32 *h)
{
    if ((pic & 0xf00) == 0x100 && (pic & 3) < 3) {
        *w = pic & 3 ? 80 : 66;
        *h = pic & 3 ? 60 : 50;
        return 0;
    }
    if ((pic & 0xf00) == 0x200 || (pic & 0xf00) == 0x300) {
        *w = (pic & 0xf00) == 0x200 ? 124 : 128;
        *h = 39;
        return 0;
    }
    if ((pic & 0xf00) == 0x400) {
        *w = 80;
        *h = 36;
        return 0;
    }
    if ((pic & 0xf00) == 0x500 && (pic & 0xff) < 9) {
        *w = (pic & 0xff) < 5 ? 66 : 80;
        *h = (pic & 0xff) < 5 ? 50 : 60;
        return 0;
    }
    return -1;
}

int drv_prof_picture(u32 pic, u32 w, u32 h, const u8 *pixels)
{
    (void)pic;
    (void)w;
    (void)h;
    (void)pixels;
    host_icons++;
    return 0;
}

u32 host_matrices;

int drv_prof_mono_matrix(u32 filter, u32 level, const s16 *matrix)
{
    (void)filter;
    (void)level;
    host_matrices += matrix ? 1 : 0;
    return 0;
}

void drv_prof_flush(void)
{
}

int drv_prof_mono_matrix_get(u32 filter, u32 level, s16 *matrix)
{
    u32 k;

    for (k = 0; k < 9; k++)                 /* rows of 256 as the camera's: the diagonal 256 - 2 * (filter + level) */
        matrix[k] = (s16)(k % 4 ? (s16)(filter + level) : (s16)(256 - 2 * (filter + level)));
    return 0;
}

int host_isp_ok;                        /* 1 - the colour path hooks install (0: the service is not available) */
int host_isp_busy;                      /* drv_isp_set answers -2 (too soon) this many times */
u32 host_isp_stock;                     /* drv_isp_set(0, 0) calls: back to the camera's own tables */

int drv_isp_install(void)
{
    return host_isp_ok ? 0 : 1;
}

int drv_isp_set(const u8 *block, u32 size)
{
    (void)size;
    if (!host_isp_ok)
        return -3;
    if (!block)
        host_isp_stock++;
    if (host_isp_busy) {
        host_isp_busy--;
        return -2;
    }
    return 0;
}

void drv_isp_census(int on)
{
    (void)on;
}

void drv_isp_service(u32 now)
{
    (void)now;
}

void drv_isp_scope(u32 scope)
{
    (void)scope;
}

void *drv_fopen_long(const char *path, const char *mode)
{
    (void)mode;
    return drv_fopen(path, "r");            /* the host's file system takes long names as they are */
}

/* Folder listing: off unless a test sets host_find (the older tests expect nothing found). Then the files of the mask's
 * folder, as the firmware's entry has them: +0 attributes (0x10 a folder), +4 made, +8 changed (DOS), +16 size, +24 name.
 * One listing at a time; the mask's pattern is not matched (the debug service lists *.*) */
int host_find;
static struct {
    DIR *d;
    char dir[512];
} find;

static int find_entry(void *entry)
{
    struct dirent *e;
    struct stat st;
    char p[1024];
    u8 *b = entry;

    while ((e = readdir(find.d)) != 0) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof(p), "%s/%s", find.dir, e->d_name);
        if (stat(p, &st))
            continue;
        *(u32 *)b = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
        *(u32 *)(b + 4) = *(u32 *)(b + 8) = 0x5d787263;
        *(u32 *)(b + 16) = (u32)st.st_size;
        snprintf((char *)b + 24, DRV_FIND_ENTRY - 24, "%s", e->d_name);
        return 0;
    }
    return -1;
}

int drv_findfirst(const char *mask, void *entry)
{
    char *slash;

    if (!host_find || find.d)
        return -1;                          /* no folder listing on the host: nothing found */
    host_path(find.dir, mask);
    slash = strrchr(find.dir, '/');
    if (slash)
        *slash = 0;
    find.d = opendir(find.dir);
    if (!find.d)
        return -1;
    if (find_entry(entry)) {
        closedir(find.d);
        find.d = 0;
        return -1;
    }
    return 1;
}

int drv_findnext(int handle, void *entry)
{
    (void)handle;
    return find.d ? find_entry(entry) : -1;
}

int drv_findclose(int handle)
{
    (void)handle;
    if (find.d)
        closedir(find.d);
    find.d = 0;
    return 0;
}

void drv_edp_gaps(u32 *min, u32 *last, u32 *count)
{
    *min = *last = *count = 0;
}

/* The film look and the look's grain (look.c, grain.c) are not in the host build: their API entries refuse */
int look_enable(omk_mod *m, const char *slot, omk_look_fn fn, void *ctx)
{
    (void)m, (void)slot, (void)fn, (void)ctx;
    return -1;
}

int look_list(omk_mod *m, const char *const *names, u32 count, u32 current)
{
    (void)m, (void)names, (void)count, (void)current;
    return -1;
}

int look_set(omk_mod *m, int on)
{
    (void)m, (void)on;
    return -1;
}

int look_get(void)
{
    return 0;
}

int look_picture_size(u32 pic, u32 *w, u32 *h)
{
    (void)pic, (void)w, (void)h;
    return -1;
}

int look_picture(omk_mod *m, u32 pic, u32 w, u32 h, const u8 *pixels)
{
    (void)m, (void)pic, (void)w, (void)h, (void)pixels;
    return -1;
}

int look_exif(omk_mod *m, const char *text)
{
    (void)m, (void)text;
    return -1;
}

int look_name(omk_mod *m, const char *text)
{
    (void)m, (void)text;
    return -1;
}

int look_art(omk_mod *m, u32 index, const void *data, u32 size)
{
    (void)m, (void)index, (void)data, (void)size;
    return -1;
}

int look_edits(omk_mod *m, const omk_look_edits *e)
{
    (void)m, (void)e;
    return -1;
}

int look_grain(omk_mod *m, u32 entry, s32 tile, u32 level)
{
    (void)m, (void)entry, (void)tile, (void)level;
    return -2;
}

int look_optics(omk_mod *m, u32 entry, const u8 *strength, u32 count)
{
    (void)m, (void)entry, (void)strength, (void)count;
    return -2;
}

void look_drop_module(omk_mod *m)
{
    (void)m;
}

void grain_drop_module(struct omk_mod *m)
{
    (void)m;
}

/* main.c is not in the host build: the tests call core's functions as firmware tasks would */
int host_in_task;                           /* 1 - the test runs as the core task */

int core_in_task(void)
{
    return host_in_task;
}

u32 grain_list(struct omk_mod *m, char (*files)[13], u32 max)
{
    (void)m, (void)files, (void)max;
    return 0;
}

/* The optics probe's driver (platform opticsdrv.c) for core/optics.c: the test sets the counts drv_optics_service gives;
 * drv_optics_arm calls host_opt_on_arm (when set) at the moment the driver would get the arming */
int host_prev_ended = 1;                    /* core_prev_ended (main.c is not in the host build) */
u32 host_opt_level, host_opt_arms;
drv_optics_info host_opt;
void (*host_opt_on_arm)(void);

int core_prev_ended(void)
{
    return host_prev_ended;
}

u32 host_opt_installs;                      /* drv_optics_install calls */

int drv_optics_install(u32 level)
{
    host_opt_installs++;
    host_opt_level = level;
    return 0;
}

void drv_optics_arm(void)
{
    host_opt_arms++;
    host_opt.armed = 1;
    if (host_opt_on_arm)
        host_opt_on_arm();
}

void drv_optics_service(drv_optics_info *st)
{
    *st = host_opt;
}

u32 host_opt_sub = 99, host_opt_crumb[DRV_OPTICS_CRUMB];

void drv_optics_sub(u32 sub)
{
    host_opt_sub = sub;
}

u32 host_opt_loop, host_opt_sleep;

void drv_optics_loop(u32 n, u32 sleep_ms)
{
    host_opt_loop = n;
    host_opt_sleep = sleep_ms;
}

int drv_optics_crumb(u32 boot, u32 *old)
{
    u32 i;

    for (i = 0; i < DRV_OPTICS_CRUMB; i++)
        old[i] = host_opt_crumb[i];
    host_opt_crumb[0] = 0;
    host_opt_crumb[1] = boot;
    return old[0] == 0x4f435242u;
}

void drv_optics_beat(u32 ms)
{
    host_opt_crumb[2] = ms;
}

const char *drv_optics_sub_name(u32 sub)
{
    static const char *const name[DRV_OPTICS_SUBS] = {"N", "A0", "A", "B", "C1", "C2", "C3", "W0", "W1", "W2", "D", "E", "F", "full",
                                                      "L0", "L1", "L2", "L3", "L4"};

    return sub < DRV_OPTICS_SUBS ? name[sub] : "?";
}

const char *drv_optics_phase(u32 phase)
{
    return phase == 7 ? "250 ms busy" : "a phase";
}

u32 drv_restart_cause(void)
{
    return 0;
}

int host_opt_worker_rc;                     /* what drv_optics_worker answers */
u32 host_opt_workers;                       /* its calls */

int drv_optics_worker(u32 *tid)
{
    host_opt_workers++;
    *tid = host_opt_worker_rc ? 0 : 0x77;
    return host_opt_worker_rc;
}

/* The optics layer: the settings kept for the test to look at */
drv_optics_layer_cfg host_opt_layer;
int host_opt_layer_rc;

int drv_optics_layer(const drv_optics_layer_cfg *c)
{
    host_opt_layer = *c;
    return host_opt_layer_rc;
}

/* The further sets and the set of each press */
drv_optics_layer_cfg host_opt_sets[DRV_OPTICS_SETS + 1];
u32 host_opt_nsets, host_opt_set;

int drv_optics_layer_set(u32 k, const drv_optics_layer_cfg *c)
{
    if (!k || k > DRV_OPTICS_SETS)
        return 1;
    host_opt_sets[k] = *c;
    if (k > host_opt_nsets)
        host_opt_nsets = k;
    return 0;
}

void drv_optics_set(u32 k)
{
    host_opt_set = k;
}

/* The strengths: the look's (look_optics_now, what the test sets) and what the driver got; the rows wanted */
u8 host_look_str[2];
int host_look_str_on;
u32 host_opt_str = 0xffffffffu, host_opt_strs;
int host_look_rows;

int look_optics_now(u8 *out)
{
    out[0] = host_look_str_on ? host_look_str[0] : 0;
    out[1] = host_look_str_on ? host_look_str[1] : 0;
    return host_look_str_on;
}

void drv_optics_strength(u32 bloom, u32 halation, u32 mono, u32 white)
{
    host_opt_str = bloom | halation << 8 | mono << 16 | white << 24;
    host_opt_strs++;
}

void drv_look_rows(int on)
{
    host_look_rows = on;
}

u32 drv_optics_layer_floor(u32 W, u32 *wsize, u32 *wbuf)
{
    *wsize = 0;
    *wbuf = 0;
    return host_opt_layer.width ? (216 * W + 50 * host_opt_layer.width) / (100 * host_opt_layer.width) : 0;
}

int drv_optics_worker_test(u32 *us, u32 *work, u32 *state)
{
    *us = *work = *state = 0;
    return -1;
}

/* ---- the debug service (core/debug.c) ---- */
u8 host_keep[0x20080] __attribute__((aligned(16)));    /* the memory that outlives a reset: kept between "power-ons" */
u32 host_keep_size = sizeof(host_keep);
host_ptp_op host_ptp[8];
u32 host_ptp_n;
int host_ptp_installs;
u8 host_ptp_out[1 << 17];                   /* what drv_ptp_send sent */
u32 host_ptp_out_n, host_ptp_total, host_ptp_sends;
const u8 *host_ptp_in;                      /* what the computer sends */
u32 host_ptp_in_n;
u8 host_mem[4096];                          /* stands for RAM at 0xa0001000 */
drv_task host_tasks[4];
u32 host_ntasks;
u32 host_app = 0x5100;                      /* what drv_ui_probe gives as the app state, unless half-pressed */
int host_ui_state;                          /* what drv_ui_probe answers: 0 a lasting state, DRV_UI_HALF ... */

int drv_keep_area(void **base, u32 *size)
{
    *base = host_keep;
    *size = host_keep_size;
    return 1;
}

int drv_ptp_add(u32 code, u32 phase, drv_ptp_fn fn)
{
    if (host_ptp_n >= 8)
        return -1;
    host_ptp[host_ptp_n].code = code;
    host_ptp[host_ptp_n].phase = phase;
    host_ptp[host_ptp_n++].fn = fn;
    return 0;
}

int drv_ptp_install(void)
{
    host_ptp_installs++;
    return 0;
}

int drv_ptp_buffer_ok(const void *buf)
{
    return !((uptr)buf & 1);
}

/* As the firmware: the first call of an operation takes the total; pieces of odd length only last */
int drv_ptp_send(const void *buf, u32 len, u32 total)
{
    if (!drv_ptp_buffer_ok(buf) || host_ptp_out_n + len > sizeof(host_ptp_out) || (host_ptp_out_n & 1))
        return 0;
    if (!host_ptp_sends++)
        host_ptp_total = total;
    memcpy(host_ptp_out + host_ptp_out_n, buf, len);
    host_ptp_out_n += len;
    return 1;
}

u32 drv_ptp_recv_len(void)
{
    return host_ptp_in_n;
}

int drv_ptp_recv(void *buf, u32 len)
{
    if (len > host_ptp_in_n)
        return 0;
    memcpy(buf, host_ptp_in, len);
    return 1;
}

u32 drv_ptp_history(void *out, u32 max)
{
    u32 n = max < 488 ? max : 488;

    memset(out, 0x48, n);
    return n;
}

const volatile u8 *drv_mem_readable(u32 addr, u32 len)
{
    if (addr >= 0x60000000 && addr < 0x70000000)
        addr += 0x40000000;
    if (!len || addr < 0xa0001000 || len > sizeof(host_mem) || addr - 0xa0001000 > sizeof(host_mem) - len)
        return 0;
    return host_mem + (addr - 0xa0001000);
}

int drv_rename(const char *from, const char *to)
{
    char a[512], b[512];

    host_path(a, from);
    host_path(b, to);
    return rename(a, b);
}

int drv_task_read(u32 cpu, u32 slot, drv_task *t)
{
    if (cpu || slot >= host_ntasks)
        return -1;
    *t = host_tasks[slot];
    return 1;
}

int drv_obj_read(u32 kind, u32 id, drv_obj *o)
{
    memset(o, 0, sizeof(*o));
    o->id = id;
    o->kind = kind;
    o->a = 1;
    return 0;
}

/* Weak: the scenario's test (test_shoot.c) has its own model of the camera's half-press state */
u32 host_keys;                              /* the raw input byte of drv_ui_probe */

__attribute__((weak)) int drv_ui_probe(u32 *app, u32 *keys)
{
    host_probes++;
    *app = host_ui_state == DRV_UI_HALF ? 0x5101 : host_app;
    *keys = host_keys;
    return host_ui_state;
}

/* The same answer from RAM words (both builds: the hold of card writes in a half-press); counted apart from the debug
 * build's probes */
u32 host_ui_reads;

__attribute__((weak)) int drv_ui_state(u32 *app, u32 *keys)
{
    host_ui_reads++;
    *app = host_ui_state == DRV_UI_HALF ? 0x5101 : host_app;
    *keys = host_keys;
    return host_ui_state;
}

/* The restart request: counted */
int host_restarts;

int drv_restart_request(void)
{
    host_restarts++;
    return 0;
}

/* Weak: the scenario (core/shoot.c, in every host program since the debug service starts it); test_shoot.c has its own */
int host_shutter_calls;

__attribute__((weak)) int drv_shutter(u32 key, int down, u32 path)
{
    (void)key, (void)down, (void)path;
    host_shutter_calls++;
    return 0;
}

__attribute__((weak)) void drv_look_stats(drv_look_info *st)
{
    memset(st, 0, sizeof(*st));
}

__attribute__((weak)) int drv_look_choose(int on, u32 index)
{
    (void)on, (void)index;
    return -1;
}

__attribute__((weak)) int drv_look_find(const char *name)
{
    (void)name;
    return -1;
}

__attribute__((weak)) void drv_prof_choose(u32 prof)
{
    (void)prof;
}

/* ---- the test scenario without hands (core/handsfree.c) ---- */
__attribute__((weak)) const char *core_run(void)
{
    return "HOSTRUN1";
}

u32 host_stuck_since;                       /* what taskdump_stuck_since gives (taskdump.c is not in the host build) */

__attribute__((weak)) u32 taskdump_stuck_since(void)
{
    return host_stuck_since;
}

int host_usb_install_rc;                    /* drv_usb_hold_install's answer */
u32 host_usb_mode, host_usb_hits, host_usb_choice, host_usb_factor, host_usb_link, host_usb_installs;
u32 host_ok_presses, host_hfw_posts, host_resets, host_reset_cause;
int host_hfw_rc;                            /* drv_restart_hfw's answer */

int drv_usb_hold_install(void)
{
    host_usb_installs++;
    return host_usb_install_rc;
}

void drv_usb_hold(u32 mode)
{
    host_usb_mode = mode;
}

u32 drv_usb_hold_get(void)
{
    return host_usb_mode;
}

u32 drv_usb_hold_hits(void)
{
    return host_usb_hits;
}

/* As the firmware's choice: with the hold on, a choice applied becomes the mode held (host_usb_take) */
void host_usb_take(u32 choice)
{
    if (host_usb_mode) {
        host_usb_hits++;
        choice = host_usb_mode;
    }
    host_usb_choice = choice;
}

void drv_usb_probe(drv_usb_info *u)
{
    u32 keys;

    u->choice = host_usb_choice;
    u->factor = host_usb_factor;
    u->link = host_usb_link;
    if (drv_ui_probe(&u->app, &keys) < 0)
        u->app = 0xffffffff;
}

int drv_ok_key(int down)
{
    if (down)
        host_ok_presses++;
    return 0;
}

int drv_restart_hfw(void)
{
    host_hfw_posts++;
    return host_hfw_rc;
}

void drv_reset(u32 cause)
{
    host_resets++;
    host_reset_cause = cause;
}
