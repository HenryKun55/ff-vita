/* Native version of the engine's oriented box overlap test (0x005FDEA0, the
 * separating axis test of two boxes, used by the collision code: 11 callers).
 *
 * bool overlap(const Mat3 *ra (ecx), const Vec3 *ca (edx), const Vec3 *ea,
 *              const Mat3 *rb, const Vec3 *cb, const Vec3 *eb)   (4 stack arguments)
 *   Box A: axes ra (3 rows of 3 floats), centre ca, half sizes ea; box B likewise.
 *   U0..U2 = B's axes in A's frame and T = (cb - ca) in A's frame (v' = (v0 r6 +
 *   v1 r7) + v2 r8, (v0 r3 + v1 r4) + v2 r5, (v0 r0 + v1 r1) + v2 r2), |U| + 1e-6,
 *   then the 15 axes: A's 3, B's 3 and the 9 cross products. An axis separates
 *   unless sum >= |projection| (a NaN separates, as the guest's fcompp/C0 test).
 *   Returns 1 when no axis separates the boxes, 0 otherwise (eax).
 *
 * Decompiled with Ghidra (ReVa) and the operation order taken from the x87 code;
 * single precision as the game's 24-bit x87. ff.cfg native_check=1 (or
 * FF_NATIVE_CHECK=1 on the desktop) runs the recompiled code too and compares. */
#include "recomp.h"
#include "runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

/* v in A's frame (the guest's 0x606ED0-style order). */
static void to_a(uint32_t r, const float v[3], float o[3])
{
    o[0] = (v[0] * lf(r + 0x18u) + v[1] * lf(r + 0x1Cu)) + v[2] * lf(r + 0x20u);
    o[1] = (v[0] * lf(r + 0x0Cu) + v[1] * lf(r + 0x10u)) + v[2] * lf(r + 0x14u);
    o[2] = (v[0] * lf(r) + v[1] * lf(r + 4u)) + v[2] * lf(r + 8u);
}

/* Separated unless s >= l (ordered). */
#define SEP(s, l) (!((s) >= (l)))

