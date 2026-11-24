/* The test scenario (core/shoot.c): a series of pictures without the user, each step with its preset, from [shoot] in
 * OMKIT.INI or a command (the debug service). The shutter button is pressed by the button's own firmware path; the presets
 * are chosen as the user chooses them in the picture mode screen. Core task only. */
#ifndef OMK_SHOOT_H
#define OMK_SHOOT_H

#include "omk_types.h"

#define SHOOT_STEPS 16                      /* presets in one scenario */
#define SHOOT_SHOTS_MAX 20                  /* pictures per step */
#define SHOOT_TOTAL_MAX 200                 /* pictures in one scenario */

enum { SHOOT_IDLE, SHOOT_RUNNING, SHOOT_DONE, SHOOT_FAILED, SHOOT_STOPPED };

typedef struct {
    u32 state;                              /* SHOOT_* */
    u32 total, taken, failed;               /* pictures planned, made, not seen made */
    u32 step, steps;                        /* the step now (from 0) and how many */
    u32 waiting;                            /* 1 - started, waiting for the camera to shoot (USB, a menu) */
} shoot_info;

/* core_main after the modules: reads [shoot]; starts it if run = boot, or once from OMKIT\SHOOT.RUN (removed first) */
void shoot_setup(u32 now);
/* Start a scenario: steps in the form of `steps =` (0 - the one of OMKIT.INI); the other keys from OMKIT.INI.
 * 0; -1 a bad step (the log says which); -2 one is running already */
int shoot_start(const char *steps, u32 delay_ms);
void shoot_stop(void);                      /* both keys released at once, the scenario ends */
void shoot_status(shoot_info *out);
/* The core loop while running: the scenario's next step. Returns how soon to call again, ms (at most SERVICE_MS) */
u32 shoot_service(u32 now, u32 most);
void shoot_power_lost(void);                /* power-off or sleep began: keys released, the scenario waits to shoot again */

/* For the host tests: parse a steps string, 0 or -1 */
int shoot_parse(const char *steps);

#endif
