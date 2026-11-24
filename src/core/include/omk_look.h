/* Film look service (core/look.c): one module owns it; the driver does the work */
#ifndef OMK_LOOK_H
#define OMK_LOOK_H

#include "omk_api.h"

int look_enable(omk_mod *m, const char *slot, omk_look_fn fn, void *ctx);
int look_list(omk_mod *m, const char *const *names, u32 count, u32 current);
int look_set(omk_mod *m, int on);
int look_get(void);
int look_picture_size(u32 pic, u32 *w, u32 *h);
int look_picture(omk_mod *m, u32 pic, u32 w, u32 h, const u8 *pixels);
int look_exif(omk_mod *m, const char *text);
int look_name(omk_mod *m, const char *text);
int look_art(omk_mod *m, u32 index, const void *data, u32 size);
int look_edits(omk_mod *m, const omk_look_edits *e);
int look_grain(omk_mod *m, u32 entry, s32 tile, u32 level);
int look_optics(omk_mod *m, u32 entry, const u8 *strength, u32 count);   /* API 20 */
/* The optics rows' strengths of the entry the look's owner has chosen (OMK_OPTICS_* order), for the look's stills; core
 * task. 1 - the look is active and its owner gave them (out filled), 0 - none (out zeros) */
int look_optics_now(u8 *out);
const char *cani_check(const u8 *b, u32 n);    /* 0 - a drawing core accepts, or why not */
void look_drop_module(omk_mod *m);          /* the owner failed module_init: release the service */
void look_service(u32 now);                 /* core loop while running: the user's choices to the owner */

/* api->every (module.c): the modules' periodic calls, from the core loop while running */
int module_every(omk_mod *m, u32 ms, void (*fn)(void *ctx), void *ctx);
void modules_tick(u32 now);

#endif
