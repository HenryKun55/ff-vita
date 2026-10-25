/* Native versions of two functions of the engine's cell tree (an octree over an
 * unsigned 16-bit integer space: nodes of 0x16 bytes in an array, word +4 the
 * parent node, words +6..+0x14 the 8 child nodes, octant bit 0 x, bit 1 y,
 * bit 2 z; cells of 12 bytes, word +4 the cell's node, word +6 its bit row).
 *
 * 0x0056C880 box walk (recursive, 469k calls per ~30 s of play):
 *   int __thiscall Tree::walk(Visitor *v, uint16_t *node, int depth,
 *                             int x0, int y0, int z0, int x1, int y1, int z1)   (ret 0x24)
 *   Tree (ecx): +0x58 the node array. Visitor: +0 / +4 / +8 objects whose first
 *   virtual method is called (thiscall) when not null, +0x10 byte "stop at the
 *   first hit". The box min (x0 y0 z0) / max (x1 y1 z1) is copied to the frame
 *   (min at esp0-0x0C, max at esp0-0x18) and passed by pointer:
 *   - v[0]->f(tree, *node, &min, &max): false returns 0 at once;
 *   - depth 0: v[2]->f(tree, node, &min, &max, 0) -> r; when *node != 0 and
 *     not (stop && r), returns 0x0056C6D0(v, node, 0, x0..z1) (a guest call, kept
 *     as such), else r;
 *   - depth > 0: v[1]->f(tree, node, &min, &max, depth) -> r; when *node != 0
 *     and not (stop && r): the centre m = (min + max) / 2 (signed, truncated,
 *     then its low 16 bits), the 8 children in octant order, each with its
 *     child index copied to the guest's own node slot (esp0+4; the callbacks
 *     may change it), recursing with depth - 1 and the octant's half of the
 *     box; after each, the slot is written back to node[*node].child[k] (node
 *     index and array re-read), r += result, stop when (stop && r).
 *     Returns r.
 *   Every guest frame is laid out where the x86 code puts it (entry esp of a
 *   child call esp0-0x5C), with the same stores (its argument slots esp0+4 /
 *   esp0+0x0C reused, the locals, the saved registers); the callbacks are called
 *   with the guest's arguments, return addresses and registers (ecx, eax, edx,
 *   ebx, ebp, esi, edi), and the ecx / edx the recompiled code leaves in Ctx are
 *   reproduced.
 *
 * 0x0056DB30 cursor step:
 *   uint32_t __thiscall Cursor::step(const int16_t d[3])   (ret 4)
 *   Cursor (ecx): +0 Tree *, +0x18 word current cell, +0x1A / +0x1C / +0x1E word
 *   x y z. Tree: +0x2C word bit row stride (dwords), +0x4C level count n,
 *   +0x58 node array, +0x60 cell array, +0x80 bit array.
 *   p' = p + d (16-bit). When ((((x^x')|(y^y')) >> 3) | (z^z')) & 0xFFFF != 0 the
 *   cursor changed cell: from the current cell's node it climbs one parent per
 *   bit of that mask >> 1, then descends with the octants of (x' >> 3, y' >> 3,
 *   z') to the new cell. Then p = p' (stored), and when the cell's bit row is 0,
 *   or the n bits (bit (x&7) + (y&7)*8 + (n-1)*64 of the row, one every
 *   8 bytes down) are all 0, the current cell becomes 0. Returns the current
 *   cell in ax; the high half of eax is the x86's: the tree pointer's (row 0)
 *   or the bit pointer's after the loop. ecx / edx as the x86 leaves them.
 *   No guest calls; its own dead stack (locals, saved registers) is not written.
 *
 * Decompiled with Ghidra (ReVa) and checked line by line against the x86 code
 * (Ghidra's pseudo-code for both matches the disassembly). Integer only. */
#include "recomp.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 0x0056C880 ----------------------------------------------------------- */

/* A virtual call as the guest makes it: thiscall on obj, through its vtable's
 * first entry, args already on the guest stack at sp + 4. */
static uint32_t vcall(Ctx *c, uint32_t obj, uint32_t sp, uint32_t ret_addr, uint32_t eax)
{
    WR32(sp, ret_addr);
    return (uint32_t)rt_call_indirect(c, RD32(RD32(obj)), sp, obj, eax);
}

static uint32_t walk(Ctx *c, uint32_t E, uint32_t esi);

