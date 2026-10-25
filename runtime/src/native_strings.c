/* Native versions of the engine's string helpers.
 *
 * 0x005F1C90 wildcard name match (25 callers, 1.3M calls in two C01A scenes:
 * looking objects up by name pattern every frame).
 *   int match(const char *name (ecx), const char *pattern (edx)), no stack
 *   arguments; 0 when the name matches. The pattern is split at '*':
 *   - a leading part before the first '*' must equal the name's start
 *     (_memicmp), the name then goes on after it;
 *   - each "*part*" is searched in the name (_memicmp at every position from the
 *     current one); the name position stays at the match (not after it);
 *   - a last "*part" without a closing '*': the name must be at least as long
 *     and _stricmp(name's tail, part) is the result (1 when the name is shorter);
 *   - no '*' left: _stricmp(pattern, name) is the result; "**" matches anything.
 *   -1 when a part is not found. The C runtime routines are the shim's
 *   (shim/src/msvcrt.c): _stricmp is strcasecmp, _memicmp compares tolower().
 *
 * Decompiled with Ghidra (ReVa), checked against the x86 code; verified with the
 * difftest build (runtime/src/difftest.c). */
#include "recomp.h"
#include "runtime.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

static int memicmp_g(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    int r = 0;
    for (uint32_t i = 0; i < n && !r; i++)
        r = tolower(a[i]) - tolower(b[i]);
    return r;
}

static uint32_t wildcard(uint32_t name, uint32_t pat)
{
    const char *const m = (const char *)g_mem;
    if (!g_mem[pat])
        return 0;
    for (;;) {
        if (g_mem[pat] == '*') {
            const uint32_t sub = pat + 1u;
            const char *close = strchr(m + sub, '*');
            if (!close) {
                const uint32_t ls = (uint32_t)strlen(m + sub), ln = (uint32_t)strlen(m + name);
                if (ln < ls)
                    return 1;
                return (uint32_t)strcasecmp(m + name + (ln - ls), m + sub);
            }
            const uint32_t size = (uint32_t)(close - m) - sub;
            if (!size)
                return 0;
            const int32_t span = (int32_t)((uint32_t)strlen(m + name) - size);
            if (span < 0)
                return 0xFFFFFFFFu;
            const uint32_t tries = (uint32_t)span + 1u;
            uint32_t i = 0;
            while (memicmp_g(g_mem + sub, g_mem + name, size)) {
                name++;
                if (++i == tries)
                    return 0xFFFFFFFFu;
            }
            pat = (uint32_t)(close - m);
        } else {
            const char *star = strchr(m + pat, '*');
            if (!star)
                return (uint32_t)strcasecmp(m + pat, m + name);
            const uint32_t len = (uint32_t)(star - m) - pat;
            if (memicmp_g(g_mem + pat, g_mem + name, len))
                return 0xFFFFFFFFu;
            name += len;
            pat = (uint32_t)(star - m);
        }
    }
}

int rt_native_wildcard(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)eax;
    const uint32_t r = wildcard(ecx, c->r[2]);
    *ret = ((uint64_t)(esp + 4u) << 32) | r;
    return 1;
}
