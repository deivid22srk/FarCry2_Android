/* Host implementation of the 15 D3DX9 functions Dunia.dll imports from
 * d3dx9_38.dll (Microsoft, June 2008). The DLL is not part of the game
 * install and is never shipped; these functions run the same work on the host
 * on top of the D3D9 bridge (runtime/d3d9_bridge.cpp).
 *
 * What is faithful:
 *   - the four math entry points (planes, transpose, inverse) are exact;
 *   - DDS and BMP/TGA image loading, DXT1/3/5 decoding, texture and cube
 *     texture creation, surface-to-surface copies, mip filtering and
 *     D3DXFillTexture;
 *   - shader input semantics and SH projection of a cube map.
 * What is deliberately loud: unsupported source formats, volume textures,
 * D3DXSaveTextureToFileInMemory (it would have to hand back an ID3DXBuffer,
 * which this runtime does not implement) and any format conversion the code
 * below does not know. They log the call and fail with E_NOTIMPL/E_FAIL
 * instead of silently returning garbage.
 *
 * All 15 are WINAPI (__stdcall): the callee pops the return address and its
 * arguments (RET with the argument count). The recompiled callers push the
 * arguments and do not adjust esp afterwards, so a cdecl return here would
 * leave the guest stack short and corrupt whatever the caller reads next. */
#include <d3d9.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

#include "d3d9_bridge.h"

// BC (DXT) emulation shared with the D3D9 bridge (runtime/d3d9_bcemu.cpp):
// create DXT textures as A8R8G8B8 on a host whose Vulkan driver lacks
// textureCompressionBC, and keep reporting the guest DXT format.
extern "C" uint32_t bc_host_format(IDirect3DDevice9 *d, uint32_t fmt);
extern "C" void bc_register_texture(void *tex, uint32_t guest_fmt);

extern "C" {
#include "rt.h"
}

#define D3DX_DEFAULT_ ((UINT)0xFFFFFFFFu)
#define D3DX_FILTER_NONE 0x00000001u
#define D3DXIFF_BMP 3
#define D3DXIFF_TGA 2
#define D3DXIFF_PNG 4
#define D3DXIFF_JPG 1
#define D3DXIFF_DDS 5
#define D3DXSH_MAXORDER 6

struct D3DXIMAGE_INFO {
    UINT Width, Height, Depth, MipLevels;
    D3DFORMAT Format;
    D3DRESOURCETYPE ResourceType;
    int ImageFileFormat;
};

struct D3DXSEMANTIC { UINT Usage, UsageIndex; };

static inline uint32_t rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t rd16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }

// ---------------------------------------------------------------- math
struct Vec4 { float x, y, z, w; };

static void plane_normalize(float *o, const float *p) {
    double n = std::sqrt((double)p[0] * p[0] + (double)p[1] * p[1] + (double)p[2] * p[2]);
    if (n < 1e-12) { o[0] = o[1] = o[2] = o[3] = 0; return; }
    double inv = 1.0 / n;
    o[0] = (float)(p[0] * inv); o[1] = (float)(p[1] * inv);
    o[2] = (float)(p[2] * inv); o[3] = (float)(p[3] * inv);
}

/* pOut = pP * pM, row-vector convention, exactly as the D3DX documentation
 * defines it. */
static void plane_transform(float *o, const float *p, const float *m) {
    float r[4];
    for (int j = 0; j < 4; j++)
        r[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j] + p[2] * m[2 * 4 + j] + p[3] * m[3 * 4 + j];
    memcpy(o, r, sizeof r);
}

static void matrix_transpose(float *o, const float *m) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) o[j * 4 + i] = m[i * 4 + j];
}

/* General 4x4 inverse by cofactors (D3DXMatrixInverse semantics: returns NULL
 * and writes 0 to the determinant output when the matrix is singular). */
static double cofactor3(const double a[4][4], int row, int col) {
    double c[3][3];
    int ci = 0;
    for (int i = 0; i < 4; i++) {
        if (i == row) continue;
        int cj = 0;
        for (int j = 0; j < 4; j++) {
            if (j == col) continue;
            c[ci][cj++] = a[i][j];
        }
        ci++;
    }
    return c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1])
         - c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0])
         + c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0]);
}

static bool matrix_inverse(float *o, const float *m, float *detOut) {
    double a[4][4], cof[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) a[i][j] = m[i * 4 + j];
    double det = 0;
    for (int j = 0; j < 4; j++) {
        cof[0][j] = cofactor3(a, 0, j);
        det += a[0][j] * ((j & 1) ? -cof[0][j] : cof[0][j]);
    }
    if (std::fabs(det) < 1e-20) {
        if (detOut) *detOut = 0;
        return false;
    }
    for (int i = 1; i < 4; i++)
        for (int j = 0; j < 4; j++) cof[i][j] = cofactor3(a, i, j);
    double idet = 1.0 / det;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            o[i * 4 + j] = (float)(((i + j) & 1 ? -cof[j][i] : cof[j][i]) * idet);
    if (detOut) *detOut = (float)det;
    return true;
}

extern "C" void shim_D3DXPlaneNormalize(CPU *c) {
    plane_normalize((float *)GPTR(ARG(0)), (const float *)GPTR(ARG(1)));
    RET(ARG(0), 2);
}

extern "C" void shim_D3DXPlaneTransform(CPU *c) {
    plane_transform((float *)GPTR(ARG(0)), (const float *)GPTR(ARG(1)), (const float *)GPTR(ARG(2)));
    RET(ARG(0), 3);
}

extern "C" void shim_D3DXMatrixTranspose(CPU *c) {
    matrix_transpose((float *)GPTR(ARG(0)), (const float *)GPTR(ARG(1)));
    RET(ARG(0), 2);
}

extern "C" void shim_D3DXMatrixInverse(CPU *c) {
    float det = 0;
    bool ok = matrix_inverse((float *)GPTR(ARG(0)), (const float *)GPTR(ARG(2)), &det);
    if (ARG(1)) *(float *)GPTR(ARG(1)) = det;
    RET(ok ? ARG(0) : 0, 3);
}

// ---------------------------------------------------------------- formats
static uint32_t pack_argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static bool is_compressed(D3DFORMAT f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

static int block_bytes(D3DFORMAT f) {
    return (f == D3DFMT_DXT1 || f == D3DFMT_DXT2) ? 8 : 16;
}

static int bytes_per_pixel(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_G16R16: case D3DFMT_R32F: case D3DFMT_G16R16F: return 4;
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5: case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: case D3DFMT_A8L8: case D3DFMT_R16F: return 2;
    case D3DFMT_A8: case D3DFMT_L8: return 1;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F:
    case D3DFMT_G32R32F: return 8;
    case D3DFMT_A32B32G32R32F: return 16;
    default: return 0;
    }
}

/* Half-float <-> 8-bit helpers. The engine's post-processing chain ends in a
 * floating-point render target, and the final image reaches the back buffer
 * through D3DXLoadSurfaceFromSurface, whose source is then one of the F16/F32
 * formats. Without a decoder for those the copy fails and nothing is ever
 * presented -- the story-mode black screen has exactly that shape. */
static inline float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1f, man = h & 0x3ff;
    uint32_t f;
    if (!exp) f = sign | (man << 13);                        /* zero / denormal */
    else if (exp == 31) f = sign | 0x7f800000u | (man << 13);/* inf / nan */
    else f = sign | ((exp + 112u) << 23) | (man << 13);
    float out;
    memcpy(&out, &f, 4);
    return out;
}