/* The k-th child call (k 0..7) of the frame at E: box b, the guest's edx at the call. */
static uint32_t walk_child(Ctx *c, uint32_t E, uint32_t esi, uint32_t edi, uint32_t depth, const uint32_t b[6],
                           uint32_t ebx, uint32_t edx, uint32_t ret_addr)
{
    const uint32_t ce = E - 0x5Cu;
    WR32(ce + 4u, edi);
    WR32(ce + 8u, E + 4u);
    WR32(ce + 12u, depth);
    for (uint32_t i = 0; i < 6u; i++)
        WR32(ce + 0x10u + 4u * i, b[i]);
    WR32(ce, ret_addr);
    c->r[2] = edx;
    c->r[3] = ebx;
    c->r[5] = depth;
    c->r[6] = esi;
    c->r[7] = edi;
    return walk(c, ce, esi);
}

/* The guest function at entry esp E (return address at E), this = esi. */
static uint32_t walk(Ctx *c, uint32_t E, uint32_t esi)
{
    const uint32_t s_ebx = c->r[3], s_ebp = c->r[5], s_esi = c->r[6], s_edi = c->r[7];
    const uint32_t a1 = RD32(E + 4u), a2 = RD32(E + 8u), a3 = RD32(E + 12u);
    const uint32_t x0 = RD32(E + 0x10u), y0 = RD32(E + 0x14u), z0 = RD32(E + 0x18u);
    const uint32_t x1 = RD32(E + 0x1Cu), y1 = RD32(E + 0x20u), z1 = RD32(E + 0x24u);
    const uint32_t mn = E - 0x0Cu, mxp = E - 0x18u;
    const uint32_t edi = a1;
    uint32_t ecx, edx, ebx, eax;

    /* Prologue: saved registers, the box copy. */
    WR32(E - 0x28u, s_ebx);
    WR32(E - 0x2Cu, s_ebp);
    WR32(E - 0x30u, s_esi);
    WR32(E - 0x08u, y0);
    WR32(E - 0x34u, s_edi);
    WR32(E - 0x14u, y1);
    WR32(E - 0x0Cu, x0);
    WR32(E - 0x04u, z0);
    WR32(E - 0x18u, x1);
    WR32(E - 0x10u, z1);
    edx = z1;

    /* v[0]->f(tree, *node, &min, &max). */
    ecx = RD32(edi);
    if (ecx) {
        const uint32_t w = RD16(a2);
        const uint32_t sp = E - 0x48u;
        WR32(sp + 4u, esi);
        WR32(sp + 8u, w);
        WR32(sp + 12u, mn);
        WR32(sp + 16u, mxp);
        c->r[2] = w;
        c->r[3] = s_ebx;
        c->r[5] = s_ebp;
        c->r[6] = esi;
        c->r[7] = edi;
        const uint32_t r = vcall(c, ecx, sp, 0x0056C8DAu, RD32(ecx));
        ecx = c->r[1];
        edx = c->r[2];
        if (!(r & 0xFFu)) {
            eax = 0;
            goto ret_plain;
        }
    }

    ebx = 0;
    if (a3 == 0)
        goto leaf;

    /* v[1]->f(tree, node, &min, &max, depth). */
    ecx = RD32(edi + 4u);
    if (ecx) {
        const uint32_t sp = E - 0x4Cu;
        WR32(sp + 4u, esi);
        WR32(sp + 8u, a2);
        WR32(sp + 12u, mn);
        WR32(sp + 16u, mxp);
        WR32(sp + 20u, a3);
        c->r[2] = a2;
        c->r[3] = 0;
        c->r[5] = a3;
        c->r[6] = esi;
        c->r[7] = edi;
        ebx = vcall(c, ecx, sp, 0x0056C914u, RD32(ecx));
        ecx = c->r[1];
        edx = c->r[2];
    }
    {
        const uint32_t idx = RD16(a2);
        WR32(E + 0x0Cu, idx);
        if (!idx)
            goto out;
        if (RD8(edi + 0x10u) && ebx)
            goto out;

        const uint32_t mx = (uint32_t)((int32_t)(x0 + x1) / 2) & 0xFFFFu;
        const uint32_t my = (uint32_t)((int32_t)(y0 + y1) / 2) & 0xFFFFu;
        const uint32_t mz = (uint32_t)((int32_t)(z0 + z1) / 2) & 0xFFFFu;
        WR16(E - 0x1Eu, my);
        WR16(E - 0x1Cu, mz);
        WR32(E + 4u, RD16(idx * 0x16u + RD32(esi + 0x58u) + 6u));
        WR32(E - 0x24u, mx);
        WR32(E + 0x0Cu, my);
        const uint32_t depth = a3 - 1u;

        static const uint32_t rets[8] = { 0x0056C9C5u, 0x0056CA25u, 0x0056CA85u, 0x0056CAE5u,
                                          0x0056CB45u, 0x0056CBA5u, 0x0056CC05u, 0x0056CC61u };
        uint32_t node = 0;
        for (uint32_t k = 0; k < 8u; k++) {
            if (k)   /* the next child of the node just written back (index and array re-read there) */
                WR32(E + 4u, RD16(node + 6u + 2u * k));
            const uint32_t b[6] = { (k & 1u) ? mx : x0, (k & 2u) ? my : y0, (k & 4u) ? mz : z0,
                                    (k & 1u) ? x1 : mx, (k & 2u) ? y1 : my, (k & 4u) ? z1 : mz };
            /* edx at the call: child 0 the pushed y0 (mov edx, [esp+0x58]), the others &slot (lea edx). */
            const uint32_t r = walk_child(c, E, esi, edi, depth, b, ebx, k ? E + 4u : y0, rets[k]);
            ebx += r;
            ecx = c->r[1];
            edx = RD32(esi + 0x58u);
            const uint32_t w = RD16(E + 4u);
            const uint32_t idx2 = RD16(a2);
            node = idx2 * 0x16u + edx;
            WR16(node + 6u + 2u * k, w);
            if (k == 7u) {
                ecx = idx2 * 0x16u;
                break;
            }
            ecx = (ecx & 0xFFFF0000u) | w;
            const uint32_t stop = RD8(edi + 0x10u);
            ecx = (ecx & 0xFFFFFF00u) | stop;
            if (stop && ebx)
                break;
        }
        goto out;
    }

leaf:
    /* v[2]->f(tree, node, &min, &max, 0). */
    ecx = RD32(edi + 8u);
    if (ecx) {
        const uint32_t sp = E - 0x4Cu;
        WR32(sp + 4u, esi);
        WR32(sp + 8u, a2);
        WR32(sp + 12u, mn);
        WR32(sp + 16u, mxp);
        WR32(sp + 20u, 0);
        c->r[2] = RD32(ecx);
        c->r[3] = 0;
        c->r[5] = a2;
        c->r[6] = esi;
        c->r[7] = edi;
        ebx = vcall(c, ecx, sp, 0x0056CCA6u, mn);
        ecx = c->r[1];
        edx = c->r[2];
    }
    if (RD16(a2) == 0)
        goto out;
    if (RD8(edi + 0x10u) && ebx)
        goto out;
    {
        /* 0x0056C6D0(v, node, 0, x0, y0, z0, x1, y1, z1), its result returned. */
        const uint32_t sp = E - 0x5Cu;
        const uint32_t p1 = RD32(E + 0x24u), p2 = RD32(E + 0x20u), p3 = RD32(E + 0x1Cu);
        const uint32_t p4 = RD32(E + 0x18u), p5 = RD32(E + 0x14u), p6 = RD32(E + 0x10u);
        WR32(sp + 0x24u, p1);
        WR32(sp + 0x20u, p2);
        WR32(sp + 0x1Cu, p3);
        WR32(sp + 0x18u, p4);
        WR32(sp + 0x14u, p5);
        WR32(sp + 0x10u, p6);
        WR32(sp + 0x0Cu, 0);
        WR32(sp + 8u, a2);
        WR32(sp + 4u, edi);
        WR32(sp, 0x0056CCE3u);
        c->r[2] = p5;
        c->r[3] = ebx;
        c->r[5] = a2;
        c->r[6] = esi;
        c->r[7] = edi;
        eax = (uint32_t)rt_call_indirect(c, 0x0056C6D0u, sp, esi, p6);
        goto ret_plain;
    }

out:
    /* 0x0056CC7D: the recompiled code stores ecx / edx on this exit only. */
    c->r[1] = ecx;
    c->r[2] = edx;
    eax = ebx;
ret_plain:
    c->r[3] = s_ebx;
    c->r[5] = s_ebp;
    c->r[6] = s_esi;
    c->r[7] = s_edi;
    return eax;
}

