/* A test scenario without hands (core/handsfree.c): OMKIT\SHOOT.RUN with hands_free = on. While it runs the camera
 * treats the USB cable as charge only - it shoots and keeps off the computer - whatever its USB mode; at the scenario's
 * end (or a failure) core writes OMKIT\SHOOT.END, lets USB go and restarts the camera, which comes back in its own USB
 * mode (MTP) for the computer. Core task only. */
#ifndef OMK_HANDSFREE_H
#define OMK_HANDSFREE_H

#include "omk_types.h"

#define HF_RESULT_FILE "SHOOT.END"          /* in the OMKit folder: the result, for the computer after the restart */
#define HF_RESULT_SIZE 256                  /* written at this fixed length: rewritten in place, the folder unchanged */

int hf_supported(void);                     /* the driver can hold USB as charge only */
/* core_main right after SHOOT.RUN was read (shoot_once), before the other hooks: the hold, if the file asks for it */
void hf_setup(int once);
/* The core loop, after shoot_service: the scenario's end, a stuck application, the restart's stages */
void hf_service(u32 now);
int hf_active(void);                        /* 1 - USB is held as charge only now */

#endif
