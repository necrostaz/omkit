/* Power states: power-off and sleep.
 * The driver hooks the firmware and reports what happens through power_hook (firmware task). The core task follows
 * in power_service: it stops card writes, does the last save before power-off and acknowledges every
 * change. The two gates (card off, sleep) wait for that acknowledgement, at most POWER_GATE_MS. */
#ifndef OMK_POWER_H
#define OMK_POWER_H

#include "omk_types.h"

#define POWER_GATE_MS 1000                  /* longest wait of a firmware task at a gate */
#define POWER_CANCEL_MS 5000                /* still alive this long after power-off began: it was cancelled */
#define POWER_SLEEP_ABORT_MS 15000          /* sleep prepared but never entered (the firmware waits up to 10 s) */

enum { POWER_RUN, POWER_OFF, POWER_SLEEP };

/* What the driver reports (power_hook kind) */
enum {
    POWER_HOOK_OFF = 1,                     /* power-off began, arg: firmware reason */
    POWER_HOOK_CARD_OFF,                    /* gate: the card is about to be unmounted */
    POWER_HOOK_SLEEP,                       /* sleep is being prepared */
    POWER_HOOK_SLEEP_GATE,                  /* gate: the firmware enters sleep, the card loses power */
    POWER_HOOK_WAKE,                        /* back from sleep, arg: firmware sleep result */
    POWER_HOOKS
};

void power_install(void);                   /* core_main: install the driver hooks, log what failed */
void power_hook(u32 kind, u32 arg);         /* driver, firmware task on CPU 0; the gates may sleep */
int power_service(u32 now);                 /* core task loop; 1 - running: card writes allowed */
int power_running(void);                    /* card and screen are ours to use */
u32 power_cancels(void);                    /* how many power-offs and sleeps did not happen (core task) */
u32 power_hook_count(u32 kind);             /* how many times that hook (POWER_HOOK_*) ran since the start */

#endif
