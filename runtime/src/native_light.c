/* Native version of the engine's per-vertex lighting (0x006A5600), the hottest
 * function of the game thread indoors (about 14% of it at the start of C01A).
 *
 * void __stdcall light_vertices(const Vertex *v, uint32_t nv, const Light *lights,
 *                               uint32_t nlights, uint32_t flags)
 *
 *   Vertex (0x24 bytes): position x y z at +0, normal x y z at +0x0C.
 *   Light (0x240 bytes; offsets below from +0x1E0, as the guest code keeps it):
 *     -0x40 type   -0x3C flags (bit 0: on when flags & 0x2000)
 *     -0x38 position  -0x2C direction or 3x3 matrix  -0x08 color (3 floats)
 *     +0x04 second color (type 3)   +0x20.. ranges and falloff factors
 *   Per vertex i: acc[i] (0x890FE0 + 16 i, RGB) += factor[i] (0x8911E0 + 16 i) * color * intensity.
 *
 * The game runs with Direct3D's single precision x87 (CreateDevice without
 * FPU_PRESERVE), so the arithmetic here is single precision too, in the guest
 * code's order. Results agree with the recompiled code (which computes in double)
 * to rounding; FF_NATIVE_CHECK=1 (desktop) runs both and reports differences.
 * The function writes only the accumulators (and its own dead stack frame, which
 * no caller reads), returns with eax 0 and pops its 5 arguments. */
#include "recomp.h"
#include "runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#define ACC 0x00890FE0u
#define FACTOR 0x008911E0u

static inline float ldf(uint32_t a)
{
    float v;
    memcpy(&v, g_mem + a, 4);
    return v;
}

static inline void stf(uint32_t a, float v) { memcpy(g_mem + a, &v, 4); }

/* acc[i] += factor[i] * color * k, per channel, as the guest rounds it. */
static inline void accumulate(uint32_t i, uint32_t col, float k)
{
    for (uint32_t ch = 0; ch < 3u; ch++) {
        uint32_t a = ACC + 16u * i + 4u * ch;
        float f = ldf(FACTOR + 16u * i + 4u * ch) * ldf(col + 4u * ch);
        stf(a, f * k + ldf(a));
    }
}

/* Type 1: point light with linear falloff beyond an inner radius. */
static void light_point(uint32_t v, uint32_t nv, uint32_t l)
{
    const float px = ldf(l - 0x38u), py = ldf(l - 0x34u), pz = ldf(l - 0x30u);
    for (uint32_t i = 0; i < nv; i++, v += 0x24u) {
        float dx = ldf(v) - px, dy = ldf(v + 4u) - py, dz = ldf(v + 8u) - pz;
        float nd = dz * ldf(v + 0x14u) + dx * ldf(v + 0x0Cu) + dy * ldf(v + 0x10u);
        if (nd >= 0.0f)
            continue;
        float d2 = dz * dz + dy * dy + dx * dx;
        if (d2 > ldf(l + 0x2Cu))
            continue;
        float dist = sqrtf(d2);
        float att = d2 < ldf(l + 0x24u) ? 1.0f : (ldf(l + 0x28u) - dist) * ldf(l + 0x30u);
        float k = -(nd / dist * att);
        accumulate(i, l - 8u, k);
    }
}

/* Type 0: spot light (point falloff, then a smooth cone). */
static void light_spot(uint32_t v, uint32_t nv, uint32_t l)
{
    const float px = ldf(l - 0x38u), py = ldf(l - 0x34u), pz = ldf(l - 0x30u);
    for (uint32_t i = 0; i < nv; i++, v += 0x24u) {
        float dx = ldf(v) - px, dy = ldf(v + 4u) - py, dz = ldf(v + 8u) - pz;
        float nd = dz * ldf(v + 0x14u) + dx * ldf(v + 0x0Cu) + dy * ldf(v + 0x10u);
        if (nd >= 0.0f)
            continue;
        float d2 = dz * dz + dy * dy + dx * dx;
        if (d2 >= ldf(l + 0x2Cu))   /* (unordered goes on, as in the guest) */
            continue;
        float dist = sqrtf(d2);
        float att = !(dist > ldf(l + 0x20u)) ? 1.0f : (ldf(l + 0x28u) - dist) * ldf(l + 0x30u);
        float inv = 1.0f / dist;
        dx *= inv;
        dy *= inv;
        dz *= inv;
        float sd = dz * ldf(l - 0x24u) + dy * ldf(l - 0x28u) + dx * ldf(l - 0x2Cu);
        if (sd < ldf(l + 0x34u))
            continue;
        float k = -(att * inv * nd);
        if (sd < ldf(l + 0x38u)) {
            float t = (sd - ldf(l + 0x34u)) * ldf(l + 0x40u);
            k *= (3.0f - (t + t)) * t * t;
        }
        accumulate(i, l - 8u, k);
    }
}

