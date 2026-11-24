/* Public API for modules (omk_api.h). The module handle selects the config section, memory owner, store and folder */
#include "omk_core.h"
#include "omk_ev.h"
#include "omk_power.h"
#include "omk_prof.h"
#include "omk_isp.h"
#include "omk_look.h"
#include "omk_log.h"
#include "omk_mem.h"
#include "omk_mod.h"
#include "omk_store.h"

static void api_log(omk_mod *m, int level, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_vprintf_from(level, m->name, fmt, ap);
    va_end(ap);
}

static const char *api_cfg_str(omk_mod *m, const char *key, const char *def)
{
    return cfg_str(m->name, key, def);
}

static int api_cfg_int(omk_mod *m, const char *key, int def)
{
    return cfg_int(m->name, key, def);
}

static int api_cfg_bool(omk_mod *m, const char *key, int def)
{
    return cfg_bool(m->name, key, def);
}

static int api_cfg_size(omk_mod *m, const char *key, u32 *out)
{
    return cfg_size(m->name, key, out);
}

static void *api_alloc(omk_mod *m, u32 size)
{
    return mem_alloc_for(m->owner, size, 0);
}

static void api_free(omk_mod *m, void *p)
{
    int o;

    if (!p)
        return;
    o = mem_block_owner(p);
    if (o != m->owner) {
        log_printf(OMK_LOG_ERROR, "memory: %s frees %x that belongs to %s, kept", m->name, (u32)(uptr)p,
                   o < 0 ? "nobody" : mem_owner_name(o));
        return;
    }
    mem_free(p);
}

static void api_mem_info(omk_mod *m, omk_mem_info *out)
{
    mem_info(m->owner, out);
}

static omk_store *api_store_open(omk_mod *m, u32 size)
{
    if (m->store)
        return m->store;
    if (m->state != MOD_LOADING) {
        log_printf(OMK_LOG_ERROR, "store %s: open only from module_init", m->name);
        return 0;
    }
    m->store = store_open_for(m->name, size, m->owner);
    if (!m->store)
        log_printf(OMK_LOG_ERROR, "store %s: cannot open %u bytes", m->name, size);
    return m->store;
}

/* The module's folder OMKIT/NAME, made at its first file call (API 19): only modules that use files get one */
static void mod_dir(omk_mod *m)
{
    if (m->dir_made)
        return;
    if (omk_dir(m->name))
        log_printf(OMK_LOG_WARN, "%s: its folder could not be made", m->name);
    else
        m->dir_made = 1;
}

static omk_file *api_file_open(omk_mod *m, const char *name, const char *mode)
{
    char n[OMK_NAME_MAX], path[OMK_PATH_MAX];
    void *fp;
    int lfn = 0;

    if (!name || !mode || (mode[0] != 'r' && mode[0] != 'w') || mode[1] ||
        (omk_name83(n, name, 1) && (mode[0] != 'r' || omk_name_long(n, name) || !(lfn = 1)))) {
        log_printf(OMK_LOG_WARN, "%s: file_open(%s, %s): an 8.3 name of A-Z 0-9 _ (r or w) or a long name (r)", m->name,
                   name ? name : "0", mode ? mode : "0");
        return 0;
    }
    if (!power_running()) {
        log_printf(OMK_LOG_WARN, "%s: file_open(%s): the card is not available now (power-off or sleep)", m->name, n);
        return 0;
    }
    mod_dir(m);
    if (omk_path(path, m->name, n)) {
        log_printf(OMK_LOG_WARN, "%s: file_open(%s): the path is longer than %u", m->name, n, OMK_PATH_MAX - 1);
        return 0;
    }
    if (mode[0] == 'w')
        drv_remove(path);                   /* "w" as in C: the file starts empty (the firmware's w does not truncate) */
    fp = lfn ? drv_fopen_long(path, "r") : drv_fopen(path, mode[0] == 'r' ? "r" : "wb");
    if (!fp)
        log_printf(OMK_LOG_DEBUG, "%s: cannot open %s\\%s", m->name, m->name, n);
    return fp;
}

static u32 api_file_read(omk_file *f, void *buf, u32 len)
{
    return f && power_running() ? drv_fread(buf, 1, len, f) : 0;
}

