/* ISP colour table service (omk_isp.h). The first module to call it owns it; the hooks go
 * in at that first call, which must come from module_init (modules load at boot, before the camera is in use). */
#include "omk_core.h"
#include "omk_log.h"
#include "omk_mod.h"
#include "omk_isp.h"
#include "omk_ispb.h"

static omk_mod *owner;
static int failed;
static int mono;                            /* the block in force is black and white (omk_ispb_mono) */
static int stock_owed;                      /* isp_drop_module: the driver refused "back to stock" as too soon (-2);
                                               isp_service asks again until it takes it */

/* 0, or a code like the API's */
static int take(omk_mod *m)
{
    int rc;

    if (owner)
        return owner == m ? 0 : -5;
    if (!m || m->state != MOD_LOADING) {
        log_printf(OMK_LOG_ERROR, "isp: %s: the first call only from module_init", m ? m->name : "?");
        return -4;
    }
    if (failed)
        return -3;
    log_checkpoint("isp: installing the colour path hooks");
    rc = drv_isp_install();
    if (rc) {
        failed = 1;
        log_printf(OMK_LOG_ERROR, "isp: colour path hooks not installed, step %d", rc);
        return -3;
    }
    owner = m;
    log_printf(OMK_LOG_INFO, "isp: colour path hooks installed, owned by %s", m->name);
    return 0;
}

int isp_override(omk_mod *m, const void *block, u32 size)
{
    int rc = take(m);

    if (rc)
        return rc;
    rc = drv_isp_set(block, size);
    if (rc >= 0) {
        mono = block ? omk_ispb_mono(block) : 0;
        stock_owed = 0;                     /* the new owner's tables (or stock) are in force */
    }
    if (rc < 0)                             /* -2 (too soon) is the owner's retry, not a fault */
        log_printf(rc == -2 ? OMK_LOG_DEBUG : OMK_LOG_WARN, "isp: %s refused: %d", block ? "override" : "back to stock", rc);
    else if (rc == 0)                       /* new tables; a fast change (1) or none (2) is only counted (the 5 s report) */
        log_printf(OMK_LOG_INFO, "isp: %s", block ? "override on" : "back to the camera's own tables");
    return rc < 0 ? rc : 0;
}

int isp_census(omk_mod *m, int on)
{
    int rc = take(m);

    if (rc)
        return rc;
#ifdef OMK_DEBUG
    drv_isp_census(on);
    log_printf(OMK_LOG_INFO, "isp: census %s (debug level)", on ? "on" : "off");
    return 0;
#else
    (void)on;                               /* the census of the colour path's jobs: a debug build's */
    return -1;
#endif
}

int isp_scope(omk_mod *m, u32 scope)
{
    int rc = take(m);

    if (rc)
        return rc;
    drv_isp_scope(scope);
    log_printf(OMK_LOG_INFO, "isp: tables for%s%s", scope & OMK_ISP_LIVE ? " live view" : "",
               scope & OMK_ISP_STILLS ? " stills" : scope & OMK_ISP_LIVE ? "" : " no jobs");
    return 0;
}

int isp_mono(void)
{
    return mono;
}

/* The owner failed module_init: its tables must not stay on the pictures (its memory is given back), and the service is
 * free for a module loaded later. The hooks stay in (drv_isp_install installs once) */
void isp_drop_module(omk_mod *m)
{
    int rc;

    if (!owner || owner != m)
        return;
    owner = 0;
    mono = 0;
#ifdef OMK_DEBUG
    drv_isp_census(0);
#endif
    rc = drv_isp_set(0, 0);
    stock_owed = rc == -2;                  /* too soon after the module's own change: isp_service repeats it */
    log_printf(OMK_LOG_WARN, "isp: %s failed to start, the service is free, the camera's own tables %s", m->name,
               rc == -2 ? "come back in a moment" : rc < 0 ? "were in force" : "are back");
}

void isp_service(u32 now)
{
    if (stock_owed)
        stock_owed = drv_isp_set(0, 0) == -2;
    if (owner)
        drv_isp_service(now);
}
