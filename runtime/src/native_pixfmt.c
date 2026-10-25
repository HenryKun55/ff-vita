/* Native versions of the engine's pixel format helpers (texture upload / colour
 * conversion to the device format).
 *
 * A pixel format descriptor (PixFmt, 0x60 bytes) holds, for the 4 channels
 * r, g, b, a (index i = 0..3):
 *   +0x00 + 4i  mask           (channel bits in the packed pixel)
 *   +0x10 + 4i  shift          (position of the mask's lowest bit)
 *   +0x20 + 4i  pad            (8 - number of bits from the lowest set bit up)
 * The conversion routine reads two of them laid out back to back: the source
 * format at +0x00 and the destination format at +0x30 (so +0x40/+0x50 are the
 * destination's shift/pad).
 *
 * 0x00618660 pixfmt_init(PixFmt *f) (stdcall, ret 4; ecx is not an input).
 *   387k calls in two scenes: twice per colour converted by 0x006183D0 and once
 *   per texture conversion in 0x006C94C0 / 0x006C9D70. For each channel:
 *   shift = 0, pad = 8; if the mask is non-zero, shift counts its trailing zero
 *   bits and pad = 8 - (bits from the lowest set bit to the highest).
 *   Leaves eax = 0, ecx = f + 0x30, edx = pad of the last non-zero channel
 *   (unchanged when every mask is 0).
 *
 * 0x006185C0 pixfmt_convert(this = PixFmt pair (ecx), uint32_t pixel,
 *   bool invert_alpha) (thiscall, ret 8). 2.5M calls in two scenes. Each
 *   channel: ((pixel & src.mask) >> src.shift << src.pad) >> dst.pad
 *   << dst.shift, the alpha one replaced by 0xFF - (... << src.pad) before the
 *   destination shifts when invert_alpha (low byte of the 2nd argument) is set.
 *   The result is the sum (add, not or) a + b + g + r. All x86 shifts use the
 *   count mod 32. Leaves ecx = dst.shift[a] (f[+0x4c]), edx = the alpha term.
 *   Callers: per-pixel loops of the texture converters 0x006C7E80 (8-bit
 *   palettised source), 0x006C89E0, 0x006C94C0 (after the DXT block decode
 *   0x006C9130), 0x006C9D70 for 32-bit destinations (16-bit destinations are
 *   inlined there), and once per colour in 0x006183D0.
 *
 * Decompiled with Ghidra (ReVa), checked against the x86 code. */
#include "recomp.h"
#include "runtime.h"

static inline uint32_t shr32(uint32_t v, uint32_t n) { return v >> (n & 31u); }
static inline uint32_t shl32(uint32_t v, uint32_t n) { return v << (n & 31u); }

int rt_native_pixfmt_init(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)ecx; (void)eax;
    const uint32_t f = RD32(esp + 4u);
    uint32_t edx = c->r[2];
    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t m = RD32(f + 4u * i);
        uint32_t shift = 0, pad = 8;
        if (m) {
            while (!(m & 1u)) {
                shift++;
                m >>= 1;
            }
            while (m) {
                m >>= 1;
                pad--;
            }
            edx = pad;
        }
        WR32(f + 0x10u + 4u * i, shift);
        WR32(f + 0x20u + 4u * i, pad);
    }
    c->r[1] = f + 0x30u;
    c->r[2] = edx;
    *ret = ((uint64_t)(esp + 8u) << 32) | 0u;
    return 1;
}

int rt_native_pixfmt_convert(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)eax;
    const uint32_t f = ecx;
    const uint32_t px = RD32(esp + 4u);
    const uint8_t inv = RD8(esp + 8u);

    uint32_t r = shr32(px & RD32(f + 0x00u), RD32(f + 0x10u));
    r = shl32(r, RD32(f + 0x20u));
    r = shr32(r, RD32(f + 0x50u));
    r = shl32(r, RD32(f + 0x40u));

    uint32_t g = shr32(px & RD32(f + 0x04u), RD32(f + 0x14u));
    g = shl32(g, RD32(f + 0x24u));
    g = shr32(g, RD32(f + 0x54u));
    g = shl32(g, RD32(f + 0x44u));

    uint32_t b = shr32(px & RD32(f + 0x08u), RD32(f + 0x18u));
    b = shl32(b, RD32(f + 0x28u));
    b = shr32(b, RD32(f + 0x58u));
    b = shl32(b, RD32(f + 0x48u));

    uint32_t a = shr32(px & RD32(f + 0x0Cu), RD32(f + 0x1Cu));
    a = shl32(a, RD32(f + 0x2Cu));
    if (inv)
        a = 0xFFu - a;
    a = shr32(a, RD32(f + 0x5Cu));
    const uint32_t ecx_out = RD32(f + 0x4Cu);
    a = shl32(a, ecx_out);

    c->r[1] = ecx_out;
    c->r[2] = a;
    *ret = ((uint64_t)(esp + 12u) << 32) | (a + b + g + r);
    return 1;
}
