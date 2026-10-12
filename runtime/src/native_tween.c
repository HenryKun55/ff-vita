/* Native version of the vertex tween loop of 0x00691CF0 ("UpdateStrip", a mesh's
 * per-frame update; the loop is its "Tween" section, 0x00692082..0x006922BB):
 * a mesh animated by keyframes gets each vertex interpolated between two frames
 * into the scratch buffer 0x7A3590 (up to 32 at a time), which is then lit.
 *
 *   Vertex (36 bytes, 9 words): every float word w = (b - a) * t + a, except the
 *   colour (+0x18): kept when both frames have the same; otherwise each ARGB
 *   channel is converted to float, interpolated the same way, truncated (_ftol)
 *   and packed as ((A * 256 + R) * 256 + G) * 256 + B (the guest's order: A, R,
 *   G, B; plain 32-bit arithmetic).
 *   At the hook: ecx the count (> 0), ebp frame a, [esp+0x28] frame b,
 *   [esp+0x10] t, ebx 0. The code after the loop reloads every register it reads
 *   but ebx (0, untouched here).
 *
 * Decompiled with Ghidra (ReVa); single precision as the game's 24-bit x87.
 * ff.cfg native_check=1 (FF_NATIVE_CHECK=1 on the desktop): the native result is
 * kept, the recompiled loop runs, and the hook at the loop's end compares. */
#include "recomp.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#define SCRATCH 0x007A3590u

static inline float lf(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

static inline void sf(uint8_t *p, float v) { memcpy(p, &v, 4); }

static inline uint32_t chan(uint32_t ca, uint32_t cb, int shift, float t)
{
    const float a = (float)((ca >> shift) & 0xFFu), b = (float)((cb >> shift) & 0xFFu);
    return (uint32_t)rt_ftol64((b - a) * t + a);
}

static void tween(uint32_t fa, uint32_t fb, uint32_t n, float t, uint8_t *out)
{
    for (uint32_t i = 0; i < n; i++, out += 36) {
        const uint8_t *a = g_mem + fa + 36u * i, *b = g_mem + fb + 36u * i;
        for (int w = 0; w < 9; w++) {
            if (w == 6)
                continue;
            const float x = lf(a + 4 * w);
            sf(out + 4 * w, (lf(b + 4 * w) - x) * t + x);
        }
        uint32_t ca, cb;
        memcpy(&ca, a + 0x18, 4);
        memcpy(&cb, b + 0x18, 4);
        uint32_t col = ca;
        if (ca != cb) {
            col = chan(ca, cb, 24, t);
            col = (col << 8) + chan(ca, cb, 16, t);
            col = (col << 8) + chan(ca, cb, 8, t);
            col = (col << 8) + chan(ca, cb, 0, t);
        }
        memcpy(out + 0x18, &col, 4);
    }
}

static int on = -1, check;
static uint8_t *ref;
static uint32_t ref_n;

int rt_block_tween(Ctx *c)
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
    const uint32_t esp = c->r[4], n = c->r[1], fa = c->r[5], fb = RD32(esp + 0x28u);
    float t;
    memcpy(&t, g_mem + esp + 0x10u, 4);
    if (check) {
        free(ref);
        ref = malloc(36u * n);
        ref_n = n;
        tween(fa, fb, n, t, ref);
        return 0;   /* the recompiled loop runs; rt_block_tween_end compares */
    }
    tween(fa, fb, n, t, g_mem + SCRATCH);
    return 1;
}

/* Hook at the loop's end (0x006922BB), native_check only; never takes over. */
int rt_block_tween_end(Ctx *c)
{
    (void)c;
    if (!ref)
        return 0;
    static unsigned calls, bad, rounding;
    calls++;
    for (uint32_t i = 0; i < ref_n * 9u; i++)
        if (memcmp(ref + 4u * i, g_mem + SCRATCH + 4u * i, 4)) {
            uint32_t x, y;
            memcpy(&x, ref + 4u * i, 4);
            memcpy(&y, g_mem + SCRATCH + 4u * i, 4);
            /* (the recompiled x87 code rounds in double: the last bit of a float word may differ) */
            if (i % 9u != 6u && (x - y + 1u) <= 2u) {
                rounding++;
                continue;
            }
            if (bad++ < 20)
                rt_log("native tween: call %u vertex %u word %u native %08X recompiled %08X", calls, i / 9u, i % 9u,
                       x, y);
            break;
        }
    if (calls % 2000u == 0u)
        rt_log("native tween: %u calls checked, %u differ, %u last-bit roundings", calls, bad, rounding);
    free(ref);
    ref = NULL;
    return 0;
}
