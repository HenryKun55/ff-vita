/* Per-frame accounting of the native code and the draw path (runtime/include/nstat.h). */
#include "nstat.h"
#include "runtime.h"
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#endif

int g_nstat;
uint32_t nstat_wlog_n;
uint32_t nstat_wlog[NSTAT_WLOG][2];
static int inited;

static const char *const names[NS_COUNT] = {
    "light (verts x lights)", "skin (verts)", "colors in (verts)", "colors out (verts)",
    "octree queries (nodes)", "  octree item boxes", "  octree callbacks", "ray-box calls",
    "particles", "draws recorded", "  draw vertex copies (bytes)", "game frame",
};

typedef struct Acc {
    uint64_t calls, items, us, repeats, repeat_items;
} Acc;

static Acc acc[NS_COUNT];
static uint32_t frames, serial = 1;

/* Keys seen this frame: open addressing, stamped with the frame serial. */
#define SEEN_SIZE 8192u
static struct {
    uint32_t stamp, id, k1, k2, k3;
} seen[SEEN_SIZE];

uint64_t nstat_now(void)
{
#if defined(__vita__)
    return sceKernelGetProcessTimeWide();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

static void nstat_init(void)
{
    g_nstat = 0;
#if defined(__vita__)
    FILE *f = fopen("ux0:data/ff/ff.cfg", "r");
    if (f) {
        char buf[128];
        while (fgets(buf, sizeof(buf), f))
            if (!strncmp(buf, "stats=", 6) && buf[6] >= '1' && buf[6] <= '9')
                g_nstat = buf[6] - '0';
        fclose(f);
    }
#else
    const char *e = getenv("FF_STATS");
    g_nstat = e && e[0] >= '1' && e[0] <= '9' ? e[0] - '0' : 0;
#endif
    if (g_nstat)
        rt_log("stats: on (per-frame averages every 120 frames)");
}

void nstat_add(int id, uint32_t calls, uint64_t items, uint64_t us)
{
    acc[id].calls += calls;
    acc[id].items += items;
    acc[id].us += us;
}

int nstat_seen(int id, uint32_t k1, uint32_t k2, uint32_t k3)
{
    uint32_t h = ((uint32_t)id * 0x9E3779B1u ^ k1 * 0x85EBCA6Bu ^ k2 * 0xC2B2AE35u ^ k3 * 0x27D4EB2Fu);
    h ^= h >> 15;
    for (uint32_t n = 0; n < 32u; n++, h++) {
        uint32_t i = h & (SEEN_SIZE - 1u);
        if (seen[i].stamp != serial) {
            seen[i].stamp = serial;
            seen[i].id = (uint32_t)id;
            seen[i].k1 = k1;
            seen[i].k2 = k2;
            seen[i].k3 = k3;
            return 0;
        }
        if (seen[i].id == (uint32_t)id && seen[i].k1 == k1 && seen[i].k2 == k2 && seen[i].k3 == k3) {
            acc[id].repeats++;
            return 1;
        }
    }
    return 0;   /* table crowded: not counted */
}

#if defined(FF_WATCH)
uint8_t *volatile rt_watch_lo, *volatile rt_watch_hi;
static struct {
    uint32_t fn, writes, changes;
} whist[64];

void rt_watch_hit(const uint8_t *p, const void *v, unsigned n)
{
    const uint32_t fn = rt_prof_cur;
    const int changed = memcmp(p, v, n) != 0;
    for (int i = 0; i < 64; i++)
        if (whist[i].fn == fn || !whist[i].fn) {
            whist[i].fn = fn;
            whist[i].writes++;
            whist[i].changes += (uint32_t)changed;
            return;
        }
}

void nstat_watch(uint8_t *lo, uint8_t *hi)
{
    rt_watch_lo = lo;
    rt_watch_hi = hi;
}

static void watch_report(void)
{
    for (int i = 0; i < 64 && whist[i].fn; i++)
        rt_log("stats:   watch: guest %08X wrote %u times into the buffer, %u changing it", whist[i].fn,
               whist[i].writes, whist[i].changes);
    memset(whist, 0, sizeof(whist));
}
#else
void nstat_watch(uint8_t *lo, uint8_t *hi)
{
    (void)lo;
    (void)hi;
}

static void watch_report(void) {}
#endif

void nstat_frame(void)
{
    static uint64_t last;
    if (!inited) {
        inited = 1;
        nstat_init();
    }
    if (!g_nstat)
        return;
    uint64_t now = nstat_now();
    if (last)
        nstat_add(NS_FRAME, 1u, 0, now - last);
    last = now;
    serial++;
    if (++frames < 120u)
        return;
    const double f = (double)frames;
    rt_log("stats: per frame over %u frames (%.1f ms a frame):", frames, (double)acc[NS_FRAME].us / 1000.0 / f);
    for (int i = 0; i < NS_COUNT - 1; i++) {
        const Acc *a = &acc[i];
        if (!a->calls && !a->items)
            continue;
        rt_log("stats:   %-28s %8.1f calls %10.0f items %7.2f ms%s%.1f repeats", names[i], (double)a->calls / f,
               (double)a->items / f, (double)a->us / 1000.0 / f, a->repeats ? "  " : "  ", (double)a->repeats / f);
    }
    watch_report();
    memset(acc, 0, sizeof(acc));
    frames = 0;
}
