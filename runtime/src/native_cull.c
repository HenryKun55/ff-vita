/* Native version of the engine's visibility test of a list of objects (0x0058DE80,
 * about 5% of the game thread looking over the city):
 *
 * uint64_t *__cdecl cull(uint64_t *bits, const Object *obj, int count)
 *
 *   Object (64 bytes): bounding sphere centre +0 and radius +0x0C; a box with axes
 *   +0x10, +0x1C, +0x28 and half sizes +0x3C, +0x38, +0x34.
 *   Two bits per object in the 64-bit words at bits, from the current bit M
 *   (0x78D140, 64 bits, kept between calls; a full word moves on to the next one):
 *   bit M "outside", bit M << 1 "crossing a plane", neither "inside".
 *   Phase 1: the 6 view planes (0x78D150, n.x n.y n.z d): the sphere outside one
 *   plane is outside; then the box against each plane, inside all of them inside.
 *   Phase 2 (when 0x78D14C, a count of groups of 4 planes at 0x78D1B0, is not 0),
 *   from the same first bit: objects not outside get bit M unless their sphere is
 *   inside one of the groups (and the crossing bit unless the box is inside it).
 *   Returns the last word written.
 *
 * ff.cfg native_check=1 (FF_NATIVE_CHECK=1 on the desktop): every call is run
 * natively and recompiled from the same state and the results compared.
 *
 * Single precision in the guest's order (the game runs the x87 at 24 bits); a
 * result is "negative" when its sign bit is set, as the guest tests it. */
#include "recomp.h"
#include "runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#define CUR_BIT 0x0078D140u
#define NGROUPS 0x0078D14Cu
#define PLANES 0x0078D150u
#define GROUPS 0x0078D1B0u

static inline float lf(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

static inline int neg(float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    return (u >> 31) != 0;
}

static inline uint64_t ld64(uint32_t a) { return (uint64_t)RD32(a) | (uint64_t)RD32(a + 4u) << 32; }

static inline void st64(uint32_t a, uint64_t v)
{
    WR32(a, (uint32_t)v);
    WR32(a + 4u, (uint32_t)(v >> 32));
}

static uint32_t cull(uint32_t bits, uint32_t objs, int32_t count)
{
    const uint64_t m0 = ld64(CUR_BIT);
    uint64_t m = m0, acc = ld64(bits);
    uint32_t out = bits;
    float pl[6][4];
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 4; j++)
            pl[i][j] = lf(g_mem + PLANES + 16u * (uint32_t)i + 4u * (uint32_t)j);

    /* Phase 1: the view planes. */
    for (int32_t k = 0; k < count; k++) {
        const uint8_t *o = g_mem + objs + 0x40u * (uint32_t)k;
        const float cx = lf(o), cy = lf(o + 4), cz = lf(o + 8), r = lf(o + 0x0C);
        uint64_t b = (m << 1) | m;
        float s[6];
        int i;
        for (i = 0; i < 6; i++) {
            s[i] = (((r - pl[i][3]) - pl[i][0] * cx) - pl[i][1] * cy) - pl[i][2] * cz;
            if (neg(s[i]))
                break;
        }
        if (i == 6) {
            b = (m << 1) & ~m;
            for (i = 0; i < 6; i++) {
                const float nx = pl[i][0], ny = pl[i][1], nz = pl[i][2];
                const float a = fabsf(((nz * lf(o + 0x24) + ny * lf(o + 0x20)) + nx * lf(o + 0x1C)) * lf(o + 0x38));
                const float bb = fabsf(((nz * lf(o + 0x30) + ny * lf(o + 0x2C)) + nx * lf(o + 0x28)) * lf(o + 0x34));
                const float c = fabsf(((nz * lf(o + 0x18) + ny * lf(o + 0x14)) + nx * lf(o + 0x10)) * lf(o + 0x3C));
                if (neg(((a + bb) + c) + (s[i] - r)))
                    break;
            }
            if (i == 6)
                b = 0;
        }
        acc |= b;
        m <<= 2;
        if (!m) {
            st64(out, acc);
            out += 8u;
            m = 1;
            acc = 0;
        }
    }
    st64(out, acc);
    st64(CUR_BIT, m);
    const uint32_t ngroups = RD32(NGROUPS);
    if (!ngroups)
        return out;

    /* Phase 2: the groups of 4 planes, over the bits of phase 1. */
    m = m0;
    out = bits;
    acc = ld64(out);
    for (int32_t k = 0; k < count; k++) {
        const uint8_t *o = g_mem + objs + 0x40u * (uint32_t)k;
        if (!(m & acc)) {
            const float cx = lf(o), cy = lf(o + 4), cz = lf(o + 8), r = lf(o + 0x0C);
            uint64_t b = ((m << 1) & ~acc) | m;
            for (uint32_t g = 0; g < ngroups; g++) {
                const uint8_t *p = g_mem + GROUPS + 0x40u * g;
                float s[4];
                int j;
                for (j = 0; j < 4; j++) {
                    const uint8_t *q = p + 16 * j;
                    s[j] = (((r - lf(q + 12)) - cx * lf(q)) - cy * lf(q + 4)) - cz * lf(q + 8);
                    if (neg(s[j]))
                        break;
                }
                if (j == 4) {
                    b &= ~m;
                    if (!b)
                        break;
                    for (j = 0; j < 4; j++) {
                        const uint8_t *q = p + 16 * j;
                        const float nx = lf(q), ny = lf(q + 4), nz = lf(q + 8);
                        float sum;
                        if (j == 0) {
                            const float a = fabsf(((lf(o + 0x1C) * nx + lf(o + 0x20) * ny) + lf(o + 0x24) * nz) *
                                                  lf(o + 0x38));
                            const float bb = fabsf(((lf(o + 0x14) * ny + lf(o + 0x10) * nx) + lf(o + 0x18) * nz) *
                                                   lf(o + 0x3C));
                            const float c = fabsf(((ny * lf(o + 0x2C) + nz * lf(o + 0x30)) + nx * lf(o + 0x28)) *
                                                  lf(o + 0x34));
                            sum = (a + bb) + c;
                        } else {
                            const float a = fabsf(((nz * lf(o + 0x30) + ny * lf(o + 0x2C)) + nx * lf(o + 0x28)) *
                                                  lf(o + 0x34));
                            const float bb = fabsf(((lf(o + 0x18) * nz + lf(o + 0x14) * ny) + nx * lf(o + 0x10)) *
                                                   lf(o + 0x3C));
                            const float c = fabsf(((lf(o + 0x1C) * nx + lf(o + 0x24) * nz) + lf(o + 0x20) * ny) *
                                                  lf(o + 0x38));
                            sum = (a + bb) + c;
                        }
                        if (neg(sum + (s[j] - r)))
                            break;
                    }
                    if (j == 4) {
                        b = 0;
                        break;
                    }
                }
                if (!b)
                    break;
            }
            acc |= b;
        }
        m <<= 2;
        st64(CUR_BIT, m);
        if (!m) {
            st64(out, acc);
            out += 8u;
            m = 1;
            st64(CUR_BIT, m);
            acc = ld64(out);
        }
    }
    st64(out, acc);
    return out;
}