static uint32_t overlap(uint32_t ra, uint32_t ca, uint32_t ea, uint32_t rb, uint32_t cb, uint32_t eb)
{
    float v[3], U0[3], U1[3], U2[3], T[3];
    v[0] = lf(rb + 0x18u), v[1] = lf(rb + 0x1Cu), v[2] = lf(rb + 0x20u);
    to_a(ra, v, U2);
    v[0] = lf(rb + 0x0Cu), v[1] = lf(rb + 0x10u), v[2] = lf(rb + 0x14u);
    to_a(ra, v, U1);
    v[0] = lf(rb), v[1] = lf(rb + 4u), v[2] = lf(rb + 8u);
    to_a(ra, v, U0);
    v[0] = lf(cb) - lf(ca), v[1] = lf(cb + 4u) - lf(ca + 4u), v[2] = lf(cb + 8u) - lf(ca + 8u);
    to_a(ra, v, T);
    const float eps = 1e-6f;   /* [0x6FF870] */
    const float aU2x = fabsf(U2[0]) + eps, aU1x = fabsf(U1[0]) + eps, aU0x = fabsf(U0[0]) + eps;
    const float aU2y = fabsf(U2[1]) + eps, aU1y = fabsf(U1[1]) + eps, aU0y = fabsf(U0[1]) + eps;
    const float aU2z = fabsf(U2[2]) + eps, aU1z = fabsf(U1[2]) + eps, aU0z = fabsf(U0[2]) + eps;
    const float a0 = lf(ea), a1 = lf(ea + 4u), a2 = lf(ea + 8u);
    const float e0 = lf(eb), e1 = lf(eb + 4u), e2 = lf(eb + 8u);
    const float Tx = T[0], Ty = T[1], Tz = T[2];

    /* A's axes and B's axes. */
    if (SEP(((aU0x * e2 + aU1x * e1) + aU2x * e0) + a0, fabsf(Tx)))
        return 0;
    if (SEP(((aU2x * a0 + aU2z * a2) + aU2y * a1) + e0, fabsf((Tz * U2[2] + Ty * U2[1]) + Tx * U2[0])))
        return 0;
    if (SEP(((aU0y * e2 + aU1y * e1) + aU2y * e0) + a1, fabsf(Ty)))
        return 0;
    if (SEP(((aU0z * e2 + aU1z * e1) + aU2z * e0) + a2, fabsf(Tz)))
        return 0;
    if (SEP(((aU1x * a0 + aU1z * a2) + aU1y * a1) + e1, fabsf((Tz * U1[2] + Ty * U1[1]) + Tx * U1[0])))
        return 0;
    if (SEP(((aU0x * a0 + aU0z * a2) + aU0y * a1) + e2, fabsf((Tz * U0[2] + Ty * U0[1]) + Tx * U0[0])))
        return 0;
    /* The cross products. */
    if (SEP(((aU0x * e1 + aU1x * e2) + aU2z * a1) + aU2y * a2, fabsf(Tz * U2[1] - Ty * U2[2])))
        return 0;
    if (SEP(((aU2x * e2 + aU1z * a1) + aU1y * a2) + aU0x * e0, fabsf(Tz * U1[1] - Ty * U1[2])))
        return 0;
    if (SEP(((aU2x * e1 + aU0z * a1) + aU0y * a2) + aU1x * e0, fabsf(Tz * U0[1] - Ty * U0[2])))
        return 0;
    if (SEP(((aU0y * e1 + aU1y * e2) + aU2z * a0) + aU2x * a2, fabsf(Tx * U2[2] - Tz * U2[0])))
        return 0;
    if (SEP(((aU2y * e2 + aU1z * a0) + aU1x * a2) + aU0y * e0, fabsf(Tx * U1[2] - Tz * U1[0])))
        return 0;
    if (SEP(((aU2y * e1 + aU0z * a0) + aU0x * a2) + aU1y * e0, fabsf(Tx * U0[2] - Tz * U0[0])))
        return 0;
    if (SEP(((aU0z * e1 + aU1z * e2) + aU2y * a0) + aU2x * a1, fabsf(Ty * U2[0] - Tx * U2[1])))
        return 0;
    if (SEP(((aU2z * e2 + aU1y * a0) + aU1x * a1) + aU0z * e0, fabsf(Ty * U1[0] - Tx * U1[1])))
        return 0;
    if (SEP(((aU2z * e1 + aU0y * a0) + aU0x * a1) + aU1z * e0, fabsf(Ty * U0[0] - Tx * U0[1])))
        return 0;
    return 1;
}

static int on = -1, check, declining;

int rt_native_obb_overlap(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    if (on < 0) {
        on = 1;
#if defined(__vita__)
        FILE *f = fopen("ux0:data/ff/ff.cfg", "r");
        if (f) {
            char buf[128];
            while (fgets(buf, sizeof(buf), f)) {
                if (!strncmp(buf, "native=0", 8))
                    on = 0;
                if (!strncmp(buf, "native_check=1", 14))
                    check = 1;
            }
            fclose(f);
        }
#else
        const char *e = getenv("FF_NATIVE"), *k = getenv("FF_NATIVE_CHECK");
        on = !(e && e[0] == '0');
        check = k && k[0] == '1';
#endif
    }
    if (!on || declining)
        return 0;
    const uint32_t edx = c->r[2];
    const uint32_t r = overlap(ecx, edx, RD32(esp + 4u), RD32(esp + 8u), RD32(esp + 12u), RD32(esp + 16u));
    if (check) {
        declining = 1;
        *ret = rt_call_indirect(c, 0x005FDEA0u, esp, ecx, eax);
        declining = 0;
        static unsigned calls, bad;
        calls++;
        if (((uint32_t)*ret & 0xFFu) != r && bad++ < 20)
            rt_log("native obb: call %u native %u recompiled %u", calls, r, (uint32_t)*ret & 0xFFu);
        if (calls % 20000u == 0u)
            rt_log("native obb: %u calls checked, %u differ", calls, bad);
        return 1;
    }
    *ret = ((uint64_t)(esp + 4u + 16u) << 32) | r;   /* 4 stack arguments popped */
    return 1;
}
