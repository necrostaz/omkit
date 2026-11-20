/* OMKit base types: shared by modules, core and the driver. Nothing platform-specific. */
#ifndef OMK_TYPES_H
#define OMK_TYPES_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed short s16;
typedef signed int s32;
typedef unsigned long uptr;                 /* address as a number: 32 bits on the camera, 64 in host tests */

#endif
