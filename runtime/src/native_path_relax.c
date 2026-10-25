/* Native version of the path finder's neighbour step (0x00571100, ~290k calls per
 * 30 s of play; 13 calls from the search loop 0x0056F8A0, one per direction).
 *
 * uint16_t __thiscall Search::relax(uint32_t from, Cursor *cur, int dir, int unused,
 *                                   int arg5)   (ret 0x14)
 *   dir == 10: returns Search::0x00571E20(from, arg5) (called as the guest does).
 *   Otherwise the neighbour in direction dir: dir == 11 takes cur+0x18 itself,
 *   else cur->0x0056DB30 (cur+0x22 == 3) or cur->0x0056DD10 (other kinds) with the
 *   direction record 0x7641AC + dir * 6 (both thiscall, ret 4, called as the guest
 *   does). 0 (low word) ends the step with that result.
 *   Then the neighbour's search node, by cur+0x22 (graph G = cur->+0):
 *     1: record P = G->+0x58 + idx * 0x16 (idx = word cur+0x18), node word P+2;
 *     2: record P = G->+0x60 + idx * 12, node word P+2;
 *     3: record P = G->+0x60 + idx * 12, node word P+0x0A, plus
 *        (byte cur+0x1A & 7) + (byte cur+0x1C & 7) * 8;
 *     other: fails.
 *   A record without a node (word 0) is given the next free one: *(cur+4)
 *   (kinds 1/2: limited to word G+0x54 with the size byte of G->+0x88 + word P * 10,
 *   which is then added to *(cur+4); kind 3: with the dword cur+8, added likewise).
 *   Out of nodes or node 0: fails, low word 0xFFFF.
 *   Nodes are 24 bytes at this->+0xAC: cost float +0x0C, parent word +0x10, flags
 *   byte +0x16 (bit 0 cleared: open), the cursor's words +0x18..+0x1E at +0/+4/+6/+8.
 *   cost = node[from].cost + 1.0001f ([0x718320]); when cost + 0.1f ([0x6FF560]) <
 *   node[n].cost (ordered) the neighbour is updated and 0x00571CE0(from, n) queues
 *   it (thiscall, ret 8, called as the guest does). Returns low word 1.
 *
 * Decompiled with Ghidra (ReVa) and checked against the x86 code. Every path gives
 * the guest's full eax (the 16-bit results keep the upper half the x86 leaves in
 * eax: a callee's result, the record pointer P, the 0xAC array pointer, ...), the
 * stores are the guest's in the same order, and the calls get the guest's stack
 * slots, return addresses and register values (ebx, ebp, esi, edi, edx), with the
 * callee-saved ones put back on return. edx/ecx on return are not reproduced
 * (caller-saved). The float sum and compare are single precision, as the x87 at
 * 24-bit precision control (one rounding of a sum of two floats, exact either way). */
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

static int on = -1;

