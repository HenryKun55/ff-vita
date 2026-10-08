/* Native versions of the two ends of the engine's vertex lighting (around
 * 0x006A5600, runtime/src/native_light.c), both per vertex of a mesh:
 *
 * 0x00691410  void __stdcall colors_in(Vertex *v, uint32_t n, uint32_t flags)
 *   The vertex colour (ARGB at +0x18, vertex stride 0x24) into the lighting
 *   factors (0x8911E0 + 16 i, RGB) and accumulators (0x890FE0 + 16 i):
 *   flags 0x2000: factors = RGB / 127.5, acc alpha = A / 127.5, acc RGB = 2 - that;
 *   otherwise:    factors = RGB / 255,   acc alpha = A / 255,   acc RGB = 0.
 *
 * 0x006915B0  void __stdcall colors_out(Vertex *dst, const Vertex *src, uint32_t n)
 *   The lit colour back: acc * 127.5 truncated (_ftol), RGB clamped to 0..255,
 *   alpha not (it wraps), packed ARGB at +0x18; the other words copied from src.
 *
 * Single precision as the game's x87; the products are exact or round the same
 * before the truncation for these 8-bit inputs. */
#include "recomp.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACC 0x00890FE0u
#define FACTOR 0x008911E0u

static inline float ldf(uint32_t a)
{
    float v;
    memcpy(&v, g_mem + a, 4);
    return v;
}

static inline void stf(uint32_t a, float v) { memcpy(g_mem + a, &v, 4); }

static inline float bits(uint32_t u)
{
    float v;
    memcpy(&v, &u, 4);
    return v;
}

static int on = -1;
#if !defined(__vita__)
static int check = -1;
static uint32_t declining;

/* FF_NATIVE_CHECK=1: the native result of [a, a + len) and [a2, a2 + len2) is kept,
 * the memory restored and the recompiled code run; then the two are compared. */
typedef int (*NativeFn)(Ctx *, uint32_t, uint32_t, uint32_t, uint64_t *);
static void check_run(const char *name, NativeFn fn, uint32_t entry, Ctx *c, uint32_t esp, uint32_t ecx,
                      uint32_t eax, uint64_t *ret, uint32_t a, uint32_t len, uint32_t a2, uint32_t len2)
{
    uint8_t *b1 = malloc(len + 1u), *b2 = malloc(len2 + 1u), *n1 = malloc(len + 1u), *n2 = malloc(len2 + 1u);
    memcpy(b1, g_mem + a, len);
    memcpy(b2, g_mem + a2, len2);
    uint64_t r;
    declining = 1;   /* the native body itself (not the check again) */
    fn(c, esp, ecx, eax, &r);
    memcpy(n1, g_mem + a, len);
    memcpy(n2, g_mem + a2, len2);
    memcpy(g_mem + a, b1, len);
    memcpy(g_mem + a2, b2, len2);
    declining = entry;
    *ret = rt_call_indirect(c, entry, esp, ecx, eax);
    declining = 0;
    static unsigned calls, bad;
    calls++;
    if (memcmp(n1, g_mem + a, len) || memcmp(n2, g_mem + a2, len2) || (uint32_t)(r >> 32) != (uint32_t)(*ret >> 32)) {
        if (bad < 20) {
            if ((uint32_t)(r >> 32) != (uint32_t)(*ret >> 32))
                rt_log("native %s: call %u esp %08X/%08X", name, calls, (uint32_t)(r >> 32),
                       (uint32_t)(*ret >> 32));
            for (uint32_t i = 0; i < len2 / 4u; i++)
                if (memcmp(n2 + 4u * i, g_mem + a2 + 4u * i, 4)) {
                    uint32_t x, y;
                    memcpy(&x, n2 + 4u * i, 4);
                    memcpy(&y, g_mem + a2 + 4u * i, 4);
                    rt_log("native %s: call %u acc word %u native %08X recompiled %08X", name, calls, i, x, y);
                    break;
                }
        }
        if (bad++ < 20)
            for (uint32_t i = 0; i < len / 4u; i++)
                if (memcmp(n1 + 4u * i, g_mem + a + 4u * i, 4)) {
                    uint32_t x, y;
                    memcpy(&x, n1 + 4u * i, 4);
                    memcpy(&y, g_mem + a + 4u * i, 4);
                    rt_log("native %s: call %u word %u native %08X recompiled %08X", name, calls, i, x, y);
                    break;
                }
    }
    if (calls % 1000u == 0u)
        rt_log("native colours: %u calls checked, %u differ", calls, bad);
    free(b1);
    free(b2);
    free(n1);
    free(n2);
}
#endif

