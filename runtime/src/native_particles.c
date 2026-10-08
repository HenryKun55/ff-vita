/* Native version of the particle quad loop of 0x006959D0 (0x00695E50..0x006960BB):
 * per particle, its position (start + velocity * exp(-age * drag) + gravity * age,
 * then the emitter's matrix and offset), size and angle (polynomials of the age),
 * colour (a table by the fraction of its life) and the four corners of its
 * billboard, rotated. The guest computes exp with f2xm1/fscale and the rotation
 * with fsin/fcos, slow to emulate; here expf, sinf and cosf in single precision
 * (the game's x87 setting).
 *
 * Particle (36 bytes): position +0, velocity +0x0C, birth time +0x18, death time
 * +0x1C (whose lowest bit also picks the spin direction), start angle +0x20.
 * Output vertex (36 bytes, the guest's edi points at +8): x y z +0, colour +0x18,
 * u v +0x1C. Inputs are the function's locals (offsets from esp, which the loop
 * does not move); afterwards the code reloads every register it reads. */
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

static inline float ldf(uint32_t a)
{
    float v;
    memcpy(&v, g_mem + a, 4);
    return v;
}

static inline void stf(uint32_t a, float v) { memcpy(g_mem + a, &v, 4); }

static void particles(Ctx *c)
{
    const uint32_t esp = c->r[4];
    const uint32_t em = RD32(esp + 0x54u);
    const uint32_t base = RD32(em + 4u), colors = RD32(em + 0x38u);
    uint32_t off = RD32(esp + 0x34u), n = RD32(esp + 0x10u), out = c->r[7];
    const float now = ldf(esp + 0xD4u), drag = ldf(esp + 0xE8u);
    const uint32_t grav = RD32(esp + 0xE4u), mat = RD32(esp + 0x6Cu), trans = RD32(esp + 0x38u);
    const float ra = ldf(esp + 0xCCu), rb = ldf(esp + 0xD0u), rc = ldf(esp + 0xD8u);
    const float sa = ldf(esp + 0xE0u), sb = ldf(esp + 0x64u), sc = ldf(esp + 0x68u);
    float m[9];
    for (int i = 0; i < 9; i++)
        m[i] = ldf(mat + 4u * (uint32_t)i);
    const float tx = ldf(trans), ty = ldf(trans + 4u), tz = ldf(trans + 8u);
    const float gx = ldf(grav), gy = ldf(grav + 4u), gz = ldf(grav + 8u);
    for (; n; n--, off += 0x24u) {
        const uint32_t p = base + off;
        const float age = now - ldf(p + 0x18u);
        const float decay = expf(-(age * drag));
        float lx = ldf(p + 0x0Cu) * decay + ldf(p), ly = ldf(p + 0x10u) * decay + ldf(p + 4u);
        float lz = ldf(p + 0x14u) * decay + ldf(p + 8u);
        lx = gx * age + lx;
        ly = gy * age + ly;
        lz = gz * age + lz;
        const float spin = (RD8(p + 0x1Cu) & 1u) ? -1.0f : 1.0f;
        const float angle = ((((age * ra) * 0.5f + rb) * age + rc) * spin) * age + ldf(p + 0x20u);
        const float size = ((age * sa) * 0.5f + sb) * age + sc;
        const float frac = age / (ldf(p + 0x1Cu) - ldf(p + 0x18u)) * -127.0f;
        const uint32_t idx = (uint32_t)rt_ftol64(frac);
        const uint32_t color = (RD32(colors - idx * 4u) >> 1) & 0x7F7F7F7Fu;
        const float ox = (lx * m[6] + ly * m[3]) + lz * m[0] + tx;
        const float oy = (lx * m[7] + ly * m[4]) + lz * m[1] + ty;
        const float oz_f = (lx * m[8] + ly * m[5]) + lz * m[2];
        const float oz = oz_f + tz;
        const float ca = cosf(angle), sn = sinf(angle);
        for (uint32_t k = 0; k < 4u; k++, out += 0x24u) {
            const float cx = size * ldf(esp + 0xF0u + 12u * k), cy = size * ldf(esp + 0xF4u + 12u * k);
            stf(out, oz);
            WR32(out + 0x14u, RD32(esp + 0xA4u + 8u * k));
            WR32(out + 0x10u, color);
            WR32(out + 0x18u, RD32(esp + 0xA8u + 8u * k));
            stf(out - 8u, (cy * sn + cx * ca) + ox);
            stf(out - 4u, (cy * ca - cx * sn) + oy);
        }
        /* The loop's locals as the guest leaves them (position and age). */
        stf(esp + 0x74u, ox);
        stf(esp + 0x78u, oy);
        stf(esp + 0x7Cu, oz);
    }
    WR32(esp + 0x34u, off);
    WR32(esp + 0x10u, 0);
    c->r[7] = out;
}

