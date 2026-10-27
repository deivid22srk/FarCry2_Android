/* Bink diagnostics: logging wrappers around the recompiled binkw32 exports.
 *
 * The game's videos (intro movies, the title screen's background) are Bink
 * files, and when they misbehave the port log used to say nothing at all:
 * the imports are bound straight to the lifted binkw32 code, which never
 * logs. The first phone run showed a green title screen (a video surface
 * whose YUV planes were never filled) and intro text without video, with no
 * way to tell whether BinkOpen failed, whether the movie file was missing,
 * or whether the decoder produced nothing.
 *
 * After the modules are mapped, binkdiag_install() re-points every binkw32
 * export at a wrapper that logs the call (rate-limited for the per-frame
 * ones) and then invokes the lifted original with rt_invoke(). The next
 * import pass binds the game's binkw32 imports to the wrappers (see
 * rt_register_data's override-in-place in runtime/shims.c).
 *
 * Bink is (c) RAD Game Tools; this file only logs calls into the user's own
 * recompiled copy and contains none of it.
 */
#include "rt.h"
#include <string.h>

/* Per-frame functions (Wait/DoFrame/NextFrame/ShouldSkip) are called once or
 * twice per presented video frame: log the first 5, then every 600th call. */
#define PERFRAME_FIRST 5
static int bink_log_gate(uint32_t *n) {
    uint32_t k = (*n)++;
    return k < PERFRAME_FIRST || k % 600 == 0;
}

/* One wrapper per entry point. It snapshots the stdcall args, calls the
 * lifted original through rt_invoke, logs, and returns.
 *
 * The argument description is a formatter, not an extra printf argument list.
 * A list expands into several arguments, and the "%s" below would then take
 * its *format string* as the value and leave the real result and call count
 * unconsumed -- the first version of this file did that and printed the raw
 * pointers in place of the result. */
