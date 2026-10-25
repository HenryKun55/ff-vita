/* Native version of the engine's in-place 3x3 matrix product (0x005F87F0).
 *
 * void mat3_mul_inplace(Mat3 *a (ecx), const Mat3 *m (edx))   (no stack arguments, plain ret)
 *   Each row v of a (3 floats) through m (9 floats) back into the same row of a,
 *   the same row formula as 0x005F8A90 (native_mat3.c):
 *     a.x = (v0 m6 + v1 m3) + v2 m0
 *     a.y = (v0 m7 + v1 m4) + v2 m1
 *     a.z = (v0 m8 + v1 m5) + v2 m2
 *   rows in the guest's order: row 2 (+0x18), row 1 (+0x0C), row 0 (+0). Each row
 *   reads all of its inputs before its three stores, so m = a gives what the
 *   guest gives (a row written is seen by the next rows' reads of m).
 *   Returns with eax = a + 0x0C, ecx = a, edx = m (as the x86 leaves them).
 *
 * Taken from the x86 code (the x87 stack traced instruction by instruction; the
 * Ghidra/ReVa server did not answer for this one). Single precision as the
 * game's 24-bit x87. The 12 bytes the guest keeps below esp are dead stack. */
#include "recomp.h"
#include "runtime.h"

#include <string.h>

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

static inline float lf(uint32_t a)
{
    float v;
    memcpy(&v, g_mem + a, 4);
    return v;
}

static inline void sf(uint32_t a, float v)
{
    memcpy(g_mem + a, &v, 4);
}

/* One row of a at r, in place. */
static inline void row(uint32_t r, uint32_t m)
{
    const float v0 = lf(r), v1 = lf(r + 4u), v2 = lf(r + 8u);
    const float x = (v0 * lf(m + 0x18u) + v1 * lf(m + 0x0Cu)) + v2 * lf(m);
    const float y = (v0 * lf(m + 0x1Cu) + v1 * lf(m + 0x10u)) + v2 * lf(m + 4u);
    const float z = (v0 * lf(m + 0x20u) + v1 * lf(m + 0x14u)) + v2 * lf(m + 8u);
    sf(r, x);
    sf(r + 4u, y);
    sf(r + 8u, z);
}

int rt_native_mat3_mul_inplace(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)eax;
    const uint32_t a = ecx, m = c->r[2];
    row(a + 0x18u, m);
    row(a + 0x0Cu, m);
    row(a, m);
    c->r[1] = a;
    c->r[2] = m;
    *ret = ((uint64_t)(esp + 4u) << 32) | (a + 0x0Cu);   /* cdecl, plain ret */
    return 1;
}
