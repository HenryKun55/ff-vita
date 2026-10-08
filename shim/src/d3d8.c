/* d3d8.dll: Direct3D 8 on the GL renderer (render_gl.c).
 *
 * The fixed-function state goes into the same RenderState the D3D7 front end of
 * the renderer consumed (render states, stage states, transforms, lights and
 * material keep their layout from D3D7 to D3D8). Textures and surfaces are
 * renderer Surfaces whose pixels live in guest memory; vertex and index buffers
 * are guest memory the draws decode. No vertex or pixel shaders: the caps
 * report a DirectX 7 class card, so the game keeps to its fixed-function paths. */
#include "ddraw_priv.h"
#include "platform.h"
#include "render.h"
#include "nstat.h"

#include <stdatomic.h>
#include <stdio.h>
#if defined(__vita__)
#include <psp2/kernel/clib.h>
#endif
#include <stdlib.h>

/* Bulk copies of vertex data (often not 16-byte aligned: 36-byte vertices) with
 * the system's copy, which handles any alignment at full speed. */
static inline void copy_bulk(void *dst, const void *src, size_t n)
{
#if defined(__vita__)
    sceClibMemcpy(dst, src, n);
#else
    memcpy(dst, src, n);
#endif
}

#define D3D_OK 0u
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_INVALIDCALL 0x8876086Cu

/* D3DFORMAT */
enum {
    FMT_UNKNOWN = 0, FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23,
    FMT_X1R5G5B5 = 24, FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26, FMT_R3G3B2 = 27, FMT_A8 = 28,
    FMT_X4R4G4B4 = 30, FMT_P8 = 41, FMT_L8 = 50, FMT_A8L8 = 51,
    FMT_D16_LOCKABLE = 70, FMT_D32 = 71, FMT_D15S1 = 73, FMT_D24S8 = 75, FMT_D16 = 80, FMT_D24X8 = 77,
    FMT_INDEX16 = 101, FMT_INDEX32 = 102,
};

/* D3DRESOURCETYPE */
enum { RT_SURFACE = 1, RT_TEXTURE = 3, RT_VERTEXBUFFER = 6, RT_INDEXBUFFER = 7 };

#define MAX_LEVELS 13

typedef struct Texture8 {
    uint32_t obj;
    uint32_t fmt, usage, pool;
    uint32_t levels;
    Surface *lv[MAX_LEVELS];
} Texture8;

/* Vertex and index buffers live in guest memory. With the pack thread, recorded
 * draws read them there later: a Lock with DISCARD (or a plain one) of a buffer
 * a recorded draw may still read gives it new memory instead (buffer renaming,
 * as drivers do); the old block is freed once the render thread is past it. */
typedef struct Buffer8 {
    uint32_t mem, size;
    uint32_t fvf_or_fmt, usage, pool;
    int index;               /* index buffer */
    uint64_t last_mark;      /* render queue mark after the last draw recorded with it */
    uint32_t gen;            /* counts writable Locks (cached vertex bounds go stale) */
    int copy_draws;          /* draws copy their vertices when recorded (see buf_Lock) */
} Buffer8;

static uint32_t d3d_vtbl, dev_vtbl, tex_vtbl, surf_vtbl, vb_vtbl, ib_vtbl;
static uint32_t d3d_obj, dev_obj;

/* Device state the renderer does not keep. */
static struct {
    uint32_t fvf;            /* current vertex shader (an FVF code) */
    uint32_t stream_vb, stream_stride;
    uint32_t ib, base_vertex;
    uint32_t textures[8];    /* guest objects set per stage */
    uint32_t mode_w, mode_h, mode_fmt;
    Surface *depth;
    Surface *back;           /* the device's back buffer (g_backbuffer while it renders) */
    Surface *target;         /* current render target (NULL: the back buffer) */
    uint32_t tex_tss[8][29]; /* stage states as the game set them (D3D8 values) */
    uint32_t clip_plane[6][4];
} dev;

static Surface *depth_surface(void);

/* DirectDraw (the movie player) may have replaced or cleared the renderer's
 * render target: the device's back buffer is it again. */
static inline Surface *back_buffer(void)
{
    g_backbuffer = dev.back;
    g_primary = dev.back;
    return dev.back;
}

/* ---- formats -------------------------------------------------------------------- */

static int fmt_pixfmt(uint32_t fmt, PixFmt *pf)
{
    memset(pf, 0, sizeof(*pf));
    switch (fmt) {
    case FMT_A8R8G8B8:
        *pf = (PixFmt){ DDPF_RGB | DDPF_ALPHAPIXELS, 32, 0xFF0000u, 0xFF00u, 0xFFu, 0xFF000000u };
        return 1;
    case FMT_X8R8G8B8:
        *pf = (PixFmt){ DDPF_RGB, 32, 0xFF0000u, 0xFF00u, 0xFFu, 0 };
        return 1;
    case FMT_R5G6B5:
        *pf = (PixFmt){ DDPF_RGB, 16, 0xF800u, 0x7E0u, 0x1Fu, 0 };
        return 1;
    case FMT_X1R5G5B5:
        *pf = (PixFmt){ DDPF_RGB, 16, 0x7C00u, 0x3E0u, 0x1Fu, 0 };
        return 1;
    case FMT_A1R5G5B5:
        *pf = (PixFmt){ DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7C00u, 0x3E0u, 0x1Fu, 0x8000u };
        return 1;
    case FMT_A4R4G4B4:
        *pf = (PixFmt){ DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0xF00u, 0xF0u, 0xFu, 0xF000u };
        return 1;
    case FMT_X4R4G4B4:
        *pf = (PixFmt){ DDPF_RGB, 16, 0xF00u, 0xF0u, 0xFu, 0 };
        return 1;
    case FOURCC_DXT1:
    case FOURCC_DXT3:
    case FOURCC_DXT5:
        *pf = (PixFmt){ DDPF_FOURCC, fmt, 0, 0, 0, 0 };
        return 1;
    case FMT_D16:
    case FMT_D16_LOCKABLE:
        *pf = (PixFmt){ DDPF_ZBUFFER, 16, 0xFFFFu, 0, 0, 0 };
        return 1;
    case FMT_D24X8:
    case FMT_D24S8:
    case FMT_D32:
        *pf = (PixFmt){ DDPF_ZBUFFER, 32, 0xFFFFFFu, 0, 0, 0 };
        return 1;
    default:
        return 0;
    }
}

static uint32_t fmt_pitch(uint32_t fmt, uint32_t w)
{
    if (fmt == FOURCC_DXT1)
        return ((w + 3u) / 4u) * 8u;
    if (fmt == FOURCC_DXT3 || fmt == FOURCC_DXT5)
        return ((w + 3u) / 4u) * 16u;
    PixFmt pf;
    fmt_pixfmt(fmt, &pf);
    return (w * (pf.bits / 8u) + 3u) & ~3u;
}

static Surface *surface8_new(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool)
{
    PixFmt pf;
    if (!fmt_pixfmt(fmt, &pf)) {
        rt_log("d3d8: unsupported surface format %u (0x%08X)", fmt, fmt);
        fmt_pixfmt(FMT_A8R8G8B8, &pf);
        fmt = FMT_A8R8G8B8;
    }
    Surface *s = calloc(1, sizeof(*s));
    s->w = w ? w : 1u;
    s->h = h ? h : 1u;
    s->pf = pf;
    s->pitch = fmt_pitch(fmt, s->w);
    s->d3dfmt = fmt;
    s->d3dusage = usage;
    s->d3dpool = pool;
    s->refs = 1;
    return s;
}

/* The guest IDirect3DSurface8 of a surface (created on first use, kept in objs[0]). */
static uint32_t surf_obj(Surface *s)
{
    if (!s->objs[0])
        s->objs[0] = com_new(surf_vtbl, s);
    return s->objs[0];
}

static void surface8_free(Surface *s)
{
    if (s->gl_tex)
        render_texture_release(s);
    if (s->mem && !s->client_mem)
        heap_free(s->mem);
    if (s->objs[0]) {
        uint32_t idx = RD32(s->objs[0] + 4u);
        if (idx < COM_MAX_OBJECTS)
            com_hosts[idx] = NULL;
    }
    free(s);
}

/* ---- IDirect3D8 --------------------------------------------------------------------- */

static void ok_ret(Ctx *c, int nargs) { stdcall_ret(c, D3D_OK, nargs); }

static void d3d_QueryInterface(Ctx *c)
{
    WR32(ARG(2), THIS);
    ok_ret(c, 3);
}

static void d3d_AddRef(Ctx *c) { stdcall_ret(c, 2, 1); }
static void d3d_Release(Ctx *c) { stdcall_ret(c, 1, 1); }
static void d3d_GetAdapterCount(Ctx *c) { stdcall_ret(c, 1, 1); }

/* GetAdapterIdentifier(adapter, flags, D3DADAPTER_IDENTIFIER8 *) */
static void d3d_GetAdapterIdentifier(Ctx *c)
{
    uint32_t p = ARG(3);
    memset(gptr(p), 0, 1068);
    snprintf(gbuf(p), 512, "nv4_disp.dll");
    snprintf(gbuf(p + 512u), 512, "NVIDIA GeForce4 Ti 4600");
    WR32(p + 1024u, (10u << 16) | 5216u);  /* DriverVersion 6.14.10.5216 (low, high part) */
    WR32(p + 1028u, (6u << 16) | 14u);
    WR32(p + 1032u, 0x10DEu);              /* VendorId */
    WR32(p + 1036u, 0x0250u);              /* DeviceId */
    WR32(p + 1064u, 1);                    /* WHQLLevel */
    ok_ret(c, 4);
}

static const uint16_t display_modes[][2] = {
    { 640, 480 }, { 800, 600 }, { 960, 544 }, { 1024, 768 }, { 1280, 720 }, { 1280, 960 }, { 1920, 1080 },
};
#define NUM_MODES (sizeof(display_modes) / sizeof(display_modes[0]))

static void write_mode(uint32_t p, uint32_t w, uint32_t h, uint32_t fmt)
{
    WR32(p + 0u, w);
    WR32(p + 4u, h);
    WR32(p + 8u, 60);
    WR32(p + 12u, fmt);
}

static void d3d_GetAdapterModeCount(Ctx *c) { stdcall_ret(c, NUM_MODES * 2u, 2); }

/* EnumAdapterModes(adapter, mode, D3DDISPLAYMODE *): every size in 32 and 16 bits. */
static void d3d_EnumAdapterModes(Ctx *c)
{
    uint32_t i = ARG(2);
    if (i >= NUM_MODES * 2u) {
        stdcall_ret(c, D3DERR_INVALIDCALL, 4);
        return;
    }
    uint32_t m = i % NUM_MODES;
    write_mode(ARG(3), display_modes[m][0], display_modes[m][1], i < NUM_MODES ? FMT_X8R8G8B8 : FMT_R5G6B5);
    ok_ret(c, 4);
}

static void d3d_GetAdapterDisplayMode(Ctx *c)
{
    write_mode(ARG(2), 1024, 768, FMT_X8R8G8B8);
    ok_ret(c, 3);
}

static void d3d_CheckDeviceType(Ctx *c) { ok_ret(c, 6); }

