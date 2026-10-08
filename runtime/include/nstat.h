/* Per-frame accounting of the native code and the draw path (ff.cfg "stats=1",
 * FF_STATS=1 on desktop): for each section, calls, items processed, time, and
 * repeats (the same work seen again within the frame: the same mesh skinned or
 * lit twice, the same segment queried twice...). Averages per frame go to the
 * log every 120 frames. Off: one predictable branch per call. */
#ifndef FF_NSTAT_H
#define FF_NSTAT_H

#include <stdint.h>

enum {
    NS_LIGHT,        /* 0x006A5600: items = vertices x lights */
    NS_SKIN,         /* 0x006953F3: items = vertices */
    NS_COLORS_IN,    /* 0x00691410: items = vertices */
    NS_COLORS_OUT,   /* 0x006915B0: items = vertices */
    NS_OCTREE,       /* 0x006167C0 top-level queries: items = nodes visited */
    NS_OCTREE_BOX,   /* item boxes tested (no time) */
    NS_OCTREE_CB,    /* callbacks into the game (no time) */
    NS_RAYBOX,       /* 0x005FF460 calls from the game */
    NS_PARTICLES,    /* 0x00695E50: items = particles */
    NS_RECORD,       /* draws recorded for the render thread: items = draws */
    NS_RECORD_COPY,  /* their vertex copies: items = bytes */
    NS_FRAME,        /* the whole game frame (Present to Present) */
    NS_COUNT
};

extern int g_nstat;   /* stats on */

uint64_t nstat_now(void);   /* microseconds */
void nstat_add(int id, uint32_t calls, uint64_t items, uint64_t us);
/* Repeats: whether key (with id) was already seen this frame (counted). */
int nstat_seen(int id, uint32_t k1, uint32_t k2, uint32_t k3);
void nstat_frame(void);   /* at Present */
void nstat_watch(uint8_t *lo, uint8_t *hi);   /* FF_WATCH builds: the range watched */
/* Guest memory written by the native code (colors_out's lit vertices): a ring of
 * ranges the vertex buffer verifier (shim/src/d3d8.c) checks changes against. */
#define NSTAT_WLOG 65536u
extern uint32_t nstat_wlog_n;   /* ranges logged so far */
extern uint32_t nstat_wlog[NSTAT_WLOG][2];
static inline void nstat_written(uint32_t addr, uint32_t len)
{
    nstat_wlog[nstat_wlog_n % NSTAT_WLOG][0] = addr;
    nstat_wlog[nstat_wlog_n % NSTAT_WLOG][1] = len;
    nstat_wlog_n++;
}

#define NSTAT_BEGIN() uint64_t nstat_t0 = g_nstat ? nstat_now() : 0
#define NSTAT_END(id, items) \
    do { \
        if (g_nstat) \
            nstat_add((id), 1u, (items), nstat_now() - nstat_t0); \
    } while (0)

#endif