#define BINK_WRAPPER(cname, nargs, rare, args_fmt)                             \
    static struct { uint32_t real; uint32_t calls; } g_bk_##cname;             \
    static void shim_bk_##cname(CPU *c) {                                      \
        uint32_t a[nargs ? nargs : 1];                                         \
        for (int i = 0; i < nargs; i++) a[i] = ARG(i);                         \
        if (g_bk_##cname.real && g_bk_##cname.real < GUEST_THUNK_BASE) {       \
            uint32_t r = rt_invoke(c, g_bk_##cname.real, nargs, a);            \
            if (rare || bink_log_gate(&g_bk_##cname.calls)) {                  \
                char abuf[192];                                                \
                args_fmt(abuf, sizeof abuf);                                   \
                LOG("bink: " #cname "(%s) -> %08x (call %u)", abuf, r,         \
                    g_bk_##cname.calls);                                       \
            }                                                                  \
            RET(r, nargs);                                                     \
        }                                                                      \
        LOG("bink: " #cname " has no lifted original; failing");               \
        RET(0, nargs);                                                         \
    }

/* arg formatters (GSTR on a guest pointer is safe: whole sections are copied
 * into guest memory and NUL-padded) */
#define A0_NAME(buf, cap) snprintf(buf, cap, "name=\"%s\" flags=%x",           \
                 GSTR(a[0]) ? GSTR(a[0]) : "?", a[1])
#define A1_PTR(buf, cap)  snprintf(buf, cap, "p=%08x", a[0])
#define A2_PTRS(buf, cap) snprintf(buf, cap, "p=%08x, %08x", a[0], a[1])
#define A3_PTRS(buf, cap) snprintf(buf, cap, "p=%08x, %08x, %08x", a[0], a[1], a[2])
#define A_NONE(buf, cap)  snprintf(buf, cap, "%s", "")
/* The BINK struct starts Width, Height, FrameCount (bink_test reads them
 * there); log them so a dead decoder (all zeros) is visible in the log. */
#define A_BINK(buf, cap)  snprintf(buf, cap, "bink=%08x [w=%u h=%u frames=%u]", \
                 a[0], a[0] >= 0x10000 ? M32(a[0]) : 0u,                       \
                 a[0] >= 0x10000 ? M32(a[0] + 4) : 0u,                         \
                 a[0] >= 0x10000 ? M32(a[0] + 8) : 0u)

/* The 16 binkw32 entry points Dunia.dll imports (from the first phone log).
 * The @N suffixes are the stdcall arg byte counts, matching nargs. */
BINK_WRAPPER(BinkSetSoundSystem,        2, 1, A2_PTRS)
BINK_WRAPPER(BinkShouldSkip,            1, 0, A1_PTR)
BINK_WRAPPER(BinkSetMemory,             2, 1, A2_PTRS)
BINK_WRAPPER(BinkPause,                 2, 1, A2_PTRS)
BINK_WRAPPER(BinkNextFrame,             1, 0, A_BINK)
BINK_WRAPPER(BinkGoto,                  3, 1, A3_PTRS)
BINK_WRAPPER(BinkClose,                 1, 1, A1_PTR)
BINK_WRAPPER(BinkGetFrameBuffersInfo,   2, 1, A2_PTRS)
BINK_WRAPPER(BinkOpen,                  2, 1, A0_NAME)
BINK_WRAPPER(BinkLogoAddress,           0, 1, A_NONE)
BINK_WRAPPER(BinkOpenDirectSound,       1, 1, A1_PTR)
BINK_WRAPPER(BinkWait,                  1, 0, A_BINK)
BINK_WRAPPER(BinkSetVolume,             3, 1, A3_PTRS)
BINK_WRAPPER(BinkSetSoundOnOff,         2, 1, A2_PTRS)

/* --- Bink frame buffers -------------------------------------------------
 *
 * Dunia does not use BinkCopyToBuffer: it asks BinkGetFrameBuffersInfo for
 * the plane geometry, allocates the planes itself and hands them over with
 * BinkRegisterFrameBuffers. Bink then decodes straight into that memory and
 * the engine uploads the planes to A8 textures that a YUV pixel shader
 * samples.
 *
 * A flat green rectangle is what that shader produces from an all-zero YUV
 * triple, so either Bink never wrote the planes or the upload to the
 * textures never happened -- and the log cannot tell those apart today.
 * Dump the BINKFRAMEBUFFERS block and sample every word in it that looks
 * like a pointer: after BinkRegisterFrameBuffers the planes are still blank,
 * after BinkDoFrame they must not be. One run then says which side is at
 * fault.
 *
 * Guest memory is one 4 GB reservation, so any guest pointer below 4 GB is
 * safe to read (untouched pages read as zero), and the 24-word dump also
 * shows the layout if this Bink build orders the fields differently. */
static uint32_t g_bk_fb;

static void bink_probe_fb(const char *why) {
    uint32_t w[24];
    if (!g_bk_fb) return;
    for (int i = 0; i < 24; i++) w[i] = M32(g_bk_fb + (uint32_t)i * 4);
    LOG("bink: fb(%s) %08x +00: %08x %08x %08x %08x %08x %08x %08x %08x", why,
        g_bk_fb, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    LOG("bink: fb %08x +20: %08x %08x %08x %08x %08x %08x %08x %08x",
        g_bk_fb, w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
    LOG("bink: fb %08x +40: %08x %08x %08x %08x %08x %08x %08x %08x",
        g_bk_fb, w[16], w[17], w[18], w[19], w[20], w[21], w[22], w[23]);
    for (int i = 0; i < 24; i++) {
        uint32_t p = w[i];
        if (p < 0x10000) continue;              /* a size or pitch, not a pointer */
        uint32_t nz = 0, sum = 0;
        for (uint32_t k = 0; k < 4096; k++) {
            uint8_t v = M8(p + k);
            if (v) nz++;
            sum += v;
        }
        LOG("bink:   +%02x = %08x: nonzero %u/4096 sum %u", i * 4, p, nz, sum);
    }
}

/* Copies the frame Bink just decoded into the *other* frame slot's planes.
 *
 * The engine hands Bink two complete sets of plane buffers (luma 1280x800 and
 * two 640x400 chroma planes each) and publishes both through this struct, but
 * Bink 1.9 only decodes into the set whose index is CurrentFrame. The engine
 * then unlocks -- and therefore uploads -- the other set, so half the plane
 * textures hold a frame from two DoFrames ago. Copying at unlock time (which
 * the D3D9 bridge also does, as a fallback) is too late for the texture that
 * is sampled but not unlocked: that one keeps stale data, which shows up as
 * green blocks and torn colour while the video plays. Filling both sets here,
 * right after the decode, makes every plane texture hold the same complete
 * frame whichever one the engine samples next. */
static void bink_publish(void) {
    uint32_t fb = g_bk_fb;
    if (!fb) return;
    uint32_t cur = M32(fb + 0x14);
    if (cur > 1) return;
    uint32_t other = 1 - cur;
    for (int p = 0; p < 3; p++) {
        uint32_t src    = M32(fb + 0x18 + 12 * (cur * 4 + (uint32_t)p) + 4);
        uint32_t spitch = M32(fb + 0x18 + 12 * (cur * 4 + (uint32_t)p) + 8);
        uint32_t dst    = M32(fb + 0x18 + 12 * (other * 4 + (uint32_t)p) + 4);
        uint32_t dpitch = M32(fb + 0x18 + 12 * (other * 4 + (uint32_t)p) + 8);
        uint32_t rows   = (p == 0) ? M32(fb + 0x08) : M32(fb + 0x10);
        if (!src || !spitch || !dst || !dpitch || !rows) continue;
        if (src == dst && spitch == dpitch) continue;
        uint32_t n = spitch < dpitch ? spitch : dpitch;
        for (uint32_t y = 0; y < rows; y++)
            memcpy(MEM + dst + y * dpitch, MEM + src + y * spitch, n);
    }
}

/* 16x8 luma plus 8x4 chroma thumbnail of the decoded frame, averaged. A flat
 * green screen, a black band on one edge, a wrong value range (0..255 versus
 * 16..235) and swapped chroma all look different here, and the numbers travel
 * in the log -- no screenshot needed to see what the video really contains. */
static void bink_thumb(void) {
    uint32_t fb = g_bk_fb;
    if (!fb) return;
    uint32_t cur = M32(fb + 0x14);
    if (cur > 1) return;
    char out[512];
    int o = 0;
    for (int p = 0; p < 3; p++) {
        uint32_t src   = M32(fb + 0x18 + 12 * (cur * 4 + (uint32_t)p) + 4);
        uint32_t pitch = M32(fb + 0x18 + 12 * (cur * 4 + (uint32_t)p) + 8);
        uint32_t w     = (p == 0) ? M32(fb + 0x04) : M32(fb + 0x0C);
        uint32_t h     = (p == 0) ? M32(fb + 0x08) : M32(fb + 0x10);
        int cw = (p == 0) ? 16 : 8, ch = (p == 0) ? 8 : 4;
        if (!src || !pitch || !w || !h) continue;
        out[o++] = (p == 0) ? 'Y' : (p == 1 ? 'U' : 'V');
        out[o++] = ':';
        for (int j = 0; j < ch; j++)
            for (int i = 0; i < cw; i++) {
                uint32_t x0 = (uint32_t)i * w / (uint32_t)cw;
                uint32_t x1 = (uint32_t)(i + 1) * w / (uint32_t)cw;
                uint32_t y0 = (uint32_t)j * h / (uint32_t)ch;
                uint32_t y1 = (uint32_t)(j + 1) * h / (uint32_t)ch;
                uint32_t sum = 0, n = 0;
                for (uint32_t y = y0; y < y1; y += 4)
                    for (uint32_t x = x0; x < x1; x += 4) { sum += M8(src + y * pitch + x); n++; }
                unsigned v = n ? sum / n : 0;
                out[o++] = "0123456789abcdef"[v >> 4];
                out[o++] = "0123456789abcdef"[v & 15];
            }
        out[o++] = ' ';
    }
    out[o] = 0;
    LOG("bink: thumb %s", out);
}

static struct { uint32_t real; uint32_t calls; } g_bk_BinkRegisterFrameBuffers;
static struct { uint32_t real; uint32_t calls; } g_bk_BinkDoFrame;

static void shim_bk_BinkRegisterFrameBuffers(CPU *c) {
    uint32_t a[2] = { ARG(0), ARG(1) };
    g_bk_fb = a[1];
    if (!g_bk_BinkRegisterFrameBuffers.real) { RET(0, 2); }
    uint32_t r = rt_invoke(c, g_bk_BinkRegisterFrameBuffers.real, 2, a);
    LOG("bink: BinkRegisterFrameBuffers(p=%08x, fb=%08x) -> %08x", a[0], a[1], r);
    bink_probe_fb("registered");
    RET(r, 2);
}

static void shim_bk_BinkDoFrame(CPU *c) {
    static uint32_t probes;
    uint32_t a[1] = { ARG(0) };
    if (!g_bk_BinkDoFrame.real) { RET(0, 1); }
    uint32_t r = rt_invoke(c, g_bk_BinkDoFrame.real, 1, a);
    if (bink_log_gate(&g_bk_BinkDoFrame.calls)) {
        char abuf[192];
        A_BINK(abuf, sizeof abuf);
        LOG("bink: BinkDoFrame(%s) -> %08x (call %u)", abuf, r,
            g_bk_BinkDoFrame.calls);
    }
    if (probes < 3) { probes++; bink_probe_fb("decoded"); }
    if (!r) {
        bink_publish();
        static uint32_t frames;
        if ((frames++ % 30) == 0) bink_thumb();
    }
    RET(r, 1);
}

/* The texture bridge asks this to tell whether the buffer it is about to
 * unlock is one of the planes Bink was handed for the current frame. */
uint32_t binkdiag_fb(void) { return g_bk_fb; }

static const struct { const char *name; ShimFn fn; } k_wrapped[] = {
    { "_BinkSetSoundSystem@8",       shim_bk_BinkSetSoundSystem },
    { "_BinkShouldSkip@4",           shim_bk_BinkShouldSkip },
    { "_BinkSetMemory@8",            shim_bk_BinkSetMemory },
    { "_BinkPause@8",                shim_bk_BinkPause },
    { "_BinkNextFrame@4",            shim_bk_BinkNextFrame },
    { "_BinkGoto@12",                shim_bk_BinkGoto },
    { "_BinkClose@4",                shim_bk_BinkClose },
    { "_BinkRegisterFrameBuffers@8", shim_bk_BinkRegisterFrameBuffers },
    { "_BinkGetFrameBuffersInfo@8",  shim_bk_BinkGetFrameBuffersInfo },
    { "_BinkOpen@8",                 shim_bk_BinkOpen },
    { "_BinkLogoAddress@0",          shim_bk_BinkLogoAddress },
    { "_BinkDoFrame@4",              shim_bk_BinkDoFrame },
    { "_BinkOpenDirectSound@4",      shim_bk_BinkOpenDirectSound },
    { "_BinkWait@4",                 shim_bk_BinkWait },
    { "_BinkSetVolume@12",           shim_bk_BinkSetVolume },
    { "_BinkSetSoundOnOff@8",        shim_bk_BinkSetSoundOnOff },
};

void binkdiag_install(void) {
    int wrapped = 0;
    for (unsigned i = 0; i < sizeof k_wrapped / sizeof k_wrapped[0]; i++) {
        uint32_t real = rt_import("binkw32.dll", k_wrapped[i].name);
        if (!real || real >= GUEST_THUNK_BASE) continue;  /* not lifted: leave the stub in place */
        /* remember the lifted original, then re-point the export at the wrapper */
        switch (i) {
        case 0:  g_bk_BinkSetSoundSystem.real = real; break;
        case 1:  g_bk_BinkShouldSkip.real = real; break;
        case 2:  g_bk_BinkSetMemory.real = real; break;
        case 3:  g_bk_BinkPause.real = real; break;
        case 4:  g_bk_BinkNextFrame.real = real; break;
        case 5:  g_bk_BinkGoto.real = real; break;
        case 6:  g_bk_BinkClose.real = real; break;
        case 7:  g_bk_BinkRegisterFrameBuffers.real = real; break;
        case 8:  g_bk_BinkGetFrameBuffersInfo.real = real; break;
        case 9:  g_bk_BinkOpen.real = real; break;
        case 10: g_bk_BinkLogoAddress.real = real; break;
        case 11: g_bk_BinkDoFrame.real = real; break;
        case 12: g_bk_BinkOpenDirectSound.real = real; break;
        case 13: g_bk_BinkWait.real = real; break;
        case 14: g_bk_BinkSetVolume.real = real; break;
        case 15: g_bk_BinkSetSoundOnOff.real = real; break;
        }
        {   char nm[96];
            snprintf(nm, sizeof nm, "binkw32!%s", k_wrapped[i].name);
            rt_register_data("binkw32.dll", k_wrapped[i].name, rt_thunk(k_wrapped[i].fn, strdup(nm)));
        }
        wrapped++;
    }
    LOG("bink: %d of %d entry points wrapped with logging", wrapped,
        (int)(sizeof k_wrapped / sizeof k_wrapped[0]));
}
