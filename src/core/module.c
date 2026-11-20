/* Module loader: [modules] in OMKIT.INI, files MODULES\NAME.OMM.
 * A failing module logs its reason, the others still load. Module code runs only after the CRC32 check. */
#include "omk_core.h"
#include "omk_ev.h"
#include "omk_log.h"
#include "omk_mem.h"
#include "omk_mod.h"
#include "omk_power.h"
#include "omk_store.h"
#include "omk_prof.h"
#include "omk_look.h"
#include "omk_isp.h"

#define MOD_DIR "MODULES"
#define MAX_IMAGE (2u << 20)
#define MAX_RES (2u << 20)
#define MAX_RES_COUNT 1024
#define MAX_DISK (64u << 20)
#define RELOC_CHUNK 64
/* The on-demand data of a module is kept in memory while its module_init runs (see load_file): at most DISK_KEEP_MAX
 * bytes, and only when the arena's largest free piece is DISK_KEEP_FREE larger than the copy - a short-lived copy is not
 * worth a module's own allocations running short. The bundled modules have 67 KB (41 profile pictures) and 15 KB (12
 * look pictures); the arena is about 3 MB */
#define DISK_KEEP_MAX (256u << 10)
#define DISK_KEEP_FREE (512u << 10)

static omk_mod mods[MOD_MAX];
static u32 nmods;

static int call_entry(void *entry, const omk_api *api, omk_mod *m)
{
    return ((int (*)(const omk_api *, omk_mod *))entry)(api, m);
}

int (*module_call)(void *entry, const omk_api *api, omk_mod *m) = call_entry;

u32 modules_count(void)
{
    return nmods;
}

omk_mod *module_at(u32 i)
{
    return i < nmods ? &mods[i] : 0;
}

static int read_exact(void *fp, void *buf, u32 n)
{
    u8 *p = buf;
    u32 got;

    while (n) {
        got = drv_fread(p, 1, n, fp);
        if (!got)
            return -1;
        p += got;
        n -= got;
    }
    return 0;
}

