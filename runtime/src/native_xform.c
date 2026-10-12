/* Native versions of the scene graph's vector transforms up the parent chain:
 *
 * 0x006071C0  void __thiscall Node::rotate_up(Vec3 *v)
 * 0x00607120  void __thiscall Node::transform_up(Vec3 *v)
 *   For the node and each parent (+0x50, until none): v = R v with the node's
 *   3x3 matrix R (9 floats at +0, rows by column: x' = (x m6 + y m3) + z m0,
 *   y' = (x m7 + y m4) + z m1, z' = (x m8 + y m5) + z m2: the guest's order);
 *   transform_up adds the node's translation (+0x24) after each rotation.
 *
 * 0x00606ED0  void __thiscall Node::untransform_down(Vec3 *v)
 * 0x00606F80  void __thiscall Node::unrotate_down(Vec3 *v)
 *   The inverse, from the topmost node under the root (the root, without a
 *   parent, is left out) down to this one: v -= translation (untransform only),
 *   then v = R^T v: x' = (x m6 + y m7) + z m8, y' = (x m3 + y m4) + z m5,
 *   z' = (x m0 + y m1) + z m2.
 * 0x006067D0  void __thiscall Node::to_world(Mat3 *axes, Vec3 *p)   (nothing without a parent)
 * 0x006065D0  void __thiscall Node::to_local(Mat3 *axes, Vec3 *p)   (nothing without a parent)
 *   The point and the three axes (+0x18, +0x0C, +0, in this order after p)
 *   through the chain up (to_world) or down (to_local).
 * 0x006069E0  void __thiscall Node::world_frame(Mat3 *axes, Vec3 *p)
 *   The axes set to (1,0,0) at +0x18, (0,1,0) at +0x0C, (0,0,1) at +0 and p to 0,
 *   then through the chain up.
 *
 * Single precision (the game runs the x87 at 24 bits). ff.cfg native_check=1
 * (FF_NATIVE_CHECK=1 on the desktop) runs the recompiled code too and compares. */
#include "recomp.h"
#include "runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

static inline float lf(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

static inline void sf(uint8_t *p, float v) { memcpy(p, &v, 4); }

static void xform(uint32_t node, uint32_t vec, int translate)
{
    uint8_t *v = g_mem + vec;
    float x = lf(v), y = lf(v + 4), z = lf(v + 8);
    for (uint32_t n = node; n; n = RD32(n + 0x50u)) {
        const uint8_t *m = g_mem + n;
        const float nx = (x * lf(m + 0x18) + y * lf(m + 0x0C)) + z * lf(m);
        const float ny = (x * lf(m + 0x1C) + y * lf(m + 0x10)) + z * lf(m + 4);
        const float nz = (x * lf(m + 0x20) + y * lf(m + 0x14)) + z * lf(m + 8);
        x = nx;
        y = ny;
        z = nz;
        if (translate) {
            x = x + lf(m + 0x24);
            y = y + lf(m + 0x28);
            z = z + lf(m + 0x2C);
        }
    }
    sf(v, x);
    sf(v + 4, y);
    sf(v + 8, z);
}

static void unxform(uint32_t node, uint32_t vec, int translate)
{
    uint32_t chain[64];
    int n = 0;
    for (uint32_t k = node; k && RD32(k + 0x50u) && n < 64; k = RD32(k + 0x50u))
        chain[n++] = k;
    uint8_t *v = g_mem + vec;
    float x = lf(v), y = lf(v + 4), z = lf(v + 8);
    while (n--) {
        const uint8_t *m = g_mem + chain[n];
        if (translate) {
            x = x - lf(m + 0x24);
            y = y - lf(m + 0x28);
            z = z - lf(m + 0x2C);
        }
        const float nx = (x * lf(m + 0x18) + y * lf(m + 0x1C)) + z * lf(m + 0x20);
        const float ny = (x * lf(m + 0x0C) + y * lf(m + 0x10)) + z * lf(m + 0x14);
        const float nz = (x * lf(m) + y * lf(m + 4)) + z * lf(m + 8);
        x = nx;
        y = ny;
        z = nz;
    }
    sf(v, x);
    sf(v + 4, y);
    sf(v + 8, z);
}

/* Depth of the chain the inverse can take (deeper: the guest code). */
static int chain_ok(uint32_t node)
{
    int n = 0;
    for (uint32_t k = node; k; k = RD32(k + 0x50u))
        if (++n > 64)
            return 0;
    return 1;
}

static int on = -1, check, declining;

static int enabled(void)
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
    return on && !declining;
}

enum { ROTATE_UP, TRANSFORM_UP, UNROTATE, UNTRANSFORM, TO_WORLD, TO_LOCAL, WORLD_FRAME };

