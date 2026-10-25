/* Native version of the engine's segment query through a quadtree (0x00614CE0,
 * recursive), with its segment / rectangle test in the ground plane (0x00614EB0)
 * and the segment / box test 0x005FF460 (ff_segment_box, native_raybox.c).
 *
 * bool __thiscall Node::segment(Query *q, Tree *tree, int size, int cx, int cz)   (ret 0x14)
 *   Node: words +0 .. +6 the four children (index into the tree, 0: none; child i
 *     is the quadrant x +- size/4 by bit 0, z +- size/4 by bit 1), +0x0C the first
 *     item (linked list: +0 next, +8 what the callback gets, words +0x0C..+0x16 the
 *     box min x y z, max x y z).
 *   Tree: +0 nodes, +0x0C node stride (child node = tree->nodes + stride * index).
 *   Query: +4 callback bool(*)(void *, Query *) (cdecl), +0x30 pointer to the
 *     segment's origin (float x y z), +0x38 / +0x3C / +0x40 its direction
 *     (float dx dy dz), +0x44 / +0x4C the reciprocals of dx / dz.
 * Every item whose box the segment hits (0x005FF460) goes to the callback; then
 * every child quadrant whose square (centre +- size/2) the segment hits in x / z
 * (0x00614EB0) is visited with half the size. Returns whether any callback
 * returned true (al; eax is that byte).
 *
 * bool __cdecl rect(float x0, float z0, float x1, float z1, Query *q, float *t)   (0x00614EB0)
 *   Origin inside the rectangle (compared as the float's integer bits, the x86's
 *   way): *t = 0, true. Otherwise the entry parameter on the x and z axes the
 *   segment moves along (-1 on a still axis), the larger of them (by fcomp: x when
 *   tx >= tz), the hit point on the other axis inside the rectangle (integer bits
 *   again), *t = that t, and the result is 0 <= t <= 1 (integer bits). Its own
 *   argument slots x1, z1 and q are overwritten as the x86 code does.
 *
 * Disassembled and checked against the x86 code (the ReVa decompiler did not
 * answer when this was written). Single precision in the x87 order. 0x00614CE0's
 * only guest memory effects besides the callbacks are on its dead stack (the
 * locals, the arguments and *t of its 0x005FF460 / 0x00614EB0 calls), so those two
 * run on host values; the callbacks and the recursion get the guest's arguments,
 * return addresses, stack depth and registers. The returned eax / ecx / edx are the
 * x86 code's (edx after a 0x005FF460 call: what native_raybox.c leaves, the item's
 * word +0x0E). */
#include "recomp.h"
#include "runtime.h"
#include "native.h"

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

static inline int32_t bits(float v)
{
    int32_t b;
    memcpy(&b, &v, 4);
    return b;
}

typedef struct Rect {
    uint32_t eax, ecx, edx;
    float slot_x1, slot_z1, slot_q;   /* what the argument slots x1, z1, q end up holding */
    int t_written;
    float t;
    int t_zero;                       /* *t = 0 as an integer store */
} Rect;

/* 0x00614EB0 on host values. out: the t pointer (for the eax / ecx it leaves). */
static void rect(float x0, float z0, float x1, float z1, uint32_t q, uint32_t out, Rect *r)
{
    const uint32_t p = RD32(q + 0x30u);
    const float dx0 = ldf(p) - x0;
    const float dz0 = ldf(p + 8u) - z0;
    const float w = x1 - x0;
    const float h = z1 - z0;
    const int32_t bdx0 = bits(dx0), bdz0 = bits(dz0), bw = bits(w), bh = bits(h);
    r->slot_q = dz0;
    r->slot_x1 = w;
    r->slot_z1 = h;
    r->edx = (uint32_t)bh;
    r->ecx = q;
    r->t_written = 0;
    r->t_zero = 0;
    if (bdx0 >= 0 && bdx0 <= bw && bdz0 >= 0 && bdz0 <= bh) {
        r->t_written = 1;
        r->t_zero = 1;
        r->eax = (out & 0xFFFFFF00u) | 1u;
        return;
    }
    float s1 = -1.0f, s2 = -1.0f;
    const int32_t ddx = (int32_t)RD32(q + 0x38u), ddz = (int32_t)RD32(q + 0x40u);
    if (ddx != 0)
        s1 = ddx >= 0 ? -(dx0 * ldf(q + 0x44u)) : (w - dx0) * ldf(q + 0x44u);
    if (ddz != 0)
        s2 = ddz >= 0 ? -(dz0 * ldf(q + 0x4Cu)) : (h - dz0) * ldf(q + 0x4Cu);
    float t;
    if (s1 >= s2) {   /* fcomp: C0 clear (unordered sets it) */
        t = s1;
        const float v = s1 * ldf(q + 0x40u) + dz0;
        r->slot_x1 = t;
        r->slot_z1 = v;
        const int32_t b = bits(v);
        if (b < 0 || b > bh) {
            r->eax = (uint32_t)b & 0xFFFFFF00u;
            return;
        }
    } else {
        t = s2;
        const float v = s2 * ldf(q + 0x38u) + dx0;
        r->slot_x1 = t;
        r->slot_z1 = v;
        const int32_t b = bits(v);
        if (b < 0 || b > bw) {
            r->eax = (uint32_t)b & 0xFFFFFF00u;
            return;
        }
    }
    r->t_written = 1;
    r->t = t;
    r->ecx = out;
    const int32_t bt = bits(t);
    r->eax = (bt < 0 || bt > 0x3F800000) ? 0u : 1u;
}