static void config(void)
{
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

static uint64_t done(Ctx *c, const uint32_t saved[4], uint32_t esp, uint32_t eax)
{
    c->r[3] = saved[0];
    c->r[5] = saved[1];
    c->r[6] = saved[2];
    c->r[7] = saved[3];
    return ((uint64_t)(esp + 4u + 0x14u) << 32) | eax;   /* thiscall, ret 0x14 */
}

int rt_native_path_relax(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    if (on < 0)
        config();
    if (!on)
        return 0;
    (void)eax;
    const uint32_t self = ecx;
    const uint32_t from = RD32(esp + 4u), cur = RD32(esp + 8u), dir = RD32(esp + 12u);
    const uint32_t saved[4] = { c->r[3], c->r[5], c->r[6], c->r[7] };   /* ebx ebp esi edi */
    /* push ecx; push ebx; push ebp; push esi; push edi; [esp+0x10] = this. */
    WR32(esp - 4u, self);
    WR32(esp - 8u, saved[0]);
    WR32(esp - 12u, saved[1]);
    WR32(esp - 16u, saved[2]);
    WR32(esp - 20u, saved[3]);
    const uint32_t sp = esp - 20u;
    c->r[5] = self;

    if (dir == 10u) {
        WR32(sp - 4u, RD32(esp + 20u));
        WR32(sp - 8u, from);
        WR32(sp - 12u, 0x00571125u);
        const uint32_t r = (uint32_t)rt_call_indirect(c, 0x00571E20u, sp - 12u, self, dir);
        *ret = done(c, saved, esp, r);
        return 1;
    }

    /* The neighbour. */
    uint32_t r, edx;
    if (dir == 11u) {
        r = RD16(cur + 0x18u);
        edx = c->r[2];
    } else {
        const uint32_t rec = dir * 6u + 0x007641ACu;
        c->r[6] = cur;
        WR32(sp - 4u, rec);
        if (RD8(cur + 0x22u) == 3u) {
            WR32(sp - 8u, 0x00571154u);
            c->r[2] = dir * 3u;
            r = (uint32_t)rt_call_indirect(c, 0x0056DB30u, sp - 8u, cur, rec);
        } else {
            WR32(sp - 8u, 0x00571168u);
            c->r[2] = rec;
            r = (uint32_t)rt_call_indirect(c, 0x0056DD10u, sp - 8u, cur, dir);
        }
        edx = c->r[2];
    }
    if (!(r & 0xFFFFu)) {
        *ret = done(c, saved, esp, r);
        return 1;
    }

    /* Its search node. */
    const uint32_t kind = RD8(cur + 0x22u);
    uint32_t P;
    if (kind == 3u) {
        const uint32_t g = RD32(cur);
        P = RD32(g + 0x60u) + RD16(cur + 0x18u) * 12u;
        if (RD16(P + 0x0Au) == 0u) {
            const uint32_t w = RD16(RD32(cur + 4u));
            const uint32_t need = w + RD32(cur + 8u);
            if ((int32_t)need > (int32_t)RD16(g + 0x54u)) {
                *ret = done(c, saved, esp, (P & 0xFFFF0000u) | 0xFFFFu);
                return 1;
            }
            WR16(P + 0x0Au, w);
            const uint32_t q = RD32(cur + 4u);
            WR16(q, RD16(q) + RD16(cur + 8u));
        }
        edx = ((RD8(cur + 0x1Au) & 7u) + (RD8(cur + 0x1Cu) & 7u) * 8u + RD16(P + 0x0Au)) & 0xFFFFu;
    } else if (kind == 1u || kind == 2u) {
        const uint32_t g = RD32(cur);
        if (kind == 1u) {
            P = RD16(cur + 0x18u) * 0x16u + RD32(g + 0x58u);
        } else {
            const uint32_t base = RD32(g + 0x60u);
            P = base + RD16(cur + 0x18u) * 12u;
            edx = base;
        }
        if (RD16(P + 2u) == 0u) {
            const uint32_t s = RD32(g + 0x88u) + RD16(P) * 10u;   /* size byte */
            const uint32_t size = RD8(s);
            const uint32_t w = RD16(RD32(cur + 4u));
            if ((int32_t)(size + w) > (int32_t)RD16(g + 0x54u)) {
                *ret = done(c, saved, esp, (P & 0xFFFF0000u) | 0xFFFFu);
                return 1;
            }
            WR16(P + 2u, w);
            const uint32_t add = RD8(s);
            const uint32_t q = RD32(cur + 4u);
            WR16(q, RD16(q) + add);
            edx = s;
        }
        edx = (edx & 0xFFFF0000u) | RD16(P + 2u);
    } else {
        /* dec eax three times: eax = kind - 3. */
        *ret = done(c, saved, esp, ((kind - 3u) & 0xFFFF0000u) | 0xFFFFu);
        return 1;
    }
    const uint32_t n = edx & 0xFFFFu;
    if (n == 0u) {
        *ret = done(c, saved, esp, (P & 0xFFFF0000u) | 0xFFFFu);
        return 1;
    }

    /* The cost through `from`. */
    const uint32_t nodes = RD32(self + 0xACu);
    const uint32_t i3 = (from & 0xFFFFu) * 3u;
    const float cost = lf(nodes + i3 * 8u + 0x0Cu) + lf(0x00718320u);
    const uint32_t b = nodes + n * 24u;
    if (!(lf(0x006FF560u) + cost < lf(b + 0x0Cu))) {
        *ret = done(c, saved, esp, (nodes & 0xFFFF0000u) | 1u);
        return 1;
    }
    const uint32_t flags = RD8(b + 0x16u);
    memcpy(g_mem + b + 0x0Cu, &cost, 4);
    WR16(b + 0x10u, from);
    WR8(b + 0x16u, flags & 0xFEu);
    WR16(b, RD16(cur + 0x18u));
    WR16(b + 4u, RD16(cur + 0x1Au));
    WR16(b + 6u, RD16(cur + 0x1Cu));
    const uint32_t w1e = RD16(cur + 0x1Eu);
    WR16(b + 8u, w1e);
    /* push edx; push ebx; call 0x00571CE0 (returns to 0x005712F0). */
    WR32(sp - 4u, edx);
    WR32(sp - 8u, from);
    WR32(sp - 12u, 0x005712F0u);
    c->r[2] = edx;
    c->r[3] = from;
    c->r[6] = cur;
    c->r[7] = i3;
    const uint32_t q = (uint32_t)rt_call_indirect(c, 0x00571CE0u, sp - 12u, self, (nodes & 0xFFFF0000u) | w1e);
    *ret = done(c, saved, esp, (q & 0xFFFF0000u) | 1u);
    return 1;
}
