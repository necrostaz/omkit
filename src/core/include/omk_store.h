/* Module store: a dictionary of key (2 bytes) to value (bytes).
 * Persistent values are written to the card by the core service loop 1.5 s after the last change, alternating between two
 * files: NAME.STA and NAME.STB, each with a version number and CRC32. At load the intact version with the higher number wins.
 * Reads come from memory only. Calls are short and can be made from any task on CPU 0. */
#ifndef OMK_STORE_H
#define OMK_STORE_H

#include "omk_api.h"

omk_store *store_open(const char *name, u32 size);  /* name up to 8 characters A-Z 0-9 _; size is the space for records */
omk_store *store_open_for(const char *name, u32 size, int owner);  /* the memory belongs to owner */
int store_get(omk_store *s, u16 key, void *buf, u32 max);   /* value length or -1; copies at most max */
int store_set(omk_store *s, u16 key, const void *data, u32 len, u32 flags);   /* 0, or -1: no space */
int store_del(omk_store *s, u16 key);                         /* 0, or -1: no such key */

void store_close(omk_store *s);         /* without writing; for a module that failed to start */
void store_service(u32 now_ms);         /* service loop only: file calls */
void store_save_all(void);
void store_retry_now(void);             /* the card may be back: failing saves are tried again without their pause */
void store_save_now(omk_store *s);      /* service task only: write this store now if it has changes */
void store_info(omk_store *s, u32 *keys, u32 *used, u32 *seq);

#endif
