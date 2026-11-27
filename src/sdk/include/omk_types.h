/* OMKit base types: shared by modules, core and the driver. Nothing platform-specific. */
#ifndef OMK_TYPES_H
#define OMK_TYPES_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed short s16;
typedef signed int s32;
typedef unsigned long uptr;                 /* address as a number: 32 bits on the camera, 64 in host tests */

/* The build's kind (BUILD=release | debug of the Makefiles): OMK_DEBUG is defined in a debug build only. A release build
 * has only what the user needs; everything for development - the debug service, dumps, probes of the camera's state, test
 * modes, the log's debug level - is compiled into a debug build alone, never switched by a setting */
#ifdef OMK_DEBUG
#define OMK_BUILD "debug"
#else
#define OMK_BUILD "release"
#endif

#endif