static int on = -1, check, declining;

int rt_native_cull(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
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
    if (!on)
        return 0;
    const uint32_t bits = RD32(esp + 4u), objs = RD32(esp + 8u);
    const int32_t count = (int32_t)RD32(esp + 12u);
    if (declining)
        return 0;
    if (check) {
        /* Native first, then the recompiled code from the same state; compare the
         * bit words, the current bit and the result. */
        const uint32_t words = (count > 0 ? (uint32_t)count : 0u) / 32u + 2u;
        uint8_t *before = malloc(8u * words), *native = malloc(8u * words);
        memcpy(before, g_mem + bits, 8u * words);
        const uint64_t m = ld64(CUR_BIT);
        const uint32_t r = cull(bits, objs, count);
        const uint64_t nm = ld64(CUR_BIT);
        memcpy(native, g_mem + bits, 8u * words);
        memcpy(g_mem + bits, before, 8u * words);
        st64(CUR_BIT, m);
        declining = 1;
        *ret = rt_call_indirect(c, 0x0058DE80u, esp, ecx, eax);
        declining = 0;
        static unsigned calls, bad;
        calls++;
        if ((memcmp(native, g_mem + bits, 8u * words) || nm != ld64(CUR_BIT) || r != (uint32_t)*ret) && bad++ < 20)
            rt_log("native cull: call %u (count %d, groups %u) differs: result %08X/%08X bit %llx/%llx", calls,
                   count, RD32(NGROUPS), r, (uint32_t)*ret, (unsigned long long)nm,
                   (unsigned long long)ld64(CUR_BIT));
        if (calls % 2000u == 0u)
            rt_log("native cull: %u calls checked, %u differ", calls, bad);
        free(before);
        free(native);
        return 1;
    }
    (void)ecx;
    (void)eax;
    *ret = ((uint64_t)(esp + 4u) << 32) | cull(bits, objs, count);   /* cdecl: only the return address */
    return 1;
}