static inline uint16_t float_to_half(float v) {
    uint32_t x;
    memcpy(&x, &v, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = (int32_t)((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) return (uint16_t)sign;
    if (e >= 31) return (uint16_t)(sign | 0x7c00u);
    return (uint16_t)(sign | ((uint32_t)e << 10) | (m >> 13));
}

static inline uint8_t f32_to_u8(float v) {
    if (!(v == v)) return 0;                    /* NaN */
    if (v <= 0) return 0;
    if (v >= 1) return 255;
    return (uint8_t)(v * 255.0f + 0.5f);
}

static inline uint8_t u16_to_u8(uint16_t v) { return (uint8_t)(v >> 8); }

static D3DFORMAT fmt_from_dds(const uint8_t *pf) {
    uint32_t flags = rd32(pf + 4), fourcc = rd32(pf + 8);
    uint32_t bits = rd32(pf + 12), am = rd32(pf + 20);
    if (flags & 0x4) {  // DDPF_FOURCC
        switch (fourcc) {
        case 0x31545844: return D3DFMT_DXT1;  // DXT1
        case 0x32545844: return D3DFMT_DXT2;
        case 0x33545844: return D3DFMT_DXT3;
        case 0x34545844: return D3DFMT_DXT4;
        case 0x35545844: return D3DFMT_DXT5;
        default: return D3DFMT_UNKNOWN;
        }
    }
    if (flags & 0x40) return D3DFMT_A8R8G8B8;  // DDPF_RGB with alpha
    if (flags & 0x20000) {  // DDPF_LUMINANCE
        return bits == 8 ? D3DFMT_L8 : bits == 16 ? D3DFMT_A8L8 : D3DFMT_UNKNOWN;
    }
    if (flags & 0x2) return D3DFMT_A8;  // DDPF_ALPHA
    switch (bits) {
    case 32: return (am & 0xFF000000u) ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
    case 24: return D3DFMT_R8G8B8;
    case 16: {
        uint32_t rm = rd32(pf + 16), gm = rd32(pf + 20);
        if (rm == 0xF800 && gm == 0x07E0) return D3DFMT_R5G6B5;
        if (rm == 0x7C00 && gm == 0x03E0) return D3DFMT_A1R5G5B5;
        if (rm == 0x0F00 && gm == 0x00F0) return D3DFMT_A4R4G4B4;
        return D3DFMT_UNKNOWN;
    }
    case 8: return D3DFMT_L8;
    default: return D3DFMT_UNKNOWN;
    }
}

struct Dds {
    uint32_t w = 0, h = 0, depth = 1, mips = 1;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    bool cube = false, volume = false;
    uint32_t pitch = 0;  // 0 = compute
    uint32_t data = 0;
};

static bool parse_dds(const uint8_t *p, uint32_t n, Dds *d) {
    if (n < 128 || memcmp(p, "DDS ", 4)) return false;
    const uint8_t *h = p + 4;
    if (rd32(h) != 124) return false;
    uint32_t flags = rd32(h + 4);
    d->h = rd32(h + 8);
    d->w = rd32(h + 12);
    d->pitch = (flags & 0x8) ? rd32(h + 16) : 0;
    d->depth = (flags & 0x800000) ? rd32(h + 20) : 1;
    d->mips = (flags & 0x20000) ? rd32(h + 24) : 1;
    if (!d->mips) d->mips = 1;
    d->fmt = fmt_from_dds(h + 72);
    uint32_t caps2 = rd32(h + 108);
    d->cube = (caps2 & 0x200) != 0;
    d->volume = (caps2 & 0x200000) != 0;
    d->data = 128;
    if (!d->w || !d->h || d->fmt == D3DFMT_UNKNOWN) return false;
    return true;
}

/* Decodes one DXT block row set into ARGB pixels. */
static void decode_bc(const uint8_t *src, int w, int h, int srcPitch, D3DFORMAT f, uint32_t *out, int outStride) {
    bool is1 = (f == D3DFMT_DXT1 || f == D3DFMT_DXT2);
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            const uint8_t *blk = src + by * srcPitch + bx * (is1 ? 8 : 16);
            uint32_t c0 = rd16(blk), c1 = rd16(blk + 2);
            auto expand = [](uint32_t c) {
                uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
                return pack_argb(0xFF, (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
            };
            auto lerp = [](uint32_t x, uint32_t y, int wx, int wy) {
                return pack_argb((((x >> 24) & 0xFF) * wx + ((y >> 24) & 0xFF) * wy) / (wx + wy),
                                 (((x >> 16) & 0xFF) * wx + ((y >> 16) & 0xFF) * wy) / (wx + wy),
                                 (((x >> 8) & 0xFF) * wx + ((y >> 8) & 0xFF) * wy) / (wx + wy),
                                 ((x & 0xFF) * wx + (y & 0xFF) * wy) / (wx + wy));
            };
            uint32_t pal[4];
            pal[0] = expand(c0);
            pal[1] = expand(c1);
            if (c0 > c1 || !is1) {
                pal[2] = lerp(pal[0], pal[1], 2, 1);
                pal[3] = lerp(pal[0], pal[1], 1, 2);
            } else {
                pal[2] = lerp(pal[0], pal[1], 1, 1);
                pal[3] = 0u;  // transparent black
            }
            uint32_t bits = rd32(blk + 4);
            uint8_t alpha[16];
            memset(alpha, 0xFF, sizeof alpha);
            if (f == D3DFMT_DXT3 || f == D3DFMT_DXT4) {
                for (int i = 0; i < 16; i++)
                    alpha[i] = (uint8_t)(((blk[i / 2] >> ((i & 1) * 4)) & 0xF) * 17);
            } else if (f == D3DFMT_DXT5) {
                uint8_t a0 = blk[0], a1 = blk[1];
                uint64_t abits = 0;
                for (int i = 0; i < 8; i++) abits |= (uint64_t)blk[2 + i] << (8 * i);
                uint8_t atab[8];
                atab[0] = a0; atab[1] = a1;
                if (a0 > a1) {
                    for (int i = 0; i < 6; i++) atab[2 + i] = (uint8_t)(((6 - i) * a0 + (1 + i) * a1) / 7);
                } else {
                    for (int i = 0; i < 4; i++) atab[2 + i] = (uint8_t)(((4 - i) * a0 + (1 + i) * a1) / 5);
                    atab[6] = 0; atab[7] = 255;
                }
                for (int i = 0; i < 16; i++) alpha[i] = atab[(abits >> (3 * i)) & 7];
            }
            for (int y = 0; y < 4; y++) {
                int py = by * 4 + y;
                if (py >= h) break;
                for (int x = 0; x < 4; x++) {
                    int px = bx * 4 + x;
                    if (px >= w) break;
                    uint32_t idx = (bits >> (2 * (y * 4 + x))) & 3;
                    uint32_t v = pal[idx];
                    out[py * outStride + px] = (v & 0x00FFFFFFu) | ((uint32_t)alpha[y * 4 + x] << 24);
                }
            }
        }
    }
}

/* Decodes an uncompressed DDS level (or any raw image) to ARGB. */
static bool decode_raw(const uint8_t *src, int w, int h, int pitch, D3DFORMAT f, uint32_t *out) {
    int bpp = bytes_per_pixel(f);
    if (!bpp) return false;
    if (!pitch) pitch = w * bpp;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = src + y * pitch;
        for (int x = 0; x < w; x++) {
            const uint8_t *p = row + x * bpp;
            uint32_t v = 0;
            switch (f) {
            case D3DFMT_A8R8G8B8: v = rd32(p); break;
            case D3DFMT_X8R8G8B8: v = rd32(p) | 0xFF000000u; break;
            case D3DFMT_A8B8G8R8: v = pack_argb(p[3], p[0], p[1], p[2]); break;
            case D3DFMT_X8B8G8R8: v = pack_argb(0xFF, p[0], p[1], p[2]); break;
            case D3DFMT_R8G8B8: v = pack_argb(0xFF, p[2], p[1], p[0]); break;
            case D3DFMT_R5G6B5: {
                uint32_t c = rd16(p);
                v = pack_argb(0xFF, ((c >> 11) & 0x1F) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3);
                break;
            }
            case D3DFMT_A1R5G5B5: {
                uint32_t c = rd16(p);
                v = pack_argb((c & 0x8000) ? 0xFF : 0, ((c >> 10) & 0x1F) << 3, ((c >> 5) & 0x1F) << 3, (c & 0x1F) << 3);
                break;
            }
            case D3DFMT_A4R4G4B4: {
                uint32_t c = rd16(p);
                v = pack_argb(((c >> 12) & 0xF) * 17, ((c >> 8) & 0xF) * 17, ((c >> 4) & 0xF) * 17, (c & 0xF) * 17);
                break;
            }
            case D3DFMT_A8: v = pack_argb(p[0], 0, 0, 0); break;
            case D3DFMT_L8: v = pack_argb(0xFF, p[0], p[0], p[0]); break;
            case D3DFMT_A8L8: v = pack_argb(p[1], p[0], p[0], p[0]); break;
            case D3DFMT_G16R16:
                v = pack_argb(0xFF, u16_to_u8(rd16(p + 2)), u16_to_u8(rd16(p)), 0);
                break;
            case D3DFMT_A16B16G16R16:
                v = pack_argb(u16_to_u8(rd16(p + 6)), u16_to_u8(rd16(p + 4)),
                              u16_to_u8(rd16(p + 2)), u16_to_u8(rd16(p)));
                break;
            case D3DFMT_R16F:
                v = pack_argb(0xFF, f32_to_u8(half_to_float(rd16(p))), 0, 0);
                break;
            case D3DFMT_G16R16F:
                v = pack_argb(0xFF, f32_to_u8(half_to_float(rd16(p + 2))),
                              f32_to_u8(half_to_float(rd16(p))), 0);
                break;
            case D3DFMT_A16B16G16R16F:
                v = pack_argb(f32_to_u8(half_to_float(rd16(p + 6))),
                              f32_to_u8(half_to_float(rd16(p + 4))),
                              f32_to_u8(half_to_float(rd16(p + 2))),
                              f32_to_u8(half_to_float(rd16(p))));
                break;
            case D3DFMT_R32F: {
                float f;
                memcpy(&f, p, 4);
                v = pack_argb(0xFF, f32_to_u8(f), 0, 0);
                break;
            }
            case D3DFMT_G32R32F: {
                float r, g;
                memcpy(&r, p, 4);
                memcpy(&g, p + 4, 4);
                v = pack_argb(0xFF, f32_to_u8(r), f32_to_u8(g), 0);
                break;
            }
            case D3DFMT_A32B32G32R32F: {
                float b, g, r, a;
                memcpy(&b, p, 4);
                memcpy(&g, p + 4, 4);
                memcpy(&r, p + 8, 4);
                memcpy(&a, p + 12, 4);
                v = pack_argb(f32_to_u8(a), f32_to_u8(r), f32_to_u8(g), f32_to_u8(b));
                break;
            }
            default: return false;
            }
            out[y * w + x] = v;
        }
    }
    return true;
}

static void write_argb(uint8_t *dst, int pitch, int w, int h, const uint32_t *src, D3DFORMAT f) {
    for (int y = 0; y < h; y++) {
        uint8_t *row = dst + y * pitch;
        for (int x = 0; x < w; x++) {
            uint32_t v = src[y * w + x];
            uint32_t a = (v >> 24) & 0xFF, r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
            uint8_t *p = row + x * std::max(1, bytes_per_pixel(f));
            switch (f) {
            case D3DFMT_A8R8G8B8: { uint32_t q = v; memcpy(p, &q, 4); break; }
            case D3DFMT_X8R8G8B8: { uint32_t q = v | 0xFF000000u; memcpy(p, &q, 4); break; }
            case D3DFMT_A8B8G8R8: p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b; p[3] = (uint8_t)a; break;
            case D3DFMT_X8B8G8R8: p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b; p[3] = 0xFF; break;
            case D3DFMT_R8G8B8: p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b; break;
            case D3DFMT_R5G6B5: { uint16_t q = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); memcpy(p, &q, 2); break; }
            case D3DFMT_A1R5G5B5: { uint16_t q = (uint16_t)((a >= 128 ? 0x8000 : 0) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)); memcpy(p, &q, 2); break; }
            case D3DFMT_A4R4G4B4: { uint16_t q = (uint16_t)(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)); memcpy(p, &q, 2); break; }
            case D3DFMT_A8: p[0] = (uint8_t)a; break;
            case D3DFMT_L8: p[0] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8); break;
            case D3DFMT_A8L8: p[0] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8); p[1] = (uint8_t)a; break;
            case D3DFMT_G16R16: {
                uint16_t q[2] = { (uint16_t)(r << 8), (uint16_t)(g << 8) };
                memcpy(p, q, 4);
                break;
            }
            case D3DFMT_A16B16G16R16: {
                uint16_t q[4] = { (uint16_t)(b << 8), (uint16_t)(g << 8),
                                  (uint16_t)(r << 8), (uint16_t)(a << 8) };
                memcpy(p, q, 8);
                break;
            }
            case D3DFMT_R16F: {
                uint16_t q = float_to_half(r / 255.0f);
                memcpy(p, &q, 2);
                break;
            }
            case D3DFMT_G16R16F: {
                uint16_t q[2] = { float_to_half(r / 255.0f), float_to_half(g / 255.0f) };
                memcpy(p, q, 4);
                break;
            }
            case D3DFMT_A16B16G16R16F: {
                uint16_t q[4] = { float_to_half(b / 255.0f), float_to_half(g / 255.0f),
                                  float_to_half(r / 255.0f), float_to_half(a / 255.0f) };
                memcpy(p, q, 8);
                break;
            }
            case D3DFMT_R32F: {
                float q = r / 255.0f;
                memcpy(p, &q, 4);
                break;
            }
            case D3DFMT_G32R32F: {
                float q[2] = { r / 255.0f, g / 255.0f };
                memcpy(p, q, 8);
                break;
            }
            case D3DFMT_A32B32G32R32F: {
                float q[4] = { b / 255.0f, g / 255.0f, r / 255.0f, a / 255.0f };
                memcpy(p, q, 16);
                break;
            }
            default: break;
            }
        }
    }
}

