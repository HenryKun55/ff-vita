/* Native version of the engine's ray/triangle intersection (0x006002A0, the
 * Moller-Trumbore test without the 1/det scaling; called from 0x00600580 and
 * others, 670k calls in two scenes).
 *
 * bool ray_tri(const Vec3 *o, const Vec3 *d, const Vec3 *v0, const Vec3 *v1,
 *              const Vec3 *v2, float *t, bool cull)   (cdecl, 7 stack arguments)
 *   e1 = v1 - v0, e2 = v2 - v0, p = d x e2:
 *     p = (e2z dy - e2y dz, e2x dz - e2z dx, e2y dx - e2x dy)
 *   det = (pz e1z + py e1y) + px e1x.
 *   cull == 0: det > 0 misses. det in (-1e-6, 1e-6) (strictly above -1e-6 and
 *   below 1e-6) misses. k = det * 1e-4, s = o - v0, u = (sz pz + sy py) + sx px,
 *   q = s x e1 = (sy e1z - sz e1y, sz e1x - sx e1z, sx e1y - sy e1x),
 *   v = (qz dz + qx dx) + qy dy, k2 = k + det, w = u + v.
 *   det < 0 (negative branch): misses if u > -k, u < k2, v > -k or w < k2.
 *   otherwise: misses if u < -k, u > k2, v < -k or w > k2.
 *   A hit stores *t = ((qz e2z + qy e2y) + qx e2x) / det and returns al = 1;
 *   a miss returns al = 0 (*t untouched).
 *   The guest reuses its argument slots: det at [esp+0x10] (v1's slot),
 *   k then k2 at [esp+0x14] (v2's), -k at [esp+0x0C] (v0's), u at [esp+0x1C]
 *   (cull's, all 4 bytes); they are written here too, on the same paths.
 *
 * eax: the upper bits are whatever the guest leaves (the low byte is the
 * result). They come from the `fnstsw ax` the recompiled code keeps (the
 * generator drops the one at 0x00600368, whose only use is the following test),
 * with the x87 status word and TOP as the recompiled code computes them
 * (rt_fcom/rt_fstsw in recomp.h); c->fsw is left as it leaves it. ecx = v0 and
 * edx = d on return. The x87 stack is left as found.
 *
 * Taken from the x86 code and the generated C (the Ghidra/ReVa server did not
 * answer for this one); the operation order is the x87 code's. Single precision
 * as the game's 24-bit x87 (the recompiled code keeps unrounded doubles between
 * stores, so rare last-bit differences of the compares are possible). */
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

/* ah & 5 has even parity: not "below" (a > b, a == b or unordered). */
#define PF(eax) rt_parity(((eax) >> 8) & 5u)
/* ah & 0x41 == 0: "above" (a > b). */
#define GT(eax) ((((eax) >> 8) & 0x41u) == 0u)

int rt_native_ray_tri(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)ecx;
    const uint32_t top0 = c->ftop;
    uint32_t sw = c->fsw;
    const uint32_t o = RD32(esp + 4u), d = RD32(esp + 8u), v0 = RD32(esp + 12u);
    const uint32_t v1 = RD32(esp + 16u), v2 = RD32(esp + 20u), tp = RD32(esp + 24u);
    const uint32_t cull = g_mem[esp + 28u];
#define FSTSW(a, b, t) \
    (sw = rt_fcom(sw, (a), (b)), eax = (eax & 0xFFFF0000u) | (rt_fstsw(sw, (top0 + (t)) & 7u) & 0xFFFFu))
    float qx, qy, qz;

    eax = (v2 & 0xFFFFFF00u) | cull;
    const float e1x = lf(v1) - lf(v0), e1y = lf(v1 + 4u) - lf(v0 + 4u), e1z = lf(v1 + 8u) - lf(v0 + 8u);
    const float e2x = lf(v2) - lf(v0), e2y = lf(v2 + 4u) - lf(v0 + 4u), e2z = lf(v2 + 8u) - lf(v0 + 8u);
    const float dx = lf(d), dy = lf(d + 4u), dz = lf(d + 8u);
    const float px = e2z * dy - e2y * dz;
    const float py = e2x * dz - e2z * dx;
    const float pz = e2y * dx - e2x * dy;
    const float det = (pz * e1z + py * e1y) + px * e1x;
    sf(esp + 0x10u, det);
    const float zero = lf(0x006FF720u), neps = lf(0x0071B5ACu), peps = lf(0x006FF870u);
    if (!cull) {
        FSTSW(det, zero, 0u);
        if (GT(eax))
            goto miss;
    }
    if (det > neps) {   /* 0x00600362: the recompiled code drops this fnstsw */
        FSTSW(det, peps, 0u);
        if (!PF(eax))
            goto miss;
    }
    const float k = det * lf(0x00713B68u);
    sf(esp + 0x14u, k);
    eax = o;
    const float sx = lf(o) - lf(v0), sy = lf(o + 4u) - lf(v0 + 4u), sz = lf(o + 8u) - lf(v0 + 8u);
    const float u = (sz * pz + sy * py) + sx * px;
    sf(esp + 0x1Cu, u);
    const float nk = -k;
    sf(esp + 0x0Cu, nk);
    FSTSW(det, zero, 7u);
    const int det_not_below = PF(eax);
    FSTSW(u, nk, 0u);
    if (!det_not_below) {
        /* det < 0 */
        if (GT(eax))
            goto miss;
        const float k2 = k + det;
        sf(esp + 0x14u, k2);
        FSTSW(u, k2, 0u);
        if (!PF(eax))
            goto miss;
        qx = sy * e1z - sz * e1y;
        qy = sz * e1x - sx * e1z;
        qz = sx * e1y - sy * e1x;
        const float v = (qz * lf(d + 8u) + qx * lf(d)) + qy * lf(d + 4u);
        FSTSW(v, nk, 7u);
        if (GT(eax))
            goto miss;
        const float w = u + v;
        FSTSW(w, k2, 7u);
        if (!PF(eax))
            goto miss;
    } else {
        if (!PF(eax))
            goto miss;
        const float k2 = k + det;
        sf(esp + 0x14u, k2);
        FSTSW(u, k2, 0u);
        if (GT(eax))
            goto miss;
        qx = sy * e1z - sz * e1y;
        qy = sz * e1x - sx * e1z;
        qz = sx * e1y - sy * e1x;
        const float v = (qz * lf(d + 8u) + qx * lf(d)) + qy * lf(d + 4u);
        FSTSW(v, nk, 7u);
        if (!PF(eax))
            goto miss;
        const float w = u + v;
        FSTSW(w, k2, 7u);
        if (GT(eax))
            goto miss;
    }
    /* Hit (0x0060053D). */
    sf(tp, ((qz * e2z + qy * e2y) + qx * e2x) / det);
    eax = (tp & 0xFFFFFF00u) | 1u;
    goto out;
miss:
    eax &= 0xFFFFFF00u;
out:
#undef FSTSW
    c->r[1] = v0;
    c->r[2] = d;
    c->fsw = sw;
    *ret = ((uint64_t)(esp + 4u) << 32) | eax;   /* cdecl */
    return 1;
}
