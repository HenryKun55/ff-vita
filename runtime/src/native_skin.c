/* Native version of the character skinning loop of 0x00694BF0 (its x87 path,
 * 0x006953F3..0x00695586), about 10% of the game thread with characters on screen.
 *
 * Source vertex (52 bytes): position x y z, weights w0 w1 w2 (w3 = 1 - (w0 + w2 + w1)),
 * bone row indices (4 bytes, multiples of 3), normal x y z, three words copied as is.
 * Palette: 3x4 matrices as rows of 4 floats (16 bytes); a bone index addresses its first row.
 * Output vertex (36 bytes): position, normal, the three copied words.
 * Per vertex the bones are summed in order until a weight is zero (that one and
 * the rest are left out): position += w * (M * p + t), normal += w * (M * n).
 * Single precision in the guest's order (the game runs the x87 at 24 bits). The
 * loop's frame locals are dead after it; the registers the code after it reads
 * are reloaded there, so only the output vertices are written. */
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

static inline float ldf(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

static inline void stf(uint8_t *p, float v) { memcpy(p, &v, 4); }

static void skin(uint32_t src, uint32_t dst, uint32_t palette, uint32_t n)
{
    for (uint32_t v = 0; v < n; v++, src += 52u, dst += 36u) {
        const uint8_t *s = g_mem + src;
        const float x = ldf(s), y = ldf(s + 4), z = ldf(s + 8);
        const float nx = ldf(s + 0x1C), ny = ldf(s + 0x20), nz = ldf(s + 0x24);
        float px = 0.0f, py = 0.0f, pz = 0.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;
        for (int b = 0; b < 4; b++) {
            float w = b < 3 ? ldf(s + 0x0C + 4 * b) : 1.0f - ((ldf(s + 0x0C) + ldf(s + 0x14)) + ldf(s + 0x10));
            if (w == 0.0f)
                break;
            const uint8_t *m = g_mem + palette + 16u * s[0x18 + b];
            px += ((x * ldf(m) + z * ldf(m + 8)) + y * ldf(m + 4) + ldf(m + 12)) * w;
            py += ((x * ldf(m + 16) + z * ldf(m + 24)) + y * ldf(m + 20) + ldf(m + 28)) * w;
            pz = ((x * ldf(m + 32) + z * ldf(m + 40)) + y * ldf(m + 36) + ldf(m + 44)) * w + pz;
            qx = ((nx * ldf(m) + nz * ldf(m + 8)) + ny * ldf(m + 4)) * w + qx;
            qy = ((nx * ldf(m + 16) + nz * ldf(m + 24)) + ny * ldf(m + 20)) * w + qy;
            qz = ((nx * ldf(m + 32) + nz * ldf(m + 40)) + ny * ldf(m + 36)) * w + qz;
        }
        uint8_t *d = g_mem + dst;
        stf(d, px);
        stf(d + 4, py);
        stf(d + 8, pz);
        stf(d + 12, qx);
        stf(d + 16, qy);
        stf(d + 20, qz);
        memcpy(d + 24, s + 0x28, 12);
    }
}

static int on = -1;

/* Block hook at 0x006953F3 (nverts > 0, ebp the function's frame). */
int rt_block_skin(Ctx *c)
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
#endif
    }
    if (!on)
        return 0;
    const uint32_t ebp = c->r[5];
    const uint32_t dst = RD32(ebp - 4u), n = RD32(ebp - 0x34u);
    const uint32_t src = RD32(ebp - 0x18u), palette = RD32(ebp - 0x2Cu);
#if !defined(__vita__)
    /* FF_NATIVE_CHECK=1: the native result goes to a buffer and the recompiled loop
     * runs; the next call compares what that loop wrote with the buffer. */
    static int check = -1;
    if (check < 0) {
        const char *k = getenv("FF_NATIVE_CHECK");
        check = k && k[0] == '1';
    }
    if (check) {
        static uint8_t *ref;
        static uint32_t ref_dst, ref_n;
        static unsigned calls, bad, rounding;
        if (ref) {
            for (uint32_t i = 0; i < ref_n * 9u; i++) {
                float a, b;
                memcpy(&a, ref + 4u * i, 4);
                memcpy(&b, g_mem + ref_dst + 4u * i, 4);
                if (memcmp(&a, &b, 4) == 0)
                    continue;
                if (fabsf(a - b) <= 1e-4f * (fabsf(b) > 1.0f ? fabsf(b) : 1.0f)) {
                    rounding++;
                } else if (bad++ < 20) {
                    rt_log("native skin: call %u vertex %u word %u native %g recompiled %g", calls, i / 9u,
                           i % 9u, (double)a, (double)b);
                }
            }
            free(ref);
        }
        uint8_t *save = malloc(36u * n);
        memcpy(save, g_mem + dst, 36u * n);
        skin(src, dst, palette, n);
        ref = malloc(36u * n);
        memcpy(ref, g_mem + dst, 36u * n);
        memcpy(g_mem + dst, save, 36u * n);
        free(save);
        ref_dst = dst;
        ref_n = n;
        if (++calls % 2000u == 0u)
            rt_log("native skin: %u calls checked, %u differences, %u rounding", calls, bad, rounding);
        return 0;
    }
#endif
    NSTAT_BEGIN();
    skin(src, dst, palette, n);
    d3d_mem_written(dst, 36u * n);
    if (g_nstat) {
        NSTAT_END(NS_SKIN, n);
        nstat_written(dst, 36u * n);
        /* The same mesh with the same pose (first bone's matrix) again this frame. */
        const uint8_t *m = g_mem + palette + 16u * g_mem[src + 0x18u];
        uint32_t h = 2166136261u;
        for (int i = 0; i < 48; i++)
            h = (h ^ m[i]) * 16777619u;
        nstat_seen(NS_SKIN, src, n, h);
    }
    return 1;
}