static uint32_t surface_pitch(D3DSURFACE_DESC *d) {
    if (is_compressed(d->Format)) return std::max(1u, (d->Width + 3) / 4) * block_bytes(d->Format);
    return d->Width * bytes_per_pixel(d->Format);
}

/* Copies/decodes `w x h` ARGB pixels into a locked surface rectangle. */
static void blit_argb(IDirect3DSurface9 *s, const RECT *dstRect, const uint32_t *px, int w, int h, D3DFORMAT fmt) {
    D3DLOCKED_RECT lr;
    RECT r = dstRect ? *dstRect : RECT{0, 0, (LONG)w, (LONG)h};
    HRESULT hr = s->LockRect(&lr, &r, 0);
    if (FAILED(hr)) { LOG("D3DX: LockRect failed (%dx%d fmt %d hr %08x)", (int)(r.right - r.left), (int)(r.bottom - r.top), (int)fmt, (unsigned)hr); return; }
    uint8_t *base = (uint8_t *)lr.pBits;
    int rw = (int)(r.right - r.left), rh = (int)(r.bottom - r.top);
    if (is_compressed(fmt)) {
        LOG("D3DX: cannot upload ARGB into a compressed surface");
    } else {
        for (int y = 0; y < std::min(h, rh); y++)
            write_argb(base + y * lr.Pitch, lr.Pitch, std::min(w, rw), 1,
                       px + y * w, fmt);
    }
    s->UnlockRect();
}

/* ---------------------------------------------------------------- image files */
static bool load_dds_level(const uint8_t *p, uint32_t n, const Dds *d, uint32_t face, uint32_t level,
                           std::vector<uint32_t> *out, int *ow, int *oh) {
    uint32_t w = std::max(1u, d->w >> level), h = std::max(1u, d->h >> level);
    uint32_t offset = d->data;
    auto level_size = [&](uint32_t lw, uint32_t lh) {
        if (is_compressed(d->fmt)) return (uint32_t)std::max(1u, (lw + 3) / 4) * std::max(1u, (lh + 3) / 4) * block_bytes(d->fmt);
        return lw * lh * bytes_per_pixel(d->fmt);
    };
    uint32_t faces = d->cube ? 6 : 1;
    uint32_t lw = d->w, lh = d->h;
    for (uint32_t l = 0; l < level; l++) {
        uint32_t sz = level_size(lw, lh);
        offset += sz * faces;
        lw = std::max(1u, lw >> 1); lh = std::max(1u, lh >> 1);
    }
    uint32_t sz = level_size(w, h);
    if (offset + (uint64_t)sz * (face + 1) > n) return false;
    const uint8_t *src = p + offset + (uint64_t)sz * face;
    out->assign((size_t)w * h, 0);
    if (is_compressed(d->fmt)) {
        int pitch = std::max(1u, (w + 3) / 4) * block_bytes(d->fmt);
        decode_bc(src, (int)w, (int)h, pitch, d->fmt, out->data(), (int)w);
    } else {
        int pitch = d->pitch ? d->pitch : (int)(w * bytes_per_pixel(d->fmt));
        if (!decode_raw(src, (int)w, (int)h, pitch, d->fmt, out->data())) return false;
    }
    *ow = (int)w; *oh = (int)h;
    return true;
}

static bool load_bmp(const uint8_t *p, uint32_t n, std::vector<uint32_t> *out, int *w, int *h, D3DFORMAT *fmt) {
    if (n < 54 || p[0] != 'B' || p[1] != 'M') return false;
    uint32_t off = rd32(p + 10);
    uint32_t hdr = rd32(p + 14);
    if (hdr < 40) return false;
    int32_t bw = (int32_t)rd32(p + 18), bh = (int32_t)rd32(p + 22);
    uint16_t bpp = rd16(p + 28);
    uint32_t comp = rd32(p + 30);
    if (bw <= 0 || bh == 0 || (bpp != 24 && bpp != 32) || comp != 0) return false;
    bool topdown = bh < 0;
    if (topdown) bh = -bh;
    int bppb = bpp / 8;
    uint32_t pitch = ((bw * bppb + 3) & ~3u);
    if (off + (uint64_t)pitch * bh > n) return false;
    out->assign((size_t)bw * bh, 0);
    for (int y = 0; y < bh; y++) {
        const uint8_t *row = p + off + (uint64_t)(topdown ? y : bh - 1 - y) * pitch;
        for (int x = 0; x < bw; x++) {
            const uint8_t *q = row + x * bppb;
            (*out)[(size_t)y * bw + x] = pack_argb(bppb == 4 ? q[3] : 0xFF, q[2], q[1], q[0]);
        }
    }
    *w = bw; *h = bh; *fmt = D3DFMT_A8R8G8B8;
    return true;
}

static bool load_tga(const uint8_t *p, uint32_t n, std::vector<uint32_t> *out, int *w, int *h, D3DFORMAT *fmt) {
    if (n < 18) return false;
    uint8_t idlen = p[0], cmap = p[1], type = p[2];
    int bw = rd16(p + 12), bh = rd16(p + 14);
    uint8_t bpp = p[16];
    if (cmap || (type != 2 && type != 10) || (bpp != 24 && bpp != 32) || bw <= 0 || bh <= 0) return false;
    uint32_t off = 18 + idlen;
    int bytes = bpp / 8;
    out->assign((size_t)bw * bh, 0);
    uint32_t o = off;
    int x = 0, y = 0;
    while (y < bh && o + bytes <= n) {
        uint8_t hdr = 0;
        int count = 1;
        bool rle = type == 10;
        if (rle) {
            if (o >= n) break;
            hdr = p[o++];
            count = (hdr & 0x7F) + 1;
            if (hdr & 0x80) {
                if (o + bytes > n) break;
                uint32_t v = pack_argb(bytes == 4 ? p[o + 3] : 0xFF, p[o + 2], p[o + 1], p[o]);
                o += bytes;
                for (int i = 0; i < count && y < bh; i++, x++) {
                    if (x >= bw) { x = 0; y++; }
                    if (y < bh) (*out)[(size_t)y * bw + x] = v;
                }
                continue;
            }
        }
        for (int i = 0; i < count && y < bh; i++) {
            if (o + bytes > n) break;
            (*out)[(size_t)y * bw + x] = pack_argb(bytes == 4 ? p[o + 3] : 0xFF, p[o + 2], p[o + 1], p[o]);
            o += bytes;
            if (++x >= bw) { x = 0; y++; }
        }
        (void)hdr;
    }
    *w = bw; *h = bh; *fmt = D3DFMT_A8R8G8B8;
    return true;
}