/* CheckDeviceFormat(adapter, type, adapter format, usage, resource type, format) */
static void d3d_CheckDeviceFormat(Ctx *c)
{
    uint32_t usage = ARG(4), fmt = ARG(6);
    PixFmt pf;
    int ok = fmt_pixfmt(fmt, &pf);
    if (fmt == FMT_D24S8 || fmt == FMT_D15S1)
        ok = 0;                                 /* no stencil */
    if ((usage & 2u) && !(pf.flags & DDPF_ZBUFFER))   /* D3DUSAGE_DEPTHSTENCIL */
        ok = 0;
    if ((usage & 1u) && fmt != FMT_X8R8G8B8 && fmt != FMT_A8R8G8B8 && fmt != FMT_R5G6B5)   /* RENDERTARGET */
        ok = 0;
    stdcall_ret(c, ok ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}

static void d3d_CheckDeviceMultiSampleType(Ctx *c)
{
    stdcall_ret(c, ARG(5) == 0 ? D3D_OK : D3DERR_NOTAVAILABLE, 6);
}

static void d3d_CheckDepthStencilMatch(Ctx *c) { ok_ret(c, 6); }

static void write_caps(uint32_t p)
{
    memset(gptr(p), 0, 212);
    float f;
    WR32(p + 0u, 1);                       /* D3DDEVTYPE_HAL */
    WR32(p + 12u, 0x00020000u);            /* Caps2: FULLSCREENGAMMA */
    WR32(p + 20u, 0x80000001u);            /* PresentationIntervals: IMMEDIATE | ONE */
    WR32(p + 28u, 0x00010000u | 0x00080000u | 0x400u | 0x200u | 0x800u | 0x10u);   /* DevCaps: HW T&L ... */
    WR32(p + 32u, 0x2u | 0x10u | 0x20u | 0x40u | 0x80u | 0x800u);   /* PrimitiveMiscCaps */
    WR32(p + 36u, 0x1u | 0x10u | 0x80u | 0x100u | 0x2000u | 0x4000u | 0x10000u | 0x100000u | 0x200000u);
    WR32(p + 40u, 0xFFu);                  /* ZCmpCaps */
    WR32(p + 44u, 0x1FFFu);                /* SrcBlendCaps */
    WR32(p + 48u, 0x1FFFu);                /* DestBlendCaps */
    WR32(p + 52u, 0xFFu);                  /* AlphaCmpCaps */
    WR32(p + 56u, 0x8u | 0x200u | 0x4000u | 0x80000u);   /* ShadeCaps */
    WR32(p + 60u, 0x1u | 0x2u | 0x4u | 0x4000u);         /* TextureCaps: PERSPECTIVE POW2 ALPHA MIPMAP */
    WR32(p + 64u, 0x100u | 0x200u | 0x10000u | 0x20000u | 0x1000000u | 0x2000000u);   /* TextureFilterCaps */
    WR32(p + 76u, 0x1u | 0x2u | 0x4u);     /* TextureAddressCaps: WRAP MIRROR CLAMP */
    WR32(p + 84u, 0x2u | 0x4u | 0x8u | 0x10u | 0x20u);
    WR32(p + 88u, 2048);                   /* MaxTextureWidth */
    WR32(p + 92u, 2048);
    WR32(p + 100u, 8192);                  /* MaxTextureRepeat */
    WR32(p + 104u, 2048);
    WR32(p + 108u, 1);                     /* MaxAnisotropy */
    f = 1e10f;
    memcpy(gptr(p + 112u), &f, 4);         /* MaxVertexW */
    f = -16384.0f;
    memcpy(gptr(p + 116u), &f, 4);
    memcpy(gptr(p + 120u), &f, 4);
    f = 16384.0f;
    memcpy(gptr(p + 124u), &f, 4);
    memcpy(gptr(p + 128u), &f, 4);
    WR32(p + 140u, 2);                     /* FVFCaps: 2 texture coordinate sets */
    WR32(p + 144u, 0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x40u | 0x80u | 0x200u | 0x800u | 0x1000u |
                   0x2000u | 0x8000u);     /* TextureOpCaps */
    WR32(p + 148u, 2);                     /* MaxTextureBlendStages */
    WR32(p + 152u, 2);                     /* MaxSimultaneousTextures */
    WR32(p + 156u, 0x1u | 0x2u | 0x8u | 0x10u | 0x20u);   /* VertexProcessingCaps */
    WR32(p + 160u, 8);                     /* MaxActiveLights */
    WR32(p + 164u, 0);                     /* MaxUserClipPlanes */
    WR32(p + 168u, 0);                     /* MaxVertexBlendMatrices */
    f = 1.0f;
    memcpy(gptr(p + 176u), &f, 4);         /* MaxPointSize */
    WR32(p + 180u, 0xFFFFFu);              /* MaxPrimitiveCount */
    WR32(p + 184u, 0xFFFFu);               /* MaxVertexIndex: 16-bit indices */
    WR32(p + 188u, 1);                     /* MaxStreams */
    WR32(p + 192u, 256);                   /* MaxStreamStride */
    WR32(p + 196u, 0xFFFE0000u);           /* VertexShaderVersion 0.0 */
    WR32(p + 204u, 0xFFFF0000u);           /* PixelShaderVersion 0.0 */
}

static void d3d_GetDeviceCaps(Ctx *c)
{
    write_caps(ARG(3));
    ok_ret(c, 4);
}

static void d3d_GetAdapterMonitor(Ctx *c) { stdcall_ret(c, 0x00070000u, 2); }

static void video_setup(uint32_t pp)
{
    uint32_t w = RD32(pp + 0u), h = RD32(pp + 4u), fmt = RD32(pp + 8u);
    if (!w || !h) {
        w = 1024;
        h = 768;
    }
    if (fmt == FMT_UNKNOWN)
        fmt = FMT_X8R8G8B8;
    rt_log("d3d8: video mode %ux%u format %u, windowed %u, auto depth %u (format %u)", w, h, fmt, RD32(pp + 28u),
           RD32(pp + 32u), RD32(pp + 36u));
    dev.mode_w = w;
    dev.mode_h = h;
    dev.mode_fmt = fmt;
    g_dd_width = (int)w;
    g_dd_height = (int)h;
    g_dd_bpp = fmt == FMT_R5G6B5 || fmt == FMT_X1R5G5B5 ? 16 : 32;
    platform_set_video_mode((int)w, (int)h);
    if (!dev.back) {
        dev.back = surface8_new(w, h, fmt, 1u, 0u);
        dev.back->is_back = 1;
    } else if (dev.back->w != w || dev.back->h != h) {
        if (dev.back->mem)
            heap_free(dev.back->mem);
        dev.back->mem = 0;
        dev.back->w = w;
        dev.back->h = h;
        dev.back->pitch = fmt_pitch(fmt, w);
    }
    back_buffer();
    if (!dev.depth) {
        dev.depth = surface8_new(w, h, FMT_D16, 2u, 0u);
        dev.depth->is_z = 1;
    }
    render_init((int)w, (int)h);
    memset(dev.textures, 0, sizeof(dev.textures));
    dev.fvf = 0;
}

/* CreateDevice(adapter, type, window, flags, D3DPRESENT_PARAMETERS *, &device) */
static void d3d_CreateDevice(Ctx *c)
{
    rt_log("IDirect3D8::CreateDevice(type %u, flags %08X)", ARG(2), ARG(4));
    video_setup(ARG(5));
    if (!dev_obj)
        dev_obj = com_new(dev_vtbl, NULL);
    WR32(ARG(6), dev_obj);
    ok_ret(c, 7);
}

static const ComMethod d3d_methods[] = {
    { "QueryInterface", d3d_QueryInterface },
    { "AddRef", d3d_AddRef },
    { "Release", d3d_Release },
    { "RegisterSoftwareDevice", NULL },
    { "GetAdapterCount", d3d_GetAdapterCount },
    { "GetAdapterIdentifier", d3d_GetAdapterIdentifier },
    { "GetAdapterModeCount", d3d_GetAdapterModeCount },
    { "EnumAdapterModes", d3d_EnumAdapterModes },
    { "GetAdapterDisplayMode", d3d_GetAdapterDisplayMode },
    { "CheckDeviceType", d3d_CheckDeviceType },
    { "CheckDeviceFormat", d3d_CheckDeviceFormat },
    { "CheckDeviceMultiSampleType", d3d_CheckDeviceMultiSampleType },
    { "CheckDepthStencilMatch", d3d_CheckDepthStencilMatch },
    { "GetDeviceCaps", d3d_GetDeviceCaps },
    { "GetAdapterMonitor", d3d_GetAdapterMonitor },
    { "CreateDevice", d3d_CreateDevice },
};

/* ---- vertices ---------------------------------------------------------------------- */

static uint32_t fvf_stride(uint32_t fvf);
static uint32_t fvf_stride_impl(uint32_t fvf)
{
    uint32_t s = 0;
    switch (fvf & 0x0Eu) {
    case 0x2u: s = 12; break;
    case 0x4u: s = 16; break;
    case 0x6u: s = 16; break;
    case 0x8u: s = 20; break;
    case 0xAu: s = 24; break;
    case 0xCu: s = 28; break;
    case 0xEu: s = 32; break;
    default: break;
    }
    if (fvf & 0x10u) s += 12;
    if (fvf & 0x20u) s += 4;
    if (fvf & 0x40u) s += 4;
    if (fvf & 0x80u) s += 4;
    uint32_t ntex = (fvf >> 8) & 0xFu;
    for (uint32_t i = 0; i < ntex; i++) {
        uint32_t f = (fvf >> (16 + i * 2)) & 3u;
        s += f == 0 ? 8u : f == 1 ? 12u : f == 2 ? 16u : 4u;
    }
    return s;
}

static uint32_t d3dcolor_to_rgba(uint32_t argb)
{
    return (argb & 0xFF00FF00u) | ((argb & 0xFFu) << 16) | ((argb >> 16) & 0xFFu);
}

typedef struct Decoded {
    int pretransformed, has_normal, has_color, has_spec, ntex;
} Decoded;

static RVertex *vbuf;
static uint32_t vbuf_cap;

/* The layout of an FVF's vertices for render_draw_guest (packs straight from the
 * game's buffer, without decode's RVertex pass): the same fields decode reads. */
static void guest_verts_of(uint32_t fvf, const uint8_t *base, uint32_t stride, GuestVerts *g)
{
    /* Field by field: a memset here is a library call per draw on the Vita. */
    g->src = NULL;
    g->has_bounds = 0;
    for (int t = 0; t < MAX_STAGES; t++)
        g->uv_off[t] = g->uv_bytes[t] = 0;
    uint32_t pos = fvf & 0x0Eu;
    g->base = base;
    g->stride = stride;
    g->pretransformed = pos == 0x4u;
    g->has_normal = (fvf & 0x10u) != 0;
    g->has_color = (fvf & 0x40u) != 0;
    g->has_spec = (fvf & 0x80u) != 0;
    g->ntex = (int)((fvf >> 8) & 0xFu);
    uint32_t o = pos == 0x4u ? 16u : pos == 0x2u ? 12u : 12u + ((pos - 0x4u) / 2u) * 4u;
    g->nrm_off = o;
    if (g->has_normal)
        o += 12;
    if (fvf & 0x20u)
        o += 4;
    g->col_off = o;
    if (g->has_color)
        o += 4;
    g->spec_off = o;
    if (g->has_spec)
        o += 4;
    for (int t = 0; t < g->ntex; t++) {
        uint32_t f = (fvf >> (16 + t * 2)) & 3u;
        uint32_t sz = f == 0 ? 8u : f == 1 ? 12u : f == 2 ? 16u : 4u;
        if (t < MAX_STAGES) {
            g->uv_off[t] = o;
            g->uv_bytes[t] = sz >= 8u ? 8u : 4u;
        }
        o += sz;
    }
    if (g->ntex > MAX_STAGES)
        g->ntex = MAX_STAGES;
}

/* Decodes `count` consecutive vertices at vb (stride bytes apart). */
static RVertex *decode(uint32_t fvf, const uint8_t *vb, uint32_t stride, uint32_t count, Decoded *d)
{
    if (count > vbuf_cap) {
        vbuf_cap = count + 256u;
        vbuf = realloc(vbuf, vbuf_cap * sizeof(RVertex));
    }
    uint32_t pos = fvf & 0x0Eu;
    d->pretransformed = pos == 0x4u;
    d->has_normal = (fvf & 0x10u) != 0;
    d->has_color = (fvf & 0x40u) != 0;
    d->has_spec = (fvf & 0x80u) != 0;
    d->ntex = (int)((fvf >> 8) & 0xFu);
    uint32_t pos_size = pos == 0x4u ? 16u : pos == 0x2u ? 12u : 12u + ((pos - 0x4u) / 2u) * 4u;
    uint32_t o = pos_size;
    uint32_t nrm_off = o;
    if (d->has_normal)
        o += 12;
    if (fvf & 0x20u)
        o += 4;
    uint32_t col_off = o;
    if (d->has_color)
        o += 4;
    uint32_t spec_off = o;
    if (d->has_spec)
        o += 4;
    uint32_t uv_off[MAX_STAGES], uv_bytes[MAX_STAGES];
    int nuv = d->ntex < MAX_STAGES ? d->ntex : MAX_STAGES;
    for (int t = 0; t < d->ntex; t++) {
        uint32_t f = (fvf >> (16 + t * 2)) & 3u;
        uint32_t sz = f == 0 ? 8u : f == 1 ? 12u : f == 2 ? 16u : 4u;
        if (t < MAX_STAGES) {
            uv_off[t] = o;
            uv_bytes[t] = sz >= 8u ? 8u : 4u;
        }
        o += sz;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = vb + (size_t)i * stride;
        RVertex *v = &vbuf[i];
        memcpy(&v->x, p, 12);
        if (d->pretransformed)
            memcpy(&v->w, p + 12, 4);
        else
            v->w = 1.0f;
        if (d->has_normal)
            memcpy(&v->nx, p + nrm_off, 12);
        else
            v->nx = v->ny = v->nz = 0.0f;
        uint32_t col = 0xFFFFFFFFu, spec = 0xFF000000u;
        if (d->has_color)
            memcpy(&col, p + col_off, 4);
        if (d->has_spec)
            memcpy(&spec, p + spec_off, 4);
        v->color = d3dcolor_to_rgba(col);
        v->spec = d3dcolor_to_rgba(spec);
        for (int t = 0; t < MAX_STAGES; t++) {
            if (t < nuv && uv_bytes[t] == 8u) {
                memcpy(v->uv[t], p + uv_off[t], 8);
            } else if (t < nuv) {
                memcpy(v->uv[t], p + uv_off[t], 4);
                v->uv[t][1] = 0.0f;
            } else {
                v->uv[t][0] = v->uv[t][1] = 0.0f;
            }
        }
    }
    return vbuf;
}

static uint32_t fvf_stride(uint32_t fvf) { return fvf_stride_impl(fvf); }

static uint32_t prim_vertices(uint32_t prim, uint32_t n)
{
    switch (prim) {
    case 1: return n;                 /* POINTLIST */
    case 2: return n * 2u;            /* LINELIST */
    case 3: return n + 1u;            /* LINESTRIP */
    case 4: return n * 3u;            /* TRIANGLELIST */
    case 5:                           /* TRIANGLESTRIP */
    case 6: return n + 2u;            /* TRIANGLEFAN */
    default: return 0;
    }
}

static uint16_t *ibuf;
static uint32_t ibuf_cap;

static uint16_t *index_scratch(uint32_t n)
{
    if (n > ibuf_cap) {
        ibuf_cap = n + 1024u;
        ibuf = realloc(ibuf, ibuf_cap * sizeof(uint16_t));
    }
    return ibuf;
}

/* Draws `nv` vertices at guest address `va` with optional indices (already
 * relative to va). */
static int offscreen(void);
static void frame_draw(void);

/* ---- pack thread (render.h) ----------------------------------------------------
 * With it on, a draw is recorded: the state snapshot (render_pk_prepare), then a
 * copy of the game's vertex bytes and the rebased indices; the render thread
 * decodes and draws them. The game thread only copies. */

typedef struct PkDraw {
    uint32_t prim, fvf, stride, nv, ni;
    uint8_t diag, trace;
    const uint8_t *verts;      /* copied vertices (frame memory) */
    const uint16_t *idx;       /* copied indices, NULL: none */
} PkDraw;

/* Render thread. */
static void pk_exec(const void *p)
{
    const PkDraw *c = p;
    render_pk_draw_begin(c->diag, c->trace);
    Decoded d;
    RVertex *v = decode(c->fvf, c->verts, c->stride, c->nv, &d);
    render_draw((int)c->prim, v, c->nv, c->idx, c->ni, d.pretransformed, d.has_normal, d.has_color, d.has_spec, d.ntex);
}

static void pk_record(uint32_t prim, uint32_t va, uint32_t stride, uint32_t nv, const uint16_t *idx, uint32_t ni)
{
    uint8_t *v = rq_data((size_t)nv * stride);
    copy_bulk(v, g_mem + va, (size_t)nv * stride);
    uint16_t *ic = NULL;
    if (idx && ni) {
        ic = rq_data((size_t)ni * sizeof(uint16_t));
        memcpy(ic, idx, (size_t)ni * sizeof(uint16_t));
    }
    uint32_t fvf = dev.fvf;
    render_pk_prepare((fvf & 0x0Eu) == 0x4u, (fvf & 0x10u) != 0);
    PkDraw *c = rq_begin_fast(pk_exec, sizeof(PkDraw));
    c->prim = prim;
    c->fvf = fvf;
    c->stride = stride;
    c->nv = nv;
    c->ni = ic ? ni : 0;
    c->diag = (uint8_t)render_diag_mode();
    c->trace = g_trace_next_frame == 2;
    c->verts = v;
    c->idx = ic;
    rq_end_fast();
    render_pk_recorded();
}

static uint16_t *read_indices(uint32_t addr, uint32_t n, int wide, uint32_t min);

/* Guest blocks of renamed buffers, freed once the render thread passed `mark`. */
#define MAX_RETIRED 256u
static struct {
    uint32_t mem;
    uint64_t mark;
} retired[MAX_RETIRED];
static uint32_t n_retired;

static void retired_collect(void)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < n_retired; i++) {
        if (rq_passed(retired[i].mark))
            heap_free(retired[i].mem);
        else
            retired[k++] = retired[i];
    }
    n_retired = k;
}