static u32 get32(const u8 *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static void put32(u8 *p, u32 v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

/* String table: {u16 count, u16 0, u16 offsets[count]}, zero-terminated ASCII strings; the last byte of the table is zero,
 * so every string ends inside it. 0 - valid */
static int check_strings(omk_mod *m, const u8 *t, u32 size)
{
    u32 count, i, off;

    if (size < 4 || t[size - 1])
        return -1;
    count = t[0] | t[1] << 8;
    if (4 + 2 * count > size)
        return -1;
    for (i = 0; i < count; i++) {
        off = t[4 + 2 * i] | t[5 + 2 * i] << 8;
        if (off < 4 + 2 * count || off >= size)
            return -1;
    }
    m->strings = t;
    m->nstrings = count;
    return 0;
}

/* Reads of the on-demand data for its CRC: 4 KB at a time (each fread is a request to the firmware file server). Cached
 * core memory: modules load in the core task only.
 * The data has to pass through here for the CRC anyway, so it is also copied into a block of core's (m->disk) that lives
 * until module_init returns: api->res_read is served from it without touching the card. A module that sets its pictures
 * at start read them one by one before - a seek and a read each, 41 times for the profiles. Without the block (too
 * large, no room) res_read reads the file as it does after module_init */
static u8 crc_chunk[4096];

/* Reads and checks the file, loads the image and resources. 0 or the reason text; the caller frees the owner's memory */
static const char *load_file(omk_mod *m, void *fp)
{
    omm_head h;
    u32 crc, i, k, n, off[RELOC_CHUNK], base, extra, total, used, largest;

    if (read_exact(fp, &h, sizeof(h)))
        return "file shorter than header";
    if (h.magic != OMM_MAGIC || h.format != OMM_FORMAT)
        return "not an OMKit module or unknown format";
    for (i = 0; i < OMM_NAME; i++)
        if (h.name[i] != (i < str_len(m->name) ? m->name[i] : 0))
            return "name in the file differs from the file name";
    if (h.api > OMK_API_VERSION)
        return "needs a newer core API";
    /* h.image >= 4: the relocation bound below is h.image - 4 (an image of 1..3 bytes would make it wrap) */
    if (h.image < 4 || h.image > MAX_IMAGE || h.bss > MAX_IMAGE || h.entry >= h.image || h.relocs > h.image / 4 + 1 ||
        h.res_count > MAX_RES_COUNT || h.res_bytes > MAX_RES || h.res_disk > MAX_DISK)
        return "sizes in the header are out of range";
    m->res_bytes = h.res_bytes;
    m->res_disk = h.res_disk;
    m->disk_at = OMM_HEAD + h.image + h.relocs * 4 + h.res_count * sizeof(omm_res) + h.res_bytes;
    m->version = h.version;
    m->image_size = h.image;
    m->bss = h.bss;
    m->entry = h.entry;
    crc = h.crc;
    h.crc = 0;
    h.crc = crc32_update(0, &h, sizeof(h));
    /* Whole cache lines (MEM_LINES): the image is written here through the arena's uncached address, but the module
     * runs at the cached alias (m->exec), and the relocations below point its own variables there too - its data and
     * bss go through the data cache of CPU 0, the only CPU module code runs on. Every other block of the arena is
     * used past the cache. A line shared with a neighbour would be written back with the neighbour's bytes as they
     * were when the line was read: the fault that corrupted a module when core's stack shared lines with the arena */
    m->image = mem_alloc_for(m->owner, h.image + h.bss, MEM_IMAGE | MEM_LINES);
    if (!m->image)
        return "no memory for the image";
    m->exec = drv_exec_addr(m->image);
    base = (u32)(uptr)m->exec;
    if (read_exact(fp, m->image, h.image))
        return "file ends inside the image";
    h.crc = crc32_update(h.crc, m->image, h.image);
    for (i = 0; i < h.relocs; i += n) {
        n = h.relocs - i < RELOC_CHUNK ? h.relocs - i : RELOC_CHUNK;
        if (read_exact(fp, off, n * 4))
            return "file ends inside relocations";
        h.crc = crc32_update(h.crc, off, n * 4);
        for (k = 0; k < n; k++) {
            if (off[k] > h.image - 4)
                return "relocation outside the image";
            put32(m->image + off[k], get32(m->image + off[k]) + base);
        }
    }
    if (h.res_count) {
        m->dir = mem_alloc_for(m->owner, h.res_count * sizeof(omm_res) + h.res_bytes, MEM_IMAGE);
        if (!m->dir)
            return "no memory for resources";
        m->res = (u8 *)(m->dir + h.res_count);
        if (read_exact(fp, m->dir, h.res_count * sizeof(omm_res) + h.res_bytes))
            return "file ends inside resources";
        h.crc = crc32_update(h.crc, m->dir, h.res_count * sizeof(omm_res) + h.res_bytes);
        for (i = 0; i < h.res_count; i++) {
            omm_res *r = &m->dir[i];
            u32 limit = r->flags & OMM_RES_ONDEMAND ? h.res_disk : h.res_bytes;

            if (r->offset > limit || r->size > limit - r->offset)
                return "resource outside the resource data";
            if (r->type == OMK_RES_STRINGS && r->id == 0) {
                if (r->flags & OMM_RES_ONDEMAND)
                    return "string table must stay in memory";
                if (check_strings(m, m->res + r->offset, r->size))
                    return "string table damaged";
            }
        }
        m->res_count = h.res_count;
    }
    if (h.res_disk && h.res_disk <= DISK_KEEP_MAX) {
        mem_arena(&total, &used, &largest);
        if (largest >= h.res_disk + DISK_KEEP_FREE)
            m->disk = mem_alloc(h.res_disk);    /* core's block: load_one frees it, whatever becomes of the module */
    }
    for (i = 0; i < h.res_disk; i += n) {   /* on-demand data: its CRC, and the copy for module_init when there is one */
        n = h.res_disk - i < sizeof(crc_chunk) ? h.res_disk - i : sizeof(crc_chunk);
        if (read_exact(fp, crc_chunk, n))
            return "file ends inside on-demand resources";
        h.crc = crc32_update(h.crc, crc_chunk, n);
        if (m->disk)
            mem_copy(m->disk + i, crc_chunk, n);
    }
    if (drv_fread(&extra, 1, 1, fp))
        return "file longer than the header says";
    if (h.crc != crc)
        return "checksum mismatch";
    mem_set(m->image + h.image, 0, h.bss);
    drv_code_sync(m->exec, h.image + h.bss);
    return 0;
}

static void load_one(const char *key)
{
    char name[OMM_NAME + 1], file[OMM_NAME + 5], path[OMK_PATH_MAX];
    omk_mod *m;
    const char *why;
    void *fp;
    u32 limit = 0, i;
    int rc;
    u32 t0;

    if (omk_name83(name, key, 0) || str_ieq(name, "CORE")) {
        log_printf(OMK_LOG_WARN, "module %s: name must be 1-8 of A-Z 0-9 _ and not CORE", key);
        return;
    }
    for (i = 0; i < nmods; i++)
        if (str_ieq(mods[i].name, name))
            return;
    if (nmods == MOD_MAX) {
        log_printf(OMK_LOG_WARN, "module %s: more than %u modules", name, MOD_MAX);
        return;
    }
    if (omk_name_ext(file, name, "OMM")) {
        log_printf(OMK_LOG_WARN, "module %s: not an 8.3 name", name);
        return;
    }
    omk_path(path, MOD_DIR, file);
    fp = drv_fopen(path, "r");
    if (!fp) {
        log_printf(OMK_LOG_WARN, "module %s: no file %s\\%s", name, MOD_DIR, file);
        return;
    }
    m = &mods[nmods];
    mem_set(m, 0, sizeof(*m));
    mem_copy(m->name, name, str_len(name) + 1);
    mem_copy(m->path, path, str_len(path) + 1);
    if (cfg_size(name, "memory", &limit) < 0)
        log_printf(OMK_LOG_WARN, "config: [%s] memory is not a size, no limit", name);
    m->owner = mem_owner(name, limit);
    if (m->owner < 0) {
        drv_fclose(fp);
        log_printf(OMK_LOG_ERROR, "module %s: no memory owner slot", name);
        return;
    }
    why = load_file(m, fp);
    drv_fclose(fp);
    if (why) {
        log_printf(OMK_LOG_ERROR, "module %s: %s, not loaded", name, why);
        mem_free(m->disk);
        m->disk = 0;
        mem_free_owner(m->owner, 0);           /* the image never ran: the cache has not seen it */
        return;
    }
    nmods++;
    m->state = MOD_LOADING;
    log_printf(OMK_LOG_INFO, "module %s %u.%u: image %u, bss %u, resources %u, at %x", name, m->version >> 16,
               m->version & 0xffff, m->image_size, m->bss, m->res_count, (u32)(uptr)m->exec);
    log_checkpoint("module %s: calling module_init", name);
    t0 = omk_ms();
    api_res_batch(1);
    rc = module_call(m->exec + m->entry, omk_api_get(), m);
    api_res_batch(0);
    mem_free(m->disk);                      /* from now on res_read reads the module file */
    m->disk = 0;
    log_checkpoint("module %s: module_init returned %d in %u ms", name, rc, omk_ms() - t0);
    if (rc) {
        m->state = MOD_FAILED;
        log_printf(OMK_LOG_ERROR, "module %s: module_init returned %d, unloaded", name, rc);
        ev_drop_module(m);
        m->power_fn = 0;
        m->tick_fn = 0;
        prof_drop_module(m);
        look_drop_module(m);
        grain_drop_module(m);               /* after the look: its entries are gone, then the grain chosen for them */
        isp_drop_module(m);
        if (m->store)
            store_close(m->store);
        m->store = 0;
        mem_free_owner(m->owner, m->image);    /* the image stays allocated: the cache may hold its lines */
        m->image = m->exec = 0;
        m->dir = 0;
        m->res = 0;
        m->res_count = 0;
        m->strings = 0;
        m->nstrings = 0;
        return;
    }
    m->state = MOD_RUNNING;
}

/* [modules]: enabled = off disables everything; NAME = on | off, line order is load order, on a repeat the last value wins.
 * 1 - done. 0 - a power-off or a sleep began between two modules: the rest is not read (a module takes up to seconds of
 * card reads and of its module_init, and the card-off gate waits for the core task one second at most); the caller
 * calls again when the camera runs, and the loading goes on from the next line. The modules already started are
 * running and get the power events; the event hooks go in only when all are loaded */
int modules_load(void)
{
    static u32 next;                        /* the first line of OMKIT.INI not looked at yet */
    const char *sec, *key, *val;
    u32 i, loaded = 0, failed = 0;
    int on;

    log_checkpoint(next ? "modules: loading the rest" : "modules: loading");
    if (!cfg_bool("modules", "enabled", 1)) {
        log_printf(OMK_LOG_INFO, "modules: disabled in OMKIT.INI");
        ev_activate();
        return 1;
    }
    for (i = next; i < cfg_count(); i++) {
        if (!power_running()) {
            next = i;
            log_printf(OMK_LOG_INFO, "modules: a power-off or a sleep began, the rest loads when the camera runs again");
            return 0;
        }
        cfg_entry(i, &sec, &key, &val);
        if (!str_ieq(sec, "modules") || str_ieq(key, "enabled"))
            continue;
        on = cfg_bool("modules", key, -1);
        if (on < 0) {
            if (cfg_str("modules", key, 0) == val)
                log_printf(OMK_LOG_WARN, "config: [modules] %s = %s, expected on or off", key, val);
            continue;
        }
        if (on)
            load_one(key);
    }
    for (i = 0; i < nmods; i++) {
        if (mods[i].state == MOD_RUNNING)
            loaded++;
        else
            failed++;
    }
    log_printf(OMK_LOG_INFO, "modules: %u running, %u failed", loaded, failed);
    log_checkpoint("modules: activating event hooks");
    ev_activate();
    log_checkpoint("modules: event hooks active");
    return 1;
}

int module_every(omk_mod *m, u32 ms, void (*fn)(void *ctx), void *ctx)
{
    if (!m || (ms && !fn))
        return -1;
    m->tick_fn = ms ? fn : 0;
    m->tick_ctx = ctx;
    m->tick_ms = ms;
    m->tick_at = omk_ms();
    return 0;
}

#define TICK_SLOW_MS 100                    /* a periodic call slower than this gets a warning */

/* The core loop, while the camera runs: each module's periodic call when it is due */
void modules_tick(u32 now)
{
    u32 i, t0, ms;
    omk_mod *m;

    for (i = 0; i < nmods; i++) {
        m = &mods[i];
        if (m->state != MOD_RUNNING || !m->tick_fn || now - m->tick_at < m->tick_ms)
            continue;
        m->tick_at = now;
        t0 = drv_ticks();
        m->tick_fn(m->tick_ctx);
        ms = drv_ticks_since(t0) / 1000;
        if (ms > TICK_SLOW_MS)
            log_printf(OMK_LOG_WARN, "%s: the periodic call took %u ms", m->name, ms);
    }
}

#define POWER_SLOW_MS 50                    /* a handler slower than this gets a warning */

static const char *const power_names[] = {"?", "power-off", "resume", "sleep", "wake"};

void modules_power(u32 event)
{
    u32 k, i, t0, ms;
    int back = event == OMK_POWER_OFF || event == OMK_POWER_SLEEP;
    omk_mod *m;

    for (k = 0; k < nmods; k++) {
        i = back ? nmods - 1 - k : k;
        m = &mods[i];
        if (m->state != MOD_RUNNING || !m->power_fn)
            continue;
        t0 = drv_ticks();
        m->power_fn(event, m->power_ctx);
        ms = drv_ticks_since(t0) / 1000;
        log_printf(ms > POWER_SLOW_MS ? OMK_LOG_WARN : OMK_LOG_DEBUG, "power: %s %s took %u ms%s", m->name,
                   event < sizeof(power_names) / sizeof(power_names[0]) ? power_names[event] : "?", ms,
                   ms > POWER_SLOW_MS ? ", too slow" : "");
    }
}