/* PNG/JPEG: only the header is parsed (for D3DXGetImageInfoFromFileInMemory).
 * The game's art is DDS today; a PNG texture would be reported, not decoded. */
static bool png_size(const uint8_t *p, uint32_t n, int *w, int *h) {
    if (n < 24 || p[0] != 0x89 || memcmp(p + 1, "PNG", 3)) return false;
    *w = (int)rd32(p + 16);
    *h = (int)rd32(p + 20);
    return *w > 0 && *h > 0;
}

static bool jpg_size(const uint8_t *p, uint32_t n, int *w, int *h) {
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;
    uint32_t i = 2;
    while (i + 9 < n) {
        if (p[i] != 0xFF) { i++; continue; }
        uint8_t m = p[i + 1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        uint16_t len = (uint16_t)((p[i + 2] << 8) | p[i + 3]);
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = (p[i + 5] << 8) | p[i + 6];
            *w = (p[i + 7] << 8) | p[i + 8];
            return *w > 0 && *h > 0;
        }
        i += 2 + len;
    }
    return false;
}

// ---------------------------------------------------------------- shims
extern "C" void shim_D3DXGetImageInfoFromFileInMemory(CPU *c) {
    /* HRESULT D3DXGetImageInfoFromFileInMemory(LPCVOID pSrcData, UINT SrcDataSize,
     *                                          D3DXIMAGE_INFO *pImageInfo);
     * The data pointer/size come first and the output struct last. Reading them
     * the other way round sent 0x1080 (the image size) in as the source buffer,
     * i.e. inside the PROT_NONE guard page at the bottom of MEM, and the first
     * UI texture load took SIGSEGV. */
    const uint8_t *p = (const uint8_t *)GPTR(ARG(0));
    uint32_t n = ARG(1);
    D3DXIMAGE_INFO *info = (D3DXIMAGE_INFO *)GPTR(ARG(2));
    HRESULT hr = D3DERR_INVALIDCALL;
    memset(info, 0, sizeof *info);
    Dds d;
    if (parse_dds(p, n, &d)) {
        info->Width = d.w; info->Height = d.h; info->Depth = d.depth;
        info->MipLevels = d.mips; info->Format = d.fmt;
        info->ResourceType = d.cube ? D3DRTYPE_CUBETEXTURE : d.volume ? D3DRTYPE_VOLUMETEXTURE : D3DRTYPE_TEXTURE;
        info->ImageFileFormat = D3DXIFF_DDS;
        hr = S_OK;
    } else {
        int w = 0, h = 0;
        D3DFORMAT f = D3DFMT_UNKNOWN;
        std::vector<uint32_t> px;
        if (load_bmp(p, n, &px, &w, &h, &f)) {
            info->Width = w; info->Height = h; info->Depth = 1; info->MipLevels = 1;
            info->Format = f; info->ResourceType = D3DRTYPE_TEXTURE; info->ImageFileFormat = D3DXIFF_BMP;
            hr = S_OK;
        } else if (load_tga(p, n, &px, &w, &h, &f)) {
            info->Width = w; info->Height = h; info->Depth = 1; info->MipLevels = 1;
            info->Format = f; info->ResourceType = D3DRTYPE_TEXTURE; info->ImageFileFormat = D3DXIFF_TGA;
            hr = S_OK;
        } else if (png_size(p, n, &w, &h)) {
            info->Width = w; info->Height = h; info->Depth = 1; info->MipLevels = 1;
            info->Format = D3DFMT_A8R8G8B8; info->ResourceType = D3DRTYPE_TEXTURE; info->ImageFileFormat = D3DXIFF_PNG;
            hr = S_OK;
        } else if (jpg_size(p, n, &w, &h)) {
            info->Width = w; info->Height = h; info->Depth = 1; info->MipLevels = 1;
            info->Format = D3DFMT_A8R8G8B8; info->ResourceType = D3DRTYPE_TEXTURE; info->ImageFileFormat = D3DXIFF_JPG;
            hr = S_OK;
        }
    }
    if (FAILED(hr)) LOG("D3DXGetImageInfoFromFileInMemory: unknown image (%u bytes)", n);
    RET(hr, 3);
}

static IDirect3DTexture9 *create_tex(IDirect3DDevice9 *dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, D3DFORMAT *hostFmt) {
    IDirect3DTexture9 *tex = nullptr;
    /* On a driver without textureCompressionBC a DXT texture must be created
     * as A8R8G8B8 and the compressed source decoded into it (see bc_host_format);
     * otherwise CreateTexture fails and the guest is handed NULL. */
    D3DFORMAT hf = (D3DFORMAT)bc_host_format(dev, (uint32_t)fmt);
    HRESULT hr = dev->CreateTexture(w, h, levels, usage, hf, pool, &tex, nullptr);
    if (FAILED(hr)) { LOG("D3DX: CreateTexture %ux%u fmt %d levels %u -> %08x", w, h, (int)fmt, levels, (unsigned)hr); return nullptr; }
    if (hf != fmt) bc_register_texture(tex, (uint32_t)fmt);
    if (hostFmt) *hostFmt = hf;
    return tex;
}

extern "C" void shim_D3DXCreateTextureFromFileInMemoryEx(CPU *c) {
    IDirect3DDevice9 *dev = (IDirect3DDevice9 *)com_host(ARG(0));
    const uint8_t *p = (const uint8_t *)GPTR(ARG(1));
    uint32_t n = ARG(2);
    UINT reqW = ARG(3), reqH = ARG(4), reqMips = ARG(5);
    DWORD usage = ARG(6);
    D3DFORMAT reqFmt = (D3DFORMAT)ARG(7);
    D3DPOOL pool = (D3DPOOL)ARG(8);
    D3DXIMAGE_INFO *srcInfo = (D3DXIMAGE_INFO *)GPTR(ARG(12));
    uint32_t outTex = ARG(14);

    Dds d;
    if (!parse_dds(p, n, &d)) {
        std::vector<uint32_t> px;
        int w = 0, h = 0;
        D3DFORMAT f = D3DFMT_UNKNOWN;
        bool ok = load_bmp(p, n, &px, &w, &h, &f) || load_tga(p, n, &px, &w, &h, &f);
        if (!ok) {
            LOG("D3DXCreateTextureFromFileInMemoryEx: unsupported image (%u bytes); only DDS/BMP/TGA are decoded", n);
            M32(outTex) = 0;
            RET(E_NOTIMPL, 15);
        }
        D3DFORMAT fmt = reqFmt == D3DFMT_UNKNOWN ? D3DFMT_A8R8G8B8 : reqFmt;
        D3DFORMAT hf;
        IDirect3DTexture9 *tex = create_tex(dev, (UINT)w, (UINT)h, 1, usage, fmt, pool, &hf);
        if (!tex) { M32(outTex) = 0; RET(E_FAIL, 15); }
        IDirect3DSurface9 *surf = nullptr;
        tex->GetSurfaceLevel(0, &surf);
        if (surf) {
            blit_argb(surf, nullptr, px.data(), w, h, hf);
            surf->Release();
        }
        if (srcInfo) {
            memset(srcInfo, 0, sizeof *srcInfo);
            srcInfo->Width = w; srcInfo->Height = h; srcInfo->Depth = 1; srcInfo->MipLevels = 1;
            srcInfo->Format = f; srcInfo->ResourceType = D3DRTYPE_TEXTURE; srcInfo->ImageFileFormat = D3DXIFF_BMP;
        }
        M32(outTex) = d3d9_wrap(tex, IFACE_IDirect3DTexture9);
        RET(S_OK, 15);
    }

    if (d.cube || d.volume) {
        LOG("D3DXCreateTextureFromFileInMemoryEx: cube/volume DDS needs the matching D3DX entry point");
        M32(outTex) = 0;
        RET(E_FAIL, 15);
    }
    UINT w = reqW == D3DX_DEFAULT_ ? d.w : reqW;
    UINT h = reqH == D3DX_DEFAULT_ ? d.h : reqH;
    UINT levels = reqMips == D3DX_DEFAULT_ ? d.mips : reqMips;
    if (levels > d.mips) levels = d.mips;
    if (!levels) levels = 1;
    D3DFORMAT fmt = reqFmt == D3DFMT_UNKNOWN ? d.fmt : reqFmt;
    D3DFORMAT hf;
    IDirect3DTexture9 *tex = create_tex(dev, w, h, levels, usage, fmt, pool, &hf);
    if (!tex) { M32(outTex) = 0; RET(E_FAIL, 15); }
    for (UINT l = 0; l < levels; l++) {
        IDirect3DSurface9 *surf = nullptr;
        if (FAILED(tex->GetSurfaceLevel(l, &surf)) || !surf) continue;
        D3DSURFACE_DESC sd;
        surf->GetDesc(&sd);
        std::vector<uint32_t> px;
        int lw = 0, lh = 0;
        if (!load_dds_level(p, n, &d, 0, l, &px, &lw, &lh)) {
            LOG("D3DX: DDS level %u is truncated", l);
            surf->Release();
            break;
        }
        /* The two cases must be exclusive: blit_argb() locks and unlocks the
         * surface itself, so locking here and then calling blit_argb() inside
         * the same lock made DXVK refuse the second lock ("Don't allow
         * multiple lockings", D3DERR_INVALIDCALL). The log filled with 234
         * "D3DX: LockRect failed" -- one per cube face level -- and the DDS
         * data never reached the texture. */
        if (is_compressed(fmt) && fmt == d.fmt && hf == fmt && (UINT)lw == sd.Width && (UINT)lh == sd.Height) {
            // raw compressed copy, respecting the surface pitch
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(surf->LockRect(&lr, nullptr, 0))) {
                uint32_t off = d.data;
                uint32_t lw2 = d.w, lh2 = d.h;
                for (UINT k = 0; k < l; k++) { off += (std::max(1u, (lw2 + 3) / 4) * std::max(1u, (lh2 + 3) / 4) * block_bytes(d.fmt)); lw2 = std::max(1u, lw2 >> 1); lh2 = std::max(1u, lh2 >> 1); }
                int rows = (lh + 3) / 4;
                int srcPitch = std::max(1, (lw + 3) / 4) * block_bytes(d.fmt);
                for (int y = 0; y < rows; y++)
                    memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, p + off + (size_t)y * srcPitch,
                           std::min<int>(srcPitch, lr.Pitch));
                surf->UnlockRect();
            }
        } else {
            blit_argb(surf, nullptr, px.data(), lw, lh, hf);
        }
        surf->Release();
    }
    if (srcInfo) {
        memset(srcInfo, 0, sizeof *srcInfo);
        srcInfo->Width = d.w; srcInfo->Height = d.h; srcInfo->Depth = d.depth; srcInfo->MipLevels = d.mips;
        srcInfo->Format = d.fmt; srcInfo->ResourceType = D3DRTYPE_TEXTURE; srcInfo->ImageFileFormat = D3DXIFF_DDS;
    }
    M32(outTex) = d3d9_wrap(tex, IFACE_IDirect3DTexture9);
    RET(S_OK, 15);
}