/* 0x00614CE0. esp: its entry esp; ebp_in: the ebp it gets (its callbacks see it). */
static uint32_t quad(Ctx *c, uint32_t node, uint32_t esp, uint32_t ebp_in, uint32_t edx_in, uint32_t *edx_out)
{
    uint32_t q = RD32(esp + 4u);
    uint32_t edx = edx_in;
    uint32_t flag = 0;

    /* The node's items the segment hits. */
    for (uint32_t item = RD32(node + 0x0Cu); item; item = RD32(item)) {
        const uint32_t p = RD32(q + 0x30u);
        const float o[3] = { ldf(p), ldf(p + 4u), ldf(p + 8u) };
        const float d[3] = { ldf(q + 0x38u), ldf(q + 0x3Cu), ldf(q + 0x40u) };
        float bx[6];
        for (int j = 0; j < 6; j++)
            bx[j] = (float)RD16(item + 0x0Cu + 2u * (uint32_t)j);
        float t = -1.0f;
        const int hit = ff_segment_box(bx, o, d, &t, NULL);
        const uint32_t eax = (RD16(item + 0x0Cu) & 0xFFFFFF00u) | (uint32_t)hit;
        edx = RD16(item + 0x0Eu);
        if (hit) {
            /* push q; push item+8; call [q+4] below the frame (0x18 + 4 registers). */
            const uint32_t sp = esp - 0x28u - 8u - 4u;
            WR32(sp + 8u, q);
            WR32(sp + 4u, item + 8u);
            WR32(sp, 0x00614D9Eu);
            c->r[2] = edx;
            c->r[3] = q;
            c->r[5] = ebp_in;
            c->r[6] = item;
            c->r[7] = node;
            const uint64_t rv = rt_call_indirect(c, RD32(q + 4u), sp, item + 8u, eax);
            flag |= (uint32_t)rv & 0xFFu;
            edx = c->r[2];
        }
    }

    /* The child quadrants the segment crosses, in order. */
    const uint32_t quarter = (uint32_t)((int32_t)RD32(esp + 0x0Cu) >> 2);
    for (uint32_t i = 0; i < 4u; i++) {
        if (RD16(node + 2u * i) == 0)
            continue;
        const uint32_t half = (uint32_t)((int32_t)RD32(esp + 0x0Cu) >> 1);
        const uint32_t x = ((i & 1u) ? quarter : 0u - quarter) + RD32(esp + 0x10u);
        const uint32_t z = ((i & 2u) ? quarter : 0u - quarter) + RD32(esp + 0x14u);
        Rect r;
        /* *t is the local at esp-8 (dead stack). */
        rect((float)(int32_t)(x - half), (float)(int32_t)(z - half), (float)(int32_t)(x + half),
             (float)(int32_t)(z + half), q, esp - 8u, &r);
        edx = r.edx;
        q = RD32(esp + 4u);   /* mov ebx, [esp+0x44] */
        if (r.eax & 0xFFu) {
            const uint32_t tree = RD32(esp + 8u);
            const uint32_t child = RD32(tree + 0x0Cu) * RD16(node + 2u * i) + RD32(tree);
            /* push z; push x; push half; push tree; push q; call. */
            const uint32_t ce = esp - 0x28u - 0x14u - 4u;
            WR32(ce + 0x14u, z);
            WR32(ce + 0x10u, x);
            WR32(ce + 0x0Cu, half);
            WR32(ce + 8u, tree);
            WR32(ce + 4u, q);
            WR32(ce, 0x00614E75u);
            flag |= quad(c, child, ce, x, RD32(tree), &edx);
        }
    }
    *edx_out = edx;
    return flag;
}

static int on = -1;

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
        const char *k = getenv("FF_NATIVE");
        on = !(k && k[0] == '0');
#endif
    }
    return on;
}

int rt_native_quad_segment(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    if (!enabled())
        return 0;
    (void)eax;
    const uint32_t ebx = c->r[3], ebp = c->r[5], esi = c->r[6], edi = c->r[7];
    uint32_t edx;
    const uint32_t flag = quad(c, ecx, esp, ebp, c->r[2], &edx);
    c->r[1] = (uint32_t)((int32_t)RD32(esp + 0x0Cu) >> 2);
    c->r[2] = edx;
    c->r[3] = ebx;
    c->r[5] = ebp;
    c->r[6] = esi;
    c->r[7] = edi;
    *ret = ((uint64_t)(esp + 4u + 0x14u) << 32) | flag;   /* ret 0x14 */
    return 1;
}

int rt_native_quad_rect(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    if (!enabled())
        return 0;
    (void)ecx;
    (void)eax;
    const uint32_t A = esp + 4u, out = RD32(A + 0x14u);
    Rect r;
    rect(ldf(A), ldf(A + 4u), ldf(A + 8u), ldf(A + 0x0Cu), RD32(A + 0x10u), out, &r);
    /* The guest's stores, in its order: q slot, x1 slot, z1 slot, then *t. */
    stf(A + 0x10u, r.slot_q);
    stf(A + 8u, r.slot_x1);
    stf(A + 0x0Cu, r.slot_z1);
    if (r.t_written) {
        if (r.t_zero)
            WR32(out, 0);
        else
            stf(out, r.t);
    }
    c->r[1] = r.ecx;
    c->r[2] = r.edx;
    *ret = ((uint64_t)(esp + 4u) << 32) | r.eax;   /* cdecl */
    return 1;
}
