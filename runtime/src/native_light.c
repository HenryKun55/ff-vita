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
#include "nstat.h"

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

static inline float lf(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

/* The light's constants, read once per call (the loops below write only the
 * accumulators, which never overlap a light, a vertex or a factor). */
typedef struct LightK {
    float px, py, pz;      /* -0x38 position */
    float m[9];            /* -0x2C direction (3) or matrix (9) */
    float col[3];          /* -0x08 colour */
    float col2[3];         /* +0x04 second colour */
    float r20, r24, r28, r2c, r30, c34, c38, c40, s44, s48, e4c, e50;
} LightK;

static void light_load(LightK *k, uint32_t l)
{
    const uint8_t *p = g_mem + l;
    k->px = lf(p - 0x38);
    k->py = lf(p - 0x34);
    k->pz = lf(p - 0x30);
    for (int j = 0; j < 9; j++)
        k->m[j] = lf(p - 0x2C + 4 * j);
    for (int j = 0; j < 3; j++) {
        k->col[j] = lf(p - 0x08 + 4 * j);
        k->col2[j] = lf(p + 0x04 + 4 * j);
    }
    k->r20 = lf(p + 0x20);
    k->r24 = lf(p + 0x24);
    k->r28 = lf(p + 0x28);
    k->r2c = lf(p + 0x2C);
    k->r30 = lf(p + 0x30);
    k->c34 = lf(p + 0x34);
    k->c38 = lf(p + 0x38);
    k->c40 = lf(p + 0x40);
    k->s44 = lf(p + 0x44);
    k->s48 = lf(p + 0x48);
    k->e4c = lf(p + 0x4C);
    k->e50 = lf(p + 0x50);
}

/* acc += factor * color * k, per channel, as the guest rounds it. */
static inline void accumulate(float *acc, const float *factor, const float *col, float k)
{
    for (int ch = 0; ch < 3; ch++)
        acc[ch] = (factor[ch] * col[ch]) * k + acc[ch];
}

static inline float *acc_of(uint32_t i) { return (float *)(void *)(g_mem + ACC + 16u * i); }
static inline const float *factor_of(uint32_t i) { return (const float *)(const void *)(g_mem + FACTOR + 16u * i); }

/* Type 1: point light with linear falloff beyond an inner radius. */
static void light_point(uint32_t v, uint32_t nv, const LightK *L)
{
    const float px = L->px, py = L->py, pz = L->pz, r24 = L->r24, r28 = L->r28, r2c = L->r2c, r30 = L->r30;
    const uint8_t *vp = g_mem + v;
    for (uint32_t i = 0; i < nv; i++, vp += 0x24) {
        const float dx = lf(vp) - px, dy = lf(vp + 4) - py, dz = lf(vp + 8) - pz;
        const float nd = dz * lf(vp + 0x14) + dx * lf(vp + 0x0C) + dy * lf(vp + 0x10);
        if (nd >= 0.0f)
            continue;
        const float d2 = dz * dz + dy * dy + dx * dx;
        if (d2 > r2c)
            continue;
        const float dist = sqrtf(d2);
        const float att = d2 < r24 ? 1.0f : (r28 - dist) * r30;
        accumulate(acc_of(i), factor_of(i), L->col, -(nd / dist * att));
    }
}

/* Type 0: spot light (point falloff, then a smooth cone). */
static void light_spot(uint32_t v, uint32_t nv, const LightK *L)
{
    const float px = L->px, py = L->py, pz = L->pz, r20 = L->r20, r28 = L->r28, r2c = L->r2c, r30 = L->r30;
    const float ax = L->m[0], ay = L->m[1], az = L->m[2], c34 = L->c34, c38 = L->c38, c40 = L->c40;
    const uint8_t *vp = g_mem + v;
    for (uint32_t i = 0; i < nv; i++, vp += 0x24) {
        float dx = lf(vp) - px, dy = lf(vp + 4) - py, dz = lf(vp + 8) - pz;
        const float nd = dz * lf(vp + 0x14) + dx * lf(vp + 0x0C) + dy * lf(vp + 0x10);
        if (nd >= 0.0f)
            continue;
        const float d2 = dz * dz + dy * dy + dx * dx;
        if (d2 >= r2c)   /* (unordered goes on, as in the guest) */
            continue;
        const float dist = sqrtf(d2);
        const float att = !(dist > r20) ? 1.0f : (r28 - dist) * r30;
        const float inv = 1.0f / dist;
        dx *= inv;
        dy *= inv;
        dz *= inv;
        const float sd = dz * az + dy * ay + dx * ax;
        if (sd < c34)
            continue;
        float k = -(att * inv * nd);
        if (sd < c38) {
            const float t = (sd - c34) * c40;
            k *= (3.0f - (t + t)) * t * t;
        }
        accumulate(acc_of(i), factor_of(i), L->col, k);
    }
}

/* Type 2: projected light (a rectangular window with soft edges). */
static void light_projected(uint32_t v, uint32_t nv, const LightK *L, uint32_t esp)
{
    const float px = L->px, py = L->py, pz = L->pz, r20 = L->r20, r28 = L->r28, r30 = L->r30;
    const float s44 = L->s44, s48 = L->s48, e4c = L->e4c, e50 = L->e50;
    const float *m = L->m;
    const uint8_t *vp = g_mem + v;
    /* The edge factor lives in the guest frame ([esp+0x10]): an iteration that
     * leaves it unset uses the previous one's. */
    float edge = ldf(esp - 0x5Cu + 0x10u);
    for (uint32_t i = 0; i < nv; i++, vp += 0x24) {
        const float dx = lf(vp) - px, dy = lf(vp + 4) - py, dz = lf(vp + 8) - pz;
        const float nd = -(dz * lf(vp + 0x14) + dx * lf(vp + 0x0C) + dy * lf(vp + 0x10));
        if (nd <= 0.0f)
            continue;
        const float z = dx * m[0] + dy * m[1] + dz * m[2];
        if (z <= 0.0f)
            continue;
        const float x = dx * m[6] + dy * m[7] + dz * m[8];
        const float y = dx * m[3] + dy * m[4] + dz * m[5];
        const float dist = sqrtf(z * z + y * y + x * x);
        if (dist > r28)
            continue;
        const float att = !(dist > r20) ? 1.0f : (r28 - dist) * r30;
        const float a = nd / dist * att;
        const float iz = 1.0f / z;
        float sx = fabsf(x * s44 * iz), sy = fabsf(y * s48 * iz);
        if (sx > 1.0f || sy > 1.0f)
            continue;
        unsigned w = 0;
        if (sx > e4c) {
            sx = (sx - e4c) * e50;
            w = 1;
        }
        if (sy > e4c) {
            sy = (sy - e4c) * e50;
            w |= 2;
        }
        switch (w) {
        case 0: edge = 1.0f; break;
        case 1: edge = 1.0f - sx; break;
        case 2: edge = 1.0f - sy; break;
        default: edge = (1.0f - sy) * (1.0f - sx); break;
        }
        accumulate(acc_of(i), factor_of(i), L->col, (3.0f - (edge + edge)) * edge * edge * a);
    }
    stf(esp - 0x5Cu + 0x10u, edge);
}

/* Type 3: ambient (flags 0x2000: color only; 0x1000000: times the factors) or
 * hemispheric: color + second color * (1 - n.dir) / 2, times the factors. */
static void light_ambient(uint32_t v, uint32_t nv, const LightK *L, uint32_t flags)
{
    const float c0 = L->col[0], c1 = L->col[1], c2 = L->col[2];
    if (flags & 0x2000u) {
        for (uint32_t i = 0; i < nv; i++) {
            float *acc = acc_of(i);
            acc[0] = acc[0] + c0;
            acc[1] = acc[1] + c1;
            acc[2] = acc[2] + c2;
        }
        return;
    }
    if (flags & 0x1000000u) {
        for (uint32_t i = 0; i < nv; i++)
            accumulate(acc_of(i), factor_of(i), L->col, 1.0f);
        return;
    }
    const float ax = L->m[0], ay = L->m[1], az = L->m[2];
    const float h0 = L->col2[0], h1 = L->col2[1], h2 = L->col2[2];
    const uint8_t *vp = g_mem + v;
    for (uint32_t i = 0; i < nv; i++, vp += 0x24) {
        const float s = ((1.0f - lf(vp + 0x14) * az) - ay * lf(vp + 0x10) - lf(vp + 0x0C) * ax) * 0.5f;
        float *acc = acc_of(i);
        const float *f = factor_of(i);
        acc[0] = (h0 * s + c0) * f[0] + acc[0];
        acc[1] = (h1 * s + c1) * f[1] + acc[1];
        acc[2] = (h2 * s + c2) * f[2] + acc[2];
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
        LightK L;
        if (type > 3u)
            continue;
#if !defined(__vita__)
        type_calls[type]++;
#endif
        if (type == 3u) {
            light_load(&L, l);
            light_ambient(verts, nv, &L, flags);
            continue;
        }
        if ((flags & 0x2000u) && !(RD8(l - 0x3Cu) & 1u))
            continue;
        if (!nv)
            continue;
        light_load(&L, l);
        if (type == 0u)
            light_spot(verts, nv, &L);
        else if (type == 1u)
            light_point(verts, nv, &L);
        else
            light_projected(verts, nv, &L, esp);
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
    NSTAT_BEGIN();
    light_vertices(c, esp);
    if (g_nstat) {
        const uint32_t nv = RD32(esp + 8u), nl = RD32(esp + 16u);
        NSTAT_END(NS_LIGHT, (uint64_t)nv * nl);
        nstat_seen(NS_LIGHT, RD32(esp + 4u), nv, RD32(esp + 12u) ^ (nl << 24) ^ RD32(esp + 20u));
    }
    *ret = ((uint64_t)(esp + 4u + 0x14u) << 32) | 0u;
    return 1;
}