extern "C" void shim_D3DXCreateCubeTextureFromFileInMemoryEx(CPU *c) {
    IDirect3DDevice9 *dev = (IDirect3DDevice9 *)com_host(ARG(0));
    const uint8_t *p = (const uint8_t *)GPTR(ARG(1));
    uint32_t n = ARG(2);
    UINT reqEdge = ARG(3), reqMips = ARG(4);
    DWORD usage = ARG(5);
    D3DFORMAT reqFmt = (D3DFORMAT)ARG(6);
    D3DPOOL pool = (D3DPOOL)ARG(7);
    D3DXIMAGE_INFO *srcInfo = (D3DXIMAGE_INFO *)GPTR(ARG(11));
    uint32_t outTex = ARG(13);
    Dds d;
    if (!parse_dds(p, n, &d) || !d.cube) {
        LOG("D3DXCreateCubeTextureFromFileInMemoryEx: only DDS cube maps are supported");
        M32(outTex) = 0;
        RET(E_NOTIMPL, 14);
    }
    UINT edge = reqEdge == D3DX_DEFAULT_ ? d.w : reqEdge;
    UINT levels = reqMips == D3DX_DEFAULT_ ? d.mips : std::min(reqMips, d.mips);
    if (!levels) levels = 1;
    D3DFORMAT fmt = reqFmt == D3DFMT_UNKNOWN ? d.fmt : reqFmt;
    /* A DXT cube map must be created as A8R8G8B8 on a driver without BC and
     * decoded below; the guest still sees DXT (bc_register_texture). Without
     * this the Adreno driver rejects the DXT1 cube map outright -- the log
     * shows "D3DX: CreateCubeTexture -> 8876017c" for every menu cube map --
     * and the guest is handed a NULL texture. */
    D3DFORMAT hf = (D3DFORMAT)bc_host_format(dev, (uint32_t)fmt);
    IDirect3DCubeTexture9 *tex = nullptr;
    HRESULT hr = dev->CreateCubeTexture(edge, levels, usage, hf, pool, &tex, nullptr);
    if (FAILED(hr) || !tex) { LOG("D3DX: CreateCubeTexture -> %08x", (unsigned)hr); M32(outTex) = 0; RET(E_FAIL, 14); }
    if (hf != fmt) bc_register_texture(tex, (uint32_t)fmt);
    for (UINT f = 0; f < 6; f++) {
        for (UINT l = 0; l < levels; l++) {
            IDirect3DSurface9 *surf = nullptr;
            if (FAILED(tex->GetCubeMapSurface((D3DCUBEMAP_FACES)f, l, &surf)) || !surf) continue;
            std::vector<uint32_t> px;
            int lw = 0, lh = 0;
            if (!load_dds_level(p, n, &d, f, l, &px, &lw, &lh)) { surf->Release(); break; }
            D3DSURFACE_DESC sd;
            surf->GetDesc(&sd);
            if (is_compressed(fmt) && fmt == d.fmt && hf == fmt) {
                /* raw compressed copy, respecting the surface pitch. blit_argb()
                 * is not used here because it locks the surface itself, and the
                 * outer lock below would make the second lock fail. */
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(surf->LockRect(&lr, nullptr, 0))) {
                    uint32_t off = d.data, lw2 = d.w, lh2 = d.h;
                    for (UINT k = 0; k < l; k++) { off += std::max(1u, (lw2 + 3) / 4) * std::max(1u, (lh2 + 3) / 4) * block_bytes(d.fmt); lw2 = std::max(1u, lw2 >> 1); lh2 = std::max(1u, lh2 >> 1); }
                    uint32_t lsz = std::max(1u, (lw2 + 3) / 4) * std::max(1u, (lh2 + 3) / 4) * block_bytes(d.fmt);
                    off += lsz * f;
                    int rows = (lh + 3) / 4, srcPitch = std::max(1, (lw + 3) / 4) * block_bytes(d.fmt);
                    for (int y = 0; y < rows; y++)
                        memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, p + off + (size_t)y * srcPitch,
                               std::min<int>(srcPitch, lr.Pitch));
                    surf->UnlockRect();
                }
            } else {
                blit_argb(surf, nullptr, px.data(), lw, lh, hf);
            }
            surf->Release();
        }
    }
    if (srcInfo) {
        memset(srcInfo, 0, sizeof *srcInfo);
        srcInfo->Width = d.w; srcInfo->Height = d.h; srcInfo->Depth = 1; srcInfo->MipLevels = d.mips;
        srcInfo->Format = d.fmt; srcInfo->ResourceType = D3DRTYPE_CUBETEXTURE; srcInfo->ImageFileFormat = D3DXIFF_DDS;
    }
    M32(outTex) = d3d9_wrap(tex, IFACE_IDirect3DCubeTexture9);
    RET(S_OK, 14);
}

extern "C" void shim_D3DXLoadSurfaceFromMemory(CPU *c) {
    IDirect3DSurface9 *dst = (IDirect3DSurface9 *)com_host(ARG(0));
    const RECT *dstRect = (const RECT *)GPTR(ARG(2));
    const uint8_t *src = (const uint8_t *)GPTR(ARG(3));
    D3DFORMAT srcFmt = (D3DFORMAT)ARG(4);
    UINT srcPitch = ARG(5);
    const RECT *srcRect = (const RECT *)GPTR(ARG(7));
    if (!dst || !src) RET(D3DERR_INVALIDCALL, 10);
    D3DSURFACE_DESC dd;
    dst->GetDesc(&dd);
    int w = srcRect ? (int)(srcRect->right - srcRect->left) : (int)dd.Width;
    int h = srcRect ? (int)(srcRect->bottom - srcRect->top) : (int)dd.Height;
    const uint8_t *p = src;
    if (srcRect) p += srcRect->top * srcPitch + srcRect->left * std::max(1, bytes_per_pixel(srcFmt));
    std::vector<uint32_t> px((size_t)w * h);
    if (!decode_raw(p, w, h, (int)srcPitch, srcFmt, px.data())) {
        LOG("D3DXLoadSurfaceFromMemory: unsupported source format %d", (int)srcFmt);
        RET(E_NOTIMPL, 10);
    }
    blit_argb(dst, dstRect, px.data(), w, h, dd.Format);
    RET(S_OK, 10);
}

/* Dunia's own blit helper (0x10414e50 in the shipped DLL) tries
 * StretchRect first and only falls back to this D3DX call when it passed
 * rects, which it does for every final composite -- so the whole frame reaches
 * the back buffer through here and the function is load-bearing: a silent
 * failure is the story-mode black screen. Log every call (rate-limited), and
 * never fail silently: when the source is a render target that cannot be
 * locked (D3D9 forbids locking D3DPOOL_DEFAULT targets; Windows fails the same
 * way), do what real D3DX does underneath and move the pixels on the GPU --
 * StretchRect when the formats allow it, else GetRenderTargetData into a
 * system-memory copy and decode from there. */