/* Frees `mem` of a buffer whose last recorded draw is `mark`, now or later. */
static void retire(uint32_t mem, uint64_t mark)
{
    if (!g_pack_thread || rq_passed(mark)) {
        heap_free(mem);
        return;
    }
    if (n_retired == MAX_RETIRED) {
        rq_sync();
        retired_collect();
    }
    retired[n_retired].mem = mem;
    retired[n_retired].mark = mark;
    n_retired++;
}

/* Draw path counts, logged every 600 frames (pack thread): draws read in place,
 * fallbacks; renames of buffers in use. */
static uint32_t st_inplace, st_other, st_renames;
static uint64_t st_rename_bytes;
static uint32_t st_rename_max, st_locks[3];   /* plain, DISCARD, NOOVERWRITE */
static uint64_t st_draw_copy_bytes, st_draw_copy_shared;

/* A recorded draw of buffer memory: vertices at verts, indices (or none) at
 * idx_src, rebased by -min on the render thread. */
typedef struct PkBufDraw {
    uint32_t prim, fvf, stride, nv, ni, min;
    uint8_t diag, trace, wide;
    const uint8_t *verts;
    const uint8_t *idx_src;
    uint32_t vaddr, gen;     /* guest address of the vertices and the buffer's version */
} PkBufDraw;

/* Render thread: bounds of the positions of the vertex ranges drawn, per buffer
 * version (static geometry is drawn every frame from unchanged buffers), so the
 * near-plane test only checks a box's corners. */
typedef struct BoundsEntry {
    uint32_t vaddr, nv, stride, gen;
    float lo[3], hi[3];
} BoundsEntry;
#define BOUNDS_SLOTS 4096u
static BoundsEntry bounds_cache[BOUNDS_SLOTS];


static void vertex_bounds(const PkBufDraw *c, GuestVerts *g)
{
    uint32_t h = (c->vaddr * 0x9E3779B1u ^ c->nv * 0x85EBCA6Bu ^ c->gen) >> 20;
    BoundsEntry *e = &bounds_cache[h & (BOUNDS_SLOTS - 1u)];
    if (e->vaddr != c->vaddr || e->nv != c->nv || e->stride != c->stride || e->gen != c->gen || !e->vaddr) {
#if defined(FF_PROFILE)
        rt_prof_render = PROF_H_NEARCLIP;
#endif
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        const uint8_t *p = c->verts;
        for (uint32_t i = 0; i < c->nv; i++, p += c->stride) {
            float v[3];
            memcpy(v, p, 12);
            for (int k = 0; k < 3; k++) {
                lo[k] = v[k] < lo[k] ? v[k] : lo[k];
                hi[k] = v[k] > hi[k] ? v[k] : hi[k];
            }
        }
        /* NaN positions never make a valid box: those draws keep the full scan. */
        if (!(lo[0] <= hi[0] && lo[1] <= hi[1] && lo[2] <= hi[2]))
            return;
        e->vaddr = c->vaddr;
        e->nv = c->nv;
        e->stride = c->stride;
        e->gen = c->gen;
        for (int k = 0; k < 3; k++) {
            e->lo[k] = lo[k];
            e->hi[k] = hi[k];
        }
#if defined(FF_PROFILE)
        rt_prof_render = PROF_H_RUN;
#endif
    }
    g->has_bounds = 1;
    for (int k = 0; k < 3; k++) {
        g->lo[k] = e->lo[k];
        g->hi[k] = e->hi[k];
    }
}

static void pk_exec_buf(const void *p)
{
    const PkBufDraw *c = p;
    static uint16_t *rib;
    static uint32_t rib_cap;
    render_pk_draw_begin(c->diag, c->trace);
#if defined(FF_PROFILE)
    rt_prof_render = PROF_H_INDEX;
#endif
    const uint16_t *idx = NULL;
    if (c->idx_src && c->ni) {
        if (c->ni > rib_cap) {
            rib_cap = c->ni + 1024u;
            rib = realloc(rib, rib_cap * sizeof(uint16_t));
        }
        const uint8_t *src = c->idx_src;
        const uint32_t min = c->min;
        if (c->wide) {
            for (uint32_t i = 0; i < c->ni; i++) {
                uint32_t v;
                memcpy(&v, src + 4u * i, 4);
                rib[i] = (uint16_t)(v - min);
            }
        } else {
            for (uint32_t i = 0; i < c->ni; i++) {
                uint16_t h;
                memcpy(&h, src + 2u * i, 2);
                rib[i] = (uint16_t)(h - min);
            }
        }
        idx = rib;
    }
#if defined(FF_PROFILE)
    rt_prof_render = PROF_H_RUN;
#endif
    /* Straight from the game's buffer when the batcher takes it; decoded otherwise
     * (near-plane clipping, big or non-triangle draws, diagnostics). */
    GuestVerts g;
    guest_verts_of(c->fvf, c->verts, c->stride, &g);
    if ((c->fvf & 0x0Eu) != 0x4u && c->vaddr)   /* not pre-transformed, not a dynamic buffer */
        vertex_bounds(c, &g);
    int done = render_draw_guest((int)c->prim, &g, c->nv, idx, idx ? c->ni : 0u);
#if defined(FF_PROFILE)
    rt_prof_render = PROF_H_RUN;
#endif
    if (done)
        return;
    Decoded d;
#if defined(FF_PROFILE)
    rt_prof_render = PROF_H_DECODE;
#endif
    RVertex *v = decode(c->fvf, c->verts, c->stride, c->nv, &d);
#if defined(FF_PROFILE)
    rt_prof_render = PROF_H_RUN;
#endif
    render_draw((int)c->prim, v, c->nv, idx, idx ? c->ni : 0u, d.pretransformed, d.has_normal, d.has_color,
                d.has_spec, d.ntex);
}

/* Mirrors of the buffers drawn with copies (copy_draws: the lit-vertex buffer,
 * rewritten with a plain Lock every frame while recorded draws read it). Its
 * draws read one of two host copies, by the parity of the frame recorded, instead
 * of copying their vertices: frame f's copy was last read by frame f - 2, which
 * the render thread has executed. A copy is brought up to date with the ranges
 * written since it was (all the writes to these buffers are native code's, which
 * reports them: d3d_mem_written; checked with stats=1 on FF_WATCH builds) before
 * a draw reads it. A range drawn earlier in the same frame and written again
 * cannot be updated under that draw: the frame's later draws copy their
 * vertices as before (and the copy is updated in the next frame).
 * ff.cfg vb_mirror=0: always copy. */
#define MIRRORS 4
#define MIRROR_PEND 1024
#define MIRROR_GRAIN 12u   /* stamp granularity: a common divisor of the vertex sizes (36) */
typedef struct Mirror {
    Buffer8 *b;
    uint32_t mem, size;
    uint8_t *m[2];
    uint32_t pend[2][MIRROR_PEND][2];   /* (offset, bytes) written since copy i was updated */
    uint32_t npend[2];
    int full[2];                        /* too many ranges: copy all */
    uint8_t *drawn;                     /* per grain: stamp of the frame that drew it from this frame's copy */
    uint32_t frame;                     /* frame of the stamps */
    uint8_t stamp;
    int conflict;                       /* this frame: draws copy their vertices */
} Mirror;
static Mirror *mirrors[MIRRORS];
static uint32_t mirror_lo = 0xFFFFFFFFu, mirror_hi;   /* guest range of all mirrored buffers */
static int mirror_on = -1;
static uint64_t st_mirror_bytes, st_mirror_draws, st_mirror_conflicts;

static void mirror_bounds(void)
{
    mirror_lo = 0xFFFFFFFFu;
    mirror_hi = 0;
    for (int i = 0; i < MIRRORS; i++)
        if (mirrors[i]) {
            if (mirrors[i]->mem < mirror_lo)
                mirror_lo = mirrors[i]->mem;
            if (mirrors[i]->mem + mirrors[i]->size > mirror_hi)
                mirror_hi = mirrors[i]->mem + mirrors[i]->size;
        }
}

static void mirror_drop(Buffer8 *b)
{
    for (int i = 0; i < MIRRORS; i++)
        if (mirrors[i] && mirrors[i]->b == b) {
            rq_sync();   /* recorded draws may read it */
            free(mirrors[i]->m[0]);
            free(mirrors[i]->m[1]);
            free(mirrors[i]->drawn);
            free(mirrors[i]);
            mirrors[i] = NULL;
            mirror_bounds();
        }
}

static void mirror_add(Buffer8 *b)
{
    if (mirror_on < 0) {
        mirror_on = 1;
#if defined(__vita__)
        FILE *f = fopen("ux0:data/ff/ff.cfg", "r");
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f))
                if (!strncmp(line, "vb_mirror=0", 11))
                    mirror_on = 0;
            fclose(f);
        }
#else
        const char *e = getenv("FF_VB_MIRROR");
        mirror_on = !(e && e[0] == '0');
#endif
    }
    if (!mirror_on || b->size % MIRROR_GRAIN)
        return;
    int k = 0;
    while (k < MIRRORS && mirrors[k])
        k++;
    if (k == MIRRORS)
        return;
    Mirror *m = calloc(1, sizeof(Mirror));
    if (!m)
        return;
    m->m[0] = malloc(b->size);
    m->m[1] = malloc(b->size);
    m->drawn = calloc(b->size / MIRROR_GRAIN, 1);
    if (!m->m[0] || !m->m[1] || !m->drawn) {
        free(m->m[0]);
        free(m->m[1]);
        free(m->drawn);
        free(m);
        return;
    }
    m->b = b;
    m->mem = b->mem;
    m->size = b->size;
    m->full[0] = m->full[1] = 1;
    m->frame = 0xFFFFFFFFu;
    mirrors[k] = m;
    mirror_bounds();
    rt_log("d3d8: vertex buffer %08X (%u KiB) mirrored for the render thread", b->mem, b->size / 1024u);
}

void d3d_mem_written(uint32_t addr, uint32_t len)
{
    if (addr >= mirror_hi || addr + len <= mirror_lo || !len)
        return;
    for (int i = 0; i < MIRRORS; i++) {
        Mirror *m = mirrors[i];
        if (!m || addr >= m->mem + m->size || addr + len <= m->mem)
            continue;
        uint32_t lo = addr < m->mem ? 0 : addr - m->mem, hi = addr + len - m->mem;
        if (hi > m->size)
            hi = m->size;
        for (int p = 0; p < 2; p++) {
            if (m->full[p])
                continue;
            uint32_t n = m->npend[p];
            if (n && m->pend[p][n - 1][0] + m->pend[p][n - 1][1] == lo) {   /* (contiguous writes) */
                m->pend[p][n - 1][1] += hi - lo;
            } else if (n < MIRROR_PEND) {
                m->pend[p][n][0] = lo;
                m->pend[p][n][1] = hi - lo;
                m->npend[p] = n + 1;
            } else {
                m->full[p] = 1;
            }
        }
    }
}

static Mirror *mirror_of(const Buffer8 *b)
{
    for (int i = 0; i < MIRRORS; i++)
        if (mirrors[i] && mirrors[i]->b == b)
            return mirrors[i]->mem == b->mem && mirrors[i]->size == b->size ? mirrors[i] : NULL;
    return NULL;
}

/* Whether [off, off + n) was drawn from this frame's copy. */
static int mirror_drawn(const Mirror *m, uint32_t off, uint32_t n)
{
    for (uint32_t g = off / MIRROR_GRAIN, e = (off + n + MIRROR_GRAIN - 1u) / MIRROR_GRAIN; g < e; g++)
        if (m->drawn[g] == m->stamp)
            return 1;
    return 0;
}

