/* Generic check of the native functions (FF_DIFFTEST builds): a sampled call of a
 * function with a native version runs both versions on the same state and
 * compares what they leave behind.
 *
 *   1. The recompiled function runs first, its stores journaled (old bytes, by
 *      rt_st* / rep movs / rep stos through RT_WATCH).
 *   2. Its result is recorded (registers, the final bytes of every address it
 *      wrote), the journal is undone and the context put back.
 *   3. The native version runs on that state; its result is kept.
 *   4. Compared: eax and esp returned, the callee-saved registers, the x87 stack
 *      top (and the value on it when the function leaves one), and every byte the
 *      recompiled side wrote except the dead stack below esp. Float words that
 *      differ in the last bits (the native code rounds in single precision, the
 *      recompiled x87 in double) are counted apart.
 *
 * Not checked: a call whose recompiled side made a host call (an import or COM
 * method: host writes are not journaled), which keeps the recompiled result; and
 * stores the native version makes where the recompiled one made none.
 *
 * ff.cfg (Vita) or the environment (desktop): difftest_first=N (default 50 checks
 * of each function), difftest_every=N (then one call in N, default 500). Results go
 * to the log: every mismatch (the first ones per function) and a summary per
 * function every 2000 checks. */
#include "recomp.h"
#include "runtime.h"

#if defined(FF_DIFFTEST)

#include <math.h>
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#else
#include <pthread.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (__thread is not per thread on the Vita: the thread is compared explicitly.) */
int rt_jn_on;
uint32_t rt_dt_imports;

static uintptr_t self_id(void)
{
#if defined(__vita__)
    return (uintptr_t)sceKernelGetThreadId();
#else
    return (uintptr_t)pthread_self();
#endif
}
static uintptr_t main_thread, jn_thread;

int rt_dt_hostcall(void)
{
    if (self_id() == jn_thread)
        rt_dt_imports++;
    return 1;
}

/* ---- journal ------------------------------------------------------------- */

typedef struct JEntry {
    uint32_t addr, n, off;   /* guest address, length, offset of its old bytes in jbytes */
} JEntry;
static JEntry *jent;
static uint32_t jcount, jcap;
static uint8_t *jbytes;
static uint32_t jb, jbcap;
static int joverflow;

void rt_journal(const uint8_t *p, unsigned n)
{
    if (p < g_mem || p + n > g_mem + g_guest_mem_size || self_id() != jn_thread)
        return;   /* host memory written through the same helpers, or another thread */
    if (jcount == jcap) {
        uint32_t nc = jcap ? jcap * 2u : 4096u;
        JEntry *ne = realloc(jent, nc * sizeof(JEntry));
        if (!ne) {
            joverflow = 1;
            return;
        }
        jent = ne;
        jcap = nc;
    }
    if (jb + n > jbcap) {
        uint32_t nc = jbcap ? jbcap * 2u : 65536u;
        while (nc < jb + n)
            nc *= 2u;
        uint8_t *nb = realloc(jbytes, nc);
        if (!nb) {
            joverflow = 1;
            return;
        }
        jbytes = nb;
        jbcap = nc;
    }
    jent[jcount].addr = (uint32_t)(p - g_mem);
    jent[jcount].n = n;
    jent[jcount].off = jb;
    memcpy(jbytes + jb, p, n);
    jb += n;
    jcount++;
}

/* ---- final bytes of the recompiled side (address -> byte) ---------------- */

typedef struct FByte {
    uint32_t addr;   /* 0: empty slot */
    uint8_t v;
} FByte;
static FByte *fb;
static uint32_t fbmask;

static void fb_reset(uint32_t need)
{
    uint32_t size = 1024u;
    while (size < need * 2u)
        size *= 2u;
    if (size - 1u != fbmask) {
        free(fb);
        fb = malloc(size * sizeof(FByte));
        fbmask = size - 1u;
    }
    memset(fb, 0, (fbmask + 1u) * sizeof(FByte));
}

static FByte *fb_slot(uint32_t a)
{
    uint32_t i = (a * 0x9E3779B1u) & fbmask;
    while (fb[i].addr && fb[i].addr != a)
        i = (i + 1u) & fbmask;
    return &fb[i];
}

/* ---- per-function statistics ----------------------------------------------- */

typedef struct FnStat {
    uint32_t addr;
    uint32_t calls, checked, bad, rounding, hostcalls, declined, logged, eax_only, argslot;
} FnStat;
#define NSTAT 256u
static FnStat stats[NSTAT];

static FnStat *stat_of(uint32_t addr)
{
    uint32_t i = (addr * 0x9E3779B1u) >> 24;
    for (uint32_t k = 0; k < NSTAT; k++, i = (i + 1u) & (NSTAT - 1u)) {
        if (stats[i].addr == addr)
            return &stats[i];
        if (!stats[i].addr) {
            stats[i].addr = addr;
            return &stats[i];
        }
    }
    return &stats[0];
}