static int enabled(void)
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
#if !defined(__vita__)
    if (check < 0) {
        const char *k = getenv("FF_NATIVE_CHECK");
        check = k && k[0] == '1';
    }
#endif
    return on;
}

int rt_native_colors_in(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)c;
    (void)ecx;
    (void)eax;
    if (!enabled())
        return 0;
#if !defined(__vita__)
    if (declining == 0x00691410u)
        return 0;
    if (check && !declining) {
        const uint32_t nn = RD32(esp + 8u);
        declining = 1;
        check_run("colors_in", rt_native_colors_in, 0x00691410u, c, esp, ecx, eax, ret, FACTOR, 16u * nn, ACC,
                  16u * nn);
        return 1;
    }
#endif
    const uint32_t v = RD32(esp + 4u), n = RD32(esp + 8u), flags = RD32(esp + 12u);
    const int lit = (flags & 0x2000u) != 0;
    const float k = bits(lit ? 0x3C008081u : 0x3B808081u);   /* [0x727BE0] 1/127.5, [0x701B6C] 1/255 */
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t col = RD32(v + 0x24u * i + 0x18u);
        stf(FACTOR + 16u * i, (float)((col >> 16) & 0xFFu) * k);
        stf(FACTOR + 16u * i + 4u, (float)((col >> 8) & 0xFFu) * k);
        stf(FACTOR + 16u * i + 8u, (float)(col & 0xFFu) * k);
        const float a = (float)(col >> 24) * k;
        stf(ACC + 16u * i + 12u, a);
        const float base = lit ? 2.0f - a : 0.0f;
        stf(ACC + 16u * i, base);
        stf(ACC + 16u * i + 4u, base);
        stf(ACC + 16u * i + 8u, base);
    }
    *ret = ((uint64_t)(esp + 4u + 12u) << 32) | (n ? 16u * n : flags);
    return 1;
}

static inline int32_t clamp255(int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

int rt_native_colors_out(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)c;
    (void)ecx;
    (void)eax;
    if (!enabled())
        return 0;
#if !defined(__vita__)
    if (declining == 0x006915B0u)
        return 0;
    if (check && !declining) {
        const uint32_t nn = RD32(esp + 12u);
        declining = 1;
        check_run("colors_out", rt_native_colors_out, 0x006915B0u, c, esp, ecx, eax, ret, RD32(esp + 4u),
                  0x24u * nn, ACC, 0);
        return 1;
    }
#endif
    const uint32_t dst = RD32(esp + 4u), src = RD32(esp + 8u), n = RD32(esp + 12u);
    const float k = 127.5f;   /* [0x713488] */
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t acc = ACC + 16u * i, d = dst + 0x24u * i, s = src + 0x24u * i;
        const int32_t r = clamp255((int32_t)(uint32_t)rt_ftol64(ldf(acc) * k));
        const int32_t g = clamp255((int32_t)(uint32_t)rt_ftol64(ldf(acc + 4u) * k));
        const int32_t b = clamp255((int32_t)(uint32_t)rt_ftol64(ldf(acc + 8u) * k));
        const uint32_t a = (uint32_t)rt_ftol64(ldf(acc + 12u) * k);
        for (uint32_t w = 0; w < 0x18u; w += 4u)   /* word by word, as the guest (src may be dst) */
            WR32(d + w, RD32(s + w));
        WR32(d + 0x18u, (((((a << 8) + (uint32_t)r) << 8) + (uint32_t)g) << 8) + (uint32_t)b);
        WR32(d + 0x1Cu, RD32(s + 0x1Cu));
        WR32(d + 0x20u, RD32(s + 0x20u));
    }
    *ret = ((uint64_t)(esp + 4u + 12u) << 32) | 0u;
    return 1;
}