/* This frame's copy of m with [voff, voff + bytes) up to date, or NULL: copy the vertices. */
static const uint8_t *mirror_read(Mirror *m, uint32_t voff, uint32_t bytes)
{
    const uint32_t f = rq_rec_frame(), p = f & 1u;
    if (m->frame != f) {
        m->frame = f;
        m->stamp = (uint8_t)(m->stamp % 255u + 1u);
        m->conflict = 0;
    }
    if (m->conflict)
        return NULL;
    if (m->full[p]) {
        if (mirror_drawn(m, 0, m->size)) {
            m->conflict = 1;
            st_mirror_conflicts++;
            return NULL;
        }
        copy_bulk(m->m[p], g_mem + m->mem, m->size);
        st_mirror_bytes += m->size;
        m->full[p] = 0;
        m->npend[p] = 0;
    }
    for (uint32_t i = 0; i < m->npend[p]; i++) {
        const uint32_t o = m->pend[p][i][0], n = m->pend[p][i][1];
        if (mirror_drawn(m, o, n)) {
            /* Keep the ranges not applied yet for the next frame. */
            memmove(m->pend[p], m->pend[p] + i, (m->npend[p] - i) * sizeof(m->pend[p][0]));
            m->npend[p] -= i;
            m->conflict = 1;
            st_mirror_conflicts++;
            return NULL;
        }
        copy_bulk(m->m[p] + o, g_mem + m->mem + o, n);
        st_mirror_bytes += n;
    }
    m->npend[p] = 0;
    memset(m->drawn + voff / MIRROR_GRAIN, m->stamp,
           (voff + bytes + MIRROR_GRAIN - 1u) / MIRROR_GRAIN - voff / MIRROR_GRAIN);
    st_mirror_draws++;
    return m->m[p] + voff;
}

/* Records a draw of vertex buffer vb (byte offset voff) with index buffer ib
 * (start index, NULL: not indexed), read in place by the render thread; 0 when
 * the general path must be taken. */
static int pk_record_buffers(uint32_t prim, Buffer8 *vb, uint32_t voff, uint32_t stride, uint32_t nv, Buffer8 *ib,
                             uint32_t start, uint32_t ni, uint32_t min)
{
    if (!g_pack_thread || !nv || dev.fvf & 1u || offscreen() || voff + (uint64_t)nv * stride > vb->size) {
        st_other++;
        return 0;
    }
    int wide = ib && ib->fvf_or_fmt == FMT_INDEX32;
    uint32_t ioff = ib ? start * (wide ? 4u : 2u) : 0u;
    if (ib && ioff + (uint64_t)ni * (wide ? 4u : 2u) > ib->size) {
        st_other++;
        return 0;
    }
    uint32_t fvf = dev.fvf;
    {
        PROF_SECTION(PROF_H_STATE);
        render_pk_prepare((fvf & 0x0Eu) == 0x4u, (fvf & 0x10u) != 0);
        PROF_SECTION_END;
    }
    PROF_SECTION(PROF_H_BATCH);
    NSTAT_BEGIN();
    /* Buffers rewritten with plain Locks: only the vertices this draw reads, copied
     * into the frame's memory (before the command is opened). */
    const uint8_t *vcopy = NULL;
    Mirror *mr = vb->copy_draws ? mirror_of(vb) : NULL;
    if (mr)
        vcopy = mirror_read(mr, voff, nv * stride);
    if (vb->copy_draws && !vcopy) {
        /* The same range drawn again in this frame (more passes over a mesh) shares
         * the copy: the frame's memory lives until the frame has been executed. */
        static struct {
            const Buffer8 *b;
            uint32_t gen, voff, bytes, frame;
            uint8_t *copy;
        } seen[256];
        const uint32_t bytes = nv * stride;
        uint32_t h = (((uint32_t)(uintptr_t)vb >> 4) ^ voff * 0x9E3779B1u ^ bytes) & 255u;
        if (seen[h].b == vb && seen[h].gen == vb->gen && seen[h].voff == voff && seen[h].bytes == bytes &&
            seen[h].frame == g_frame_serial) {
            vcopy = seen[h].copy;
            st_draw_copy_shared += bytes;
        } else {
            st_draw_copy_bytes += bytes;
            NSTAT_BEGIN();
            uint8_t *nc = rq_data(bytes);
            copy_bulk(nc, g_mem + vb->mem + voff, bytes);
            vcopy = nc;
            NSTAT_END(NS_RECORD_COPY, bytes);
            seen[h].b = vb;
            seen[h].gen = vb->gen;
            seen[h].voff = voff;
            seen[h].bytes = bytes;
            seen[h].frame = g_frame_serial;
            seen[h].copy = nc;
        }
    }
    PkBufDraw *c = rq_begin_fast(pk_exec_buf, sizeof(PkBufDraw));
    c->prim = prim;
    c->fvf = fvf;
    c->stride = stride;
    c->nv = nv;
    c->ni = ib ? ni : 0u;
    c->min = min;
    c->wide = (uint8_t)wide;
    c->diag = (uint8_t)render_diag_mode();
    c->trace = g_trace_next_frame == 2;
    if (vcopy) {
        c->verts = vcopy;
        c->vaddr = 0;   /* rewritten every frame: no cached bounds */
    } else {
        c->verts = g_mem + vb->mem + voff;
        c->vaddr = (vb->usage & 0x200u) ? 0u : vb->mem + voff;   /* DYNAMIC: rewritten every frame, no cache */
    }
    c->idx_src = ib ? g_mem + ib->mem + ioff : NULL;
    c->gen = vb->gen;
    rq_end_fast();
    render_pk_recorded();
    uint64_t mark = rq_mark();
    if (!vb->copy_draws)
        vb->last_mark = mark;
    if (ib)
        ib->last_mark = mark;
    st_inplace++;
    NSTAT_END(NS_RECORD, 1u);
    PROF_SECTION_END;
    return 1;
}

static void draw_verts(uint32_t prim, uint32_t va, uint32_t stride, uint32_t nv, const uint16_t *idx, uint32_t ni)
{
    if (!nv || dev.fvf & 1u || offscreen())
        return;
    if (!stride)
        stride = fvf_stride(dev.fvf);
    if (g_pack_thread) {
        pk_record(prim, va, stride, nv, idx, ni);
        return;
    }
    Decoded d;
    RVertex *v = decode(dev.fvf, g_mem + va, stride, nv, &d);
    render_draw((int)prim, v, nv, idx, ni, d.pretransformed, d.has_normal, d.has_color, d.has_spec, d.ntex);
}

/* Indices from guest memory, rebased by -min; 16 or 32 bits wide. */
static uint16_t *read_indices(uint32_t addr, uint32_t n, int wide, uint32_t min)
{
    uint16_t *out = index_scratch(n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = wide ? RD32(addr + 4u * i) : RD16(addr + 2u * i);
        out[i] = (uint16_t)(v - min);
    }
    return out;
}

/* ---- IDirect3DDevice8 ------------------------------------------------------------------ */

static void dev_QueryInterface(Ctx *c)
{
    WR32(ARG(2), THIS);
    ok_ret(c, 3);
}

static void dev_AddRef(Ctx *c) { stdcall_ret(c, 2, 1); }
static void dev_Release(Ctx *c) { stdcall_ret(c, 1, 1); }
static void dev_TestCooperativeLevel(Ctx *c) { ok_ret(c, 1); }
static void dev_GetAvailableTextureMem(Ctx *c) { stdcall_ret(c, 64u << 20, 1); }
static void dev_ResourceManagerDiscardBytes(Ctx *c) { ok_ret(c, 2); }

static void dev_GetDirect3D(Ctx *c)
{
    WR32(ARG(1), d3d_obj);
    ok_ret(c, 2);
}

static void dev_GetDeviceCaps(Ctx *c)
{
    write_caps(ARG(1));
    ok_ret(c, 2);
}

static void dev_GetDisplayMode(Ctx *c)
{
    write_mode(ARG(1), dev.mode_w, dev.mode_h, dev.mode_fmt);
    ok_ret(c, 2);
}

static void dev_GetCreationParameters(Ctx *c)
{
    uint32_t p = ARG(1);
    WR32(p + 0u, 0);
    WR32(p + 4u, 1);
    WR32(p + 8u, 0x00010010u);
    WR32(p + 12u, 0x40u);                  /* HARDWARE_VERTEXPROCESSING */
    ok_ret(c, 2);
}

static void dev_SetCursorProperties(Ctx *c) { ok_ret(c, 4); }
static void dev_SetCursorPosition(Ctx *c) { stdcall_ret(c, 0, 4); }
static void dev_ShowCursor(Ctx *c) { stdcall_ret(c, 0, 2); }

static void dev_Reset(Ctx *c)
{
    render_flush();
    video_setup(ARG(1));
    ok_ret(c, 2);
}

static void dev_Present(Ctx *c)
{
    static uint32_t frames;
    if (++frames % 600u == 0u) {
        rt_log("d3d8 draws (600 frames): read in place %u, other %u; buffers renamed %u (%u KiB copied, largest %u "
               "KiB); locks: %u plain, %u discard, %u nooverwrite; draws copied %u KiB (%u KiB shared)", st_inplace,
               st_other, st_renames, (unsigned)(st_rename_bytes / 1024u), st_rename_max / 1024u, st_locks[0],
               st_locks[1], st_locks[2], (unsigned)(st_draw_copy_bytes / 1024u),
               (unsigned)(st_draw_copy_shared / 1024u));
        rt_log("d3d8 mirrored buffers (600 frames): %llu draws read them, %llu KiB brought up to date, %llu "
               "frames copying after a conflict", (unsigned long long)st_mirror_draws,
               (unsigned long long)(st_mirror_bytes / 1024u), (unsigned long long)st_mirror_conflicts);
        st_mirror_draws = st_mirror_bytes = st_mirror_conflicts = 0;
        st_draw_copy_bytes = st_draw_copy_shared = 0;
        st_inplace = st_other = st_renames = 0;
        st_rename_bytes = 0;
        st_rename_max = st_locks[0] = st_locks[1] = st_locks[2] = 0;
    }
    nstat_frame();
    retired_collect();
    back_buffer();
    render_present();
    /* Clear clears only the viewport, so what is outside the game's viewport
     * (the cutscene bars) is never drawn: start every frame black, as a fresh
     * D3D back buffer is (vitaGL's are not cleared). */
    render_clear_new_frame(dev.mode_w, dev.mode_h);
    ok_ret(c, 5);
}

static void dev_GetBackBuffer(Ctx *c)
{
    uint32_t o = surf_obj(back_buffer());
    com_addref(o);
    WR32(ARG(3), o);
    ok_ret(c, 4);
}

static void dev_GetRasterStatus(Ctx *c)
{
    WR32(ARG(1), 1);                       /* InVBlank */
    WR32(ARG(1) + 4u, 0);
    ok_ret(c, 2);
}

static void dev_SetGammaRamp(Ctx *c) { stdcall_ret(c, 0, 3); }

static void dev_GetGammaRamp(Ctx *c)
{
    uint32_t p = ARG(1);
    for (uint32_t ch = 0; ch < 3u; ch++)
        for (uint32_t i = 0; i < 256u; i++)
            WR16(p + ch * 512u + i * 2u, i * 257u);
    stdcall_ret(c, 0, 2);
}

/* CreateTexture(w, h, levels, usage, format, pool, &texture) */
static void dev_CreateTexture(Ctx *c)
{
    uint32_t w = ARG(1), h = ARG(2), levels = ARG(3), usage = ARG(4), fmt = ARG(5), pool = ARG(6);
    PixFmt pf;
    if (!fmt_pixfmt(fmt, &pf) || (pf.flags & DDPF_ZBUFFER)) {
        rt_log("CreateTexture %ux%u: format %u (0x%08X) not supported", w, h, fmt, fmt);
        WR32(ARG(7), 0);
        stdcall_ret(c, D3DERR_INVALIDCALL, 8);
        return;
    }
    {
        static uint32_t fmts_seen[32];
        static int nf;
        int known = 0;
        for (int i = 0; i < nf; i++)
            known |= fmts_seen[i] == fmt;
        if (!known && nf < 32) {
            fmts_seen[nf++] = fmt;
            rt_log("d3d8: first texture of format %u (0x%08X): %ux%u, %u levels, usage %X, pool %u", fmt, fmt, w, h,
                   levels, usage, pool);
        }
    }
    uint32_t full = 1;
    while ((w >> full) || (h >> full))
        full++;
    if (!levels || levels > full)
        levels = full;
    if (levels > MAX_LEVELS)
        levels = MAX_LEVELS;
    Texture8 *t = calloc(1, sizeof(*t));
    t->fmt = fmt;
    t->usage = usage;
    t->pool = pool;
    t->levels = levels;
    for (uint32_t i = 0; i < levels; i++) {
        uint32_t lw = w >> i, lh = h >> i;
        Surface *s = surface8_new(lw ? lw : 1u, lh ? lh : 1u, fmt, usage, pool);
        s->is_texture = 1;
        s->container = t;
        s->mip_levels = (int)(levels - i);
        if (i) {
            t->lv[i - 1]->attached = s;
            s->parent = t->lv[i - 1];
        }
        t->lv[i] = s;
    }
    t->obj = com_new(tex_vtbl, t);
    WR32(ARG(7), t->obj);
    ok_ret(c, 8);
}

static Buffer8 *buffer_new(uint32_t size, uint32_t usage, uint32_t fvf_or_fmt, uint32_t pool, int index)
{
    Buffer8 *b = calloc(1, sizeof(*b));
    b->size = size;
    b->usage = usage;
    b->fvf_or_fmt = fvf_or_fmt;
    b->pool = pool;
    b->index = index;
    b->mem = heap_alloc(size ? size : 4u);
    if (!b->mem)
        rt_fatal(NULL, "d3d8: out of guest memory for a %u byte buffer", size);
    return b;
}

/* CreateVertexBuffer(length, usage, fvf, pool, &vb) */
static void dev_CreateVertexBuffer(Ctx *c)
{
    Buffer8 *b = buffer_new(ARG(1), ARG(2), ARG(3), ARG(4), 0);
    WR32(ARG(5), com_new(vb_vtbl, b));
    ok_ret(c, 6);
}

/* CreateIndexBuffer(length, usage, format, pool, &ib) */
static void dev_CreateIndexBuffer(Ctx *c)
{
    Buffer8 *b = buffer_new(ARG(1), ARG(2), ARG(3), ARG(4), 1);
    WR32(ARG(5), com_new(ib_vtbl, b));
    ok_ret(c, 6);
}

