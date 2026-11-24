/* The memory the debug service may read for the computer (ptp.c), plain C so that the host tests run it as it is.
 * RAM: 256 MB at 0x60000000 (cached) and 0xa0000000 (uncached); the camera's own buffers lie as high as 0xaf240000.
 * Read past the cache always (the data caches of the three CPUs are not coherent: a read shows memory, not a cache).
 * 0x7..., the windows at 0xb..., the registers at 0x8... and 0x9... - no: a read there may fault or have an effect.
 * 0 - where to read; -1 - not allowed */
#ifndef OMK_MEMREAD_H
#define OMK_MEMREAD_H

#include "omk_types.h"

static inline int mem_read_ok(u32 addr, u32 len, u32 *at)
{
    if (addr >= 0x60000000u && addr < 0x70000000u)
        addr += 0x40000000u;
    if (!len || addr < 0xa0000000u || addr >= 0xb0000000u || len > 0xb0000000u - addr)
        return -1;
    *at = addr;
    return 0;
}

#endif
