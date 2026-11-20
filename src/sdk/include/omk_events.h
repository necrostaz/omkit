/* OMKit events: semantic ids and data. The driver installs the hook, a module sees only these structures.
 * The driver decides which events and phases exist on this firmware: registering
 * an unsupported one returns an error, and the module decides by itself whether to work without it. */
#ifndef OMK_EVENTS_H
#define OMK_EVENTS_H

#include "omk_types.h"

/* Handler phases. Call order: NOTIFY_BEFORE, FILTER_BEFORE, REPLACE (chain, next()), FILTER_AFTER, NOTIFY_AFTER */
#define OMK_PH_NOTIFY_BEFORE 0x01           /* read the data before the original function (a copy: changing it is useless) */
#define OMK_PH_FILTER_BEFORE 0x02           /* modify the input data */
#define OMK_PH_REPLACE 0x04                 /* do it yourself or call api->next(ev): on down the chain, the original at the end */
#define OMK_PH_FILTER_AFTER 0x08            /* modify the result */
#define OMK_PH_NOTIFY_AFTER 0x10            /* read the outcome (a copy) */

/* The firmware checks whether a picture mode is allowed: when building the list of the direct selection screen (once per
 * mode in the list, 41 on OM-5 Mark II 1.1) and when the dial steps over the picture mode cell in the SCP.
 * Phases: NOTIFY_BEFORE, REPLACE, FILTER_AFTER, NOTIFY_AFTER. */
#define OMK_EV_PICTMODE_CHECK 1
typedef struct {
    u32 mode;                               /* picture mode number */
    u32 allowed;                            /* outcome: 1 - allowed as is, 0 - the firmware will replace it (hidden, restricted) */
} omk_ev_pictmode_check;

/* PRELIMINARY: a call that accompanies an S1 press (firmware API 0x80001 on OM-5 Mark II 1.1). On the camera there are two
 * calls per press, one in the emulator. Meaning not understood: an event for experiments, not for module logic. No data.
 * Phases: NOTIFY_BEFORE. */
#define OMK_EV_S1_API 2

#define OMK_EV_COUNT 3                      /* ids 1..OMK_EV_COUNT-1 */

#endif