/* CreateRenderTarget(w, h, format, multisample, lockable, &surface) */
static void dev_CreateRenderTarget(Ctx *c)
{
    rt_log("CreateRenderTarget %ux%u format %u: an off-screen surface", ARG(1), ARG(2), ARG(3));
    Surface *s = surface8_new(ARG(1), ARG(2), ARG(3), 1u, 0u);
    WR32(ARG(6), surf_obj(s));
    ok_ret(c, 7);
}

/* CreateDepthStencilSurface(w, h, format, multisample, &surface) */
static void dev_CreateDepthStencilSurface(Ctx *c)
{
    Surface *s = surface8_new(ARG(1), ARG(2), ARG(3), 2u, 0u);
    s->is_z = 1;
    WR32(ARG(5), surf_obj(s));
    ok_ret(c, 6);
}

/* CreateImageSurface(w, h, format, &surface): system memory. */
static void dev_CreateImageSurface(Ctx *c)
{
    Surface *s = surface8_new(ARG(1), ARG(2), ARG(3), 0u, 2u);
    WR32(ARG(4), surf_obj(s));
    ok_ret(c, 5);
}

static Surface *surface_of(uint32_t obj) { return obj ? (Surface *)com_host(obj) : NULL; }

/* Byte copy of a rectangle between two surfaces of the same format. */
static void copy_rect(Surface *dst, int32_t dx, int32_t dy, Surface *src, const Rect *sr)
{
    surface_alloc_mem(dst);
    surface_alloc_mem(src);
    if (surface_is_dxt(src)) {
        if (dx == 0 && dy == 0 && sr->l == 0 && sr->t == 0 && src->w == dst->w && src->h == dst->h)
            memcpy(g_mem + dst->mem, g_mem + src->mem, surface_bytes(src));
        surface_set_dirty(dst);
        return;
    }
    uint32_t bpp = src->pf.bits / 8u;
    for (int32_t y = sr->t; y < sr->b; y++) {
        int32_t ty = dy + (y - sr->t);
        if (y < 0 || y >= (int32_t)src->h || ty < 0 || ty >= (int32_t)dst->h)
            continue;
        int32_t x0 = sr->l, x1 = sr->r, tx = dx;
        if (x0 < 0) {
            tx -= x0;
            x0 = 0;
        }
        if (x1 > (int32_t)src->w)
            x1 = (int32_t)src->w;
        if (tx + (x1 - x0) > (int32_t)dst->w)
            x1 = x0 + (int32_t)dst->w - tx;
        if (x1 <= x0 || tx < 0)
            continue;
        memmove(g_mem + dst->mem + (uint32_t)ty * dst->pitch + (uint32_t)tx * bpp,
                g_mem + src->mem + (uint32_t)y * src->pitch + (uint32_t)x0 * bpp, (size_t)(x1 - x0) * bpp);
    }
    surface_set_dirty(dst);
}

/* CopyRects(src, rects, count, dst, points) */
static void dev_CopyRects(Ctx *c)
{
    back_buffer();
    Surface *src = surface_of(ARG(1)), *dst = surface_of(ARG(4));
    uint32_t rects = ARG(2), n = ARG(3), points = ARG(5);
    if (!src || !dst) {
        stdcall_ret(c, D3DERR_INVALIDCALL, 6);
        return;
    }
    if (!rects)
        n = 1;
    for (uint32_t i = 0; i < n; i++) {
        Rect sr = { 0, 0, (int32_t)src->w, (int32_t)src->h };
        if (rects) {
            sr.l = (int32_t)RD32(rects + i * 16u);
            sr.t = (int32_t)RD32(rects + i * 16u + 4u);
            sr.r = (int32_t)RD32(rects + i * 16u + 8u);
            sr.b = (int32_t)RD32(rects + i * 16u + 12u);
        }
        int32_t dx = sr.l, dy = sr.t;
        if (points) {
            dx = (int32_t)RD32(points + i * 8u);
            dy = (int32_t)RD32(points + i * 8u + 4u);
        }
        if (dst == dev.back) {
            frame_draw();
            Rect dr = { dx, dy, dx + (sr.r - sr.l), dy + (sr.b - sr.t) };
            render_blit(src, &sr, &dr, 0);
        } else {
            copy_rect(dst, dx, dy, src, &sr);
        }
    }
    ok_ret(c, 6);
}

/* UpdateTexture(src, dst): system memory texture into a video memory one. */
static void dev_UpdateTexture(Ctx *c)
{
    Texture8 *s = com_host(ARG(1)), *d = com_host(ARG(2));
    if (s && d)
        for (uint32_t i = 0; i < s->levels && i < d->levels; i++) {
            surface_alloc_mem(s->lv[i]);
            surface_alloc_mem(d->lv[i]);
            uint32_t n = surface_bytes(s->lv[i]);
            if (n == surface_bytes(d->lv[i]))
                memcpy(g_mem + d->lv[i]->mem, g_mem + s->lv[i]->mem, n);
            surface_set_dirty(d->lv[i]);
        }
    ok_ret(c, 3);
}

static void dev_GetFrontBuffer(Ctx *c) { ok_ret(c, 2); }

/* Render to texture is not supported yet: while a texture is the target, its
 * draws and clears are dropped (the texture stays as it is). */
static void dev_SetRenderTarget(Ctx *c)
{
    Surface *s = surface_of(ARG(1));
    if (s) {
        render_flush();
        dev.target = s == dev.back ? NULL : s;
    }
    ok_ret(c, 3);
}

static int offscreen(void) { return dev.target != NULL; }

/* Before a draw: into the back buffer, maybe over the previous frame (render_frame_draw). */
static void frame_draw(void)
{
    if (!offscreen())
        render_frame_draw();
}

static void dev_GetRenderTarget(Ctx *c)
{
    uint32_t o = dev.target ? surf_obj(dev.target) : surf_obj(back_buffer());
    com_addref(o);
    WR32(ARG(1), o);
    ok_ret(c, 2);
}

static Surface *depth_surface(void) { return dev.depth; }

static void dev_GetDepthStencilSurface(Ctx *c)
{
    uint32_t o = surf_obj(depth_surface());
    com_addref(o);
    WR32(ARG(1), o);
    ok_ret(c, 2);
}

static void dev_BeginScene(Ctx *c)
{
    back_buffer();
    ok_ret(c, 1);
}

static void dev_EndScene(Ctx *c)
{
    render_flush();
    ok_ret(c, 1);
}

/* Clear(count, rects, flags, color, z, stencil) */
static void dev_Clear(Ctx *c)
{
    back_buffer();
    if (offscreen()) {
        ok_ret(c, 7);
        return;
    }
    uint32_t n = ARG(1), rects = ARG(2), flags = ARG(3), color = ARG(4);
    float z;
    uint32_t zb = ARG(5);
    memcpy(&z, &zb, 4);
    Rect r[16];
    if (n > 16u)
        n = 16u;
    for (uint32_t i = 0; i < n && rects; i++) {
        r[i].l = (int32_t)RD32(rects + i * 16u);
        r[i].t = (int32_t)RD32(rects + i * 16u + 4u);
        r[i].r = (int32_t)RD32(rects + i * 16u + 8u);
        r[i].b = (int32_t)RD32(rects + i * 16u + 12u);
    }
    render_clear(rects ? n : 0u, r, flags & 3u, color, z);
    ok_ret(c, 7);
}

/* ---- state ------------------------------------------------------------------------- */

static float *transform_slot(uint32_t type)
{
    switch (type) {
    case 2: return g_rs.view;
    case 3: return g_rs.proj;
    case 256: return g_rs.world;
    default: return NULL;
    }
}

static void set_transform(uint32_t type, const float *m16)
{
    float *m = transform_slot(type);
    if (m && memcmp(m, m16, 64) != 0) {
        render_state_changing();
        g_rs_parts |= m == g_rs.world ? RS_PART_WORLD : RS_PART_XF;
        memcpy(m, m16, 64);
        if (m == g_rs.view)
            g_light_serial++;
    }
}

static void dev_SetTransform(Ctx *c);
static void dev_GetTransform(Ctx *c)
{
    float *m = transform_slot(ARG(1));
    if (m)
        memcpy(gptr(ARG(2)), m, 64);
    else {
        float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        memcpy(gptr(ARG(2)), id, 64);
    }
    ok_ret(c, 3);
}

static void dev_MultiplyTransform(Ctx *c)
{
    float *m = transform_slot(ARG(1));
    if (m) {
        float b[16], r[16];
        memcpy(b, gptr(ARG(2)), 64);
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r[i * 4 + j] = m[i * 4] * b[j] + m[i * 4 + 1] * b[4 + j] + m[i * 4 + 2] * b[8 + j] + m[i * 4 + 3] * b[12 + j];
        set_transform(ARG(1), r);
    }
    ok_ret(c, 3);
}

static void set_viewport(const uint8_t *v)
{
    int32_t vi[4];
    float vz[2];
    memcpy(vi, v, 16);
    memcpy(vz, v + 16, 8);
    if (vi[0] != g_rs.vp_x || vi[1] != g_rs.vp_y || vi[2] != g_rs.vp_w || vi[3] != g_rs.vp_h ||
        vz[0] != g_rs.vp_minz || vz[1] != g_rs.vp_maxz) {
        render_state_changing();
        g_rs_parts |= RS_PART_XF;
        g_rs.vp_x = vi[0];
        g_rs.vp_y = vi[1];
        g_rs.vp_w = vi[2];
        g_rs.vp_h = vi[3];
        g_rs.vp_minz = vz[0];
        g_rs.vp_maxz = vz[1];
    }
}

static void get_viewport(uint8_t *v)
{
    int32_t vi[4] = { g_rs.vp_x, g_rs.vp_y, g_rs.vp_w, g_rs.vp_h };
    memcpy(v, vi, 16);
    memcpy(v + 16, &g_rs.vp_minz, 4);
    memcpy(v + 20, &g_rs.vp_maxz, 4);
}

static void dev_GetViewport(Ctx *c)
{
    get_viewport(gptr(ARG(1)));
    ok_ret(c, 2);
}

static void set_material(const uint8_t *g)
{
    if (memcmp(g_rs.mat_diffuse, g, 16) || memcmp(g_rs.mat_ambient, g + 16, 16) ||
        memcmp(g_rs.mat_specular, g + 32, 16) || memcmp(g_rs.mat_emissive, g + 48, 16) ||
        memcmp(&g_rs.mat_power, g + 64, 4)) {
        render_state_changing();
        g_light_serial++;
        memcpy(g_rs.mat_diffuse, g, 16);
        memcpy(g_rs.mat_ambient, g + 16, 16);
        memcpy(g_rs.mat_specular, g + 32, 16);
        memcpy(g_rs.mat_emissive, g + 48, 16);
        memcpy(&g_rs.mat_power, g + 64, 4);
    }
}

static void get_material(uint8_t *m)
{
    memcpy(m, g_rs.mat_diffuse, 16);
    memcpy(m + 16, g_rs.mat_ambient, 16);
    memcpy(m + 32, g_rs.mat_specular, 16);
    memcpy(m + 48, g_rs.mat_emissive, 16);
    memcpy(m + 64, &g_rs.mat_power, 4);
}

static void dev_GetMaterial(Ctx *c)
{
    get_material(gptr(ARG(1)));
    ok_ret(c, 2);
}

#define D3DLIGHT8_SIZE 104u
_Static_assert(offsetof(Light, enabled) == D3DLIGHT8_SIZE, "Light starts with the D3DLIGHT8 layout");

static void set_light(uint32_t i, const uint8_t *p)
{
    if (i < MAX_LIGHTS && memcmp(&g_rs.lights[i], p, D3DLIGHT8_SIZE) != 0) {
        render_state_changing();
        g_light_serial++;
        memcpy(&g_rs.lights[i], p, D3DLIGHT8_SIZE);
    }
}

static void light_enable(uint32_t i, uint32_t on)
{
    if (i < MAX_LIGHTS && g_rs.lights[i].enabled != (on != 0)) {
        render_state_changing();
        g_light_serial++;
        g_rs.lights[i].enabled = on != 0;
    }
}

static void dev_GetLight(Ctx *c)
{
    uint32_t i = ARG(1);
    if (i < MAX_LIGHTS)
        memcpy(gptr(ARG(2)), &g_rs.lights[i], D3DLIGHT8_SIZE);
    ok_ret(c, 3);
}

static void dev_GetLightEnable(Ctx *c)
{
    uint32_t i = ARG(1);
    WR32(ARG(2), i < MAX_LIGHTS ? (uint32_t)g_rs.lights[i].enabled : 0u);
    ok_ret(c, 3);
}

static void dev_SetClipPlane(Ctx *c) { ok_ret(c, 3); }
static void dev_GetClipPlane(Ctx *c)
{
    memset(gptr(ARG(2)), 0, 16);
    ok_ret(c, 3);
}

static void set_render_state(uint32_t s, uint32_t value)
{
    if (s < 256u && g_rs.rs[s] != value) {
        render_state_changing();
        g_rs_parts |= RS_PART_RS;
        /* SPECULARENABLE, AMBIENT, COLORVERTEX, NORMALIZENORMALS, DIFFUSEMATERIALSOURCE */
        if (s == 29u || s == 139u || s == 141u || s == 143u || s == 145u)
            g_light_serial++;
        g_rs.rs[s] = value;
    }
}

static void dev_GetRenderState(Ctx *c)
{
    uint32_t s = ARG(1);
    WR32(ARG(2), s < 256u ? g_rs.rs[s] : 0u);
    ok_ret(c, 3);
}

static void set_texture(uint32_t stage, uint32_t obj)
{
    if (stage >= 8u)
        return;
    dev.textures[stage] = obj;
    if (stage >= MAX_STAGES)
        return;
    Texture8 *t = obj ? com_host(obj) : NULL;
    Surface *s = t ? t->lv[0] : NULL;
    if (g_rs.stage[stage].texture != s) {
        render_state_changing();
        g_rs_parts |= RS_PART_STAGE;
        g_rs.stage[stage].texture = s;
    }
}

