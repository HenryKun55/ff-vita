/* Native version of the engine's block list iterator step.
 *
 * 0x005F37C0 (1.3M calls in two scenes): thiscall on a block list (ecx), one
 * stack argument, the iterator {+0 block, +4 index}; ret 4. Returns a pointer
 * to the current element and advances the iterator, or 0 at the end:
 *   - a list whose +0x0C is negative logs an error through the object at
 *     [0x0078D0A4] (and may break): declined, the recompiled body handles it;
 *   - index == list+0x10 (elements per block): go to the next block
 *     (block+4), index = 0;
 *   - no block: return 0;
 *   - the last block (block+4 == 0) with index == [list+8]+8 (the count used in
 *     it): the iterator's block becomes 0, return 0;
 *   - otherwise: index += list+0x18 (stride in dwords), return
 *     block + 0x10 + old index * 4.
 * Leaves ecx = the iterator and edx = the index (or the next block), as the
 * x86 code does.
 *
 * Decompiled with Ghidra (ReVa), checked against the x86 code. */
#include "recomp.h"
#include "runtime.h"

int rt_native_block_iter(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)eax;
    const uint32_t list = ecx;
    if ((int32_t)RD32(list + 0x0Cu) < 0)
        return 0;
    const uint32_t it = RD32(esp + 4u);
    uint32_t edx = RD32(it + 4u);
    if (edx == RD32(list + 0x10u)) {
        const uint32_t blk = RD32(it);
        WR32(it + 4u, 0);
        edx = RD32(blk + 4u);
        WR32(it, edx);
    }
    uint32_t r = RD32(it);
    if (r) {
        const uint32_t used = RD32(list + 8u);
        edx = RD32(it + 4u);
        if (edx == RD32(used + 8u) && RD32(r + 4u) == 0) {
            WR32(it, 0);
            r = 0;
        } else {
            WR32(it + 4u, RD32(list + 0x18u) + edx);
            r = r + edx * 4u + 0x10u;
        }
    }
    c->r[1] = it;
    c->r[2] = edx;
    *ret = ((uint64_t)(esp + 8u) << 32) | r;
    return 1;
}
