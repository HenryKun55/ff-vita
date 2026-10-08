/* Shared by the native versions of game functions (runtime/src/native_*.c). Not
 * included by the generated code, so changes here rebuild only those files. */
#ifndef FF_NATIVE_H
#define FF_NATIVE_H

#include <stdint.h>

/* The segment / axis-aligned box test of 0x005FF460 (native_raybox.c) on host
 * values: box = min x y z, max x y z; o + t d the segment. Returns 1 on a hit with
 * 0 <= t <= 1; *t is written as the guest writes it (0 when o is inside, the
 * entry parameter when the hit point is inside the box's other two slabs). The
 * guest's dz argument slot gets that hit point coordinate; *dz_slot (if not NULL)
 * receives it the same way. */
int ff_segment_box(const float box[6], const float o[3], const float d[3], float *t, float *dz_slot);

#endif
