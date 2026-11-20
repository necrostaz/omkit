/* core memory manager. Modules never allocate memory themselves: everything goes through core.
 * Sources: driver regions (the arena) and, if enabled in OMKIT.INI, the firmware heap with a limit, for data only. */
#ifndef OMK_MEM_H
#define OMK_MEM_H

#include "omk_api.h"                /* omk_mem_info */

#define MEM_OWNER_CORE 0
#define MEM_OWNERS 32
#define MEM_IMAGE 1                         /* code, resources, core data: arena only */
/* The block's data starts and ends on a MEM_LINE boundary (the size is rounded up to it), arena only. For memory that is
 * used through a data cache while its neighbours in the arena are used past it - a module's image, whose variables go
 * through the cache of CPU 0: a cache line written back never carries a neighbour's bytes (a block header, another
 * module's allocation) as they were when the line was read. 64: the first-level line is 16 bytes, a second level with
 * a longer line is not ruled out, and 64 is what the driver's blocks shared between the CPUs are aligned to for the
 * same reason. Costs up to 112 bytes in front (left as a free block) and 63 at the end */
#define MEM_LINES 2
#define MEM_LINE 64

void mem_init(void);
void mem_setup(int fw_heap, u32 fw_limit);  /* from [core]: firmware heap and its limit */
int mem_owner(const char *name, u32 limit); /* owner number or -1; limit 0 - unlimited */
const char *mem_owner_name(int owner);

void *mem_alloc_for(int owner, u32 size, u32 flags);    /* aligned to 16 (MEM_LINES: to MEM_LINE); 0 - out of memory, reason in the log */
void *mem_alloc(u32 size);                  /* for core: arena */
void mem_free(void *p);

void mem_info(int owner, omk_mem_info *out);
void mem_arena(u32 *total, u32 *used, u32 *largest);
void mem_service(u32 now);                  /* service loop: failure log, fill level, firmware heap */

void mem_copy(void *dst, const void *src, u32 n);   /* the regions may overlap */
void mem_set(void *dst, int c, u32 n);
int mem_block_owner(void *p);            /* block owner or -1 */
void mem_free_owner(int owner, void *keep);   /* all blocks of the owner in the arena except keep: for a module that failed to start */

#endif