/* Type 2: projected light (a rectangular window with soft edges). */
static void light_projected(uint32_t v, uint32_t nv, uint32_t l, uint32_t esp)
{
    const float px = ldf(l - 0x38u), py = ldf(l - 0x34u), pz = ldf(l - 0x30u);
    const uint32_t m = l - 0x2Cu;
    /* The edge factor lives in the guest frame ([esp+0x10]): an iteration that
     * leaves it unset uses the previous one's. */
    float edge = ldf(esp - 0x5Cu + 0x10u);
    for (uint32_t i = 0; i < nv; i++, v += 0x24u) {
        float dx = ldf(v) - px, dy = ldf(v + 4u) - py, dz = ldf(v + 8u) - pz;
        float nd = -(dz * ldf(v + 0x14u) + dx * ldf(v + 0x0Cu) + dy * ldf(v + 0x10u));
        if (nd <= 0.0f)
            continue;
        float x = dx * ldf(m + 0x18u) + dy * ldf(m + 0x1Cu) + dz * ldf(m + 0x20u);
        float y = dx * ldf(m + 0x0Cu) + dy * ldf(m + 0x10u) + dz * ldf(m + 0x14u);
        float z = dx * ldf(m) + dy * ldf(m + 4u) + dz * ldf(m + 8u);
        if (z <= 0.0f)
            continue;
        float dist = sqrtf(z * z + y * y + x * x);
        if (dist > ldf(l + 0x28u))
            continue;
        float att = !(dist > ldf(l + 0x20u)) ? 1.0f : (ldf(l + 0x28u) - dist) * ldf(l + 0x30u);
        float a = nd / dist * att;
        float iz = 1.0f / z;
        float sx = fabsf(x * ldf(l + 0x44u) * iz), sy = fabsf(y * ldf(l + 0x48u) * iz);
        if (sx > 1.0f || sy > 1.0f)
            continue;
        unsigned w = 0;
        if (sx > ldf(l + 0x4Cu)) {
            sx = (sx - ldf(l + 0x4Cu)) * ldf(l + 0x50u);
            w = 1;
        }
        if (sy > ldf(l + 0x4Cu)) {
            sy = (sy - ldf(l + 0x4Cu)) * ldf(l + 0x50u);
            w |= 2;
        }
        switch (w) {
        case 0: edge = 1.0f; break;
        case 1: edge = 1.0f - sx; break;
        case 2: edge = 1.0f - sy; break;
        default: edge = (1.0f - sy) * (1.0f - sx); break;
        }
        float k = (3.0f - (edge + edge)) * edge * edge * a;
        accumulate(i, l - 8u, k);
    }
    stf(esp - 0x5Cu + 0x10u, edge);
}

/* Type 3: ambient (flags 0x2000: color only; 0x1000000: times the factors) or
 * hemispheric: color + second color * (1 - n.dir) / 2, times the factors. */
static void light_ambient(uint32_t v, uint32_t nv, uint32_t l, uint32_t flags)
{
    if (flags & 0x2000u) {
        for (uint32_t i = 0; i < nv; i++)
            for (uint32_t ch = 0; ch < 3u; ch++) {
                uint32_t a = ACC + 16u * i + 4u * ch;
                stf(a, ldf(a) + ldf(l - 8u + 4u * ch));
            }
        return;
    }
    if (flags & 0x1000000u) {
        for (uint32_t i = 0; i < nv; i++)
            accumulate(i, l - 8u, 1.0f);
        return;
    }
    for (uint32_t i = 0; i < nv; i++, v += 0x24u) {
        float s = ((1.0f - ldf(v + 0x14u) * ldf(l - 0x24u)) - ldf(l - 0x28u) * ldf(v + 0x10u) -
                   ldf(v + 0x0Cu) * ldf(l - 0x2Cu)) * 0.5f;
        for (uint32_t ch = 0; ch < 3u; ch++) {
            float c = ldf(l + 4u + 4u * ch) * s + ldf(l - 8u + 4u * ch);
            uint32_t a = ACC + 16u * i + 4u * ch;
            stf(a, c * ldf(FACTOR + 16u * i + 4u * ch) + ldf(a));
        }
    }
}