static void dev_GetTexture(Ctx *c)
{
    uint32_t stage = ARG(1);
    uint32_t o = stage < 8u ? dev.textures[stage] : 0u;
    if (o)
        com_addref(o);
    WR32(ARG(2), o);
    ok_ret(c, 3);
}

/* D3D8 texture stage state -> the renderer's (D3D7 values: mip filter + 1). */
static uint32_t *tss_slot(StageState *st, uint32_t type)
{
    switch (type) {
    case 1: return &st->colorop;
    case 2: return &st->colorarg1;
    case 3: return &st->colorarg2;
    case 4: return &st->alphaop;
    case 5: return &st->alphaarg1;
    case 6: return &st->alphaarg2;
    case 11: return &st->texcoordindex;
    case 13: return &st->addressu;
    case 14: return &st->addressv;
    case 16: return &st->magfilter;
    case 17: return &st->minfilter;
    case 18: return &st->mipfilter;
    default: return NULL;
    }
}

static void set_tss(uint32_t stage, uint32_t type, uint32_t value)
{
    if (stage < 8u && type < 29u)
        dev.tex_tss[stage][type] = value;
    if (stage >= MAX_STAGES)
        return;
    uint32_t v = value;
    if (type == 18u)
        v = value + 1u;                         /* D3DTEXF_NONE 0 -> D3DTFP_NONE 1 ... */
    else if (type == 16u || type == 17u)
        v = value == 0u ? 1u : value > 2u ? 2u : value;
    uint32_t *p = tss_slot(&g_rs.stage[stage], type);
    if (p && *p != v) {
        render_state_changing();
        g_rs_parts |= RS_PART_STAGE;
        *p = v;
    }
}

static void dev_GetTextureStageState(Ctx *c)
{
    uint32_t stage = ARG(1), type = ARG(2);
    WR32(ARG(3), stage < 8u && type < 29u ? dev.tex_tss[stage][type] : 0u);
    ok_ret(c, 4);
}

static void dev_ValidateDevice(Ctx *c)
{
    WR32(ARG(1), 1);
    ok_ret(c, 2);
}

static void dev_GetInfo(Ctx *c) { stdcall_ret(c, S_FALSE, 4); }
static void dev_SetPaletteEntries(Ctx *c) { ok_ret(c, 3); }
static void dev_SetCurrentTexturePalette(Ctx *c) { ok_ret(c, 2); }

static void set_vertex_shader(uint32_t h)
{
    static uint32_t seen[64];
    static int nseen;
    int known = 0;
    for (int i = 0; i < nseen; i++)
        known |= seen[i] == h;
    if (!known && nseen < 64) {
        seen[nseen++] = h;
        rt_log("d3d8: vertex shader / FVF %08X (stride %u)", h, fvf_stride(h));
    }
    if ((h & 1u) && h != dev.fvf)
        rt_log("SetVertexShader(%08X): programmable vertex shaders are not supported", h);
    dev.fvf = h;
}

static void dev_GetVertexShader(Ctx *c)
{
    WR32(ARG(1), dev.fvf);
    ok_ret(c, 2);
}

static void set_stream(uint32_t stream, uint32_t vb, uint32_t stride)
{
    if (stream != 0u)
        return;
    dev.stream_vb = vb;
    dev.stream_stride = stride;
}

static void dev_GetStreamSource(Ctx *c)
{
    uint32_t o = ARG(1) == 0u ? dev.stream_vb : 0u;
    if (o)
        com_addref(o);
    WR32(ARG(2), o);
    WR32(ARG(3), dev.stream_stride);
    ok_ret(c, 4);
}

static void set_indices(uint32_t ib, uint32_t base)
{
    dev.ib = ib;
    dev.base_vertex = base;
}

static void dev_GetIndices(Ctx *c)
{
    if (dev.ib)
        com_addref(dev.ib);
    WR32(ARG(1), dev.ib);
    WR32(ARG(2), dev.base_vertex);
    ok_ret(c, 3);
}

/* ---- state blocks --------------------------------------------------------------------
 * A block is a list of recorded state calls. Between BeginStateBlock and
 * EndStateBlock the calls are recorded instead of applied (as in D3D8);
 * CaptureStateBlock refreshes each recorded value from the device. */

enum { SB_RS, SB_TSS, SB_TEX, SB_XFORM, SB_MATERIAL, SB_LIGHT, SB_LIGHTON, SB_VIEWPORT, SB_VSHADER, SB_STREAM,
       SB_INDICES };

typedef struct SbEntry {
    uint8_t op;
    uint32_t a, b, c;
    uint8_t data[104];
} SbEntry;

typedef struct StateBlock {
    SbEntry *e;
    uint32_t n, cap;
} StateBlock;

#define MAX_BLOCKS 4096u
static StateBlock *blocks[MAX_BLOCKS];
static StateBlock *recording;

static SbEntry *sb_slot(StateBlock *b, uint8_t op, uint32_t a, uint32_t bb)
{
    /* A later call for the same state replaces the earlier one. */
    for (uint32_t i = 0; i < b->n; i++)
        if (b->e[i].op == op && b->e[i].a == a && b->e[i].b == bb)
            return &b->e[i];
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2u : 32u;
        b->e = realloc(b->e, b->cap * sizeof(SbEntry));
    }
    SbEntry *e = &b->e[b->n++];
    memset(e, 0, sizeof(*e));
    e->op = op;
    e->a = a;
    e->b = bb;
    return e;
}

static void sb_capture_entry(SbEntry *e)
{
    switch (e->op) {
    case SB_RS: e->c = e->a < 256u ? g_rs.rs[e->a] : 0u; break;
    case SB_TSS: e->c = e->a < 8u && e->b < 29u ? dev.tex_tss[e->a][e->b] : 0u; break;
    case SB_TEX: e->c = e->a < 8u ? dev.textures[e->a] : 0u; break;
    case SB_XFORM: {
        float *m = transform_slot(e->a);
        if (m)
            memcpy(e->data, m, 64);
        break;
    }
    case SB_MATERIAL: get_material(e->data); break;
    case SB_LIGHT: if (e->a < MAX_LIGHTS) memcpy(e->data, &g_rs.lights[e->a], D3DLIGHT8_SIZE); break;
    case SB_LIGHTON: e->c = e->a < MAX_LIGHTS ? (uint32_t)g_rs.lights[e->a].enabled : 0u; break;
    case SB_VIEWPORT: get_viewport(e->data); break;
    case SB_VSHADER: e->c = dev.fvf; break;
    case SB_STREAM: e->b = dev.stream_vb; e->c = dev.stream_stride; break;
    case SB_INDICES: e->a = dev.ib; e->c = dev.base_vertex; break;
    default: break;
    }
}

static void sb_apply_entry(const SbEntry *e)
{
    switch (e->op) {
    case SB_RS: set_render_state(e->a, e->c); break;
    case SB_TSS: set_tss(e->a, e->b, e->c); break;
    case SB_TEX: set_texture(e->a, e->c); break;
    case SB_XFORM: set_transform(e->a, (const float *)e->data); break;
    case SB_MATERIAL: set_material(e->data); break;
    case SB_LIGHT: set_light(e->a, e->data); break;
    case SB_LIGHTON: light_enable(e->a, e->c); break;
    case SB_VIEWPORT: set_viewport(e->data); break;
    case SB_VSHADER: set_vertex_shader(e->c); break;
    case SB_STREAM: set_stream(0, e->b, e->c); break;
    case SB_INDICES: set_indices(e->a, e->c); break;
    default: break;
    }
}

static uint32_t sb_add(StateBlock *b)
{
    for (uint32_t i = 1; i < MAX_BLOCKS; i++)
        if (!blocks[i]) {
            blocks[i] = b;
            return i;
        }
    rt_fatal(NULL, "too many state blocks");
}

static void dev_BeginStateBlock(Ctx *c)
{
    recording = calloc(1, sizeof(StateBlock));
    ok_ret(c, 1);
}

static void dev_EndStateBlock(Ctx *c)
{
    uint32_t h = recording ? sb_add(recording) : 0u;
    recording = NULL;
    WR32(ARG(1), h);
    ok_ret(c, 2);
}

static void dev_ApplyStateBlock(Ctx *c)
{
    uint32_t h = ARG(1);
    StateBlock *b = h < MAX_BLOCKS ? blocks[h] : NULL;
    if (b)
        for (uint32_t i = 0; i < b->n; i++)
            sb_apply_entry(&b->e[i]);
    ok_ret(c, 2);
}

static void dev_CaptureStateBlock(Ctx *c)
{
    uint32_t h = ARG(1);
    StateBlock *b = h < MAX_BLOCKS ? blocks[h] : NULL;
    if (b)
        for (uint32_t i = 0; i < b->n; i++)
            sb_capture_entry(&b->e[i]);
    ok_ret(c, 2);
}

static void dev_DeleteStateBlock(Ctx *c)
{
    uint32_t h = ARG(1);
    if (h < MAX_BLOCKS && blocks[h]) {
        free(blocks[h]->e);
        free(blocks[h]);
        blocks[h] = NULL;
    }
    ok_ret(c, 2);
}

/* CreateStateBlock(type, &handle): D3DSBT_ALL 1, PIXELSTATE 2, VERTEXSTATE 3. */
static void dev_CreateStateBlock(Ctx *c)
{
    uint32_t type = ARG(1);
    StateBlock *b = calloc(1, sizeof(*b));
    for (uint32_t s = 7; s < 175u; s++)
        sb_slot(b, SB_RS, s, 0);
    for (uint32_t st = 0; st < MAX_STAGES; st++)
        for (uint32_t t = 1; t < 29u; t++)
            sb_slot(b, SB_TSS, st, t);
    if (type != 2u) {
        sb_slot(b, SB_VSHADER, 0, 0);
        for (uint32_t i = 0; i < MAX_LIGHTS; i++) {
            sb_slot(b, SB_LIGHT, i, 0);
            sb_slot(b, SB_LIGHTON, i, 0);
        }
    }
    if (type == 1u) {
        for (uint32_t st = 0; st < MAX_STAGES; st++)
            sb_slot(b, SB_TEX, st, 0);
        sb_slot(b, SB_XFORM, 2, 0);
        sb_slot(b, SB_XFORM, 3, 0);
        sb_slot(b, SB_XFORM, 256, 0);
        sb_slot(b, SB_MATERIAL, 0, 0);
        sb_slot(b, SB_VIEWPORT, 0, 0);
        sb_slot(b, SB_STREAM, 0, 0);
        sb_slot(b, SB_INDICES, 0, 0);
    }
    for (uint32_t i = 0; i < b->n; i++)
        sb_capture_entry(&b->e[i]);
    WR32(ARG(2), sb_add(b));
    ok_ret(c, 3);
}

/* The state setters: recorded while a state block is being recorded. */

static void dev_SetTransform(Ctx *c)
{
    if (recording) {
        SbEntry *e = sb_slot(recording, SB_XFORM, ARG(1), 0);
        memcpy(e->data, gptr(ARG(2)), 64);
    } else {
        float m[16];
        memcpy(m, gptr(ARG(2)), 64);
        set_transform(ARG(1), m);
    }
    ok_ret(c, 3);
}

static void dev_SetViewport(Ctx *c)
{
    if (recording)
        memcpy(sb_slot(recording, SB_VIEWPORT, 0, 0)->data, gptr(ARG(1)), 24);
    else
        set_viewport(gptr(ARG(1)));
    ok_ret(c, 2);
}

static void dev_SetMaterial(Ctx *c)
{
    if (recording)
        memcpy(sb_slot(recording, SB_MATERIAL, 0, 0)->data, gptr(ARG(1)), 68);
    else
        set_material(gptr(ARG(1)));
    ok_ret(c, 2);
}

static void dev_SetLight(Ctx *c)
{
    if (recording)
        memcpy(sb_slot(recording, SB_LIGHT, ARG(1), 0)->data, gptr(ARG(2)), D3DLIGHT8_SIZE);
    else
        set_light(ARG(1), gptr(ARG(2)));
    ok_ret(c, 3);
}

static void dev_LightEnable(Ctx *c)
{
    if (recording)
        sb_slot(recording, SB_LIGHTON, ARG(1), 0)->c = ARG(2);
    else
        light_enable(ARG(1), ARG(2));
    ok_ret(c, 3);
}

static void dev_SetRenderState(Ctx *c)
{
    if (recording)
        sb_slot(recording, SB_RS, ARG(1), 0)->c = ARG(2);
    else
        set_render_state(ARG(1), ARG(2));
    ok_ret(c, 3);
}

static void dev_SetTexture(Ctx *c)
{
    if (recording)
        sb_slot(recording, SB_TEX, ARG(1), 0)->c = ARG(2);
    else
        set_texture(ARG(1), ARG(2));
    ok_ret(c, 3);
}

static void dev_SetTextureStageState(Ctx *c)
{
    if (recording)
        sb_slot(recording, SB_TSS, ARG(1), ARG(2))->c = ARG(3);
    else
        set_tss(ARG(1), ARG(2), ARG(3));
    ok_ret(c, 4);
}

static void dev_SetVertexShader(Ctx *c)
{
    if (recording)
        sb_slot(recording, SB_VSHADER, 0, 0)->c = ARG(1);
    else
        set_vertex_shader(ARG(1));
    ok_ret(c, 2);
}

static void dev_SetStreamSource(Ctx *c)
{
    if (recording) {
        SbEntry *e = sb_slot(recording, SB_STREAM, 0, 0);
        e->b = ARG(2);
        e->c = ARG(3);
    } else {
        set_stream(ARG(1), ARG(2), ARG(3));
    }
    ok_ret(c, 4);
}

static void dev_SetIndices(Ctx *c)
{
    if (recording) {
        SbEntry *e = sb_slot(recording, SB_INDICES, 0, 0);
        e->a = ARG(1);
        e->c = ARG(2);
    } else {
        set_indices(ARG(1), ARG(2));
    }
    ok_ret(c, 3);
}

