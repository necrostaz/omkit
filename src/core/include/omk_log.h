/* Log for core and modules: a RAM buffer, written to the card by the core service loop, not by the calling task. */
#ifndef OMK_LOG_H
#define OMK_LOG_H

#include <stdarg.h>
#include "omk_api.h"                /* OMK_LOG_* levels */
#include "omk_drv.h"

void log_setup(int enabled, int level);
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

int log_flush(void);                    /* service loop only: file calls. 1 - the log file has the text logged so far */
/* The log file is kept open between writes (log.c): closed by the core task when the camera leaves the running state,
 * before a file job of the debug service, and by the card-off gate after its last flush */
void log_close(void);
u32 log_room(void);                     /* free bytes in the ring */
/* Boot checkpoint, core task only: at debug level the line goes to the card at once, so a crash right after it leaves a
 * trace (the camera may go down before the next regular write). Below debug - a debug line that is not even kept */
void log_checkpoint(const char *fmt, ...);
void log_service(u32 now_ms);
void log_retry_now(void);               /* the card may be back: no pause before the next try (see log.c) */
/* The header of a run: an empty line, then the line at info level (CORE.LOG holds many runs one after another) */
void log_banner(const char *fmt, ...);

#endif