static uint32_t cfg_first = 50u, cfg_every = 500u;

static void read_cfg(void)
{
    static int done;
    if (done)
        return;
    done = 1;
#if defined(__vita__)
    FILE *f = fopen("ux0:data/ff/ff.cfg", "r");
    if (f) {
        char buf[128];
        while (fgets(buf, sizeof(buf), f)) {
            if (!strncmp(buf, "difftest_first=", 15))
                cfg_first = (uint32_t)strtoul(buf + 15, NULL, 10);
            else if (!strncmp(buf, "difftest_every=", 15))
                cfg_every = (uint32_t)strtoul(buf + 15, NULL, 10);
        }
        fclose(f);
    }
#else
    const char *a = getenv("FF_DIFFTEST_FIRST"), *b = getenv("FF_DIFFTEST_EVERY");
    if (a)
        cfg_first = (uint32_t)strtoul(a, NULL, 10);
    if (b)
        cfg_every = (uint32_t)strtoul(b, NULL, 10);
#endif
    if (!cfg_every)
        cfg_every = 1u;
    rt_log("difftest: every native function checked on its first %u calls, then one call in %u", cfg_first,
           cfg_every);
}

/* Two float words within a few units in the last place (same sign). */
static int near_float(uint32_t x, uint32_t y)
{
    float a, b;
    memcpy(&a, &x, 4);
    memcpy(&b, &y, 4);
    if (isnan(a) || isnan(b))
        return 0;
    if ((x ^ y) & 0x80000000u)
        return fabsf(a - b) <= 1e-5f;
    uint32_t d = x > y ? x - y : y - x;
    /* (also two values near zero: a cancellation the double-precision side rounds differently) */
    return d <= 32u || fabsf(a - b) <= 1e-5f;   /* single precision vs the recompiled x87's double: a few roundings apart */
}

static void summary(const FnStat *s)
{
    rt_log("difftest %08X: %u calls, %u checked: %u differ, %u float roundings only, %u eax only (void?), %u "
           "argument slots only; %u not checked (host calls), %u declined",
           s->addr, s->calls, s->checked, s->bad, s->rounding, s->eax_only, s->argslot, s->hostcalls, s->declined);
}

/* The address whose next entry hook must decline (the recompiled leg's own entry). */
static uint32_t skip_addr;
static int busy;

