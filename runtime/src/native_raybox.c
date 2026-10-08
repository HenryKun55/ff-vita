/* Native version of the engine's segment / axis-aligned box test (0x005FF460),
 * about 7% of the game thread (collision and visibility queries).
 *
 * bool __cdecl segment_box(float minx, miny, minz, maxx, maxy, maxz,
 *                          float ox, oy, oz, float dx, dy, dz, float *t)
 *
 * Origin inside the box: *t = 0, true. Otherwise the slab method: the entry
 * parameter on each axis the segment moves along (planes facing the origin), the
 * largest of them, and the hit point inside the other two slabs; *t is written
 * when the point is, and the result is 0 <= *t <= 1. Every comparison keeps the
 * guest's outcome for unordered operands. Single precision, as the game's x87.
 * Side effects as the guest's: *t, and the dz argument slot gets the second
 * coordinate of the hit point. Callers test al only. */
#include "recomp.h"
#include "runtime.h"
#include "native.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

static inline float ldf(uint32_t a)
{
    float v;
    memcpy(&v, g_mem + a, 4);
    return v;
}

static inline void stf(uint32_t a, float v) { memcpy(g_mem + a, &v, 4); }

/* a <= b for the guest's `fcomp; test ah, 0x41; jp` (unordered counts as above). */
static inline int le(float a, float b) { return a <= b; }

int ff_segment_box(const float box[6], const float o[3], const float d[3], float *t_out, float *dz_slot)
{
    const float minx = box[0], miny = box[1], minz = box[2], maxx = box[3], maxy = box[4], maxz = box[5];
    const float ox = o[0], oy = o[1], oz = o[2], dx = d[0], dy = d[1], dz = d[2];
    if (le(minx, ox) && le(ox, maxx) && le(miny, oy) && le(oy, maxy) && le(minz, oz) && le(oz, maxz)) {
        *t_out = 0.0f;
        return 1;
    }
    float t0 = -1.0f, t1 = -1.0f, t2 = -1.0f;
    if (dx != 0.0f)
        t0 = ((dx < 0.0f ? maxx : minx) - ox) / dx;
    if (dy != 0.0f)
        t1 = ((dy < 0.0f ? maxy : miny) - oy) / dy;
    if (dz != 0.0f)
        t2 = ((dz < 0.0f ? maxz : minz) - oz) / dz;
    float t, slot;
    int in;
    if (t0 >= t1 && t0 >= t2) {
        t = t0;
        float py = t * dy + oy, pz = t * dz + oz;
        slot = pz;
        in = !(miny > py || py > maxy || minz > pz || pz > maxz);
    } else if (t1 >= t2) {
        t = t1;
        float pz = t * dz + oz, px = t * dx + ox;
        slot = px;
        in = !(minz > pz || pz > maxz || minx > px || px > maxx);
    } else {
        t = t2;
        float px = t * dx + ox, py = t * dy + oy;
        slot = py;
        in = !(minx > px || px > maxx || miny > py || py > maxy);
    }
    if (dz_slot)
        *dz_slot = slot;
    if (!in)
        return 0;
    *t_out = t;
    return t >= 0.0f && t <= 1.0f;
}

static uint32_t segment_box(uint32_t esp)
{
    const uint32_t A = esp + 4u;   /* first argument */
    float box[6], o[3], d[3];
    for (int i = 0; i < 6; i++)
        box[i] = ldf(A + 4u * (uint32_t)i);
    for (int i = 0; i < 3; i++) {
        o[i] = ldf(A + 24u + 4u * (uint32_t)i);
        d[i] = ldf(A + 36u + 4u * (uint32_t)i);
    }
    const uint32_t out = RD32(A + 48u);
    float t = ldf(out), slot = ldf(A + 44u);
    const int r = ff_segment_box(box, o, d, &t, &slot);
    stf(A + 44u, slot);
    stf(out, t);
    return (uint32_t)r;
}

static int on = -1;
#if !defined(__vita__)
static int check_on;
static int declining;
#endif

int rt_native_segment_box(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    if (on < 0) {
        on = 1;
#if defined(__vita__)
        FILE *f = fopen("ux0:data/ff/ff.cfg", "r");
        if (f) {
            char buf[128];
            while (fgets(buf, sizeof(buf), f))
                if (!strncmp(buf, "native=0", 8))
                    on = 0;
            fclose(f);
        }
#else
        const char *e = getenv("FF_NATIVE");
        on = !(e && e[0] == '0');
        const char *k = getenv("FF_NATIVE_CHECK");
        check_on = k && k[0] == '1';
#endif
    }
    if (!on)
        return 0;
#if !defined(__vita__)
    if (declining)
        return 0;
    if (check_on) {
        /* Native first, then the recompiled code on the same input; compare. */
        const uint32_t A = esp + 4u, out = RD32(A + 48u);
        uint32_t dz_slot = RD32(A + 44u), out_before = RD32(out);
        uint32_t r = segment_box(esp);
        uint32_t n_out = RD32(out), n_slot = RD32(A + 44u);
        WR32(out, out_before);
        WR32(A + 44u, dz_slot);
        declining = 1;
        *ret = rt_call_indirect(c, 0x005FF460u, esp, ecx, eax);
        declining = 0;
        static unsigned calls, bad, rounding;
        calls++;
        float a, b;
        memcpy(&a, &n_out, 4);
        float rb = ldf(out);
        b = rb;
        int same_t = n_out == RD32(out) || fabsf(a - b) <= 1e-5f * (fabsf(b) > 1.0f ? fabsf(b) : 1.0f);
        if (((uint32_t)*ret & 0xFFu) != r) {
            if (bad++ < 20)
                rt_log("native segment_box: call %u result %u/%u t %08X/%08X", calls, r, (uint32_t)*ret & 0xFFu,
                       n_out, RD32(out));
        } else if (!same_t || n_slot != RD32(A + 44u)) {
            rounding++;
        }
        if (calls % 100000u == 0u)
            rt_log("native segment_box: %u calls checked, %u different results, %u rounding differences", calls,
                   bad, rounding);
        return 1;
    }
#endif
    (void)ecx;
    uint32_t r = segment_box(esp);
    *ret = ((uint64_t)(esp + 4u) << 32) | (eax & 0xFFFFFF00u) | r;
    return 1;
}
