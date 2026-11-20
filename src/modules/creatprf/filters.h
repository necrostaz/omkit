/* CREATPRF: the alternative colour filters of monochrome profiles, computed from the camera's own matrices */
#ifndef CREATPRF_FILTERS_H
#define CREATPRF_FILTERS_H

#include "omk_types.h"

/* The softer level 2 of a filter: the midpoint of the camera's levels 1 and 2. Matrices are 3 x 3 by rows, each row sums
 * to 256 (the camera's fixed point); the result's rows do too */
void alt_level2(const s16 *level1, const s16 *level2, s16 *out);

#endif