int rt_difftest(uint32_t addr, RtNativeFn fn, Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    /* Checks on the game's main thread only (the first to get here). */
    if (!main_thread)
        main_thread = self_id();
    if (self_id() != main_thread)
        return fn(c, esp, ecx, eax, ret);
    if (skip_addr == addr) {
        skip_addr = 0;
        return 0;   /* run the recompiled body */
    }
    if (busy) {
        /* Inside a check: in the recompiled leg nested natives decline (their stores
         * must be journaled too); in the native leg they run. */
        return rt_jn_on ? 0 : fn(c, esp, ecx, eax, ret);
    }
    read_cfg();
    FnStat *s = stat_of(addr);
    s->calls++;
    if (!(s->calls <= cfg_first || s->calls % cfg_every == 0u))
        return fn(c, esp, ecx, eax, ret);
    busy = 1;

    /* 1. Recompiled, journaled. */
    const Ctx c0 = *c;
    jcount = jb = 0;
    joverflow = 0;
    const uint32_t imports0 = rt_dt_imports;
    skip_addr = addr;
    jn_thread = self_id();
    rt_jn_on = 1;
    const uint64_t rr = rt_call_indirect(c, addr, esp, ecx, eax);
    rt_jn_on = 0;
    skip_addr = 0;
    const Ctx cr = *c;
    if (rt_dt_imports != imports0 || joverflow) {
        /* The recompiled result stands (nothing undone). */
        s->hostcalls++;
        *ret = rr;
        busy = 0;
        return 1;
    }

    /* 2. Its final bytes, then undo. */
    fb_reset(jb + 16u);
    for (uint32_t i = 0; i < jcount; i++)
        for (uint32_t k = 0; k < jent[i].n; k++) {
            FByte *f = fb_slot(jent[i].addr + k);
            f->addr = jent[i].addr + k;
        }
    for (uint32_t i = 0; i <= fbmask; i++)
        if (fb[i].addr)
            fb[i].v = g_mem[fb[i].addr];
    for (uint32_t i = jcount; i-- > 0;)
        memcpy(g_mem + jent[i].addr, jbytes + jent[i].off, jent[i].n);
    *c = c0;

    /* 3. Native. */
    uint64_t rn;
    if (!fn(c, esp, ecx, eax, &rn)) {
        /* Declined: put the recompiled result back. */
        for (uint32_t i = 0; i <= fbmask; i++)
            if (fb[i].addr)
                g_mem[fb[i].addr] = fb[i].v;
        *c = cr;
        *ret = rr;
        s->declined++;
        busy = 0;
        return 1;
    }
    *ret = rn;

    /* 4. Compare. */
    s->checked++;
    char what[160];
    int bad = 0, rounding = 0;
    what[0] = 0;
    /* eax alone may be a void function's leftover; argument slots (MSVC code reuses
     * them as locals, the caller never reads them back) are reported apart. */
    int eax_diff = 0, arg_diff = 0;
    if ((uint32_t)(rn >> 32) != (uint32_t)(rr >> 32)) {
        bad = 1;
        snprintf(what, sizeof(what), "returned esp %08X, recompiled %08X", (uint32_t)(rn >> 32),
                 (uint32_t)(rr >> 32));
    } else if ((uint32_t)rn != (uint32_t)rr) {
        eax_diff = 1;
        snprintf(what, sizeof(what), "returned eax %08X, recompiled %08X", (uint32_t)rn, (uint32_t)rr);
    }
    const uint32_t new_esp = (uint32_t)(rr >> 32), args_hi = new_esp + 0x40u;
    static const int saved[] = { 3, 5, 6, 7 };
    static const char *const rname[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    for (int k = 0; k < 4 && !bad; k++)
        if (c->r[saved[k]] != cr.r[saved[k]]) {
            bad = 1;
            snprintf(what, sizeof(what), "%s %08X, recompiled %08X", rname[saved[k]], c->r[saved[k]],
                     cr.r[saved[k]]);
        }
    if (!bad && c->ftop != cr.ftop) {
        bad = 1;
        snprintf(what, sizeof(what), "x87 top %u, recompiled %u", c->ftop, cr.ftop);
    }
    if (!bad && cr.ftop != c0.ftop) {
        /* A value left on the x87 stack (float return). */
        const double a = c->st[c->ftop & 7u], b = cr.st[cr.ftop & 7u];
        if (a != b) {
            const float fa = (float)a, fb2 = (float)b;
            uint32_t x, y;
            memcpy(&x, &fa, 4);
            memcpy(&y, &fb2, 4);
            if (near_float(x, y)) {
                rounding = 1;
            } else {
                bad = 1;
                snprintf(what, sizeof(what), "st0 %.9g, recompiled %.9g", a, b);
            }
        }
    }
    /* Memory: by aligned word, so float roundings are recognised. */
    const uint32_t dead_lo = esp - 0x10000u;
    for (uint32_t i = 0; i <= fbmask && !bad; i++) {
        const uint32_t a = fb[i].addr;
        if (!a || (a >= dead_lo && a < new_esp) || g_mem[a] == fb[i].v)
            continue;
        if (a >= new_esp && a < args_hi) {
            arg_diff = 1;
            continue;
        }
        const uint32_t w = a & ~3u;
        uint32_t want = 0, have;
        int whole = 1;
        for (uint32_t k = 0; k < 4; k++) {
            FByte *f = fb_slot(w + k);
            if (f->addr != w + k) {
                whole = 0;
                break;
            }
            want |= (uint32_t)f->v << (8u * k);
        }
        memcpy(&have, g_mem + w, 4);
        if (whole && near_float(have, want)) {
            rounding = 1;
            continue;
        }
        bad = 1;
        snprintf(what, sizeof(what), "memory %08X: %02X, recompiled %02X (word %08X, recompiled %08X%s)", a,
                 g_mem[a], fb[i].v, have, whole ? want : 0u, whole ? "" : ", partly written");
    }
    if (!bad && eax_diff) {
        s->eax_only++;
        if (s->logged++ < 10u)
            rt_log("difftest %08X: call %u: %s (memory and other registers equal)", addr, s->calls, what);
    } else if (!bad && arg_diff) {
        s->argslot++;
    }
    if (bad) {
        s->bad++;
        if (s->logged++ < 10u)
            rt_log("difftest %08X: call %u (ecx %08X, stack args %08X %08X %08X) differs: %s", addr, s->calls,
                   ecx, RD32(esp + 4u), RD32(esp + 8u), RD32(esp + 12u), what);
    } else if (rounding) {
        s->rounding++;
    }
    if (s->checked % 2000u == 0u)
        summary(s);
    static uint32_t all_checked;
    if (++all_checked % 5000u == 0u)
        for (uint32_t i = 0; i < NSTAT; i++)
            if (stats[i].addr && stats[i].checked)
                summary(&stats[i]);
    busy = 0;
    return 1;
}
#endif /* FF_DIFFTEST */