static void dev_SetClipStatus(Ctx *c) { ok_ret(c, 2); }
static void dev_GetClipStatus(Ctx *c)
{
    memset(gptr(ARG(1)), 0, 8);
    ok_ret(c, 2);
}

/* ---- drawing --------------------------------------------------------------------------- */

static Buffer8 *stream_buffer(void)
{
    return dev.stream_vb ? com_host(dev.stream_vb) : NULL;
}

/* DrawPrimitive(type, start vertex, primitive count) */
static void dev_DrawPrimitive(Ctx *c)
{
    frame_draw();
    uint32_t prim = ARG(1), start = ARG(2), n = ARG(3);
    Buffer8 *vb = stream_buffer();
    if (vb) {
        uint32_t stride = dev.stream_stride ? dev.stream_stride : fvf_stride(dev.fvf);
        uint32_t nv = prim_vertices(prim, n);
        if (!pk_record_buffers(prim, vb, start * stride, stride, nv, NULL, 0, 0, 0))
            draw_verts(prim, vb->mem + start * stride, stride, nv, NULL, 0);
    }
    ok_ret(c, 4);
}

/* DrawIndexedPrimitive(type, min index, vertex count, start index, primitive count) */
static void dev_DrawIndexedPrimitive(Ctx *c)
{
    frame_draw();
    uint32_t prim = ARG(1), min = ARG(2), nv = ARG(3), start = ARG(4), n = ARG(5);
    Buffer8 *vb = stream_buffer();
    Buffer8 *ib = dev.ib ? com_host(dev.ib) : NULL;
    if (vb && ib) {
        int wide = ib->fvf_or_fmt == FMT_INDEX32;
        uint32_t ni = prim_vertices(prim, n);
        uint32_t stride = dev.stream_stride ? dev.stream_stride : fvf_stride(dev.fvf);
        uint32_t voff = (dev.base_vertex + min) * stride;
        if (!pk_record_buffers(prim, vb, voff, stride, nv, ib, start, ni, min)) {
            uint16_t *idx = read_indices(ib->mem + start * (wide ? 4u : 2u), ni, wide, min);
            draw_verts(prim, vb->mem + voff, stride, nv, idx, ni);
        }
    }
    ok_ret(c, 6);
}

/* DrawPrimitiveUP(type, primitive count, vertices, stride) */
static void dev_DrawPrimitiveUP(Ctx *c)
{
    frame_draw();
    uint32_t prim = ARG(1), n = ARG(2);
    draw_verts(prim, ARG(3), ARG(4), prim_vertices(prim, n), NULL, 0);
    dev.stream_vb = 0;                       /* as D3D8 does */
    dev.stream_stride = 0;
    ok_ret(c, 5);
}

/* DrawIndexedPrimitiveUP(type, min index, vertex count, primitive count, indices,
 * index format, vertices, stride) */
static void dev_DrawIndexedPrimitiveUP(Ctx *c)
{
    frame_draw();
    uint32_t prim = ARG(1), min = ARG(2), nv = ARG(3), n = ARG(4), ip = ARG(5), fmt = ARG(6);
    uint32_t va = ARG(7), stride = ARG(8);
    uint32_t ni = prim_vertices(prim, n);
    uint16_t *idx = read_indices(ip, ni, fmt == FMT_INDEX32, min);
    draw_verts(prim, va + min * stride, stride, nv, idx, ni);
    dev.stream_vb = 0;
    dev.stream_stride = 0;
    dev.ib = 0;
    ok_ret(c, 9);
}

static void dev_DeleteVertexShader(Ctx *c) { ok_ret(c, 2); }
static void dev_SetVertexShaderConstant(Ctx *c) { ok_ret(c, 4); }
static void dev_SetPixelShader(Ctx *c) { ok_ret(c, 2); }
static void dev_GetPixelShader(Ctx *c)
{
    WR32(ARG(1), 0);
    ok_ret(c, 2);
}
static void dev_DeletePixelShader(Ctx *c) { ok_ret(c, 2); }
static void dev_SetPixelShaderConstant(Ctx *c) { ok_ret(c, 4); }

static const ComMethod dev_methods[] = {
    { "QueryInterface", dev_QueryInterface },
    { "AddRef", dev_AddRef },
    { "Release", dev_Release },
    { "TestCooperativeLevel", dev_TestCooperativeLevel },
    { "GetAvailableTextureMem", dev_GetAvailableTextureMem },
    { "ResourceManagerDiscardBytes", dev_ResourceManagerDiscardBytes },
    { "GetDirect3D", dev_GetDirect3D },
    { "GetDeviceCaps", dev_GetDeviceCaps },
    { "GetDisplayMode", dev_GetDisplayMode },
    { "GetCreationParameters", dev_GetCreationParameters },
    { "SetCursorProperties", dev_SetCursorProperties },
    { "SetCursorPosition", dev_SetCursorPosition },
    { "ShowCursor", dev_ShowCursor },
    { "CreateAdditionalSwapChain", NULL },
    { "Reset", dev_Reset },
    { "Present", dev_Present },
    { "GetBackBuffer", dev_GetBackBuffer },
    { "GetRasterStatus", dev_GetRasterStatus },
    { "SetGammaRamp", dev_SetGammaRamp },
    { "GetGammaRamp", dev_GetGammaRamp },
    { "CreateTexture", dev_CreateTexture },
    { "CreateVolumeTexture", NULL },
    { "CreateCubeTexture", NULL },
    { "CreateVertexBuffer", dev_CreateVertexBuffer },
    { "CreateIndexBuffer", dev_CreateIndexBuffer },
    { "CreateRenderTarget", dev_CreateRenderTarget },
    { "CreateDepthStencilSurface", dev_CreateDepthStencilSurface },
    { "CreateImageSurface", dev_CreateImageSurface },
    { "CopyRects", dev_CopyRects },
    { "UpdateTexture", dev_UpdateTexture },
    { "GetFrontBuffer", dev_GetFrontBuffer },
    { "SetRenderTarget", dev_SetRenderTarget },
    { "GetRenderTarget", dev_GetRenderTarget },
    { "GetDepthStencilSurface", dev_GetDepthStencilSurface },
    { "BeginScene", dev_BeginScene },
    { "EndScene", dev_EndScene },
    { "Clear", dev_Clear },
    { "SetTransform", dev_SetTransform },
    { "GetTransform", dev_GetTransform },
    { "MultiplyTransform", dev_MultiplyTransform },
    { "SetViewport", dev_SetViewport },
    { "GetViewport", dev_GetViewport },
    { "SetMaterial", dev_SetMaterial },
    { "GetMaterial", dev_GetMaterial },
    { "SetLight", dev_SetLight },
    { "GetLight", dev_GetLight },
    { "LightEnable", dev_LightEnable },
    { "GetLightEnable", dev_GetLightEnable },
    { "SetClipPlane", dev_SetClipPlane },
    { "GetClipPlane", dev_GetClipPlane },
    { "SetRenderState", dev_SetRenderState },
    { "GetRenderState", dev_GetRenderState },
    { "BeginStateBlock", dev_BeginStateBlock },
    { "EndStateBlock", dev_EndStateBlock },
    { "ApplyStateBlock", dev_ApplyStateBlock },
    { "CaptureStateBlock", dev_CaptureStateBlock },
    { "DeleteStateBlock", dev_DeleteStateBlock },
    { "CreateStateBlock", dev_CreateStateBlock },
    { "SetClipStatus", dev_SetClipStatus },
    { "GetClipStatus", dev_GetClipStatus },
    { "GetTexture", dev_GetTexture },
    { "SetTexture", dev_SetTexture },
    { "GetTextureStageState", dev_GetTextureStageState },
    { "SetTextureStageState", dev_SetTextureStageState },
    { "ValidateDevice", dev_ValidateDevice },
    { "GetInfo", dev_GetInfo },
    { "SetPaletteEntries", dev_SetPaletteEntries },
    { "GetPaletteEntries", NULL },
    { "SetCurrentTexturePalette", dev_SetCurrentTexturePalette },
    { "GetCurrentTexturePalette", NULL },
    { "DrawPrimitive", dev_DrawPrimitive },
    { "DrawIndexedPrimitive", dev_DrawIndexedPrimitive },
    { "DrawPrimitiveUP", dev_DrawPrimitiveUP },
    { "DrawIndexedPrimitiveUP", dev_DrawIndexedPrimitiveUP },
    { "ProcessVertices", NULL },
    { "CreateVertexShader", NULL },
    { "SetVertexShader", dev_SetVertexShader },
    { "GetVertexShader", dev_GetVertexShader },
    { "DeleteVertexShader", dev_DeleteVertexShader },
    { "SetVertexShaderConstant", dev_SetVertexShaderConstant },
    { "GetVertexShaderConstant", NULL },
    { "GetVertexShaderDeclaration", NULL },
    { "GetVertexShaderFunction", NULL },
    { "SetStreamSource", dev_SetStreamSource },
    { "GetStreamSource", dev_GetStreamSource },
    { "SetIndices", dev_SetIndices },
    { "GetIndices", dev_GetIndices },
    { "CreatePixelShader", NULL },
    { "SetPixelShader", dev_SetPixelShader },
    { "GetPixelShader", dev_GetPixelShader },
    { "DeletePixelShader", dev_DeletePixelShader },
    { "SetPixelShaderConstant", dev_SetPixelShaderConstant },
    { "GetPixelShaderConstant", NULL },
    { "GetPixelShaderFunction", NULL },
    { "DrawRectPatch", NULL },
    { "DrawTriPatch", NULL },
    { "DeletePatch", NULL },
};

/* ---- resources: common methods -------------------------------------------------------------- */

static void res_QueryInterface(Ctx *c)
{
    com_addref(THIS);
    WR32(ARG(2), THIS);
    ok_ret(c, 3);
}

static void res_AddRef(Ctx *c) { stdcall_ret(c, com_addref(THIS), 1); }

static void res_GetDevice(Ctx *c)
{
    WR32(ARG(1), dev_obj);
    ok_ret(c, 2);
}

static void res_SetPrivateData(Ctx *c) { ok_ret(c, 5); }
static void res_GetPrivateData(Ctx *c) { stdcall_ret(c, 0x88760866u, 4); }   /* D3DERR_NOTFOUND */
static void res_FreePrivateData(Ctx *c) { ok_ret(c, 2); }
static void res_SetPriority(Ctx *c) { stdcall_ret(c, 0, 2); }
static void res_GetPriority(Ctx *c) { stdcall_ret(c, 0, 1); }
static void res_PreLoad(Ctx *c) { stdcall_ret(c, 0, 1); }

/* ---- IDirect3DTexture8 ---------------------------------------------------------------------- */

static void tex_Release(Ctx *c)
{
    uint32_t o = THIS;
    Texture8 *t = com_host(o);
    uint32_t n = com_release(o);
    if (n == 0 && t) {
        for (int i = 0; i < MAX_STAGES; i++)
            if (g_rs.stage[i].texture == t->lv[0]) {
                render_state_changing();
                g_rs.stage[i].texture = NULL;
            }
        for (uint32_t i = 0; i < 8u; i++)
            if (dev.textures[i] == o)
                dev.textures[i] = 0;
        render_flush();
        for (uint32_t i = 0; i < t->levels; i++)
            surface8_free(t->lv[i]);
        free(t);
    }
    stdcall_ret(c, n, 1);
}

static void tex_GetType(Ctx *c) { stdcall_ret(c, RT_TEXTURE, 1); }
static void tex_SetLOD(Ctx *c) { stdcall_ret(c, 0, 2); }
static void tex_GetLOD(Ctx *c) { stdcall_ret(c, 0, 1); }

static void tex_GetLevelCount(Ctx *c)
{
    Texture8 *t = com_host(THIS);
    stdcall_ret(c, t ? t->levels : 0u, 1);
}

static void write_surface_desc(uint32_t d, Surface *s)
{
    WR32(d + 0u, s->d3dfmt);
    WR32(d + 4u, RT_SURFACE);
    WR32(d + 8u, s->d3dusage);
    WR32(d + 12u, s->d3dpool);
    WR32(d + 16u, surface_bytes(s));
    WR32(d + 20u, 0);
    WR32(d + 24u, s->w);
    WR32(d + 28u, s->h);
}

static void tex_GetLevelDesc(Ctx *c)
{
    Texture8 *t = com_host(THIS);
    uint32_t l = ARG(1);
    if (!t || l >= t->levels) {
        stdcall_ret(c, D3DERR_INVALIDCALL, 3);
        return;
    }
    write_surface_desc(ARG(2), t->lv[l]);
    ok_ret(c, 3);
}

static void tex_GetSurfaceLevel(Ctx *c)
{
    Texture8 *t = com_host(THIS);
    uint32_t l = ARG(1);
    if (!t || l >= t->levels) {
        WR32(ARG(2), 0);
        stdcall_ret(c, D3DERR_INVALIDCALL, 3);
        return;
    }
    uint32_t o = surf_obj(t->lv[l]);
    com_addref(o);
    com_addref(THIS);                        /* the surface holds its texture */
    WR32(ARG(2), o);
    ok_ret(c, 3);
}

/* D3DLOCKED_RECT for a surface (rect: guest RECT or 0). */
static void lock_surface(Surface *s, uint32_t out, uint32_t rect)
{
    if (s == dev.back)
        render_readback(s);
    surface_alloc_mem(s);
    uint32_t off = 0;
    if (rect) {
        uint32_t x = RD32(rect), y = RD32(rect + 4u);
        if (surface_is_dxt(s))
            off = (y / 4u) * s->pitch + (x / 4u) * (s->pf.bits == FOURCC_DXT1 ? 8u : 16u);
        else
            off = y * s->pitch + x * (s->pf.bits / 8u);
    }
    WR32(out, s->pitch);
    WR32(out + 4u, s->mem + off);
    s->locked = 1;
}

static void unlock_surface(Surface *s)
{
    s->locked = 0;
    if (s == dev.back)
        render_writeback(s);
    else
        surface_set_dirty(s);
}