static int native_light_on = -1;
#if !defined(__vita__)
static unsigned type_calls[4];   /* lights of each type seen (FF_NATIVE_CHECK report) */
#endif

static int light_vertices(Ctx *c, uint32_t esp)
{
    (void)c;
    const uint32_t verts = RD32(esp + 4u), nv = RD32(esp + 8u), lights = RD32(esp + 12u);
    const uint32_t nl = RD32(esp + 16u), flags = RD32(esp + 20u);
    for (uint32_t k = 0; k < nl; k++) {
        const uint32_t l = lights + 0x1E0u + 0x240u * k;
        const uint32_t type = RD32(l - 0x40u);
        if (type > 3u)
            continue;
#if !defined(__vita__)
        type_calls[type]++;
#endif
        if (type == 3u) {
            light_ambient(verts, nv, l, flags);
            continue;
        }
        if ((flags & 0x2000u) && !(RD8(l - 0x3Cu) & 1u))
            continue;
        if (!nv)
            continue;
        if (type == 0u)
            light_spot(verts, nv, l);
        else if (type == 1u)
            light_point(verts, nv, l);
        else
            light_projected(verts, nv, l, esp);
    }
    return 1;
}

#if !defined(__vita__)
/* FF_NATIVE_CHECK=1: run the recompiled code on the same accumulators and report
 * where the results differ by more than rounding. */
static int check_on = -1;
static uint32_t declining;

static void check(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    const uint32_t nv = RD32(esp + 8u);
    const uint32_t n = (nv ? nv : 1u) * 16u;
    uint8_t *before = malloc(n), *native = malloc(n);
    memcpy(before, g_mem + ACC, n);
    const uint32_t edge = RD32(esp - 0x5Cu + 0x10u);
    light_vertices(c, esp);
    memcpy(native, g_mem + ACC, n);
    memcpy(g_mem + ACC, before, n);
    WR32(esp - 0x5Cu + 0x10u, edge);
    declining = 1;
    *ret = rt_call_indirect(c, 0x006A5600u, esp, ecx, eax);
    declining = 0;
    static unsigned calls, bad;
    calls++;
    for (uint32_t i = 0; i < n / 4u; i++) {
        float a, b;
        memcpy(&a, native + 4u * i, 4);
        memcpy(&b, g_mem + ACC + 4u * i, 4);
        float tol = 1e-4f * (fabsf(b) > 1.0f ? fabsf(b) : 1.0f);
        if (fabsf(a - b) > tol && bad++ < 20)
            rt_log("native light: call %u acc[%u] native %g recompiled %g", calls, i, (double)a, (double)b);
    }
    if (calls % 5000u == 0u)
        rt_log("native light: %u calls checked, %u differences (lights by type: spot %u point %u projected %u "
               "ambient %u)", calls, bad, type_calls[0], type_calls[1], type_calls[2], type_calls[3]);
    free(before);
    free(native);
}
#endif

int rt_native_light_vertices(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    if (native_light_on < 0) {
        native_light_on = 1;
#if defined(__vita__)
        FILE *f = fopen("ux0:data/ff/ff.cfg", "r");
        if (f) {
            char buf[128];
            while (fgets(buf, sizeof(buf), f))
                if (!strncmp(buf, "native=0", 8))
                    native_light_on = 0;
            fclose(f);
        }
#else
        const char *e = getenv("FF_NATIVE");
        native_light_on = !(e && e[0] == '0');
        const char *k = getenv("FF_NATIVE_CHECK");
        check_on = k && k[0] == '1';
#endif
        rt_log("native lighting: %s", native_light_on ? "on" : "off");
    }
    if (!native_light_on)
        return 0;
#if !defined(__vita__)
    if (declining)
        return 0;
    if (check_on) {
        check(c, esp, ecx, eax, ret);
        return 1;
    }
#endif
    (void)ecx;
    (void)eax;
    light_vertices(c, esp);
    *ret = ((uint64_t)(esp + 4u + 0x14u) << 32) | 0u;
    return 1;
}