static int on = -1;
#if !defined(__vita__)
static int check = -1;
static uint8_t *ref;
static uint32_t ref_at, ref_len;
static unsigned calls, bad, rounding;
#endif

/* Block hook at 0x00695E50, continuing at 0x006960BB. */
int rt_block_particles(Ctx *c)
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
    if (!on || !RD32(c->r[4] + 0x10u))
        return 0;
#if !defined(__vita__)
    /* FF_NATIVE_CHECK=1: the native vertices go to a buffer, the recompiled loop
     * runs, and the next call compares what it wrote (positions within rounding,
     * the rest exactly). */
    if (check < 0) {
        const char *k = getenv("FF_NATIVE_CHECK");
        check = k && k[0] == '1';
    }
    if (check) {
        free(ref);
        const uint32_t n = RD32(c->r[4] + 0x10u), at = c->r[7] - 8u, len = n * 4u * 36u;
        uint8_t *save = malloc(len);
        memcpy(save, g_mem + at, len);
        uint8_t locals[0x100];
        memcpy(locals, g_mem + c->r[4], sizeof(locals));
        const uint32_t edi = c->r[7];
        particles(c);
        ref = malloc(len);
        memcpy(ref, g_mem + at, len);
        memcpy(g_mem + at, save, len);
        memcpy(g_mem + c->r[4], locals, sizeof(locals));
        c->r[7] = edi;
        free(save);
        ref_at = at;
        ref_len = len;
        return 0;
    }
#endif
    NSTAT_BEGIN();
    const uint32_t np = RD32(c->r[4] + 0x10u), pout = c->r[7] - 8u;
    particles(c);
    d3d_mem_written(pout, np * 4u * 36u);
    if (g_nstat)
        nstat_written(pout, np * 4u * 36u);
    NSTAT_END(NS_PARTICLES, np);
    return 1;
}

/* Block hook at the loop's end (0x006960BB), FF_NATIVE_CHECK only: compares what
 * the recompiled loop just wrote with the native result. Never takes over. */
int rt_block_particles_end(Ctx *c)
{
    (void)c;
#if !defined(__vita__)
    if (ref) {
        for (uint32_t i = 0; i < ref_len / 4u; i++) {
            uint32_t a, b;
            memcpy(&a, ref + 4u * i, 4);
            memcpy(&b, g_mem + ref_at + 4u * i, 4);
            if (a == b)
                continue;
            uint32_t w = i % 9u;   /* words 0-2 x y z, 6 colour, 7-8 u v */
            float fa, fb;
            memcpy(&fa, &a, 4);
            memcpy(&fb, &b, 4);
            if (w <= 2u && fabsf(fa - fb) <= 2e-3f * (fabsf(fb) > 1.0f ? fabsf(fb) : 1.0f))
                rounding++;
            else if (bad++ < 20)
                rt_log("native particles: call %u vertex %u word %u native %08X recompiled %08X", calls,
                       i / 9u, w, a, b);
        }
        free(ref);
        ref = NULL;
    }
    if (check > 0 && ++calls % 500u == 0u)
        rt_log("native particles: %u calls checked, %u differences, %u rounding", calls, bad, rounding);
#endif
    return 0;
}