/* ---- 0x0056DB30 ----------------------------------------------------------- */

static uint32_t cursor_step(Ctx *c, uint32_t t, uint32_t p)
{
    const uint32_t y = RD16(t + 0x1Cu), x = RD16(t + 0x1Au);
    const uint32_t ny = (RD16(p + 2u) + y) & 0xFFFFu;
    const uint32_t nz0 = RD16(p + 4u);
    const uint32_t nx = (RD16(p) + x) & 0xFFFFu;
    const uint32_t z = RD16(t + 0x1Eu);
    const uint32_t nz = (nz0 + z) & 0xFFFFu;
    const uint32_t mask = ((((y ^ ny) | (x ^ nx)) >> 3) | (z ^ nz)) & 0xFFFFu;

    if (mask) {
        const uint32_t tree = RD32(t);
        const uint32_t nodes = RD32(tree + 0x58u);
        const uint32_t cur = RD16(t + 0x18u);
        const uint32_t cells = RD32(tree + 0x60u);
        const uint32_t Y = ny >> 2, Z = (nz << 2) & 0xFFFFu, X = nx >> 3;
        uint32_t node = RD16(cells + cur * 12u + 4u) * 0x16u + nodes;
        uint32_t m = mask >> 1, lvl = 0;
        while (m) {
            node = RD16(node + 4u) * 0x16u + nodes;
            m >>= 1;
            lvl++;
        }
        for (; lvl; lvl--) {
            const uint32_t s = lvl & 31u;
            const uint32_t oct = ((X >> s) & 1u) | ((Y >> s) & 2u) | ((Z >> s) & 4u);
            node = RD16(node + 6u + oct * 2u) * 0x16u + nodes;
        }
        const uint32_t oct = (Z & 4u) | (Y & 2u) | (X & 1u);
        WR16(t + 0x18u, RD16(node + 6u + oct * 2u));
    }

    WR16(t + 0x1Au, RD16(t + 0x1Au) + RD16(p));
    WR16(t + 0x1Cu, RD16(t + 0x1Cu) + RD16(p + 2u));
    WR16(t + 0x1Eu, RD16(t + 0x1Eu) + RD16(p + 4u));

    const uint32_t idx = RD16(t + 0x18u);
    const uint32_t tree = RD32(t);
    const uint32_t cell = RD32(tree + 0x60u) + idx * 12u;
    const uint32_t row = RD16(cell + 6u);
    c->r[2] = t;
    if (!row) {
        WR16(t + 0x18u, 0);
        c->r[1] = cell;
        return tree & 0xFFFF0000u;
    }
    const uint32_t n = RD32(tree + 0x4Cu);
    uint32_t bit = (RD8(t + 0x1Cu) & 7u) + n * 8u;
    bit = (RD8(t + 0x1Au) & 7u) + bit * 8u - 0x40u;
    const uint32_t off = (uint32_t)RD16(tree + 0x2Cu) * row + (uint32_t)((int32_t)bit >> 5);
    const uint32_t sh = bit & 0x1Fu;
    uint32_t a = RD32(tree + 0x80u) + off * 4u;
    c->r[1] = sh;
    if ((int32_t)(n - 1u) >= 0) {
        uint32_t bits = 0, k = n;
        do {
            const uint32_t v = (uint32_t)((int32_t)RD32(a) >> sh);
            bits += bits;
            a -= 8u;
            bits |= v & 1u;
        } while (--k);
        if (bits)
            return (a & 0xFFFF0000u) | RD16(t + 0x18u);
    }
    WR16(t + 0x18u, 0);
    return (a & 0xFFFF0000u) | RD16(t + 0x18u);
}

/* ---- entry points ----------------------------------------------------------- */

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

int rt_native_cell_tree_walk(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    if (!enabled())
        return 0;
    (void)eax;
    const uint32_t r = walk(c, esp, ecx);
    *ret = ((uint64_t)(esp + 4u + 0x24u) << 32) | r;   /* ret 0x24 */
    return 1;
}

int rt_native_cell_cursor_step(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    if (!enabled())
        return 0;
    (void)eax;
    const uint32_t r = cursor_step(c, ecx, RD32(esp + 4u));
    *ret = ((uint64_t)(esp + 4u + 4u) << 32) | r;   /* ret 4 */
    return 1;
}
