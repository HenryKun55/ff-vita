/* Native version of the engine's geometry reference resolver, GeomRefToPtr
 * (0x0067EA90, ~624k calls per 30 s of play; called by 0x0067EB30 and 0x00674563).
 *
 * void *__thiscall GeomTable::RefToPtr(uint32_t ref)   (ret 4)
 *   ref == 0: returns 0.
 *   ref bit 30 set: an offset h = ref & 0x3FFFFFFF into the geometry block
 *     (this+0x14)->+4. Valid when h < this+0x68 and h is 16-byte aligned: returns
 *     block + h, or 0 when the dword at block + h + 0x3C has bit 26 (0x04000000).
 *     An invalid offset logs "ERROR: GeomRefToPtr received ..." through the
 *     engine's log object ([0x78D0A4] vtable +0x20, then +0x2C) and returns 0:
 *     that path is declined here (no stores happen before it, the recompiled body
 *     runs it).
 *   otherwise: a handle. The handle table this+8 is asked to validate it (its
 *     virtual +0x14, thiscall, one argument, ret 4: called as the guest calls it,
 *     same stack slots, registers and return address); when it returns al == 0
 *     the result is 0, else (this+0x0C)[ref & table+0x10] (dword entries).
 *
 * Decompiled with Ghidra (ReVa) and checked against the x86 code: eax on every
 * path is exactly the guest's (a full dword: xor eax,eax / the pointer / the
 * table entry). ebx and esi are given the guest's values (ref, this) around the
 * call and put back; edx at the call is the table's vtable pointer as in the
 * guest. edx/ecx on return are not reproduced (caller-saved). */
#include "recomp.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int rt_native_geomref_to_ptr(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    if (on < 0)
        config();
    if (!on)
        return 0;
    const uint32_t self = ecx;
    const uint32_t ref = RD32(esp + 4u);
    uint32_t r;
    if (ref == 0u) {
        r = 0;
    } else if (ref & 0x40000000u) {
        const uint32_t h = ref & 0x3FFFFFFFu;
        if (!(RD32(self + 0x68u) > h) || (h & 0xFu))
            return 0;   /* the error log path: the recompiled body runs it */
        const uint32_t base = RD32(RD32(self + 0x14u) + 4u);
        r = (RD32(base + h + 0x3Cu) & 0x04000000u) ? 0u : base + h;
    } else {
        /* push ebx; push esi; push ref; call [vtable + 0x14] (returns to 0x0067EB0C). */
        const uint32_t ebx0 = c->r[3], esi0 = c->r[6];
        const uint32_t table = RD32(self + 8u);
        const uint32_t vt = RD32(table);
        WR32(esp - 4u, ebx0);
        WR32(esp - 8u, esi0);
        WR32(esp - 12u, ref);
        WR32(esp - 16u, 0x0067EB0Cu);
        c->r[2] = vt;
        c->r[3] = ref;
        c->r[6] = self;
        const uint32_t ok = (uint32_t)rt_call_indirect(c, RD32(vt + 0x14u), esp - 16u, table, eax);
        c->r[3] = ebx0;
        c->r[6] = esi0;
        if (!(ok & 0xFFu)) {
            r = 0;
        } else {
            const uint32_t idx = RD32(RD32(self + 8u) + 0x10u) & ref;
            r = RD32(RD32(self + 0x0Cu) + idx * 4u);
        }
    }
    *ret = ((uint64_t)(esp + 4u + 4u) << 32) | r;   /* thiscall, ret 4 */
    return 1;
}
