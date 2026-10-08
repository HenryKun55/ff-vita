/* Native version of the engine's segment query through its collision octree
 * (0x006167C0, about 8% of the game thread), with its priority queue helpers
 * (0x006162C0 push, 0x006163C0 pop, 0x00616320 sift down).
 *
 * bool __thiscall Node::segment(Query *q, int size, int cx, int cy, int cz)
 *   Node (6 bytes, ecx): word 0 bits 3..14 item count (bit 15: last child record),
 *     its low 3 bits the octant within the parent; word 1 first child record
 *     (index into q->children, 6 bytes each); word 2 first item (index into
 *     q->items, 16 bytes each: integer box min x y z, max x y z at +4).
 *   Query: +0 float best t so far, +4 callback bool(*)(Item *, Query *) (cdecl),
 *     +0x0C children, +0x10 items, +0x30 / +0x34 pointers to the segment's ends.
 * Every item whose box the segment hits nearer than q->t goes to the callback (which
 * may lower q->t); then the child octants the segment hits are visited nearest first,
 * until one starts beyond q->t. Returns whether any callback returned true.
 *
 * The child octants are queued exactly as the guest's heap does (same order for
 * equal distances), the callbacks get the same arguments in the same order, and
 * the guest stack below the entry esp is used as the guest's frames would be. */
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

typedef struct Entry {
    float key;
    uint32_t node;
    uint32_t oct;
} Entry;

/* 0x006162C0: insert into the heap h[0..n) (parent of i is i >> 1). */
static void heap_push(Entry *h, int n, const Entry *e)
{
    int i = n;
    while (i > 0) {
        int p = i >> 1;
        if (!(h[p].key > e->key))
            break;
        h[i] = h[p];
        i = p;
    }
    h[i] = *e;
}

/* 0x00616320 on b = h - 1 (1-based), m elements, from i. */
static void heap_sift(Entry *h, int m, int i)
{
    for (;;) {
        int cand = (2 * i + 1 <= m && h[2 * i].key < h[i - 1].key) ? 2 * i + 1 : i;
        if (2 * i <= m && h[2 * i - 1].key < h[cand - 1].key)
            cand = 2 * i;
        if (cand == i)
            return;
        Entry t = h[i - 1];
        h[i - 1] = h[cand - 1];
        h[cand - 1] = t;
        i = cand;
    }
}

/* 0x006163C0: remove the first of n. */
static Entry heap_pop(Entry *h, int n)
{
    Entry top = h[0];
    h[0] = h[n - 1];
    heap_sift(h, n - 1, 1);
    return top;
}

/* The callback, called as the guest calls it (cdecl, item then query). */
static uint32_t callback(Ctx *c, uint32_t q, uint32_t item, uint32_t esp)
{
    uint32_t sp = esp - 0xB0u - 8u;   /* the guest frame's depth: 0xA0 + 4 registers + 2 arguments */
    WR32(sp, item);
    WR32(sp + 4u, q);
    sp -= 4u;
    WR32(sp, 0x006168CFu);            /* its return address in the guest */
    uint64_t r = rt_call_indirect(c, RD32(q + 4u), sp, 0, 0);
    return (uint32_t)r & 0xFFu;
}

static int depth_guard;

static uint32_t segment(Ctx *c, uint32_t node, uint32_t q, int32_t size, int32_t cx, int32_t cy, int32_t cz,
                        uint32_t esp)
{
    const uint32_t s = RD32(q + 0x30u), e = RD32(q + 0x34u);
    const float o[3] = { ldf(s), ldf(s + 4u), ldf(s + 8u) };
    const float d[3] = { ldf(e) - o[0], ldf(e + 4u) - o[1], ldf(e + 8u) - o[2] };
    uint32_t flag = 0;

    /* The node's own items. */
    const uint32_t count = (RD16(node) >> 3) & 0xFFFu;
    if (count) {
        uint32_t item = RD16(node + 4u) * 16u + RD32(q + 0x10u);
        for (uint32_t k = 0; k < count; k++, item += 16u) {
            float box[6];
            for (int j = 0; j < 6; j++)
                box[j] = (float)RD16(item + 4u + 2u * (uint32_t)j);
            float t = -1.0f;
            if (ff_segment_box(box, o, d, &t, NULL) && ldf(q) > t)
                flag |= callback(c, q, item, esp);
        }
    }

    /* The child octants the segment crosses, nearest first. */
    Entry heap[64];
    int n = 0;
    const uint32_t first = RD16(node + 2u);
    const int32_t half = size >> 1, quarter = size >> 2;
    if (first) {
        uint32_t rec = RD32(q + 0x0Cu) + first * 6u;
        if (rec) {
            for (;;) {
                const uint32_t oct = RD8(rec) & 7u;
                const int32_t x = (oct & 1u) ? cx + quarter : cx - quarter;
                const int32_t y = (oct & 2u) ? cy + quarter : cy - quarter;
                const int32_t z = (oct & 4u) ? cz + quarter : cz - quarter;
                const float box[6] = { (float)(x - half), (float)(y - half), (float)(z - half),
                                       (float)(x + half), (float)(y + half), (float)(z + half) };
                float t = -1.0f;
                if (ff_segment_box(box, o, d, &t, NULL) && n < 64) {
                    const Entry en = { t, rec, oct };
                    heap_push(heap, n, &en);
                    n++;
                }
                const uint32_t last = RD16(rec) & 0x8000u;
                rec += 6u;
                if (last)
                    break;
            }
        }
    }
    for (int i = 0, left = n; i < n; i++, left--) {
        const Entry top = heap_pop(heap, left);
        if (!(ldf(q) > top.key))
            break;
        const int32_t x = (top.oct & 1u) ? cx + quarter : cx - quarter;
        const int32_t y = (top.oct & 2u) ? cy + quarter : cy - quarter;
        const int32_t z = (top.oct & 4u) ? cz + quarter : cz - quarter;
        /* The child's entry esp: below this frame, its 5 arguments and return address. */
        flag |= segment(c, top.node, q, half, x, y, z, esp - 0xB0u - 0x14u - 4u);
    }
    return flag;
}

static int on = -1;

int rt_native_octree_segment(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
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
    if (!on || depth_guard)
        return 0;
    depth_guard = 1;
    const uint32_t flag = segment(c, ecx, RD32(esp + 4u), (int32_t)RD32(esp + 8u), (int32_t)RD32(esp + 12u),
                                  (int32_t)RD32(esp + 16u), (int32_t)RD32(esp + 20u), esp);
    depth_guard = 0;
    (void)eax;
    *ret = ((uint64_t)(esp + 4u + 0x14u) << 32) | flag;
    return 1;
}
