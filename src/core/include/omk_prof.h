/* Picture profiles service (core/prof.c): one module owns it; the driver does the work */
#ifndef OMK_PROF_H
#define OMK_PROF_H

#include "omk_api.h"

int prof_count(u32 *colors, u32 *monos);
int prof_enable(omk_mod *m, omk_prof_fn fn, void *ctx);
int prof_set(omk_mod *m, u32 prof);
u32 prof_get(void);
int prof_picture_size(u32 pic, u32 *w, u32 *h);
int prof_picture(omk_mod *m, u32 pic, u32 w, u32 h, const u8 *pixels);
int prof_mono_matrix(omk_mod *m, u32 filter, u32 level, const s16 *matrix);
int prof_mono_matrix_get(u32 filter, u32 level, s16 *matrix);
void prof_drop_module(omk_mod *m);          /* the owner failed module_init: release the service */
int prof_owned(void);                       /* 1 - a module owns the profiles (prof_service keeps the hooks) */
void prof_service(u32 now);                 /* core loop while running: the user's choices to the owner, upkeep */

#endif
