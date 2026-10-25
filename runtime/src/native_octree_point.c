/* Native version of the engine's point query through its collision octree
 * (0x00616B70, recursive; the segment query of the same tree is native_octree.c).
 *
 * void __thiscall Node::point(Query *q, int cx, int cy, int cz)   (ret 0x10)
 *   Node (6 bytes, ecx): word 0 bits 3..14 item count (bit 15: last child record),
 *     its low 3 bits the octant within the parent; word 1 first child record
 *     (index into q->children, 6 bytes each); word 2 first item (index into
 *     q->items, 16 bytes each: unsigned 16-bit box min x y z at +4, max x y z at +0x0A).
 *   Query: +4 callback void(*)(Item *, Query *) (cdecl), +0x0C children, +0x10 items,
 *     +0x2C depth (incremented on entry, decremented on exit; the cube's half size
 *     is 0x10000 >> depth), +0x50 / +0x54 / +0x58 the point (int x y z).
 * Every item of the node whose box contains the point (min <= p <= max, signed
 * compares) goes to the callback; then every child record, in order, whose cube
 * (centre c +- size/4, half size 0x10000 >> depth) contains the point
 * (centre - half <= p < centre + half) is visited, recursively.
 *
 * Decompiled with Ghidra (ReVa) and checked against the x86 code. Every guest
 * memory effect is reproduced: the depth counter (re-read from memory where the
 * guest re-reads it, the callbacks see it incremented), the point re-read for each
 * test, the guest's reuse of its q argument slot (esp+4) for the current child
 * record, and the callbacks called with the guest's arguments, return address,
 * stack depth and registers (ecx, eax, edx, ebx, ebp, esi, edi as the x86 has them
 * at the call). The returned eax / ecx / edx are the x86 code's (eax: 0x8000 after
 * the child loop, ecx: the pushed ecx slot, which the guest overwrites with half
 * the child size). Only the dead stack below esp (its saved registers, its local)
 * is not written. */
#include "recomp.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Out {
    uint32_t eax, ecx, edx;
} Out;

/* esp: the guest entry esp of this frame (return address at esp). */
static void point(Ctx *c, uint32_t node, uint32_t esp, uint32_t ecx_in, uint32_t edx_in, Out *o)
{
    const uint32_t q = RD32(esp + 4u);
    uint32_t edx = edx_in, ecx_out = ecx_in;

    WR32(q + 0x2Cu, RD32(q + 0x2Cu) + 1u);
    uint32_t eax = ((uint32_t)RD16(node) >> 3) & 0xFFFu;

    /* The node's items. */
    if (eax) {
        edx = RD32(q + 0x10u);
        uint32_t item = RD16(node + 4u) * 16u + edx;
        for (uint32_t n = eax; n; n--, item += 16u) {
            eax = RD32(q + 0x50u);
            if ((int32_t)eax < (int32_t)RD16(item + 4u))
                continue;
            edx = RD16(item + 0x0Au);
            if ((int32_t)eax > (int32_t)edx)
                continue;
            eax = RD32(q + 0x54u);
            if ((int32_t)eax < (int32_t)RD16(item + 6u))
                continue;
            edx = RD16(item + 0x0Cu);
            if ((int32_t)eax > (int32_t)edx)
                continue;
            const uint32_t ecx = RD16(item + 8u);
            eax = RD32(q + 0x58u);
            if ((int32_t)eax < (int32_t)ecx)
                continue;
            edx = RD16(item + 0x0Eu);
            if ((int32_t)eax > (int32_t)edx)
                continue;
            /* push edi; push esi; call [edi+4] below the 5 saved registers. */
            const uint32_t sp = esp - 20u - 12u;
            WR32(sp + 8u, q);
            WR32(sp + 4u, item);
            WR32(sp, 0x00616BE1u);
            c->r[2] = edx;
            c->r[3] = node;
            c->r[5] = n;
            c->r[6] = item;
            c->r[7] = q;
            const uint64_t rv = rt_call_indirect(c, RD32(q + 4u), sp, ecx, eax);
            eax = (uint32_t)rv;
            edx = c->r[2];
        }
    }

    /* The child octants containing the point. */
    eax = (eax & 0xFFFF0000u) | RD16(node + 2u);
    if (eax & 0xFFFFu) {
        eax = (eax & 0xFFFFu) * 3u;
        uint32_t rec = RD32(q + 0x0Cu) + eax * 2u;
        WR32(esp + 4u, rec);
        if (rec) {
            const uint32_t s = (uint32_t)((int32_t)0x10000 >> (RD32(q + 0x2Cu) & 31u));
            const uint32_t half = (uint32_t)((int32_t)s >> 1);
            ecx_out = half;
            for (;;) {
                const uint32_t oct = RD8(rec) & 7u;
                /* The centre is re-read from the argument slots each time, as the guest does. */
                const uint32_t x = ((oct & 1u) ? half : 0u - half) + RD32(esp + 8u);
                const uint32_t y = RD32(esp + 12u) + ((oct & 2u) ? half : 0u - half);
                const uint32_t z = RD32(esp + 16u) + ((oct & 4u) ? half : 0u - half);
                edx = y;
                int in = 0;
                eax = RD32(q + 0x50u);
                if ((int32_t)eax < (int32_t)(x + s) && (int32_t)eax >= (int32_t)(x - s)) {
                    eax = RD32(q + 0x54u);
                    if ((int32_t)eax < (int32_t)(y + s) && (int32_t)eax >= (int32_t)(y - s)) {
                        eax = RD32(q + 0x58u);
                        if ((int32_t)eax < (int32_t)(z + s) && (int32_t)eax >= (int32_t)(z - s))
                            in = 1;
                    }
                }
                rec = RD32(esp + 4u);   /* both paths take the record back from the slot */
                if (in) {
                    /* push z; push y; push x; push q; call: the child's entry esp. */
                    const uint32_t ce = esp - 20u - 16u - 4u;
                    WR32(ce + 16u, z);
                    WR32(ce + 12u, y);
                    WR32(ce + 8u, x);
                    WR32(ce + 4u, q);
                    WR32(ce, 0x00616C9Eu);
                    Out co;
                    point(c, rec, ce, rec, y, &co);
                    edx = co.edx;
                }
                const uint32_t w = RD16(rec);
                rec += 6u;
                WR32(esp + 4u, rec);
                eax = w & 0x8000u;
                if (eax)
                    break;
            }
        }
    }

    WR32(q + 0x2Cu, RD32(q + 0x2Cu) - 1u);
    o->eax = eax;
    o->ecx = ecx_out;
    o->edx = edx;
}

static int on = -1;

int rt_native_octree_point(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
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
    if (!on)
        return 0;
    (void)eax;
    const uint32_t ebx = c->r[3], ebp = c->r[5], esi = c->r[6], edi = c->r[7];
    Out o;
    point(c, ecx, esp, ecx, c->r[2], &o);
    c->r[1] = o.ecx;
    c->r[2] = o.edx;
    c->r[3] = ebx;
    c->r[5] = ebp;
    c->r[6] = esi;
    c->r[7] = edi;
    *ret = ((uint64_t)(esp + 4u + 0x10u) << 32) | o.eax;   /* ret 0x10 */
    return 1;
}