static u32 api_file_write(omk_file *f, const void *buf, u32 len)
{
    return f && power_running() ? drv_fwrite(buf, 1, len, f) : 0;
}

/* Every findnext reads the folder from its start again (the firmware's walk): the count is capped */
#define LIST_SCAN_MAX 256

/* The files of the module folder: names of `stride` bytes (13 - 8.3 only; OMK_NAME_MAX - long names too) */
static u32 file_list(omk_mod *m, const char *pattern, char *names, u32 stride, u32 max)
{
    static u8 entry[DRV_FIND_ENTRY];        /* core task only */
    char path[OMK_PATH_MAX], n[OMK_NAME_MAX];
    u32 i, k, got = 0, seen = 0, skipped = 0;
    int h, rc;

    if (!pattern || !names || !max || !power_running() || drv_cpu())
        return 0;
    for (k = 0; pattern[k]; k++) {
        char c = pattern[k];

        if (k > 12 || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '*' ||
                        c == '?' || c == '.')) {
            log_printf(OMK_LOG_WARN, "%s: file_list(%s): a pattern of 8.3 characters and * ?", m->name, pattern);
            return 0;
        }
    }
    mod_dir(m);
    if (omk_path(path, m->name, pattern))
        return 0;
    mem_set(entry, 0, sizeof(entry));
    h = drv_findfirst(path, entry);
    if (h == -1 || !h)
        return 0;
    for (rc = 0; !rc && seen < LIST_SCAN_MAX; seen++) {
        const char *name = (const char *)entry + 24;
        u32 attr = entry[0] | entry[1] << 8, size = entry[16] | entry[17] << 8 | entry[18] << 16 | (u32)entry[19] << 24;

        if (!(attr & 0x10) && size && name[0] != '.') {
            if (omk_name83(n, name, 1) && (stride < OMK_NAME_MAX || omk_name_long(n, name))) {
                skipped++;
            } else if (got < max) {
                for (i = 0; i < stride && (!i || n[i - 1]); i++)
                    names[got * stride + i] = n[i];
                got++;
            }
        }
        mem_set(entry, 0, sizeof(entry));
        rc = drv_findnext(h, entry);
    }
    drv_findclose(h);
    if (skipped)
        log_printf(OMK_LOG_INFO, "%s: file_list(%s): %u names skipped (%s)", m->name, pattern, skipped,
                   stride < OMK_NAME_MAX ? "not 8.3" : "longer than 39 or not printable ASCII");
    return got;
}

static u32 api_file_list(omk_mod *m, const char *pattern, char (*names)[13], u32 max)
{
    return file_list(m, pattern, names ? names[0] : 0, 13, max);
}

static u32 api_file_list_long(omk_mod *m, const char *pattern, char (*names)[OMK_NAME_MAX], u32 max)
{
    return file_list(m, pattern, names ? names[0] : 0, OMK_NAME_MAX, max);
}

static void api_file_close(omk_file *f)
{
    if (f)
        drv_fclose(f);
}

static const omm_res *find_res(omk_mod *m, u32 type, u32 id)
{
    u32 i;

    for (i = 0; i < m->res_count; i++)
        if (m->dir[i].type == type && m->dir[i].id == id)
            return &m->dir[i];
    return 0;
}

static const void *api_res(omk_mod *m, u32 type, u32 id, u32 *size)
{
    const omm_res *r = find_res(m, type, id);

    if (size)
        *size = r ? r->size : 0;
    return r && !(r->flags & OMM_RES_ONDEMAND) ? m->res + r->offset : 0;
}

/* On-demand resource: read from the module file, positioned with the firmware's fseek; the file CRC was checked at load.
 * While a module's module_init runs (core task) the data is usually in memory (m->disk, module.c) and nothing is read
 * from the card. Without that copy the file stays open between the reads of one module_init - a module loading many
 * resources at start opens it once (54 profile pictures: about 2 s of opens before core 0.77); after module_init each
 * read opens it */
static void *batch_fp;
static omk_mod *batch_mod;
static int batching;

void api_res_batch(int on)
{
    batching = on;
    if (!on && batch_fp) {
        drv_fclose(batch_fp);
        batch_fp = 0;
        batch_mod = 0;
    }
}