/* Does the work; returns eax as the guest leaves it. */
static uint32_t work(int op, uint32_t node, uint32_t a1, uint32_t a2, uint32_t eax)
{
    const uint32_t parent = RD32(node + 0x50u);
    switch (op) {
    case ROTATE_UP:
        xform(node, a1, 0);
        return 0;
    case TRANSFORM_UP:
        xform(node, a1, 1);
        return 0;
    case UNROTATE:
    case UNTRANSFORM:
        unxform(node, a1, op == UNTRANSFORM);
        return parent ? a1 : eax;
    case TO_WORLD:
        if (!parent)
            return 0;
        xform(node, a2, 1);
        xform(node, a1 + 0x18u, 0);
        xform(node, a1 + 0x0Cu, 0);
        xform(node, a1, 0);
        return 0;
    case TO_LOCAL:
        if (!parent)
            return eax;
        unxform(node, a2, 1);
        unxform(node, a1 + 0x18u, 0);
        unxform(node, a1 + 0x0Cu, 0);
        unxform(node, a1, 0);
        /* (the last unrotate_down call, on the parent, leaves its argument or,
         * when the parent is the root, the previous axis) */
        return RD32(parent + 0x50u) ? a1 : a1 + 0x0Cu;
    default: { /* WORLD_FRAME */
        static const uint32_t one = 0x3F800000u;
        const uint32_t axes[9] = { 0, 0, one, 0, one, 0, one, 0, 0 };
        for (int i = 0; i < 9; i++)
            WR32(a1 + 4u * (uint32_t)i, axes[i]);
        WR32(a2, 0);
        WR32(a2 + 4u, 0);
        WR32(a2 + 8u, 0);
        xform(node, a1 + 0x18u, 0);
        xform(node, a1 + 0x0Cu, 0);
        xform(node, a1, 0);
        xform(node, a2, 1);
        return parent ? 0 : a2;
    }
    }
}

static const uint32_t entries[7] = { 0x006071C0u, 0x00607120u, 0x00606F80u, 0x00606ED0u,
                                     0x006067D0u, 0x006065D0u, 0x006069E0u };

static int run(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret, int op)
{
    if (!enabled() || ((op == UNROTATE || op == UNTRANSFORM || op == TO_LOCAL) && !chain_ok(ecx)))
        return 0;
    const int two = op >= TO_WORLD;
    const uint32_t a1 = RD32(esp + 4u), a2 = two ? RD32(esp + 8u) : 0u;
    const uint32_t l1 = two ? 36u : 12u, l2 = two ? 12u : 0u;
    if (check) {
        uint8_t before[48], native[48], guest[48];
        memcpy(before, g_mem + a1, l1);
        memcpy(before + l1, g_mem + a2, l2);
        const uint32_t r = work(op, ecx, a1, a2, eax);
        memcpy(native, g_mem + a1, l1);
        memcpy(native + l1, g_mem + a2, l2);
        memcpy(g_mem + a1, before, l1);
        memcpy(g_mem + a2, before + l1, l2);
        declining = 1;
        *ret = rt_call_indirect(c, entries[op], esp, ecx, eax);
        declining = 0;
        memcpy(guest, g_mem + a1, l1);
        memcpy(guest + l1, g_mem + a2, l2);
        static unsigned calls, bad, rounding;
        calls++;
        if (r != (uint32_t)*ret && bad++ < 20)
            rt_log("native xform %08X: call %u eax %08X/%08X", entries[op], calls, r, (uint32_t)*ret);
        for (uint32_t i = 0; i < (l1 + l2) / 4u; i++) {
            float x, y;
            memcpy(&x, native + 4u * i, 4);
            memcpy(&y, guest + 4u * i, 4);
            if (!memcmp(&x, &y, 4))
                continue;
            if (fabsf(x - y) <= 1e-5f * (fabsf(y) > 1.0f ? fabsf(y) : 1.0f))
                rounding++;
            else if (bad++ < 20)
                rt_log("native xform %08X: call %u word %u native %g recompiled %g", entries[op], calls, i,
                       (double)x, (double)y);
        }
        if (calls % 20000u == 0u)
            rt_log("native xform: %u calls checked, %u differ, %u rounding", calls, bad, rounding);
        return 1;
    }
    const uint32_t r = work(op, ecx, a1, a2, eax);
    *ret = ((uint64_t)(esp + 4u + (two ? 8u : 4u)) << 32) | r;
    return 1;
}

int rt_native_rotate_up(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    return run(c, esp, ecx, eax, ret, ROTATE_UP);
}

int rt_native_transform_up(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    return run(c, esp, ecx, eax, ret, TRANSFORM_UP);
}

int rt_native_unrotate_down(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    return run(c, esp, ecx, eax, ret, UNROTATE);
}

int rt_native_untransform_down(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    return run(c, esp, ecx, eax, ret, UNTRANSFORM);
}

int rt_native_to_world(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    return run(c, esp, ecx, eax, ret, TO_WORLD);
}

int rt_native_to_local(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    return run(c, esp, ecx, eax, ret, TO_LOCAL);
}

int rt_native_world_frame(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    return run(c, esp, ecx, eax, ret, WORLD_FRAME);
}