static int g_lsfs_logs;

extern "C" void shim_D3DXLoadSurfaceFromSurface(CPU *c) {
    IDirect3DSurface9 *dst = (IDirect3DSurface9 *)com_host(ARG(0));
    const RECT *dstRect = (const RECT *)GPTR(ARG(2));
    IDirect3DSurface9 *src = (IDirect3DSurface9 *)com_host(ARG(3));
    const RECT *srcRect = (const RECT *)GPTR(ARG(5));
    if (!dst || !src) RET(D3DERR_INVALIDCALL, 8);
    D3DSURFACE_DESC sd, dd;
    src->GetDesc(&sd);
    dst->GetDesc(&dd);
    bool verbose = g_lsfs_logs < 16 || (g_lsfs_logs % 300) == 0;
    if (verbose)
        LOG("D3DXLoadSurfaceFromSurface #%d: %ux%u fmt %d pool %u usage %x -> "
            "%ux%u fmt %d pool %u usage %x, rects %d/%d",
            g_lsfs_logs, sd.Width, sd.Height, (int)sd.Format, (unsigned)sd.Pool,
            (unsigned)sd.Usage, dd.Width, dd.Height, (int)dd.Format,
            (unsigned)dd.Pool, (unsigned)dd.Usage, !!srcRect, !!dstRect);
    g_lsfs_logs++;
    int w = srcRect ? (int)(srcRect->right - srcRect->left) : (int)sd.Width;
    int h = srcRect ? (int)(srcRect->bottom - srcRect->top) : (int)sd.Height;
    if (w <= 0 || h <= 0) { LOG("D3DXLoadSurfaceFromSurface: empty rect"); RET(D3DERR_INVALIDCALL, 8); }
    std::vector<uint32_t> px((size_t)w * h);
    D3DLOCKED_RECT lr;
    RECT sr = srcRect ? *srcRect : RECT{0, 0, (LONG)sd.Width, (LONG)sd.Height};
    HRESULT lhr = src->LockRect(&lr, &sr, D3DLOCK_READONLY);
    if (SUCCEEDED(lhr)) {
        const uint8_t *base = (const uint8_t *)lr.pBits;
        if (is_compressed(sd.Format)) {
            decode_bc(base, w, h, lr.Pitch, sd.Format, px.data(), w);
        } else if (!decode_raw(base, w, h, lr.Pitch, sd.Format, px.data())) {
            src->UnlockRect();
            LOG("D3DXLoadSurfaceFromSurface: unsupported source format %d", (int)sd.Format);
            RET(E_NOTIMPL, 8);
        }
        src->UnlockRect();
        blit_argb(dst, dstRect, px.data(), w, h, dd.Format);
        RET(S_OK, 8);
    }

    /* The source will not hand out CPU memory. A D3DPOOL_DEFAULT render target
     * (the post-processing chain's output) is the case that matters; try the
     * GPU first, exactly the ordering real D3DX uses internally. */
    bool src_is_rt = sd.Pool == D3DPOOL_DEFAULT && (sd.Usage & D3DUSAGE_RENDERTARGET);
    LOG("D3DXLoadSurfaceFromSurface: src lock failed (fmt %d pool %u hr %08x, rt=%d)",
        (int)sd.Format, (unsigned)sd.Pool, (unsigned)lhr, (int)src_is_rt);
    if (!src_is_rt) RET(lhr, 8);
    IDirect3DDevice9 *dev = nullptr;
    if (FAILED(src->GetDevice(&dev)) || !dev) RET(lhr, 8);
    RECT dr = dstRect ? *dstRect : RECT{0, 0, (LONG)dd.Width, (LONG)dd.Height};
    HRESULT ghr = dev->StretchRect(src, &sr, dst, &dr, D3DTEXF_LINEAR);
    if (SUCCEEDED(ghr)) {
        LOG("D3DXLoadSurfaceFromSurface: carried by StretchRect");
        dev->Release();
        RET(S_OK, 8);
    }
    /* Formats StretchRect refuses (float targets included): copy through a
     * system-memory staging surface and finish on the CPU. */
    IDirect3DSurface9 *stage = nullptr;
    HRESULT chr = dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format,
                                                   D3DPOOL_SYSTEMMEM, &stage, nullptr);
    if (SUCCEEDED(chr)) chr = dev->GetRenderTargetData(src, stage);
    if (SUCCEEDED(chr)) {
        D3DLOCKED_RECT l2;
        chr = stage->LockRect(&l2, &sr, D3DLOCK_READONLY);
        if (SUCCEEDED(chr)) {
            if (!decode_raw((const uint8_t *)l2.pBits, w, h, l2.Pitch, sd.Format, px.data())) {
                LOG("D3DXLoadSurfaceFromSurface: unsupported staged source format %d", (int)sd.Format);
                chr = E_NOTIMPL;
            } else {
                blit_argb(dst, dstRect, px.data(), w, h, dd.Format);
            }
            stage->UnlockRect();
        }
    } else {
        LOG("D3DXLoadSurfaceFromSurface: staging failed (%08x); stretch was %08x",
            (unsigned)chr, (unsigned)ghr);
    }
    if (stage) stage->Release();
    dev->Release();
    RET(SUCCEEDED(chr) ? S_OK : chr, 8);
}

/* Mip chain filter for 2-D textures: box-average 2x2 (or 4x4 for larger
 * reductions). Compressed textures are left as they are with a one-time note;
 * DDS art ships its mips, so this path only serves generated textures. */
extern "C" void shim_D3DXFilterTexture(CPU *c) {
    IDirect3DBaseTexture9 *base = (IDirect3DBaseTexture9 *)com_host(ARG(0));
    UINT srcLevel = ARG(2);
    if (!base) RET(D3DERR_INVALIDCALL, 4);
    D3DRESOURCETYPE type = base->GetType();
    if (type == D3DRTYPE_CUBETEXTURE) {
        // filter each face independently by recursing on GetCubeMapSurface is
        // not exposed here; report instead of writing wrong pixels
        LOG("D3DXFilterTexture: cube maps keep the mips stored in the DDS file");
        RET(S_OK, 4);
    }
    if (type != D3DRTYPE_TEXTURE) {
        LOG("D3DXFilterTexture: unsupported resource type %d", (int)type);
        RET(E_NOTIMPL, 4);
    }
    IDirect3DTexture9 *tex = (IDirect3DTexture9 *)base;
    D3DSURFACE_DESC sd;
    IDirect3DSurface9 *s0 = nullptr;
    tex->GetSurfaceLevel(srcLevel, &s0);
    if (!s0) RET(D3DERR_INVALIDCALL, 4);
    s0->GetDesc(&sd);
    UINT levels = tex->GetLevelCount();
    UINT w = sd.Width, h = sd.Height;
    D3DFORMAT fmt = sd.Format;
    s0->Release();
    if (is_compressed(fmt)) {
        LOG("D3DXFilterTexture: compressed texture keeps its stored mip levels");
        RET(S_OK, 4);
    }
    if (!bytes_per_pixel(fmt)) {
        LOG("D3DXFilterTexture: unsupported format %d", (int)fmt);
        RET(E_NOTIMPL, 4);
    }
    std::vector<uint32_t> prev((size_t)w * h);
    {
        IDirect3DSurface9 *s = nullptr;
        tex->GetSurfaceLevel(srcLevel, &s);
        D3DLOCKED_RECT lr;
        if (!s || FAILED(s->LockRect(&lr, nullptr, D3DLOCK_READONLY))) { if (s) s->Release(); RET(D3DERR_INVALIDCALL, 4); }
        decode_raw((const uint8_t *)lr.pBits, w, h, lr.Pitch, fmt, prev.data());
        s->UnlockRect();
        s->Release();
    }
    UINT pw = w, ph = h;
    for (UINT l = srcLevel + 1; l < levels; l++) {
        IDirect3DSurface9 *s = nullptr;
        tex->GetSurfaceLevel(l, &s);
        if (!s) break;
        D3DSURFACE_DESC d2;
        s->GetDesc(&d2);
        int nw = d2.Width, nh = d2.Height;
        std::vector<uint32_t> cur((size_t)nw * nh);
        int fx = std::max(1, (int)pw / nw), fy = std::max(1, (int)ph / nh);
        for (int y = 0; y < nh; y++) {
            for (int x = 0; x < nw; x++) {
                uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
                for (int j = 0; j < fy; j++)
                    for (int i = 0; i < fx; i++) {
                        int sx = x * fx + i, sy = y * fy + j;
                        if (sx >= (int)pw || sy >= (int)ph) continue;
                        uint32_t v = prev[(size_t)sy * pw + sx];
                        a += (v >> 24) & 0xFF; r += (v >> 16) & 0xFF; g += (v >> 8) & 0xFF; b += v & 0xFF; n++;
                    }
                if (!n) n = 1;
                cur[(size_t)y * nw + x] = pack_argb(a / n, r / n, g / n, b / n);
            }
        }
        blit_argb(s, nullptr, cur.data(), nw, nh, fmt);
        s->Release();
        prev.swap(cur);
        pw = nw; ph = nh;
    }
    RET(S_OK, 4);
}

