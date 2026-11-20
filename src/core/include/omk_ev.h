/* core event manager. Registration happens while modules load; ev_activate sorts
 * the handlers, applies OMKIT.INI and installs the hooks: after that the lists are immutable and are read without the lock. */
#ifndef OMK_EV_H
#define OMK_EV_H

#include "omk_api.h"

/* For the driver: an event hook fired. orig calls the original firmware function with the event data (ctx is the hook
 * registers). Returns 1 if the original was called (through the chain or directly), 0 if not: a REPLACE handler did not call it */
typedef void (*ev_orig_fn)(void *data, void *ctx);
int ev_dispatch(u32 event, void *data, ev_orig_fn orig, void *ctx);

int ev_register(omk_mod *m, u32 event, u32 phase, int priority, omk_handler fn, void *ctx);
void ev_drop_module(omk_mod *m);            /* the module failed module_init: its handlers are removed */
void ev_activate(void);                     /* after all modules are loaded */
void ev_next(omk_ev *ev);
int ev_defer(omk_mod *m, void (*fn)(void *arg), void *arg);
void ev_service(u32 now);                   /* service loop: deferred calls, counters to the log */
const char *ev_name(u32 event);

#endif
