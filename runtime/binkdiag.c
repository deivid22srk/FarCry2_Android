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
 * lifted original through rt_invoke, logs, and returns. */
#define BINK_WRAPPER(cname, nargs, rare, args_log)                             \
    static struct { uint32_t real; uint32_t calls; } g_bk_##cname;             \
    static void shim_bk_##cname(CPU *c) {                                      \
        uint32_t a[nargs ? nargs : 1];                                         \
        for (int i = 0; i < nargs; i++) a[i] = ARG(i);                         \
        if (g_bk_##cname.real && g_bk_##cname.real < GUEST_THUNK_BASE) {       \
            uint32_t r = rt_invoke(c, g_bk_##cname.real, nargs, a);            \
            if (rare || bink_log_gate(&g_bk_##cname.calls))                    \
                LOG("bink: " #cname "(%s) -> %08x (call %u)", args_log, r,     \
                    g_bk_##cname.calls);                                       \
            RET(r, nargs);                                                     \
        }                                                                      \
        LOG("bink: " #cname " has no lifted original; failing");               \
        RET(0, nargs);                                                         \
    }

/* arg formatters (GSTR on a guest pointer is safe: whole sections are copied
 * into guest memory and NUL-padded) */
#define A0_NAME  "name=\"%s\" flags=%x", GSTR(a[0]) ? GSTR(a[0]) : "?", a[1]
#define A1_PTR   "p=%08x", a[0]
#define A2_PTRS  "p=%08x, %08x", a[0], a[1]
#define A3_PTRS  "p=%08x, %08x, %08x", a[0], a[1], a[2]
#define A_NONE   ""
/* The BINK struct starts Width, Height, FrameCount (bink_test reads them
 * there); log them so a dead decoder (all zeros) is visible in the log. */
#define A_BINK   "bink=%08x [w=%u h=%u frames=%u]", a[0], a[0] >= 0x10000 ? M32(a[0]) : 0, \
                 a[0] >= 0x10000 ? M32(a[0] + 4) : 0, a[0] >= 0x10000 ? M32(a[0] + 8) : 0

/* The 16 binkw32 entry points Dunia.dll imports (from the first phone log).
 * The @N suffixes are the stdcall arg byte counts, matching nargs. */
BINK_WRAPPER(BinkSetSoundSystem,        2, 1, A2_PTRS)
BINK_WRAPPER(BinkShouldSkip,            1, 0, A1_PTR)
BINK_WRAPPER(BinkSetMemory,             2, 1, A2_PTRS)
BINK_WRAPPER(BinkPause,                 2, 1, A2_PTRS)
BINK_WRAPPER(BinkNextFrame,             1, 0, A_BINK)
BINK_WRAPPER(BinkGoto,                  3, 1, A3_PTRS)
BINK_WRAPPER(BinkClose,                 1, 1, A1_PTR)
BINK_WRAPPER(BinkRegisterFrameBuffers,  2, 1, A2_PTRS)
BINK_WRAPPER(BinkGetFrameBuffersInfo,   2, 1, A2_PTRS)
BINK_WRAPPER(BinkOpen,                  2, 1, A0_NAME)
BINK_WRAPPER(BinkLogoAddress,           0, 1, A_NONE)
BINK_WRAPPER(BinkDoFrame,               1, 0, A_BINK)
BINK_WRAPPER(BinkOpenDirectSound,       1, 1, A1_PTR)
BINK_WRAPPER(BinkWait,                  1, 0, A_BINK)
BINK_WRAPPER(BinkSetVolume,             3, 1, A3_PTRS)
BINK_WRAPPER(BinkSetSoundOnOff,         2, 1, A2_PTRS)

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
