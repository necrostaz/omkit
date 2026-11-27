/* ISP colour table service (core/isp.c): one module owns it; the driver does the work */
#ifndef OMK_ISP_H
#define OMK_ISP_H

#include "omk_api.h"

int isp_override(omk_mod *m, const void *block, u32 size);
int isp_census(omk_mod *m, int on);
int isp_scope(omk_mod *m, u32 scope);
void isp_service(u32 now);                  /* core loop while running: counters and the census to the log */
void isp_drop_module(omk_mod *m);           /* the owner failed module_init: stock tables, the service is free */
int isp_mono(void);                         /* 1 - the block in force is black and white (the look's grain: no colour) */
u32 isp_white(void);                        /* the block in force's white, a code of Y (omk_ispb_white): 0 - not known */

#endif