/* LockRect(level, &locked, rect, flags) */
static void tex_LockRect(Ctx *c)
{
    Texture8 *t = com_host(THIS);
    uint32_t l = ARG(1);
    if (!t || l >= t->levels) {
        stdcall_ret(c, D3DERR_INVALIDCALL, 5);
        return;
    }
    lock_surface(t->lv[l], ARG(2), ARG(3));
    ok_ret(c, 5);
}

static void tex_UnlockRect(Ctx *c)
{
    Texture8 *t = com_host(THIS);
    uint32_t l = ARG(1);
    if (t && l < t->levels)
        unlock_surface(t->lv[l]);
    ok_ret(c, 2);
}

static void tex_AddDirtyRect(Ctx *c) { ok_ret(c, 2); }

static const ComMethod tex_methods[] = {
    { "QueryInterface", res_QueryInterface },
    { "AddRef", res_AddRef },
    { "Release", tex_Release },
    { "GetDevice", res_GetDevice },
    { "SetPrivateData", res_SetPrivateData },
    { "GetPrivateData", res_GetPrivateData },
    { "FreePrivateData", res_FreePrivateData },
    { "SetPriority", res_SetPriority },
    { "GetPriority", res_GetPriority },
    { "PreLoad", res_PreLoad },
    { "GetType", tex_GetType },
    { "SetLOD", tex_SetLOD },
    { "GetLOD", tex_GetLOD },
    { "GetLevelCount", tex_GetLevelCount },
    { "GetLevelDesc", tex_GetLevelDesc },
    { "GetSurfaceLevel", tex_GetSurfaceLevel },
    { "LockRect", tex_LockRect },
    { "UnlockRect", tex_UnlockRect },
    { "AddDirtyRect", tex_AddDirtyRect },
};

/* ---- IDirect3DSurface8 -------------------------------------------------------------------- */

static void surf_Release(Ctx *c)
{
    uint32_t o = THIS;
    Surface *s = surface_of(o);
    uint32_t n = RD32(o + 8u);
    if (s && s->container) {
        /* A texture level: the texture owns it; drop the reference it held. */
        Texture8 *t = s->container;
        if (n > 1u)
            WR32(o + 8u, n - 1u);
        uint32_t tn = RD32(t->obj + 8u);
        if (tn > 1u)
            WR32(t->obj + 8u, tn - 1u);
        stdcall_ret(c, n > 1u ? n - 1u : 1u, 1);
        return;
    }
    if (s && (s == dev.back || s == dev.depth)) {
        if (n > 1u)
            WR32(o + 8u, n - 1u);
        stdcall_ret(c, n > 1u ? n - 1u : 1u, 1);
        return;
    }
    n = com_release(o);
    if (n == 0 && s) {
        s->objs[0] = 0;
        surface8_free(s);
    }
    stdcall_ret(c, n, 1);
}

static void surf_GetContainer(Ctx *c)
{
    Surface *s = surface_of(THIS);
    uint32_t o = s && s->container ? ((Texture8 *)s->container)->obj : dev_obj;
    com_addref(o);
    WR32(ARG(2), o);
    ok_ret(c, 3);
}

static void surf_GetDesc(Ctx *c)
{
    Surface *s = surface_of(THIS);
    if (s)
        write_surface_desc(ARG(1), s);
    ok_ret(c, 2);
}

static void surf_LockRect(Ctx *c)
{
    Surface *s = surface_of(THIS);
    if (!s) {
        stdcall_ret(c, D3DERR_INVALIDCALL, 4);
        return;
    }
    lock_surface(s, ARG(1), ARG(2));
    ok_ret(c, 4);
}

static void surf_UnlockRect(Ctx *c)
{
    Surface *s = surface_of(THIS);
    if (s)
        unlock_surface(s);
    ok_ret(c, 1);
}

static const ComMethod surf_methods[] = {
    { "QueryInterface", res_QueryInterface },
    { "AddRef", res_AddRef },
    { "Release", surf_Release },
    { "GetDevice", res_GetDevice },
    { "SetPrivateData", res_SetPrivateData },
    { "GetPrivateData", res_GetPrivateData },
    { "FreePrivateData", res_FreePrivateData },
    { "GetContainer", surf_GetContainer },
    { "GetDesc", surf_GetDesc },
    { "LockRect", surf_LockRect },
    { "UnlockRect", surf_UnlockRect },
};

/* ---- vertex and index buffers ------------------------------------------------------------- */

static void buf_Release(Ctx *c)
{
    uint32_t o = THIS;
    Buffer8 *b = com_host(o);
    uint32_t n = com_release(o);
    if (n == 0 && b) {
        if (dev.stream_vb == o)
            dev.stream_vb = 0;
        if (dev.ib == o)
            dev.ib = 0;
        mirror_drop(b);
        retire(b->mem, b->last_mark);
        free(b);
    }
    stdcall_ret(c, n, 1);
}

static void vb_GetType(Ctx *c) { stdcall_ret(c, RT_VERTEXBUFFER, 1); }
static void ib_GetType(Ctx *c) { stdcall_ret(c, RT_INDEXBUFFER, 1); }

/* Lock(offset, size, &data, flags) */
/* Lock(offset, size, &data, flags): D3DLOCK_DISCARD 0x2000, NOOVERWRITE 0x1000,
 * READONLY 0x10. A writing Lock (but NOOVERWRITE, whose writes avoid the data in
 * use) of a buffer recorded draws may still read renames it. */
/* stats=1: what the game changes in a buffer drawn with copies (copy_draws)
 * between two Locks, against what the native code logged writing
 * (nstat_written): words changed, of which explained by those writes, and
 * words written but unchanged. */
static void verify_copy_buffer(const Buffer8 *b)
{
    static struct {
        const Buffer8 *b;
        uint32_t mem, size, log_at;
        uint8_t *snap;
    } snaps[8];
    int k = 0;
    while (k < 8 && snaps[k].b && snaps[k].b != b)
        k++;
    if (k == 8)
        return;
    if (snaps[k].b == b && snaps[k].mem == b->mem && snaps[k].size == b->size) {
        const uint32_t words = b->size / 4u;
        uint8_t *cov = calloc(words, 1);
        uint32_t from = snaps[k].log_at, to = nstat_wlog_n, logged = 0;
        if (to - from > NSTAT_WLOG)
            from = to - NSTAT_WLOG;
        for (uint32_t i = from; i != to; i++) {
            const uint32_t a = nstat_wlog[i % NSTAT_WLOG][0], n = nstat_wlog[i % NSTAT_WLOG][1];
            if (a + n <= b->mem || a >= b->mem + b->size)
                continue;
            logged++;
            uint32_t lo = a < b->mem ? 0 : (a - b->mem) / 4u, hi = (a + n - b->mem) / 4u;
            if (hi > words)
                hi = words;
            memset(cov + lo, 1, hi - lo);
        }
        uint32_t changed = 0, explained = 0, rewritten_same = 0, first_unexplained = 0xFFFFFFFFu;
        const uint32_t *now = (const uint32_t *)(const void *)(g_mem + b->mem);
        const uint32_t *old = (const uint32_t *)(const void *)snaps[k].snap;
        for (uint32_t w = 0; w < words; w++) {
            if (now[w] != old[w]) {
                changed++;
                if (cov[w])
                    explained++;
                else if (first_unexplained == 0xFFFFFFFFu)
                    first_unexplained = w * 4u;
            } else if (cov[w]) {
                rewritten_same++;
            }
        }
        static uint32_t reports;
        if (reports++ % 60u == 0u)
            rt_log("verify buffer %08X (%u KiB): %u words changed since the last Lock, %u by logged native "
                   "writes (%u ranges), %u unexplained (first at +%u); %u written words unchanged",
                   b->mem, b->size / 1024u, changed, explained, logged, changed - explained,
                   first_unexplained == 0xFFFFFFFFu ? 0u : first_unexplained, rewritten_same);
        free(cov);
    } else {
        free(snaps[k].snap);
        snaps[k].snap = malloc(b->size);
        snaps[k].b = b;
        snaps[k].mem = b->mem;
        snaps[k].size = b->size;
    }
    memcpy(snaps[k].snap, g_mem + b->mem, b->size);
    snaps[k].log_at = nstat_wlog_n;
}

static void buf_Lock(Ctx *c)
{
    Buffer8 *b = com_host(THIS);
    if (!b) {
        stdcall_ret(c, D3DERR_INVALIDCALL, 5);
        return;
    }
    uint32_t fl = ARG(4);
    if (g_nstat >= 2 && b->copy_draws && !(fl & 0x10u)) {
        verify_copy_buffer(b);
        nstat_watch(g_mem + b->mem, g_mem + b->mem + b->size);
    }
    if (!(fl & 0x10u))   /* not READONLY */
        b->gen++;
    st_locks[(fl & 0x2000u) ? 1 : (fl & 0x1000u) ? 2 : 0]++;
    if (g_pack_thread && !(fl & 0x1010u) && b->last_mark && !rq_passed(b->last_mark)) {
        uint32_t nm = heap_alloc(b->size ? b->size : 4u);
        if (nm) {
            if (!(fl & 0x2000u)) {   /* a plain Lock keeps the contents */
                copy_bulk(g_mem + nm, g_mem + b->mem, b->size);
                st_rename_bytes += b->size;
                /* A vertex buffer the game rewrites with plain Locks while recorded draws
                 * still read it (the lit-vertex buffer: 4.5 MiB, whole-buffer Lock every
                 * frame): its draws copy their vertices from now on, so later Locks
                 * neither wait nor copy the whole buffer. */
                if (!b->index)
                    b->copy_draws = 1;
            }
            if (b->size > st_rename_max)
                st_rename_max = b->size;
            retire(b->mem, b->last_mark);
            b->mem = nm;
            b->last_mark = 0;
            st_renames++;
            if (b->copy_draws && !mirror_of(b)) {
                mirror_drop(b);   /* (one of an older block) */
                mirror_add(b);
            }
        } else {
            rq_sync();   /* no memory for a new block: wait for the render thread */
        }
    }
    WR32(ARG(3), b->mem + ARG(1));
    ok_ret(c, 5);
}

static void buf_Unlock(Ctx *c) { ok_ret(c, 1); }

static void vb_GetDesc(Ctx *c)
{
    Buffer8 *b = com_host(THIS);
    uint32_t d = ARG(1);
    WR32(d + 0u, 100u);                      /* D3DFMT_VERTEXDATA */
    WR32(d + 4u, RT_VERTEXBUFFER);
    WR32(d + 8u, b->usage);
    WR32(d + 12u, b->pool);
    WR32(d + 16u, b->size);
    WR32(d + 20u, b->fvf_or_fmt);
    ok_ret(c, 2);
}

static void ib_GetDesc(Ctx *c)
{
    Buffer8 *b = com_host(THIS);
    uint32_t d = ARG(1);
    WR32(d + 0u, b->fvf_or_fmt);
    WR32(d + 4u, RT_INDEXBUFFER);
    WR32(d + 8u, b->usage);
    WR32(d + 12u, b->pool);
    WR32(d + 16u, b->size);
    ok_ret(c, 2);
}

static const ComMethod vb_methods[] = {
    { "QueryInterface", res_QueryInterface },
    { "AddRef", res_AddRef },
    { "Release", buf_Release },
    { "GetDevice", res_GetDevice },
    { "SetPrivateData", res_SetPrivateData },
    { "GetPrivateData", res_GetPrivateData },
    { "FreePrivateData", res_FreePrivateData },
    { "SetPriority", res_SetPriority },
    { "GetPriority", res_GetPriority },
    { "PreLoad", res_PreLoad },
    { "GetType", vb_GetType },
    { "Lock", buf_Lock },
    { "Unlock", buf_Unlock },
    { "GetDesc", vb_GetDesc },
};

static const ComMethod ib_methods[] = {
    { "QueryInterface", res_QueryInterface },
    { "AddRef", res_AddRef },
    { "Release", buf_Release },
    { "GetDevice", res_GetDevice },
    { "SetPrivateData", res_SetPrivateData },
    { "GetPrivateData", res_GetPrivateData },
    { "FreePrivateData", res_FreePrivateData },
    { "SetPriority", res_SetPriority },
    { "GetPriority", res_GetPriority },
    { "PreLoad", res_PreLoad },
    { "GetType", ib_GetType },
    { "Lock", buf_Lock },
    { "Unlock", buf_Unlock },
    { "GetDesc", ib_GetDesc },
};

/* ---- entry point ------------------------------------------------------------------------- */

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

SHIM(d3d8, Direct3DCreate8)
{
    rt_log("Direct3DCreate8(SDK %u)", ARG(0));
    if (!d3d_obj) {
        d3d_vtbl = com_vtable("IDirect3D8", d3d_methods, COUNT(d3d_methods));
        dev_vtbl = com_vtable("IDirect3DDevice8", dev_methods, COUNT(dev_methods));
        tex_vtbl = com_vtable("IDirect3DTexture8", tex_methods, COUNT(tex_methods));
        surf_vtbl = com_vtable("IDirect3DSurface8", surf_methods, COUNT(surf_methods));
        vb_vtbl = com_vtable("IDirect3DVertexBuffer8", vb_methods, COUNT(vb_methods));
        ib_vtbl = com_vtable("IDirect3DIndexBuffer8", ib_methods, COUNT(ib_methods));
        d3d_obj = com_new(d3d_vtbl, NULL);
    }
    stdcall_ret(c, d3d_obj, 1);
}

_Static_assert(COUNT(d3d_methods) == 16, "IDirect3D8 has 16 methods");
_Static_assert(COUNT(dev_methods) == 97, "IDirect3DDevice8 has 97 methods");
_Static_assert(COUNT(tex_methods) == 19, "IDirect3DTexture8 has 19 methods");
_Static_assert(COUNT(surf_methods) == 11, "IDirect3DSurface8 has 11 methods");
_Static_assert(COUNT(vb_methods) == 14, "IDirect3DVertexBuffer8 has 14 methods");
