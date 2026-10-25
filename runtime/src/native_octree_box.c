/* Native version of the engine's box query through its collision octree
 * (0x006164F0, recursive), with the "every item of a subtree" walk it calls
 * (0x00616740). The segment and point queries of the same tree are
 * native_octree.c and native_octree_point.c.
 *
 * void __thiscall Node::box(Query *q, int level, int cx, int cy, int cz)   (ret 0x14)
 *   Node (6 bytes, ecx): word 0 bits 3..14 item count (bit 15: last child record),
 *     its low 3 bits the octant within the parent; word 1 first child record
 *     (index into q->children, 6 bytes each); word 2 first item (index into
 *     q->items, 16 bytes each: unsigned 16-bit box min x y z at +4, max at +0x0A).
 *   Query: +4 callback void(*)(Item *, Query *) (cdecl), +0x0C children, +0x10 items,
 *     +0x14 / +0x18 / +0x1C the query box min x y z, +0x20 / +0x24 / +0x28 max (int).
 * The cube's size is 0x10000 >> (level + 1). Every item of the node whose box
 * overlaps the query box (strictly: qmin < max and qmax > min) goes to the callback.
 * Then each child record, in order, whose cube (centre c +- size/2, half size
 * size) overlaps the query box (qmin < centre + size, qmax >= centre - size): if the
 * query box contains the cube (qmin <= centre - size, qmax >= centre + size) every
 * item of the child and of its whole subtree goes to the callback (the child's
 * items, then 0x00616740 on each of its child records); otherwise the child is
 * visited recursively with level + 1.
 *
 * void __thiscall Node::all(Query *q)   (0x00616740, ret 4): every item of the node
 *   to the callback, then itself on each child record, in order.
 *
 * Disassembled and checked against the x86 code (the ReVa decompiler did not
 * answer when this was written). Every guest memory effect is reproduced: the
 * level argument slot (esp+8) overwritten with level + 1, the q argument slot
 * (esp+4) reused for the current child record, the centre and level re-read from
 * the argument slots each time, the callbacks and 0x00616740 frames called with the
 * guest's arguments, return addresses, stack depth and registers (eax, ecx, edx,
 * ebx, ebp, esi, edi as the x86 has them at the call). The returned eax / ecx / edx
 * are the x86 code's. Only the dead stack below esp (saved registers, locals) is
 * not written. */
#include "recomp.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Out {
    uint32_t eax, ecx, edx;
} Out;

/* The callback, called as the guest calls it: push q; push item; call [q+4].
 * sp: the return address slot. */
static uint32_t callback(Ctx *c, uint32_t q, uint32_t item, uint32_t sp, uint32_t ra, uint32_t ecx, uint32_t eax,
                         uint32_t edx, uint32_t ebx, uint32_t ebp, uint32_t esi, uint32_t edi, uint32_t *ecx_out,
                         uint32_t *edx_out)
{
    WR32(sp + 8u, q);
    WR32(sp + 4u, item);
    WR32(sp, ra);
    c->r[2] = edx;
    c->r[3] = ebx;
    c->r[5] = ebp;
    c->r[6] = esi;
    c->r[7] = edi;
    const uint64_t rv = rt_call_indirect(c, RD32(q + 4u), sp, ecx, eax);
    *ecx_out = c->r[1];
    *edx_out = c->r[2];
    return (uint32_t)rv;
}

/* 0x00616740. esp: its entry esp (return address at esp). */
static void all(Ctx *c, uint32_t node, uint32_t esp, uint32_t ecx_in, uint32_t edx_in, Out *o)
{
    const uint32_t q = RD32(esp + 4u);
    uint32_t ecx = ecx_in, edx = edx_in;
    uint32_t eax = ((uint32_t)RD16(node) >> 3) & 0xFFFu;
    if (eax) {
        edx = RD32(q + 0x10u);
        uint32_t item = RD16(node + 4u) * 16u + edx;
        /* push ebx, esi, edi, ebp; push edi; push esi; call */
        for (uint32_t n = eax; n; n--, item += 16u)
            eax = callback(c, q, item, esp - 0x1Cu, 0x00616775u, ecx, eax, edx, node, n, item, q, &ecx, &edx);
    }
    eax = (eax & 0xFFFF0000u) | RD16(node + 2u);
    if (eax & 0xFFFFu) {
        ecx = RD32(q + 0x0Cu);
        eax = (eax & 0xFFFFu) * 3u;
        uint32_t rec = ecx + eax * 2u;
        if (rec) {
            const uint32_t ce = esp - 0x14u;   /* 3 saved registers, push edi, call */
            for (;;) {
                WR32(ce + 4u, q);
                WR32(ce, 0x006167A0u);
                Out co;
                all(c, rec, ce, rec, edx, &co);
                ecx = co.ecx;
                edx = co.edx;
                eax = RD16(rec) & 0x8000u;
                rec += 6u;
                if (eax)
                    break;
            }
        }
    }
    o->eax = eax;
    o->ecx = ecx;
    o->edx = edx;
}