static u32 api_res_read(omk_mod *m, u32 type, u32 id, u32 offset, void *buf, u32 len)
{
    const omm_res *r = find_res(m, type, id);
    u32 at, got = 0;
    void *fp;

    if (!r || !(r->flags & OMM_RES_ONDEMAND) || offset >= r->size || !len) {
        if (r && !(r->flags & OMM_RES_ONDEMAND))
            log_printf(OMK_LOG_WARN, "%s: resource %u/%u is in memory, use res", m->name, type, id);
        return 0;
    }
    if (len > r->size - offset)
        len = r->size - offset;
    if (m->disk) {                          /* module_init runs: the whole on-demand data is in memory, checked (CRC) */
        mem_copy(buf, m->disk + r->offset + offset, len);
        return len;
    }
    if (batching && batch_fp && batch_mod == m) {
        fp = batch_fp;
    } else {
        if (batching && batch_fp) {         /* another module's file was open */
            drv_fclose(batch_fp);
            batch_fp = 0;
            batch_mod = 0;
        }
        fp = drv_fopen(m->path, "r");
        if (!fp) {
            log_printf(OMK_LOG_WARN, "%s: cannot reopen the module file for resource %u/%u", m->name, type, id);
            return 0;
        }
        if (batching) {
            batch_fp = fp;
            batch_mod = m;
        }
    }
    at = m->disk_at + r->offset + offset;
    if (drv_fseek(fp, at) || drv_ftell(fp) != at)
        log_printf(OMK_LOG_WARN, "%s: resource %u/%u: cannot seek to %u", m->name, type, id, at);
    else
        got = drv_fread(buf, 1, len, fp);
    if (fp != batch_fp)
        drv_fclose(fp);
    if (got != len)
        log_printf(OMK_LOG_WARN, "%s: resource %u/%u read %u of %u bytes", m->name, type, id, got, len);
    return got;
}

static const char *api_str(omk_mod *m, u32 id)
{
    if (!m->strings || id >= m->nstrings)
        return "";
    return (const char *)m->strings + (m->strings[4 + 2 * id] | m->strings[5 + 2 * id] << 8);
}

static int api_on(omk_mod *m, u32 event, u32 phase, int priority, omk_handler fn, void *ctx)
{
    return ev_register(m, event, phase, priority, fn, ctx);
}

static int api_on_power(omk_mod *m, omk_power_fn fn, void *ctx)
{
    if (!m || m->state != MOD_LOADING) {
        log_printf(OMK_LOG_ERROR, "%s: on_power only from module_init", m ? m->name : "?");
        return -3;
    }
    m->power_fn = fn;
    m->power_ctx = ctx;
    return 0;
}

/* The on-screen text was removed in core 0.94 (the UI hung after a shot while it was drawn): the slot stays, the table layout of
 * modules built for API 13 does not change */
static int api_overlay_removed(omk_mod *m, const char *text, u32 ms)
{
    (void)text;
    (void)ms;
    log_printf(OMK_LOG_WARN, "%s: the on-screen text was removed in core 0.94", m->name);
    return -1;
}

/* API 19: 0 free, 1 the camera writes a picture (core defers its own writes then too), -1 power-off or sleep */
static int api_card_busy(void)
{
    if (!power_running())
        return -1;
    return drv_card_busy() || drv_prof_take_recent() ? 1 : 0;
}

static const omk_api api = {
    OMK_API_VERSION, sizeof(omk_api),
    api_log,
    api_cfg_str, api_cfg_int, api_cfg_bool, api_cfg_size,
    api_alloc, api_free, api_mem_info,
    api_store_open, store_get, store_set, store_del,
    api_file_open, api_file_read, api_file_write, api_file_close,
    api_res, api_res_read, api_str,
    drv_sleep, omk_ms,
    api_on, ev_next, ev_defer,
    api_overlay_removed,
    CORE_VERSION,
    api_on_power,
    prof_count, prof_enable, prof_set, prof_get,
    prof_picture_size, prof_picture,
    prof_mono_matrix,
    isp_override, isp_census, isp_scope,
    look_enable, look_list, look_set, look_get, look_picture_size, look_picture,
    module_every,
    look_exif,
    api_file_list,
    look_name,
    look_art,
    api_file_list_long,
    look_edits,
    grain_list,
    look_grain,
    prof_mono_matrix_get,
    api_card_busy,
    look_optics,
};

const omk_api *omk_api_get(void)
{
    return &api;
}
