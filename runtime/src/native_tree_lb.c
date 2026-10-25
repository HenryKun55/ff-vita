/* Native version of a binary search tree lookup: the first node whose key is
 * at least a given key (lower bound).
 *
 * 0x00622850 (251k calls in two scenes): thiscall on the tree (ecx), stack
 * arguments (int key, Node *node); ret 8. Node: +4 left, +8 right, +0x0C key
 * (signed). Tree: +0 vtable, +4 root, +8 a node that is never returned (the
 * sentinel). node 0 means the root; 0 when both are 0.
 *   - node key > key: look in the left subtree through the vtable's +0x0C entry
 *     (this function in every vtable that holds it); its result r is returned
 *     when r != 0 and key <= r's key; otherwise node is the answer (0 when it is
 *     the sentinel);
 *   - node key == key: node (0 when it is the sentinel);
 *   - node key < key: the right subtree's result when r != 0 and key <= r's key,
 *     otherwise 0.
 * The recursion stays native while the vtable entry is this function; another
 * entry is called as the guest calls it. ecx and edx are left as the x86 code
 * leaves them.
 *
 * Decompiled with Ghidra (ReVa), checked against the x86 code. */
#include "recomp.h"
#include "runtime.h"

#define SELF 0x00622850u

static uint32_t lower_bound(Ctx *c, uint32_t esp, uint32_t self, uint32_t key, uint32_t node,
                            uint32_t *ecx, uint32_t *edx);

/* The call through [vtable + 0x0C] as the guest makes it (args at sp + 4).
 * Left subtree: eax = the child, edx = the vtable, edi = the node.
 * Right subtree: eax = the vtable, edx unchanged, edi = the child. */
static uint32_t vcall(Ctx *c, uint32_t esp, uint32_t self, uint32_t key, uint32_t child,
                      uint32_t node, int right, uint32_t *ecx, uint32_t *edx)
{
    const uint32_t vt = RD32(self);
    if (!right)
        *edx = vt;
    const uint32_t fn = RD32(vt + 0x0Cu);
    const uint32_t sp = esp - 24u;   /* 3 saved registers, 2 arguments, return address */
    *ecx = self;
    if (fn == SELF)
        return lower_bound(c, sp, self, key, child, ecx, edx);
    const uint32_t ebx = c->r[3], esi = c->r[6], edi = c->r[7];
    WR32(sp + 8u, child);
    WR32(sp + 4u, key);
    WR32(sp, right ? 0x006228B5u : 0x00622887u);
    c->r[2] = *edx;
    c->r[3] = key;
    c->r[6] = self;
    c->r[7] = right ? child : node;
    const uint32_t r = (uint32_t)rt_call_indirect(c, fn, sp, self, right ? vt : child);
    *ecx = c->r[1];
    *edx = c->r[2];
    c->r[3] = ebx;
    c->r[6] = esi;
    c->r[7] = edi;
    return r;
}

static uint32_t lower_bound(Ctx *c, uint32_t esp, uint32_t self, uint32_t key, uint32_t node,
                            uint32_t *ecx, uint32_t *edx)
{
    if (!node) {
        node = RD32(self + 4u);
        if (!node)
            return 0;
    }
    const int32_t nk = (int32_t)RD32(node + 0x0Cu);
    if (nk > (int32_t)key) {
        const uint32_t left = RD32(node + 4u);
        if (left) {
            const uint32_t r = vcall(c, esp, self, key, left, node, 0, ecx, edx);
            if (r && (int32_t)key <= (int32_t)RD32(r + 0x0Cu))
                return r;
        }
    } else if (nk != (int32_t)key) {
        const uint32_t right = RD32(node + 8u);
        if (right) {
            const uint32_t r = vcall(c, esp, self, key, right, node, 1, ecx, edx);
            if (r && (int32_t)key <= (int32_t)RD32(r + 0x0Cu))
                return r;
        }
        return 0;
    }
    return node == RD32(self + 8u) ? 0 : node;
}

int rt_native_tree_lb(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)eax;
    uint32_t e_cx = ecx, e_dx = c->r[2];
    const uint32_t r = lower_bound(c, esp, ecx, RD32(esp + 4u), RD32(esp + 8u), &e_cx, &e_dx);
    c->r[1] = e_cx;
    c->r[2] = e_dx;
    *ret = ((uint64_t)(esp + 12u) << 32) | r;
    return 1;
}