extern "C" void shim_D3DXFillTexture(CPU *c) {
    IDirect3DTexture9 *tex = (IDirect3DTexture9 *)com_host(ARG(0));
    uint32_t fn = ARG(1), data = ARG(2);
    if (!tex || !fn) RET(D3DERR_INVALIDCALL, 3);
    CPU *cpu = rt_current_cpu();
    UINT levels = tex->GetLevelCount();
    uint32_t out = gmalloc(16), tc = gmalloc(8), ts = gmalloc(8);
    for (UINT l = 0; l < levels; l++) {
        IDirect3DSurface9 *s = nullptr;
        tex->GetSurfaceLevel(l, &s);
        if (!s) continue;
        D3DSURFACE_DESC d;
        s->GetDesc(&d);
        if (d.Format != D3DFMT_A8R8G8B8) {
            LOG("D3DXFillTexture: format %d is not filled (A8R8G8B8 only)", (int)d.Format);
            s->Release();
            continue;
        }
        D3DLOCKED_RECT lr;
        if (FAILED(s->LockRect(&lr, nullptr, 0))) { s->Release(); continue; }
        M32(ts) = 1.0f; M32(ts + 4) = 1.0f;
        for (UINT y = 0; y < d.Height; y++) {
            for (UINT x = 0; x < d.Width; x++) {
                MF32(tc) = (x + 0.5f) / d.Width;
                MF32(tc + 4) = (y + 0.5f) / d.Height;
                uint32_t args[4] = {out, tc, ts, data};
                rt_invoke(cpu, fn, 4, args);
                uint32_t v = pack_argb((uint32_t)(MF32(out + 12) * 255.0f + 0.5f),
                                       (uint32_t)(MF32(out + 0) * 255.0f + 0.5f),
                                       (uint32_t)(MF32(out + 4) * 255.0f + 0.5f),
                                       (uint32_t)(MF32(out + 8) * 255.0f + 0.5f));
                uint8_t *px = (uint8_t *)lr.pBits + y * lr.Pitch + x * 4;
                *(uint32_t *)px = v;
            }
        }
        s->UnlockRect();
        s->Release();
    }
    gfree(out); gfree(tc); gfree(ts);
    RET(S_OK, 3);
}

/* Writes a DDS file (all mip levels) or a 32-bit BMP. PNG/JPG/TGA saving is
 * not implemented; the caller gets E_NOTIMPL and a log line. */
