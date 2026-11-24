/* Log for core and modules: a RAM buffer, written to the card by the core service loop, not by the calling task. */
#ifndef OMK_LOG_H
#define OMK_LOG_H

#include <stdarg.h>
#include "omk_api.h"                /* OMK_LOG_* levels */
#include "omk_drv.h"

void log_setup(int enabled, int level, const char *run_path);   /* run_path 0 - no per-power-on log */
int log_level(void);                    /* the most detail anyone keeps: the card's level or the tee's */
/* The debug service (core 1.40): fn gets every line up to lvl with its time, without its line end, under the firmware lock
 * (a copy into RAM only: it must not take the lock again); log_card(0) - nothing is written to the card from now on, nor kept for it */
typedef void (*log_tee_fn)(int lvl, u32 ms, const char *text, u32 len);
void log_tee(log_tee_fn fn, int lvl);
void log_card(int on);

/* Format: %s %c %d %u %x %X, zero-padded width for numbers: %08x. The line is terminated automatically */
void log_printf(int level, const char *fmt, ...);
void log_vprintf(int level, const char *fmt, va_list ap);
void log_vprintf_from(int level, const char *who, const char *fmt, va_list ap);   /* line "who: text" */

void log_flush(void);                   /* service loop only: file calls */
u32 log_room(void);                     /* free bytes in the ring */
/* Boot checkpoint, core task only: at debug level the line goes to the card at once, so a crash right after it leaves a
 * trace (the camera may go down before the next regular write). Below debug - a debug line that is not even kept */
void log_checkpoint(const char *fmt, ...);
void log_service(u32 now_ms);
void log_retry_now(void);               /* the card may be back: no pause before the next try (see log.c) */
/* Before the first write of this power-on: the previous run did not end with a power-off, so CORE.LOG keeps its text and
 * this run is appended to it (up to LOG_KEEP_MAX), instead of starting empty. */
void log_keep_previous(void);

#endif
