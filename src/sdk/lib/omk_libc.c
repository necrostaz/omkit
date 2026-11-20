/* Minimal libc for modules: GCC itself calls memcpy and memset when copying structures and zeroing arrays.
 * Built into every module (OMKit/src/modules/Makefile). */
#include "omk_types.h"

typedef unsigned long size_t;

/* Word-wise when source and destination have the same alignment, bytes otherwise; overlapping regions are allowed.
 * may_alias: the words may be parts of any object (GCC's way to stay within the aliasing rules) */
typedef u32 __attribute__((may_alias)) word_t;

static void copy_up(u8 *d, const u8 *s, u32 n)
{
    if (!(((uptr)d ^ (uptr)s) & 3)) {
        for (; ((uptr)d & 3) && n; n--)
            *d++ = *s++;
        for (; n >= 4; n -= 4, d += 4, s += 4)
            *(word_t *)d = *(const word_t *)s;
    }
    while (n--)
        *d++ = *s++;
}

static void copy_down(u8 *d, const u8 *s, u32 n)
{
    d += n;
    s += n;
    if (!(((uptr)d ^ (uptr)s) & 3)) {
        for (; ((uptr)d & 3) && n; n--)
            *--d = *--s;
        for (; n >= 4; n -= 4) {
            d -= 4;
            s -= 4;
            *(word_t *)d = *(const word_t *)s;
        }
    }
    while (n--)
        *--d = *--s;
}

static void fill(u8 *d, int c, u32 n)
{
    u32 w = (u8)c * 0x01010101u;

    for (; ((uptr)d & 3) && n; n--)
        *d++ = c;
    for (; n >= 4; n -= 4, d += 4)
        *(word_t *)d = w;
    while (n--)
        *d++ = c;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    copy_up(dst, src, n);
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *p = src;

    if (d <= p || d >= p + n)
        copy_up(d, p, n);
    else
        copy_down(d, p, n);
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    fill(dst, c, n);
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;

    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}