static HRESULT save_texture(const char *path, int format, IDirect3DBaseTexture9 *base) {
    if (!base) return D3DERR_INVALIDCALL;
    if (base->GetType() != D3DRTYPE_TEXTURE) {
        LOG("D3DXSaveTexture: only 2-D textures are written");
        return E_NOTIMPL;
    }
    IDirect3DTexture9 *tex = (IDirect3DTexture9 *)base;
    if (format != D3DXIFF_DDS && format != D3DXIFF_BMP) {
        LOG("D3DXSaveTexture: format %d is not implemented (DDS and BMP are)", format);
        return E_NOTIMPL;
    }
    UINT levels = tex->GetLevelCount();
    D3DSURFACE_DESC d;
    IDirect3DSurface9 *s0 = nullptr;
    tex->GetSurfaceLevel(0, &s0);
    if (!s0) return E_FAIL;
    s0->GetDesc(&d);
    FILE *f = fopen(path, "wb");
    if (!f) { s0->Release(); LOG("D3DXSaveTexture: cannot write %s", path); return E_FAIL; }
    if (format == D3DXIFF_BMP) {
        D3DLOCKED_RECT lr;
        int w = d.Width, h = d.Height;
        uint8_t hdr[54] = {'B', 'M'};
        uint32_t imgsz = w * h * 4;
        uint32_t filesz = 54 + imgsz;
        memcpy(hdr + 2, &filesz, 4);
        uint32_t off = 54;
        memcpy(hdr + 10, &off, 4);
        uint32_t hs = 40;
        memcpy(hdr + 14, &hs, 4);
        memcpy(hdr + 18, &w, 4);
        int32_t nh = -h;
        memcpy(hdr + 22, &nh, 4);
        uint16_t planes = 1, bpp = 32;
        memcpy(hdr + 26, &planes, 2);
        memcpy(hdr + 28, &bpp, 2);
        fwrite(hdr, 1, 54, f);
        if (SUCCEEDED(s0->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const uint8_t *q = (const uint8_t *)lr.pBits + y * lr.Pitch + x * 4;
                    uint8_t bgra[4] = {q[0], q[1], q[2], 0xFF};
                    fwrite(bgra, 1, 4, f);
                }
            }
            s0->UnlockRect();
        }
        s0->Release();
        fclose(f);
        return S_OK;
    }
    // DDS
    uint8_t hdr[128];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "DDS ", 4);
    uint32_t size = 124, flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000;
    uint32_t hgt = d.Height, wid = d.Width;
    uint32_t mips = levels;
    memcpy(hdr + 4, &size, 4);
    memcpy(hdr + 8, &flags, 4);
    memcpy(hdr + 12, &hgt, 4);
    memcpy(hdr + 16, &wid, 4);
    memcpy(hdr + 28, &mips, 4);
    uint32_t pfSize = 32;
    memcpy(hdr + 76, &pfSize, 4);
    uint32_t pfFlags = 0;
    uint32_t fourcc = 0, bits = 0, rm = 0, gm = 0, bm = 0, am = 0;
    switch (d.Format) {
    case D3DFMT_DXT1: fourcc = 0x31545844; pfFlags = 0x4; break;
    case D3DFMT_DXT3: fourcc = 0x33545844; pfFlags = 0x4; break;
    case D3DFMT_DXT5: fourcc = 0x35545844; pfFlags = 0x4; break;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
        pfFlags = 0x40; bits = 32; rm = 0x00FF0000; gm = 0x0000FF00; bm = 0x000000FF;
        am = d.Format == D3DFMT_A8R8G8B8 ? 0xFF000000 : 0; break;
    case D3DFMT_R8G8B8: pfFlags = 0x40; bits = 24; rm = 0xFF0000; gm = 0xFF00; bm = 0xFF; break;
    case D3DFMT_R5G6B5: pfFlags = 0x40; bits = 16; rm = 0xF800; gm = 0x07E0; bm = 0x1F; break;
    case D3DFMT_A1R5G5B5: pfFlags = 0x41; bits = 16; rm = 0x7C00; gm = 0x03E0; bm = 0x1F; am = 0x8000; break;
    case D3DFMT_A4R4G4B4: pfFlags = 0x41; bits = 16; rm = 0xF00; gm = 0xF0; bm = 0xF; am = 0xF000; break;
    case D3DFMT_L8: pfFlags = 0x20000; bits = 8; break;
    case D3DFMT_A8L8: pfFlags = 0x20001; bits = 16; break;
    case D3DFMT_A8: pfFlags = 0x2; bits = 8; break;
    default:
        s0->Release();
        fclose(f);
        LOG("D3DXSaveTexture: format %d cannot be written as DDS", (int)d.Format);
        return E_NOTIMPL;
    }
    memcpy(hdr + 80, &pfFlags, 4);
    memcpy(hdr + 84, &fourcc, 4);
    memcpy(hdr + 88, &bits, 4);
    memcpy(hdr + 92, &rm, 4);
    memcpy(hdr + 96, &gm, 4);
    memcpy(hdr + 100, &bm, 4);
    memcpy(hdr + 104, &am, 4);
    uint32_t caps = 0x1000 | (levels > 1 ? 0x400008 : 0);
    memcpy(hdr + 108, &caps, 4);
    fwrite(hdr, 1, 128, f);
    s0->Release();
    for (UINT l = 0; l < levels; l++) {
        IDirect3DSurface9 *s = nullptr;
        tex->GetSurfaceLevel(l, &s);
        if (!s) break;
        D3DSURFACE_DESC sd;
        s->GetDesc(&sd);
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(s->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            if (is_compressed(sd.Format)) {
                int rows = (sd.Height + 3) / 4;
                int pitch = std::max(1u, (sd.Width + 3) / 4) * block_bytes(sd.Format);
                for (int y = 0; y < rows; y++) fwrite((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, 1, pitch, f);
            } else {
                int bpp = bytes_per_pixel(sd.Format);
                for (UINT y = 0; y < sd.Height; y++)
                    fwrite((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, 1, sd.Width * bpp, f);
            }
            s->UnlockRect();
        }
        s->Release();
    }
    fclose(f);
    return S_OK;
}

extern "C" void shim_D3DXSaveTextureToFileW(CPU *c) {
    char host[1024];
    rt_host_path((const char *)GPTR(ARG(0)), host, sizeof host);
    RET(save_texture(host, (int)ARG(1), (IDirect3DBaseTexture9 *)com_host(ARG(2))), 4);
}

extern "C" void shim_D3DXSaveTextureToFileInMemory(CPU *c) {
    LOG("D3DXSaveTextureToFileInMemory: needs an ID3DXBuffer object, which this runtime does not "
        "implement; use D3DXSaveTextureToFile with a path instead");
    M32(ARG(0)) = 0;
    RET(E_NOTIMPL, 4);
}

/* ---------------------------------------------------------------- shaders */
#define D3DSIO_DCL 0x1F
#define D3DSIO_END 0x0000
#define D3DSP_REGTYPE_MASK 0x70000000u
#define D3DSP_REGTYPE_SHIFT 28
#define D3DSPR_INPUT 1

extern "C" void shim_D3DXGetShaderInputSemantics(CPU *c) {
    const uint32_t *code = (const uint32_t *)GPTR(ARG(0));
    D3DXSEMANTIC *out = (D3DXSEMANTIC *)GPTR(ARG(1));
    uint32_t cntp = ARG(2);
    if (!code || !cntp) RET(E_INVALIDARG, 3);
    uint32_t version = code[0];
    if ((version & 0xFFFF0000u) != 0xFFFE0000u) { *(uint32_t *)GPTR(cntp) = 0; RET(E_FAIL, 3); }
    UINT major = (version >> 8) & 0xFF;
    UINT n = 0;
    if (major == 1) {
        for (int i = 1; i < 256; i++) {
            uint32_t t = code[i];
            if (!(t & 0x80000000u)) break;
            if (out) { out[n].Usage = t & 0xF; out[n].UsageIndex = (t >> 16) & 0xF; }
            n++;
        }
    } else {
        for (int i = 1; i < 65536; i++) {
            uint32_t t = code[i];
            if ((t & 0xFFFFu) == D3DSIO_END) break;
            if ((t & 0xFFFFu) == D3DSIO_DCL) {
                uint32_t decl = code[i + 1];
                uint32_t regtype = (decl & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT;
                if (regtype == D3DSPR_INPUT) {
                    if (out) { out[n].Usage = decl & 0xF; out[n].UsageIndex = (decl >> 16) & 0xF; }
                    n++;
                }
                i += 1;
            } else {
                i += (t >> 24) & 0xF;  // parameter tokens
            }
        }
    }
    *(uint32_t *)GPTR(cntp) = n;
    RET(S_OK, 3);
}

/* Spherical harmonics projection of a cube map, D3DXSHProjectCubeMap order
 * semantics: Order^2 coefficients per channel, bands l = 0 .. Order-1. */
static void sh_coeff(double x, double y, double z, int l, int m, double *out) {
    const double pi = 3.14159265358979323846;
    double v = 0;
    switch (l) {
    case 0: v = 0.28209479177387814; break;
    case 1:
        if (m == -1) v = 0.4886025119029199 * y;
        else if (m == 0) v = 0.4886025119029199 * z;
        else v = 0.4886025119029199 * x;
        break;
    case 2:
        if (m == -2) v = 1.0925484305920792 * x * y;
        else if (m == -1) v = 1.0925484305920792 * y * z;
        else if (m == 0) v = 0.31539156525252005 * (3 * z * z - 1);
        else if (m == 1) v = 1.0925484305920792 * x * z;
        else v = 0.5462742152960396 * (x * x - y * y);
        break;
    case 3:
        if (m == -3) v = 0.5900435899266435 * y * (3 * x * x - y * y);
        else if (m == -2) v = 2.890611442640554 * x * y * z;
        else if (m == -1) v = 0.4570457994644658 * y * (5 * z * z - 1);
        else if (m == 0) v = 0.3731763325901154 * z * (5 * z * z - 3);
        else if (m == 1) v = 0.4570457994644658 * x * (5 * z * z - 1);
        else if (m == 2) v = 1.445305721320277 * z * (x * x - y * y);
        else v = 0.5900435899266435 * x * (x * x - 3 * y * y);
        break;
    case 4:
        if (m == -4) v = 2.5033429417967046 * x * y * (x * x - y * y);
        else if (m == -3) v = 1.7701307697799304 * y * z * (3 * x * x - y * y);
        else if (m == -2) v = 0.9461746957575601 * x * y * (7 * z * z - 1);
        else if (m == -1) v = 0.6690465435572892 * y * z * (7 * z * z - 3);
        else if (m == 0) v = 0.10578554691520431 * (35 * z * z * z * z - 30 * z * z + 3);
        else if (m == 1) v = 0.6690465435572892 * x * z * (7 * z * z - 3);
        else if (m == 2) v = 0.47308734787878004 * (x * x - y * y) * (7 * z * z - 1);
        else if (m == 3) v = 1.7701307697799304 * x * z * (x * x - 3 * y * y);
        else v = 0.6258357354491761 * (x * x * (x * x - 3 * y * y) - y * y * (3 * x * x - y * y));
        break;
    default: break;
    }
    (void)pi;
    *out = v;
}

extern "C" void shim_D3DXSHProjectCubeMap(CPU *c) {
    UINT order = ARG(0);
    IDirect3DCubeTexture9 *cube = (IDirect3DCubeTexture9 *)com_host(ARG(1));
    float *ro = (float *)GPTR(ARG(2)), *go = (float *)GPTR(ARG(3)), *bo = (float *)GPTR(ARG(4));
    if (!cube || order < 1 || order > 5 || !ro || !go || !bo) RET(E_INVALIDARG, 5);
    UINT ncoef = order * order;
    for (UINT i = 0; i < ncoef; i++) { ro[i] = go[i] = bo[i] = 0; }
    const int S = 16;  // samples per face edge
    double weight = 4.0 * 3.14159265358979323846 / (6.0 * S * S);
    for (int face = 0; face < 6; face++) {
        IDirect3DSurface9 *s = nullptr;
        if (FAILED(cube->GetCubeMapSurface((D3DCUBEMAP_FACES)face, 0, &s)) || !s) continue;
        D3DSURFACE_DESC d;
        s->GetDesc(&d);
        D3DLOCKED_RECT lr;
        if (FAILED(s->LockRect(&lr, nullptr, D3DLOCK_READONLY))) { s->Release(); continue; }
        for (int sy = 0; sy < S; sy++) {
            for (int sx = 0; sx < S; sx++) {
                int x = (int)((sx + 0.5f) * d.Width / S), y = (int)((sy + 0.5f) * d.Height / S);
                const uint8_t *q = (const uint8_t *)lr.pBits + y * lr.Pitch + x * 4;
                double a = q[3] / 255.0, r = q[2] / 255.0, g = q[1] / 255.0, b = q[0] / 255.0;
                double u = 2.0 * (sx + 0.5) / S - 1.0, v = 2.0 * (sy + 0.5) / S - 1.0;
                double dx, dy, dz;
                switch (face) {
                case 0: dx = 1; dy = -v; dz = -u; break;
                case 1: dx = -1; dy = -v; dz = u; break;
                case 2: dx = u; dy = 1; dz = v; break;
                case 3: dx = u; dy = -1; dz = -v; break;
                case 4: dx = u; dy = -v; dz = 1; break;
                default: dx = -u; dy = -v; dz = -1; break;
                }
                double len = std::sqrt(dx * dx + dy * dy + dz * dz);
                dx /= len; dy /= len; dz /= len;
                for (int l = 0; l < (int)order; l++)
                    for (int m = -l; m <= l; m++) {
                        double ylm;
                        sh_coeff(dx, dy, dz, l, m, &ylm);
                        double k = ylm * weight;
                        UINT idx = l * l + (m + l);
                        ro[idx] += (float)(r * k); go[idx] += (float)(g * k); bo[idx] += (float)(b * k);
                    }
                (void)a;
            }
        }
        s->UnlockRect();
        s->Release();
    }
    RET(S_OK, 5);
}

static const ShimDef defs[] = {
    {"d3dx9_38.dll", "D3DXPlaneNormalize", shim_D3DXPlaneNormalize},
    {"d3dx9_38.dll", "D3DXPlaneTransform", shim_D3DXPlaneTransform},
    {"d3dx9_38.dll", "D3DXFillTexture", shim_D3DXFillTexture},
    {"d3dx9_38.dll", "D3DXLoadSurfaceFromSurface", shim_D3DXLoadSurfaceFromSurface},
    {"d3dx9_38.dll", "D3DXSaveTextureToFileW", shim_D3DXSaveTextureToFileW},
    {"d3dx9_38.dll", "D3DXMatrixTranspose", shim_D3DXMatrixTranspose},
    {"d3dx9_38.dll", "D3DXCreateCubeTextureFromFileInMemoryEx", shim_D3DXCreateCubeTextureFromFileInMemoryEx},
    {"d3dx9_38.dll", "D3DXSHProjectCubeMap", shim_D3DXSHProjectCubeMap},
    {"d3dx9_38.dll", "D3DXFilterTexture", shim_D3DXFilterTexture},
    {"d3dx9_38.dll", "D3DXSaveTextureToFileInMemory", shim_D3DXSaveTextureToFileInMemory},
    {"d3dx9_38.dll", "D3DXLoadSurfaceFromMemory", shim_D3DXLoadSurfaceFromMemory},
    {"d3dx9_38.dll", "D3DXGetImageInfoFromFileInMemory", shim_D3DXGetImageInfoFromFileInMemory},
    {"d3dx9_38.dll", "D3DXGetShaderInputSemantics", shim_D3DXGetShaderInputSemantics},
    {"d3dx9_38.dll", "D3DXCreateTextureFromFileInMemoryEx", shim_D3DXCreateTextureFromFileInMemoryEx},
    {"d3dx9_38.dll", "D3DXMatrixInverse", shim_D3DXMatrixInverse},
    {nullptr, nullptr, nullptr},
};

extern "C" void d3dx9_init(void) { rt_register_shims(defs); }
