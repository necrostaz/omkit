/* Task dump for hangs: the state of every firmware task in the log. core/taskdump.c */
#ifndef OMK_TASKDUMP_H
#define OMK_TASKDUMP_H

#include "omk_types.h"

void taskdump_setup(u32 now);               /* after the config and the log, before the modules: its buffer comes from the arena */
void taskdump_service(u32 now);             /* core loop, every tick while the camera runs */
/* The application stuck: the time (omk_ms) its dump was asked for, while it stays in that state; 0 - not stuck */
u32 taskdump_stuck_since(void);

#endif
