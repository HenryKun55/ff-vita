/* Native version of the engine's 3x3 matrix product (0x005F8A90, 29 call sites).
 *
 * void mat3_mul(Mat3 *a (ecx), const Mat3 *b (edx), const Mat3 *m)   (1 stack argument, ret 4)
 *   Each row v of b (3 floats) through m (9 floats, the same convention as the
 *   scene graph's rotations in native_xform.c) into the same row of a:
 *     a.x = (v0 m6 + v1 m3) + v2 m0
 *     a.y = (v0 m7 + v1 m4) + v2 m1
 *     a.z = (v0 m8 + v1 m5) + v2 m2
 *   rows in the guest's order: row 2 (+0x18), row 1 (+0x0C), row 0 (+0). Each row
 *   reads all of its inputs before its three stores, so a = b or a = m gives what
 *   the guest gives (a row written is seen by the next rows' reads).
 *   Returns with eax = b + 0x0C, ecx = a, edx = b (as the x86 leaves them).
 *
 * Decompiled with Ghidra (ReVa); the operation order is taken from the x87 code
 * (Ghidra's pseudo-code reorders the sums). Single precision as the game's 24-bit
 * x87 (a product or sum outside float's exponent range, or a float denormal, can
 * differ: the x87 keeps its 15-bit exponent until the store). ff.cfg
 * native_check=1 (FF_NATIVE_CHECK=1 on the desktop) runs the recompiled code too
 * and compares the 36 bytes written. */
#include "recomp.h"
#include "runtime.h"

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

static inline void sf(uint32_t a, float v)
{
    memcpy(g_mem + a, &v, 4);
}

/* One row: v = b's row at src, into a's row at dst. */
static inline void row(uint32_t dst, uint32_t src, uint32_t m)
{
    const float v0 = lf(src), v1 = lf(src + 4u), v2 = lf(src + 8u);
    const float x = (v0 * lf(m + 0x18u) + v1 * lf(m + 0x0Cu)) + v2 * lf(m);
    const float y = (v0 * lf(m + 0x1Cu) + v1 * lf(m + 0x10u)) + v2 * lf(m + 4u);
    const float z = (v0 * lf(m + 0x20u) + v1 * lf(m + 0x14u)) + v2 * lf(m + 8u);
    sf(dst, x);
    sf(dst + 4u, y);
    sf(dst + 8u, z);
}

static void mat3_mul(uint32_t a, uint32_t b, uint32_t m)
{
    row(a + 0x18u, b + 0x18u, m);
    row(a + 0x0Cu, b + 0x0Cu, m);
    row(a, b, m);
}

static int on = -1, check, declining;

int rt_native_mat3_mul(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
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
    const uint32_t a = ecx, b = c->r[2], m = RD32(esp + 4u);
    if (check) {
        /* Only a's 36 bytes are written: the recompiled code from a copy of them,
         * then the native code from the same state, then the recompiled result kept. */
        uint8_t before[36], guest[36];
        memcpy(before, g_mem + a, 36);
        declining = 1;
        *ret = rt_call_indirect(c, 0x005F8A90u, esp, ecx, eax);
        declining = 0;
        memcpy(guest, g_mem + a, 36);
        memcpy(g_mem + a, before, 36);
        mat3_mul(a, b, m);
        static unsigned calls, bad;
        calls++;
        if (memcmp(guest, g_mem + a, 36) && bad++ < 20)
            rt_log("native mat3: call %u differs (a %08X b %08X m %08X)", calls, a, b, m);
        if (calls % 20000u == 0u)
            rt_log("native mat3: %u calls checked, %u differ", calls, bad);
        memcpy(g_mem + a, guest, 36);
        return 1;
    }
    mat3_mul(a, b, m);
    c->r[1] = a;
    c->r[2] = b;
    *ret = ((uint64_t)(esp + 4u + 4u) << 32) | (b + 0x0Cu);   /* ret 4 */
    return 1;
}