/* 0x006164F0. esp: its entry esp. ebp_in..: only for the registers it leaves. */
static void box(Ctx *c, uint32_t node, uint32_t esp, uint32_t edx_in, Out *o)
{
    (void)edx_in;
    const uint32_t lvl = RD32(esp + 8u) + 1u;
    const uint32_t size = (uint32_t)((int32_t)0x10000 >> (lvl & 31u));
    const uint32_t q = RD32(esp + 4u);
    uint32_t eax = ((uint32_t)RD16(node) >> 3) & 0xFFFu;
    uint32_t ecx = lvl, edx = size;
    WR32(esp + 8u, lvl);
    const uint32_t cb_sp = esp - 0x2Cu - 8u - 4u;   /* below its frame: 2 arguments, return address */

    /* The node's items overlapping the query box. */
    if (eax) {
        uint32_t item = RD16(node + 4u) * 16u + RD32(q + 0x10u);
        for (uint32_t n = eax; n; n--, item += 16u) {
            eax = RD16(item + 0x0Au);
            if ((int32_t)RD32(q + 0x14u) >= (int32_t)eax)
                continue;
            ecx = RD16(item + 4u);
            if ((int32_t)RD32(q + 0x20u) <= (int32_t)ecx)
                continue;
            eax = RD16(item + 0x0Cu);
            if ((int32_t)RD32(q + 0x18u) >= (int32_t)eax)
                continue;
            ecx = RD16(item + 6u);
            if ((int32_t)RD32(q + 0x24u) <= (int32_t)ecx)
                continue;
            eax = RD16(item + 0x0Eu);
            if ((int32_t)RD32(q + 0x1Cu) >= (int32_t)eax)
                continue;
            ecx = RD16(item + 8u);
            if ((int32_t)RD32(q + 0x28u) <= (int32_t)ecx)
                continue;
            uint32_t dummy;
            eax = callback(c, q, item, cb_sp, 0x0061656Du, ecx, eax, edx, node, n, q, item, &ecx, &dummy);
            edx = size;   /* mov edx, [esp+0x18]: the size local */
        }
    }

    /* The child octants overlapping the query box. */
    eax = (eax & 0xFFFF0000u) | RD16(node + 2u);
    if (eax & 0xFFFFu) {
        ecx = RD32(q + 0x0Cu);
        eax = ecx + (eax & 0xFFFFu) * 6u;
        WR32(esp + 4u, eax);
        if (eax) {
            const uint32_t half = (uint32_t)((int32_t)edx >> 1);
            uint32_t rec = eax;
            for (;;) {
                const uint32_t w = RD16(rec);
                const uint32_t oct = w & 7u;
                const uint32_t x = ((oct & 1u) ? half : 0u - half) + RD32(esp + 0x0Cu);
                const uint32_t y = RD32(esp + 0x10u) + ((oct & 2u) ? half : 0u - half);
                const uint32_t z = RD32(esp + 0x14u) + ((oct & 4u) ? half : 0u - half);
                const int32_t q14 = (int32_t)RD32(q + 0x14u);
                edx = y;
                ecx = z;
                int skip = 1, partial = 0;
                if (q14 < (int32_t)(x + size) && (int32_t)RD32(q + 0x20u) >= (int32_t)(x - size) &&
                    (int32_t)RD32(q + 0x18u) < (int32_t)(y + size)) {
                    edx = y - size;
                    if ((int32_t)RD32(q + 0x24u) >= (int32_t)(y - size) &&
                        (int32_t)RD32(q + 0x1Cu) < (int32_t)(z + size) &&
                        (int32_t)RD32(q + 0x28u) >= (int32_t)(z - size)) {
                        skip = 0;
                        partial = q14 > (int32_t)(x - size) || (int32_t)RD32(q + 0x20u) < (int32_t)(x + size) ||
                                  (int32_t)RD32(q + 0x18u) > (int32_t)(y - size) ||
                                  (int32_t)RD32(q + 0x24u) < (int32_t)(y + size) ||
                                  (int32_t)RD32(q + 0x1Cu) > (int32_t)(z - size) ||
                                  (int32_t)RD32(q + 0x28u) < (int32_t)(z + size);
                    }
                }
                if (!skip && partial) {
                    /* push z; push y; push x; push level+1; push q; call: the child's entry esp. */
                    const uint32_t ce = esp - 0x2Cu - 0x14u - 4u;
                    const uint32_t r = RD32(esp + 4u);
                    WR32(ce + 0x14u, z);
                    WR32(ce + 0x10u, y);
                    WR32(ce + 0x0Cu, x);
                    WR32(ce + 8u, RD32(esp + 8u));
                    WR32(ce + 4u, q);
                    WR32(ce, 0x00616715u);
                    Out co;
                    box(c, r, ce, x, &co);
                    edx = co.edx;
                } else if (!skip) {
                    /* Contained: the child's items, then its whole subtree. */
                    const uint32_t cnt = (w >> 3) & 0xFFFu;
                    edx = size;
                    if (cnt) {
                        eax = RD32(esp + 4u);
                        ecx = RD32(q + 0x10u);
                        uint32_t item = RD16(eax + 4u) * 16u + ecx;
                        for (uint32_t n = cnt; n; n--, item += 16u)
                            eax = callback(c, q, item, cb_sp, 0x006166BAu, ecx, eax, edx, n, (uint32_t)q14, q, item,
                                           &ecx, &edx);
                    }
                    const uint32_t r = RD32(esp + 4u);
                    const uint32_t idx = RD16(r + 2u);
                    if (idx) {
                        edx = idx * 3u;
                        uint32_t ch = RD32(q + 0x0Cu) + edx * 2u;
                        if (ch) {
                            const uint32_t ae = esp - 0x2Cu - 4u - 4u;   /* push esi; call */
                            for (;;) {
                                WR32(ae + 4u, q);
                                WR32(ae, 0x006166E8u);
                                Out ao;
                                all(c, ch, ae, ch, edx, &ao);
                                edx = ao.edx;
                                const uint32_t last = RD16(ch) & 0x8000u;
                                ch += 6u;
                                if (last)
                                    break;
                            }
                        }
                    }
                }
                rec = RD32(esp + 4u);
                eax = RD16(rec) & 0x8000u;
                rec += 6u;
                WR32(esp + 4u, rec);
                ecx = rec;
                if (eax)
                    break;
            }
        }
    }
    o->eax = eax;
    o->ecx = ecx;
    o->edx = edx;
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

int rt_native_octree_box(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    if (!enabled())
        return 0;
    (void)eax;
    const uint32_t ebx = c->r[3], ebp = c->r[5], esi = c->r[6], edi = c->r[7];
    Out o;
    box(c, ecx, esp, c->r[2], &o);
    c->r[1] = o.ecx;
    c->r[2] = o.edx;
    c->r[3] = ebx;
    c->r[5] = ebp;
    c->r[6] = esi;
    c->r[7] = edi;
    *ret = ((uint64_t)(esp + 4u + 0x14u) << 32) | o.eax;   /* ret 0x14 */
    return 1;
}

int rt_native_octree_all(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    if (!enabled())
        return 0;
    (void)eax;
    const uint32_t ebx = c->r[3], ebp = c->r[5], esi = c->r[6], edi = c->r[7];
    Out o;
    all(c, ecx, esp, ecx, c->r[2], &o);
    c->r[1] = o.ecx;
    c->r[2] = o.edx;
    c->r[3] = ebx;
    c->r[5] = ebp;
    c->r[6] = esi;
    c->r[7] = edi;
    *ret = ((uint64_t)(esp + 4u + 4u) << 32) | o.eax;   /* ret 4 */
    return 1;
}
