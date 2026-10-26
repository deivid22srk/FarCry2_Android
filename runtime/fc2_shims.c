/* Far Cry 2 (PC, 2008) runtime shims.
 *
 * Everything here exists because Dunia.dll / farcry2.exe import it and the
 * shared runtime does not cover it. Three groups:
 *
 *   1. MSVCR80.dll - the VC8 C runtime. The C half is aliased to the shared
 *      msvcrt table (runtime/msvcrt.c + runtime/crt_shims.c); this file adds
 *      the VC8 entry points that table does not have.
 *   2. MSVCP80.dll - the VC8 C++ library. std::basic_string<char/wchar_t> and
 *      char_traits are implemented against the documented VC8 layout
 *      (16-byte inline buffer, size, capacity). The iostream/stringstream
 *      entry points are NOT implemented: they fail loudly with their mangled
 *      name, so a run that needs them stops at one identifiable import instead
 *      of corrupting memory. See docs/RECON.md, "Known gaps".
 *   3. Win32 APIs the shared runtime lacks: Winsock/WinHTTP/WinINet (no
 *      network), gdiplus, dbghelp, EAX, Secur32/Crypt32 (online only), named
 *      pipes, PSAPI, IP Helper.
 *
 * Calling conventions matter here: the MSVCR80 exports are __cdecl (RETC),
 * the MSVCP80 member functions are __thiscall (RET with the argument count,
 * leaving `this` for the caller), and Win32 is __stdcall (RET).
 */
#define _GNU_SOURCE
#include "rt.h"
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <wctype.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <wchar.h>
#include <stdarg.h>
#include <sys/stat.h>

void msvcrt_register_as_msvcr80(void);

/* once-per-import logging keeps a 60 fps failure path from filling log.txt */
static char g_once_seen[64][72];
static int g_once_n;
static int once(const char *what) {
    for (int i = 0; i < g_once_n; i++)
        if (!strcmp(g_once_seen[i], what)) return 0;
    if (g_once_n < 64) snprintf(g_once_seen[g_once_n++], sizeof g_once_seen[0], "%s", what);
    LOG("fc2: %s (not implemented, documented failure value)", what);
    return 1;
}

/* ==================================================================== */
/* MSVCR80: the C entry points that msvcrt's table does not have        */
/* ==================================================================== */
#define RETXMMF(v) do { c->xmm[0].f[0] = (float)(v); c->r[0] = 0; c->r[4] += 4; return; } while (0)
#define RETXMMD(v) do { c->xmm[0].d[0] = (double)(v); c->r[0] = 0; c->r[4] += 4; return; } while (0)

/* __libm_sse2_* take their argument in xmm0 and return in xmm0. */
#define SSE2_1(name, expr) \
    SHIM(name) { double x = c->xmm[0].d[0]; (void)x; RETXMMD(expr); }
SSE2_1(__libm_sse2_sin, sin(x))
SSE2_1(__libm_sse2_cos, cos(x))
SSE2_1(__libm_sse2_tan, tan(x))
SSE2_1(__libm_sse2_asin, asin(x))
SSE2_1(__libm_sse2_acos, acos(x))
SSE2_1(__libm_sse2_atan, atan(x))
SSE2_1(__libm_sse2_exp, exp(x))
SSE2_1(__libm_sse2_log, log(x))
SSE2_1(__libm_sse2_log10, log10(x))
SHIM(__libm_sse2_pow) { double a = c->xmm[0].d[0], b = c->xmm[1].d[0]; RETXMMD(pow(a, b)); }
SHIM(__libm_sse2_atan2) { double a = c->xmm[0].d[0], b = c->xmm[1].d[0]; RETXMMD(atan2(a, b)); }

SHIM(memcmp) { RETC(memcmp(GPTR(ARG(0)), GPTR(ARG(1)), ARG(2))); }
SHIM(strcmp) { RETC(strcmp(GSTR(ARG(0)), GSTR(ARG(1)))); }
SHIM(strlen) { RETC((uint32_t)strlen(GSTR(ARG(0)))); }
SHIM(wcslen) { RETC((uint32_t)wcslen((const wchar_t *)GPTR(ARG(0)))); }
SHIM(vfprintf) { RETC(0); }                   /* a guest va_list cannot cross */
SHIM(_vsnprintf_s) { RETC(-1); }
SHIM(vsprintf_s) { RETC(-1); }
SHIM(_vscprintf) { RETC(-1); }
SHIM(strftime) {
    const char *fmt = GSTR(ARG(2));
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    uint32_t t = ARG(3);
    if (t) {
        tm.tm_sec = M32(t + 0); tm.tm_min = M32(t + 4); tm.tm_hour = M32(t + 8);
        tm.tm_mday = M32(t + 12); tm.tm_mon = M32(t + 16); tm.tm_year = M32(t + 20);
        tm.tm_wday = M32(t + 24); tm.tm_yday = M32(t + 28); tm.tm_isdst = M32(t + 32);
    }
    RETC((uint32_t)strftime(GSTR(ARG(0)), ARG(1), fmt, &tm));
}
SHIM(strnlen) { RETC((uint32_t)strnlen(GSTR(ARG(0)), ARG(1))); }
SHIM(_memicmp) {
    const uint8_t *a = GPTR(ARG(0)), *b = GPTR(ARG(1));
    uint32_t n = ARG(2);
    int r = 0;
    for (uint32_t i = 0; i < n && !r; i++) r = tolower(a[i]) - tolower(b[i]);
    RETC(r);
}
SHIM(_strtoui64) { RETC((uint32_t)strtoull(GSTR(ARG(0)), NULL, ARG(2))); }
SHIM(_atoi64) { RETC((uint32_t)strtoll(GSTR(ARG(0)), NULL, 10)); }
SHIM(_itow) {
    wchar_t *d = GPTR(ARG(1));
    if (d) swprintf(d, 32, L"%d", (int32_t)ARG(0));
    RETC((uint32_t)(uintptr_t)d - (uint32_t)(uintptr_t)MEM);
}
SHIM(_wtoi) { RETC((uint32_t)wcstol((const wchar_t *)GPTR(ARG(0)), NULL, 10)); }
SHIM(_wcslwr) {
    wchar_t *p = GPTR(ARG(0));
    for (wchar_t *q = p; q && *q; q++) *q = towlower(*q);
    RETC((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)MEM);
}
SHIM(towupper) { RETC(towupper((wint_t)ARG(0))); }
SHIM(towlower) { RETC(towlower((wint_t)ARG(0))); }
SHIM(wcsstr) { RETC((uint32_t)(uintptr_t)wcsstr((const wchar_t *)GPTR(ARG(0)), (const wchar_t *)GPTR(ARG(1))) - (uint32_t)(uintptr_t)MEM); }
SHIM(wcsncpy) { RETC((uint32_t)(uintptr_t)wcsncpy((wchar_t *)GPTR(ARG(0)), (const wchar_t *)GPTR(ARG(1)), ARG(2)) - (uint32_t)(uintptr_t)MEM); }
SHIM(wcscspn) { RETC((uint32_t)wcscspn((const wchar_t *)GPTR(ARG(0)), (const wchar_t *)GPTR(ARG(1)))); }
SHIM(_vswprintf) { RETC(-1); }
SHIM(_vswprintf_c_l) { RETC(-1); }
/* ldiv_t is 8 bytes, returned in eax:edx (no hidden pointer on x86). */
SHIM(ldiv) {
    int32_t a = (int32_t)ARG(0), b = (int32_t)ARG(1);
    c->r[0] = (uint32_t)(a / b);
    c->r[2] = (uint32_t)(a % b);
    c->r[4] += 4;
    return;
}
SHIM(_get_heap_handle) { RETC(0x00A40000u); }
SHIM(_malloc_crt) { RETC(gmalloc(ARG(0))); }
SHIM(_encode_pointer) { RETC(ARG(0)); }
SHIM(_decode_pointer) { RETC(ARG(0)); }
SHIM(_encoded_null) { RETC(0); }
SHIM(__iob_func) {
    extern uint32_t msvcrt_iob(void);
    RETC(msvcrt_iob());
}
SHIM(_chmod) { RETC(0); }
SHIM(rename) { RETC(rename(GSTR(ARG(0)), GSTR(ARG(1)))); }
SHIM(remove) { RETC(remove(GSTR(ARG(0)))); }
SHIM(_time64) { RETC((uint32_t)time(NULL)); }
SHIM(_utime64) { RETC(0); }
SHIM(_localtime64) { RETC((uint32_t)(uintptr_t)localtime((const time_t *)GPTR(ARG(0))) - (uint32_t)(uintptr_t)MEM); }
SHIM(_gmtime64) { RETC((uint32_t)(uintptr_t)gmtime((const time_t *)GPTR(ARG(0))) - (uint32_t)(uintptr_t)MEM); }
SHIM(_mktime64) { RETC((uint32_t)mktime((struct tm *)GPTR(ARG(0)))); }
SHIM(_localtime64_s) { RETC(0); }
SHIM(_difftime64) { RETC((uint32_t)(int32_t)difftime((time_t)ARG(0), (time_t)ARG(1))); }
SHIM(islower) { RETC(islower(ARG(0))); }
SHIM(isupper) { RETC(isupper(ARG(0))); }
SHIM(ispunct) { RETC(ispunct(ARG(0))); }
SHIM(iscntrl) { RETC(iscntrl(ARG(0))); }
SHIM(strpbrk) { RETC((uint32_t)(uintptr_t)strpbrk(GSTR(ARG(0)), GSTR(ARG(1))) - (uint32_t)(uintptr_t)MEM); }
SHIM(strcspn) { RETC((uint32_t)strcspn(GSTR(ARG(0)), GSTR(ARG(1)))); }
SHIM(strcoll) { RETC(strcoll(GSTR(ARG(0)), GSTR(ARG(1)))); }
SHIM(printf) { RETC(0); }
SHIM(strerror) { RETC((uint32_t)(uintptr_t)strerror(ARG(0)) - (uint32_t)(uintptr_t)MEM); }

/* _s variants: the destination-size check is real, the rest forwards. */
static uint32_t safe_copy(void *dst, size_t dstsz, const void *src, size_t n) {
    if (!dst || !dstsz || n >= dstsz) return 0x16; /* EINVAL */
    memcpy(dst, src, n);
    ((char *)dst)[n] = 0;
    return 0;
}
SHIM(strcpy_s) { RETC(safe_copy(GPTR(ARG(0)), ARG(1), GSTR(ARG(2)), strlen(GSTR(ARG(2))))); }
SHIM(strncpy_s) { RETC(safe_copy(GPTR(ARG(0)), ARG(1), GSTR(ARG(2)), strnlen(GSTR(ARG(2)), ARG(3)))); }
SHIM(strcat_s) {
    char *d = GPTR(ARG(0));
    size_t dl = d ? strlen(d) : 0;
    RETC(safe_copy(d ? d + dl : NULL, ARG(1) > dl ? ARG(1) - dl : 0, GSTR(ARG(2)), strlen(GSTR(ARG(2)))));
}
SHIM(strtok_s) { RETC((uint32_t)(uintptr_t)strtok_r(GSTR(ARG(0)), GSTR(ARG(1)), (char **)GPTR(ARG(2))) - (uint32_t)(uintptr_t)MEM); }
SHIM(memmove_s) {
    void *d = GPTR(ARG(0));
    size_t dn = ARG(1), n = ARG(3);
    if (!d || n > dn) RETC(0x16);
    memmove(d, GPTR(ARG(2)), n);
    RETC(0);
}
SHIM(sprintf_s) { RETC(-1); }
SHIM(sscanf_s) { RETC(-1); }
SHIM(_snprintf_s) { RETC(-1); }
SHIM(_controlfp_s) { if (ARG(2)) M32(ARG(2)) = 0x0008001F; RETC(0); }

/* VC8 exception machinery. The lifted code keeps x86 stack frames, so a real
 * implementation is possible (walk fs:[0] and call the guest handler), but it
 * is not written yet: stop with the guest address instead of jumping anywhere. */
SHIM(_CxxThrowException) {
    rt_fatal("C++ exception thrown at guest %08x (type %08x); SEH/C++ unwinding is not implemented "
             "for this port yet - see docs/RECON.md, 'Known gaps'", M32(c->r[4]), ARG(1));
}
SHIM(__CxxFrameHandler3) {
    rt_fatal("__CxxFrameHandler3 reached (guest exception at %08x); C++ unwinding is not implemented",
             M32(c->r[4]));
}
SHIM(_except_handler3) {
    rt_fatal("_except_handler3 reached (guest exception at %08x); SEH unwinding is not implemented",
             M32(c->r[4]));
}
SHIM(_except_handler4_common) {
    rt_fatal("_except_handler4_common reached (guest exception at %08x); SEH unwinding is not implemented",
             M32(c->r[4]));
}
SHIM(__CppXcptFilter) { RETC(1); }       /* EXCEPTION_CONTINUE_SEARCH */
SHIM(_crt_debugger_hook) { RETC(0); }
SHIM(_invalid_parameter_noinfo) { RETC(0); }
SHIM(__clean_type_info_names_internal) { RETC(0); }
SHIM(type_info_dtor_internal) { RET(0, 1); }
SHIM(_initterm_e) {
    for (uint32_t p = ARG(0); p < ARG(1); p += 4) {
        uint32_t fn = M32(p);
        if (fn) { uint32_t dummy = 0; rt_invoke(c, fn, 0, &dummy); }
    }
    RETC(0);
}

/* _beginthreadex: same guest-thread machinery as CreateThread, different
 * return convention (uintptr_t handle, 0 on failure). */
typedef struct { uint32_t start, arg; } BThread;
static void *bthread_main(void *p) {
    BThread *t = p;
    CPU *cpu = rt_new_cpu();
    rt_thread_init(cpu);
    uint32_t a[1] = {t->arg};
    rt_invoke(cpu, t->start, 1, a);
    free(t);
    return NULL;
}
SHIM(_beginthreadex) {
    BThread *t = calloc(1, sizeof *t);
    t->start = ARG(2);
    t->arg = ARG(3);
    pthread_t pt;
    if (pthread_create(&pt, NULL, bthread_main, t)) { free(t); RETC(0); }
    pthread_detach(pt);
    if (ARG(5)) M32(ARG(5)) = (uint32_t)(uintptr_t)pt;
    RETC((uint32_t)(uintptr_t)pt);
}

/* ==================================================================== */
/* MSVCP80: VC8 std::string ABI                                          */
/* ==================================================================== */
/* MSVC 8 layout, x86: union { char _Buf[16]; char *_Ptr; } _Bx; size_t
 * _Mysize; size_t _Myres. Inline when _Myres < 16 (8 for wchar_t),
 * otherwise _Bx is a guest heap pointer. */
typedef struct { uint32_t bx; uint32_t pad[3]; uint32_t size, res; } Vc8Str;

static char *vc8_ptr(Vc8Str *s) { return s->res >= 16 ? (char *)(uintptr_t)s->bx : (char *)s; }
static wchar_t *vc8_wptr(Vc8Str *s) { return s->res >= 8 ? (wchar_t *)(uintptr_t)s->bx : (wchar_t *)s; }
static void vc8_set_c(Vc8Str *s, const char *p, uint32_t n) {
    if (n < 16) {
        s->res = 15;
        memcpy(s, p, n);
        ((char *)s)[n] = 0;
    } else {
        uint32_t d = gmalloc(n + 1);
        memcpy(MEM + d, p, n);
        M8(d + n) = 0;
        s->bx = d;
        s->res = n;
    }
    s->size = n;
}
static void vc8_set_w(Vc8Str *s, const wchar_t *p, uint32_t n) {
    if (n < 8) {
        s->res = 7;
        memcpy(s, p, 2 * n);
        ((wchar_t *)s)[n] = 0;
    } else {
        uint32_t d = gmalloc(2 * (n + 1));
        memcpy(MEM + d, p, 2 * n);
        M16(d + 2 * n) = 0;
        s->bx = d;
        s->res = n;
    }
    s->size = n;
}

/* thiscall: RET(v, nargs) leaves `this` for the caller, as MSVC ret N does. */
SHIM(str_ctor_cstr) {
    Vc8Str *s = GPTR(ARG(0));
    const char *p = GSTR(ARG(1));
    vc8_set_c(s, p ? p : "", p ? (uint32_t)strlen(p) : 0);
    RET(0, 1);
}
SHIM(str_ctor_copy) {
    Vc8Str *s = GPTR(ARG(0)), *o = GPTR(ARG(1));
    vc8_set_c(s, vc8_ptr(o), o->size);
    RET(0, 1);
}
SHIM(str_dtor) {
    Vc8Str *s = GPTR(ARG(0));
    if (s->res >= 16) gfree(s->bx);
    s->res = 15;
    s->size = 0;
    ((char *)s)[0] = 0;
    RET(0, 0);
}
SHIM(str_size) { RET(((Vc8Str *)GPTR(ARG(0)))->size, 0); }
SHIM(str_c_str) { RET((uint32_t)(uintptr_t)vc8_ptr(GPTR(ARG(0))) - (uint32_t)(uintptr_t)MEM, 0); }
SHIM(str_wc_str) { RET((uint32_t)(uintptr_t)vc8_wptr(GPTR(ARG(0))) - (uint32_t)(uintptr_t)MEM, 0); }
SHIM(str_w_ctor_cstr) {
    Vc8Str *s = GPTR(ARG(0));
    const wchar_t *p = (const wchar_t *)GPTR(ARG(1));
    vc8_set_w(s, p ? p : L"", p ? (uint32_t)wcslen(p) : 0);
    RET(0, 1);
}
SHIM(str_w_dtor) {
    Vc8Str *s = GPTR(ARG(0));
    if (s->res >= 8) gfree(s->bx);
    s->res = 7;
    s->size = 0;
    ((wchar_t *)s)[0] = 0;
    RET(0, 0);
}

/* char_traits: static members are __cdecl. */
SHIM(tr_length) { RETC((uint32_t)strlen(GSTR(ARG(0)))); }
SHIM(tr_wlength) { RETC((uint32_t)wcslen((const wchar_t *)GPTR(ARG(0)))); }
SHIM(tr_assign_c) { M8(ARG(0)) = M8(ARG(1)); RETC(0); }
SHIM(tr_assign_w) { M16(ARG(0)) = M16(ARG(1)); RETC(0); }
SHIM(tr_assign_n) {
    uint32_t d = ARG(0);
    memset(MEM + d, (int)ARG(2), ARG(1));
    RETC(d);
}
SHIM(tr_assign_wn) {
    uint32_t d = ARG(0);
    for (uint32_t i = 0; i < ARG(1); i++) M16(d + 2 * i) = (uint16_t)ARG(2);
    RETC(d);
}
SHIM(tr_eq) { RETC(M8(ARG(0)) == M8(ARG(1))); }
SHIM(tr_weq) { RETC(M16(ARG(0)) == M16(ARG(1))); }
SHIM(tr_eq_int) { RETC(M32(ARG(0)) == M32(ARG(1))); }
SHIM(tr_weq_int) { RETC(M16(ARG(0)) == M16(ARG(1))); }
SHIM(tr_eof) { RETC(0xFFFFFFFFu); }
SHIM(tr_weof) { RETC(0xFFFFu); }
SHIM(tr_compare) {
    const uint8_t *a = GPTR(ARG(0)), *b = GPTR(ARG(1));
    int r = 0;
    for (uint32_t i = 0; i < ARG(2) && !r; i++) r = a[i] - b[i];
    RETC(r);
}
SHIM(tr_find) {
    const uint8_t *p = GPTR(ARG(0)), *ch = GPTR(ARG(2));
    for (uint32_t i = 0; i < ARG(1); i++)
        if (p[i] == *ch) RETC((uint32_t)((uintptr_t)(p + i) - (uintptr_t)MEM));
    RETC(0);
}
SHIM(tr_copy_s) {
    memcpy(GPTR(ARG(0)), GPTR(ARG(2)), ARG(3));
    RETC(ARG(0));
}
SHIM(tr_move_s) {
    memmove(GPTR(ARG(0)), GPTR(ARG(2)), ARG(3));
    RETC(ARG(0));
}
SHIM(tr_xlen) { rt_fatal("std::string length_error (_Xlen) reached"); }
SHIM(tr_xran) { rt_fatal("std::string out_of_range (_Xran) reached"); }

/* ios_base / basic_ios (thiscall) */
SHIM(ios_good) { RET(M32(ARG(0) + 4) == 0, 0); }
SHIM(ios_flags) { RET(M32(ARG(0) + 0x10), 0); }
SHIM(ios_width_get) { RET(M32(ARG(0) + 0x0C), 0); }
SHIM(ios_width_set) {
    uint32_t old = M32(ARG(0) + 0x0C);
    M32(ARG(0) + 0x0C) = ARG(1);
    RET(old, 1);
}
SHIM(ios_setstate) { M32(ARG(0) + 4) |= ARG(1); RET(0, 2); }
SHIM(ios_clear) { M32(ARG(0) + 4) = ARG(1); RET(0, 2); }
SHIM(num_epsilon) { RETXMMF(1.192092896e-07f); }
SHIM(uncaught_exception) { RETC(0); }

/* ==================================================================== */
/* Win32 APIs the shared runtime lacks                                  */
/* ==================================================================== */
/* ---- Winsock: single player must run offline; every call fails the way
 * Windows does with no network, and WSAStartup succeeds so the engine's
 * network thread exits through the normal path. ---- */
SHIM(WSAStartup) {
    if (ARG(1)) { memset(GPTR(ARG(1)), 0, 400); M16(ARG(1)) = 0x0202; }
    once("WS2_32.WSAStartup (offline: no sockets)");
    RET(0, 2);
}
SHIM(WSACleanup) { RET(0, 0); }
SHIM(WSAGetLastError) { RET(10050, 0); }       /* WSAENETDOWN */
SHIM(socket) { once("WS2_32.socket (offline)"); RET(0xFFFFFFFFu, 3); }
SHIM(closesocket) { RET(0, 1); }
SHIM(connect) { RET(0xFFFFFFFFu, 3); }
SHIM(bind) { RET(0xFFFFFFFFu, 3); }
SHIM(listen) { RET(0xFFFFFFFFu, 2); }
SHIM(accept) { RET(0xFFFFFFFFu, 3); }
SHIM(send) { RET(0xFFFFFFFFu, 4); }
SHIM(recv) { RET(0xFFFFFFFFu, 4); }
SHIM(sendto) { RET(0xFFFFFFFFu, 6); }
SHIM(recvfrom) { RET(0xFFFFFFFFu, 6); }
SHIM(select) { RET(0, 5); }
SHIM(setsockopt) { RET(0xFFFFFFFFu, 5); }
SHIM(getsockopt) { if (ARG(4)) M32(ARG(4)) = 0; RET(0xFFFFFFFFu, 5); }
SHIM(ioctlsocket) { RET(0xFFFFFFFFu, 3); }
SHIM(shutdown) { RET(0xFFFFFFFFu, 2); }
SHIM(gethostbyname) { RET(0, 1); }
SHIM(gethostbyaddr) { RET(0, 3); }
SHIM(gethostname) { RET(0xFFFFFFFFu, 2); }
SHIM(getaddrinfo) { RET(11001, 4); }            /* WSAHOST_NOT_FOUND */
SHIM(freeaddrinfo) { RET(0, 1); }
SHIM(getsockname) { RET(0xFFFFFFFFu, 3); }
SHIM(inet_addr) { RET(0xFFFFFFFFu, 1); }
SHIM(inet_ntoa) { RET((uint32_t)(uintptr_t)"0.0.0.0" - (uint32_t)(uintptr_t)MEM, 1); }
SHIM(htonl) { RET(ARG(0), 1); }
SHIM(htons) { RET(((ARG(0) & 0xFF) << 8) | ((ARG(0) >> 8) & 0xFF), 1); }
SHIM(ntohl) { RET(ARG(0), 1); }
SHIM(ntohs) { RET(((ARG(0) & 0xFF) << 8) | ((ARG(0) >> 8) & 0xFF), 1); }
SHIM(WSAAsyncSelect) { RET(0xFFFFFFFFu, 4); }
SHIM(__WSAFDIsSet) { RET(0, 2); }

/* ---- WinHTTP / WinINet: no HTTP. ---- */
#define HTTP0(name, n) SHIM(name) { once("HTTP." #name " (offline)"); RET(0, n); }
HTTP0(WinHttpOpen, 4) HTTP0(WinHttpConnect, 4) HTTP0(WinHttpOpenRequest, 7)
HTTP0(WinHttpSendRequest, 6) HTTP0(WinHttpReceiveResponse, 3)
HTTP0(WinHttpQueryHeaders, 6) HTTP0(WinHttpReadData, 4)
HTTP0(WinHttpQueryDataAvailable, 3) HTTP0(WinHttpWriteData, 4)
HTTP0(WinHttpCloseHandle, 1) HTTP0(WinHttpSetOption, 4)
HTTP0(WinHttpAddRequestHeaders, 4) HTTP0(WinHttpSetTimeouts, 5)
HTTP0(WinHttpSetStatusCallback, 4) HTTP0(WinHttpCrackUrl, 4)
HTTP0(WinHttpGetIEProxyConfigForCurrentUser, 1)
HTTP0(WinHttpGetDefaultProxyConfiguration, 1)
HTTP0(InternetSetCookieW, 3) HTTP0(InternetGetCookieW, 4)

/* ---- TLS/SSL for online services: unsupported. ---- */
#define SEC0(name, n) SHIM(name) { once("Secur32." #name " (online only)"); RET(0x80090304u, n); }
SEC0(AcquireCredentialsHandleA, 9) SEC0(FreeCredentialsHandle, 1)
SEC0(InitializeSecurityContextA, 9) SEC0(DeleteSecurityContext, 1)
SEC0(QueryContextAttributesA, 3) SEC0(EncryptMessage, 4) SEC0(DecryptMessage, 4)
SEC0(ApplyControlToken, 2) SEC0(FreeContextBuffer, 1)
SHIM(CertGetCertificateChain) { RET(0, 9); }
SHIM(CertFreeCertificateChain) { RET(0, 1); }
SHIM(CertFreeCertificateContext) { RET(1, 1); }
SHIM(CertVerifyCertificateChainPolicy) { RET(0, 4); }

/* ---- Crypt32/Advapi hashing (online auth): accepts, returns fixed bytes. */
SHIM(CryptAcquireContextA) { if (ARG(0)) M32(ARG(0)) = 1; RET(1, 5); }
SHIM(CryptReleaseContext) { RET(1, 2); }
SHIM(CryptCreateHash) { if (ARG(5)) M32(ARG(5)) = 2; RET(1, 6); }
SHIM(CryptHashData) { RET(1, 4); }
SHIM(CryptGetHashParam) { if (ARG(4)) M32(ARG(4)) = ARG(2); if (ARG(2)) memset(GPTR(ARG(2)), 0, ARG(3)); RET(1, 5); }
SHIM(CryptDestroyHash) { RET(1, 1); }
SHIM(CryptGenRandom) { if (ARG(2)) memset(GPTR(ARG(1)), 0x5A, ARG(2)); RET(1, 3); }

/* ---- IP Helper / PSAPI ---- */
SHIM(GetAdaptersInfo) { if (ARG(1)) M32(ARG(1)) = 0; RET(232, 2); } /* ERROR_NO_DATA */
SHIM(GetPerformanceInfo) { if (ARG(0)) memset(GPTR(ARG(0)), 0, 48); RET(1, 2); }

/* ---- RPC ---- */
SHIM(UuidToStringA) {
    static const char hex[] = "0123456789abcdef";
    if (!ARG(1)) RET(87, 2);
    uint32_t o = gmalloc(40);
    char *s = GSTR(o);
    const uint8_t *u = GPTR(ARG(0));
    int k = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s[k++] = '-';
        s[k++] = hex[u[i] >> 4];
        s[k++] = hex[u[i] & 15];
    }
    s[k] = 0;
    M32(ARG(1)) = o;
    RET(0, 2);
}
SHIM(RpcStringFreeA) { if (ARG(0)) gfree(M32(ARG(0))); RET(0, 1); }

/* ---- OLE ---- */
SHIM(CoCreateGuid) {
    if (ARG(0)) {
        static uint32_t ctr;
        uint8_t *g = GPTR(ARG(0));
        for (int i = 0; i < 16; i++) g[i] = (uint8_t)(++ctr * 37 + i);
        g[6] = (g[6] & 0x0F) | 0x40;
        g[8] = (g[8] & 0x3F) | 0x80;
    }
    RET(0, 1);
}
SHIM(CreateStreamOnHGlobal) { RET(0x80004005u, 3); } /* E_FAIL */

/* ---- gdiplus: startup succeeds; image calls fail and the engine falls
 * back to its own drawing. ---- */
SHIM(GdiplusStartup) { if (ARG(0)) M32(ARG(0)) = 1; RET(0, 3); }
SHIM(GdiplusShutdown) { RET(0, 1); }
SHIM(GdipAlloc) { RET(gmalloc(ARG(0)), 1); }
SHIM(GdipFree) { gfree(ARG(0)); RET(0, 1); }
#define GDIP_FAIL(name, n) SHIM(name) { once("gdiplus." #name " (image path unsupported)"); RET(0x80004005u, n); }
GDIP_FAIL(GdipCloneImage, 4) GDIP_FAIL(GdipDisposeImage, 1)
GDIP_FAIL(GdipCreateBitmapFromStream, 2) GDIP_FAIL(GdipCreateHBITMAPFromBitmap, 4)

/* ---- dbghelp: the engine's own crash reporter. No symbols, no walking. ---- */
SHIM(SymInitialize) { RET(1, 3); }
SHIM(SymCleanup) { RET(1, 1); }
SHIM(SymGetOptions) { RET(0, 0); }
SHIM(SymSetOptions) { RET(ARG(0), 1); }
SHIM(SymSetSearchPath) { RET(1, 2); }
SHIM(SymLoadModule) { RET(0, 6); }
SHIM(SymUnloadModule) { RET(1, 3); }
SHIM(SymGetModuleInfo) { RET(0, 3); }
SHIM(SymGetModuleInfo64) { RET(0, 3); }
SHIM(SymGetSymFromAddr) { RET(0, 4); }
SHIM(SymGetSymFromAddr64) { RET(0, 4); }
SHIM(SymGetLineFromAddr) { RET(0, 4); }
SHIM(SymGetLineFromAddr64) { RET(0, 4); }
SHIM(SymFindFileInPath) { RET(0, 8); }
SHIM(SymFunctionTableAccess64) { RET(0, 2); }
SHIM(StackWalk64) { RET(0, 9); }
SHIM(EnumerateLoadedModules) { RET(0, 3); }
SHIM(EnumerateLoadedModules64) { RET(0, 3); }

/* ---- EAX Unified: report "no EAX device" so the engine uses DirectSound8. ---- */
SHIM(EAXDirectSoundCreate8) { once("EAX.DLL!#6 EAXDirectSoundCreate8 (no EAX; DirectSound8 fallback)"); RET(0x88780082u, 3); }

/* ---- DirectSound enumeration (ordinal #2): no devices, default is used. ---- */
SHIM(DirectSoundEnumerateA) { RET(0, 2); }

/* ---- winmm ---- */
SHIM(waveOutGetDevCapsA) { RET(6, 3); } /* MMSYSERR_NODRIVER */

/* ---- kernel32 gaps used by Dunia ---- */
SHIM(OpenThread) { RET(0, 3); }
SHIM(GetThreadContext) { RET(0, 2); }
SHIM(GetSystemPowerStatus) {
    if (ARG(0)) {
        uint8_t *p = GPTR(ARG(0));
        p[0] = 1;    /* AC online */
        p[1] = 255;  /* unknown battery */
        p[2] = 100;  /* 100% */
        M32(ARG(0) + 4) = 0;
    }
    RET(1, 1);
}
SHIM(SetThreadPriorityBoost) { RET(1, 2); }
SHIM(SetEnvironmentVariableW) { RET(1, 2); }
SHIM(GetPrivateProfileStringW) { RET(0, 6); }
SHIM(InterlockedExchangeAdd) { RET(__atomic_fetch_add((uint32_t *)GPTR(ARG(0)), ARG(1), __ATOMIC_SEQ_CST), 2); }

/* Named pipes: the engine creates one for its console/remote log. Refuse
 * cleanly; the callers treat it as "not available". */
SHIM(CreateNamedPipeA) { once("kernel32.CreateNamedPipeA (no pipes)"); RET(0xFFFFFFFFu, 7); }
SHIM(ConnectNamedPipe) { RET(0, 2); }
SHIM(DisconnectNamedPipe) { RET(0, 1); }
SHIM(GetNamedPipeHandleStateA) { RET(0, 5); }
SHIM(SetNamedPipeHandleState) { RET(0, 4); }
SHIM(PeekNamedPipe) { RET(0, 5); }
SHIM(ReadDirectoryChangesW) { RET(0, 6); }
SHIM(DeviceIoControl) { RET(0, 8); }
SHIM(LoadResource) { once("kernel32.LoadResource (resources not exposed)"); RET(0, 2); }
SHIM(SizeofResource) { RET(0, 2); }
SHIM(FindResourceA) { RET(0, 3); }
SHIM(LockResource) { RET(ARG(0), 1); }

/* ==================================================================== */
/* imports the first boot of the game was observed to need             */
/* (docs/RECON.md, "Known gaps": these used to abort)                   */
/* ==================================================================== */

/* UTF-16 guest string -> newly allocated ANSI guest string. */
static uint32_t narrow_dup(uint32_t w) {
    char buf[520];
    size_t i = 0;
    for (; w && i < sizeof buf - 1 && M16(w + 2 * i); i++) buf[i] = (char)M16(w + 2 * i);
    buf[i] = 0;
    return gstrdup(buf);
}

/* ---- MSVCR80: CRT entry points the shared table does not have -------- */
/* _configthreadlocale(_ENABLE_PER_THREAD_LOCALE): 0 = the previous mode. */
SHIM(_configthreadlocale) { RETC(0); }
/* The CRT's invalid-parameter handler. In a real process it ends the
 * process; there is nothing sensible to return to. */
SHIM(_invoke_watson) {
    rt_fatal("_invoke_watson: the CRT invalid-parameter handler was called");
}
SHIM(memchr) {
    uint32_t n = ARG(2);
    const void *r = n ? memchr(GPTR(ARG(0)), (int)(ARG(1) & 0xFF), n) : NULL;
    RETC(r ? (uint32_t)((const uint8_t *)r - (const uint8_t *)MEM) : 0);
}
/* _itoa_s(value, buffer, sizeInChars, radix): 0 on success, EINVAL/ERANGE
 * otherwise, lowercase digits, buffer emptied on failure. */
SHIM(_itoa_s) {
    uint32_t buf = ARG(1), size = ARG(2);
    int32_t v = (int32_t)ARG(0), radix = (int32_t)ARG(3);
    if (size) ((char *)MEM)[buf] = 0;
    if (!buf || !size || radix < 2 || radix > 36) RETC(22);
    char tmp[70];
    int i = 0, neg = (radix == 10 && v < 0);
    uint32_t u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    do { int d = (int)(u % (uint32_t)radix); tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (uint32_t)radix; } while (u);
    if (neg) tmp[i++] = '-';
    if ((uint32_t)i + 1 > size) { rt_set_last_error(c, 34); RETC(34); }  /* ERANGE */
    for (int k = 0; k < i; k++) ((char *)MEM)[buf + k] = tmp[i - 1 - k];
    RETC(0);
}

/* ---- MSVCP80: std::exception (VC8 layout) and the wide char_traits ----
 * [0] vftable, [4] char *_Mywhat, [8] char _Mydofree. The copies are
 * leaked: the game only builds these while unwinding, which stops anyway. */
SHIM(exc_vcall) {
    rt_fatal("std::exception virtual call: C++ unwinding is not implemented");
}
/* A one-slot guest vtable whose entry is a thunk that stops with the above
 * message, so a virtual call through an exception object is diagnosable
 * instead of jumping to address 0. */
static uint32_t exc_vtable(void) {
    static uint32_t vt;
    if (!vt) {
        vt = gmalloc(8);
        if (vt) { M32(vt) = rt_thunk(shim_exc_vcall, "std::exception vtable"); M32(vt + 4) = 0; }
    }
    return vt;
}
SHIM(exc_ctor) {
    uint32_t t = c->r[1];
    M32(t) = exc_vtable();
    M32(t + 4) = gstrdup("Unknown exception");
    M8(t + 8) = 0;
    RET(t, 0);
}
SHIM(exc_ctor_msg) {
    uint32_t t = c->r[1], pp = ARG(0);
    const char *m = pp && M32(pp) ? GSTR(M32(pp)) : NULL;
    M32(t) = exc_vtable();
    M32(t + 4) = gstrdup(m ? m : "Unknown exception");
    M8(t + 8) = 0;
    RET(t, 1);
}
SHIM(exc_ctor_copy) {
    uint32_t t = c->r[1], s = ARG(0);
    M32(t) = exc_vtable();
    M32(t + 4) = gstrdup(s && M32(s + 4) ? GSTR(M32(s + 4)) : "Unknown exception");
    M8(t + 8) = 0;
    RET(t, 1);
}
SHIM(exc_dtor) { RET(c->r[1], 0); }
SHIM(exc_what) { RET(M32(c->r[1] + 4), 0); }

/* ?uncaught_exception@std@@YA_NXZ */
SHIM(std_uncaught_exception) { RETC(0); }
/* char_traits<unsigned short>: compare(s1, s2, n) / find(s, n, c). */
static int cmp16(uint32_t a, uint32_t b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint16_t x = M16(a + 2 * i), y = M16(b + 2 * i);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
SHIM(tr_compare_w) { RETC(cmp16(ARG(0), ARG(1), ARG(2))); }
SHIM(tr_find_w) {
    uint32_t s = ARG(0), n = ARG(1), cp = ARG(2);
    uint16_t ch = cp ? M16(cp) : 0;
    for (uint32_t i = 0; i < n; i++) if (M16(s + 2 * i) == ch) RETC(s + 2 * i);
    RETC(0);
}

/* ---- KERNEL32 -------------------------------------------------------- */
SHIM(lstrlenA) { const char *s = GSTR(ARG(0)); RET(s ? (uint32_t)strlen(s) : 0, 1); }
SHIM(lstrlenW) {
    uint32_t s = ARG(0), n = 0;
    while (s && M16(s + 2 * n)) n++;
    RET(n, 1);
}
/* GetCurrentDirectoryA(n, buf): the process CWD is the game folder. The
 * returned length excludes the NUL; if the buffer is too small, the length
 * needed including the NUL. */
SHIM(GetCurrentDirectoryA) {
    uint32_t cap = ARG(0), buf = ARG(1);
    char cwd[1024];
    if (!getcwd(cwd, sizeof cwd)) RET(0, 2);
    for (char *q = cwd; *q; q++) if (*q == '/') *q = '\\';
    uint32_t n = (uint32_t)strlen(cwd);
    if (cap > n) { memcpy(MEM + buf, cwd, n + 1); RET(n, 2); }
    RET(n + 1, 2);
}
SHIM(CreateDirectoryW) {
    uint32_t s = narrow_dup(ARG(0));
    char host[1024];
    rt_host_path(GSTR(s), host, sizeof host);
    for (char *q = host + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(host, 0755); *q = '/'; }
    int ok = mkdir(host, 0755) == 0;
    if (!ok && errno == EEXIST) rt_set_last_error(c, 183);
    gfree(s);
    RET(ok, 2);
}
SHIM(GetFileAttributesW) {
    uint32_t s = narrow_dup(ARG(0));
    char host[1024];
    struct stat st;
    uint32_t r = 0xFFFFFFFFu;
    if (rt_host_path(GSTR(s), host, sizeof host) && stat(host, &st) == 0)
        r = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
    else
        rt_set_last_error(c, 2);
    gfree(s);
    RET(r, 1);
}
/* The sandbox has no attribute bits; the game only clears the read-only
 * flag around its own saves, which the open modes already handle. */
SHIM(SetFileAttributesW) { once("kernel32.SetFileAttributesW (no attribute bits)"); RET(1, 2); }
SHIM(GetLocalTime) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    uint32_t o = ARG(0);
    M16(o) = (uint16_t)(tm.tm_year + 1900);
    M16(o + 2) = (uint16_t)(tm.tm_mon + 1);
    M16(o + 4) = (uint16_t)tm.tm_wday;
    M16(o + 6) = (uint16_t)tm.tm_mday;
    M16(o + 8) = (uint16_t)tm.tm_hour;
    M16(o + 10) = (uint16_t)tm.tm_min;
    M16(o + 12) = (uint16_t)tm.tm_sec;
    M16(o + 14) = 0;
    RET(0, 1);
}
/* VirtualQuery(addr, mbi, len): a faithful-enough picture of the guest map.
 * The three images and the heap read as committed, everything else as free,
 * so the game's address-space walk steps over regions instead of probing
 * every page. */
SHIM(VirtualQuery) {
    uint32_t a = ARG(0), out = ARG(1), len = ARG(2);
    if (len < 28) RET(0, 3);
    memset(MEM + out, 0, 28);
    M32(out) = a;
    uint32_t base = 0, end = 0, protect = 0x01 /* PAGE_NOACCESS */, state = 0x10000 /* MEM_FREE */, type = 0;
    if (a >= 0x00400000u && a < 0x00409000u) { base = 0x00400000u; end = 0x00409000u; protect = 0x20; state = 0x1000; type = 0x1000000; }
    else if (a >= 0x09000000u && a < 0x09038000u) { base = 0x09000000u; end = 0x09038000u; protect = 0x20; state = 0x1000; type = 0x1000000; }
    else if (a >= 0x10000000u && a < 0x1192a000u) { base = 0x10000000u; end = 0x1192a000u; protect = 0x20; state = 0x1000; type = 0x1000000; }
    else if (a >= GUEST_STACK_BASE && a < GUEST_MISC_BASE + 0x10000u) { base = GUEST_STACK_BASE; end = GUEST_MISC_BASE + 0x10000u; protect = 0x04; state = 0x1000; type = 0x20000; }
    else if (a >= GUEST_HEAP_BASE && a < GUEST_HEAP_END) { base = GUEST_HEAP_BASE; end = GUEST_HEAP_END; protect = 0x04; state = 0x1000; type = 0x20000; }
    else if (a >= GUEST_VA_BASE && a < GUEST_VA_END) { base = GUEST_VA_BASE; end = GUEST_VA_END; protect = 0x04; state = 0x1000; type = 0x20000; }
    else { base = a; end = a + 0x1000; }
    M32(out + 4) = base;                    /* AllocationBase */
    M32(out + 8) = protect;                 /* AllocationProtect */
    M32(out + 12) = end - a;                /* RegionSize */
    M32(out + 16) = state;
    M32(out + 20) = protect;
    M32(out + 24) = type;
    RET(28, 3);
}
/* GetDiskFreeSpaceExA: 64 GiB free, enough for any save-size check. */
SHIM(GetDiskFreeSpaceExA) {
    for (int i = 1; i <= 3; i++) {
        uint32_t p = ARG(i);
        if (!p) continue;
        M32(p) = 0;
        M32(p + 4) = 0x10;
    }
    RET(1, 4);
}
SHIM(GetLogicalDriveStringsA) {
    uint32_t cap = ARG(0), buf = ARG(1);
    if (cap < 5) RET(0, 2);
    memcpy(MEM + buf, "C:\\\0\0", 5);
    RET(4, 2);
}
SHIM(GetThreadLocale) { RET(0x0409, 0); }  /* en-US */
SHIM(OpenMutexA) {
    static int seen;
    if (!seen++) LOG("fc2: kernel32.OpenMutexA -> not found (no single-instance check)");
    rt_set_last_error(c, 2);
    RET(0, 3);
}
SHIM(PulseEvent) { RET(1, 1); }
SHIM(SuspendThread) { once("kernel32.SuspendThread (threads are never suspended)"); RET(0xFFFFFFFFu, 1); }
/* DuplicateHandle: only the pseudo current-process/thread handles are
 * duplicated; real handles would need the handle table's type. */
SHIM(DuplicateHandle) {
    uint32_t src = ARG(1), out = ARG(3);
    if (src == 0xFFFFFFFFu || src == 0xFFFFFFFEu) {
        if (out) M32(out) = h_new(H_OTHER, NULL);
        RET(1, 7);
    }
    once("kernel32.DuplicateHandle (only the pseudo handles)");
    if (out) M32(out) = 0;
    RET(0, 7);
}

/* ---- private profiles: a small real INI reader/writer ---------------- */
static int ini_get(const char *host, const char *app, const char *key, char *out, size_t cap) {
    FILE *f = fopen(host, "r");
    if (!f) return 0;
    char line[1024];
    int in_section = 0, found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t len = strlen(p);
        while (len && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t'))
            p[--len] = 0;
        if (*p == '[') {
            char *e = strchr(p, ']');
            if (e) *e = 0;
            in_section = !strcasecmp(p + 1, app);
            continue;
        }
        if (!in_section || !*p || *p == ';' || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        size_t kl = strlen(p);
        while (kl && (p[kl - 1] == ' ' || p[kl - 1] == '\t')) p[--kl] = 0;
        if (strcasecmp(p, key)) continue;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        snprintf(out, cap, "%s", v);
        found = 1;
    }
    fclose(f);
    return found;
}
/* Append to a bounded buffer, keeping `on` inside `cap` even if truncated. */
static void emit(char *out, size_t cap, size_t *on, const char *fmt, ...) {
    if (*on + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(out + *on, cap - *on, fmt, ap);
    va_end(ap);
    if (w <= 0) return;
    *on += ((size_t)w < cap - *on) ? (size_t)w : cap - *on - 1;
}

/* Rewrites the whole file: replaces the key inside its section, inserts it
 * at the end of the section, or appends a new section. */
static int ini_write(const char *host, const char *app, const char *key, const char *val) {
    char *data = NULL;
    size_t n = 0;
    FILE *f = fopen(host, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            data = malloc((size_t)sz + 1);
            if (data) { n = fread(data, 1, (size_t)sz, f); data[n] = 0; }
        }
        fclose(f);
    }
    if (!data) { data = malloc(1); if (!data) return 0; data[0] = 0; }

    size_t cap = n + strlen(app) + strlen(key) + strlen(val) + 64;
    char *out = malloc(cap);
    if (!out) { free(data); return 0; }
    size_t on = 0;
    int in_section = 0, replaced = 0;
#define PUT(...) emit(out, cap, &on, __VA_ARGS__)
    char *q = data;
    while (*q) {
        char *nl = strchr(q, '\n');
        size_t len = nl ? (size_t)(nl - q) + 1 : strlen(q);
        /* does this line open the target section? */
        char *t = q;
        while (*t == ' ' || *t == '\t') t++;
        int is_header = (*t == '[');
        char header_app[256] = "";
        if (is_header) {
            char *e = strchr(t, ']');
            size_t hl = e ? (size_t)(e - t - 1) : 0;
            if (hl >= sizeof header_app) hl = sizeof header_app - 1;
            memcpy(header_app, t + 1, hl);
            header_app[hl] = 0;
        }
        if (is_header && in_section && !replaced) {
            /* leaving the section without the key: add it before the header */
            PUT("%s=%s\n", key, val);
            replaced = 1;
        }
        if (is_header) {
            in_section = !strcasecmp(header_app, app);
        } else if (in_section && !replaced) {
            char *eq = strchr(t, '=');
            if (eq) {
                size_t kl = (size_t)(eq - t);
                while (kl && (t[kl - 1] == ' ' || t[kl - 1] == '\t')) kl--;
                if (kl == strlen(key) && !strncasecmp(t, key, kl)) {
                    PUT("%s=%s\n", key, val);
                    replaced = 1;
                    q += len;
                    continue;
                }
            }
        }
        memcpy(out + on, q, len);
        on += len;
        q += len;
    }
    if (in_section && !replaced) { PUT("%s=%s\n", key, val); replaced = 1; }
    if (!replaced) {
        if (on && out[on - 1] != '\n') out[on++] = '\n';
        PUT("[%s]\n%s=%s\n", app, key, val);
    }
#undef PUT
    int ok = 0;
    f = fopen(host, "wb");
    if (f) { ok = fwrite(out, 1, on, f) == on; fclose(f); }
    free(out);
    free(data);
    return ok;
}
SHIM(GetPrivateProfileStringA) {
    uint32_t cap = ARG(4), buf = ARG(3);
    const char *file = GSTR(ARG(5));
    char host[1024];
    rt_host_path(file && *file ? file : "port/settings.ini", host, sizeof host);
    char val[512];
    const char *r = NULL;
    if (ini_get(host, GSTR(ARG(0)), GSTR(ARG(1)), val, sizeof val)) r = val;
    else r = GSTR(ARG(2)) ? GSTR(ARG(2)) : "";
    size_t n = strlen(r);
    if (cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(MEM + buf, r, c);
        ((char *)MEM)[buf + c] = 0;
    }
    RET((uint32_t)n, 6);
}
SHIM(WritePrivateProfileStringA) {
    const char *file = GSTR(ARG(3));
    if (!file || !*file) RET(0, 4);
    char host[1024];
    rt_host_path(file, host, sizeof host);
    const char *v = GSTR(ARG(2));
    RET(ini_write(host, GSTR(ARG(0)), GSTR(ARG(1)), v ? v : ""), 4);
}

/* ---- USER32 ---------------------------------------------------------- */
/* wsprintfA(buf, fmt, ...) is cdecl and takes its args from the stack. */
SHIM(wsprintfA) {
    char tmp[1024];
    size_t n = guest_vformat(tmp, sizeof tmp, GSTR(ARG(1)), c->r[4] + 12);
    uint32_t buf = ARG(0);
    if (buf) memcpy(MEM + buf, tmp, n + 1);
    RETC((uint32_t)n);
}
SHIM(GetKeyState) {
    extern const uint8_t *user32_vk_states(void);
    uint32_t vk = ARG(0) & 0xFF;
    const uint8_t *s = user32_vk_states();
    RET((s[vk] & 0x80) ? 0x8000u : 0, 1);
}
SHIM(PostMessageA) {
    static int seen;
    if (!seen++) LOG("fc2: user32.PostMessageA dropped (msg %u)", ARG(1));
    RET(1, 4);
}
SHIM(SetWindowTextA) { RET(1, 2); }
SHIM(UpdateLayeredWindow) { once("user32.UpdateLayeredWindow (no layered windows)"); RET(0, 8); }
SHIM(EnumDisplayDevicesA) { RET(0, 4); }
SHIM(ToAsciiEx) { RET(0, 6); }
SHIM(IsCharAlphaNumericW) {
    uint16_t ch = (uint16_t)ARG(0);
    RET(ch < 128 && isalnum((int)ch), 1);
}
SHIM(OpenClipboard) { RET(1, 1); }
SHIM(CloseClipboard) { RET(1, 0); }
SHIM(EmptyClipboard) { RET(1, 0); }
SHIM(GetClipboardData) { RET(0, 1); }
SHIM(SetClipboardData) { RET(0, 2); }
SHIM(IsClipboardFormatAvailable) { RET(0, 1); }

/* ---- GDI32: the two object calls the shared table lacks -------------- */
SHIM(CreateSolidBrush) { RET(0x3000Au, 1); }
SHIM(GetStockObject) { RET(0x3000Bu, 1); }

/* ---- SHELL32: the Documents folder the game stores its profile in ----
 * The Windows path below maps (kernel32.c's rt_host_path) onto g_save_dir,
 * which the launcher points inside the game folder. */
SHIM(SHGetFolderPathW) {
    static const char k_docs[] = "C:\\users\\player\\Documents";
    uint32_t buf = ARG(4);
    if (!buf) RET(0x80070057u, 5);           /* E_INVALIDARG */
    for (size_t i = 0; i <= sizeof k_docs - 1; i++) M16(buf + 2 * i) = (uint8_t)k_docs[i];
    RET(0, 5);                                /* S_OK */
}

/* ---- ADVAPI32: no registry; the game falls back to its ini files ----- */
SHIM(RegCreateKeyExA) {
    static int seen;
    if (!seen++) LOG("fc2: advapi32.RegCreateKeyExA([%s]) -> no registry", GSTR(ARG(1)) ? GSTR(ARG(1)) : "");
    if (ARG(7)) M32(ARG(7)) = 0;
    if (ARG(8)) M32(ARG(8)) = 0;
    RET(1, 9);                                /* ERROR_INVALID_FUNCTION */
}

/* ==================================================================== */
/* registration                                                         */
/* ==================================================================== */
static const ShimDef defs[] = {
#define M(n) {"msvcr80.dll", #n, shim_##n},
    M(__libm_sse2_sin) M(__libm_sse2_cos) M(__libm_sse2_tan) M(__libm_sse2_asin)
    M(__libm_sse2_acos) M(__libm_sse2_atan) M(__libm_sse2_exp) M(__libm_sse2_log)
    M(__libm_sse2_log10) M(__libm_sse2_pow) M(__libm_sse2_atan2)
    M(memcmp) M(strcmp) M(strlen) M(wcslen) M(vfprintf) M(_vsnprintf_s) M(vsprintf_s)
    M(_vscprintf) M(strftime) M(strnlen) M(_memicmp) M(_strtoui64) M(_atoi64)
    M(_itow) M(_wtoi) M(_wcslwr) M(towupper) M(towlower) M(wcsstr) M(wcsncpy)
    M(wcscspn) M(_vswprintf) M(_vswprintf_c_l) M(ldiv) M(_get_heap_handle)
    M(_malloc_crt) M(_encode_pointer) M(_decode_pointer) M(_encoded_null)
    M(__iob_func) M(_chmod) M(rename) M(remove) M(_time64) M(_utime64)
    M(_localtime64) M(_gmtime64) M(_mktime64) M(_localtime64_s) M(_difftime64)
    M(islower) M(isupper) M(ispunct) M(iscntrl) M(strpbrk) M(strcspn) M(strcoll)
    M(printf) M(strerror)
    M(strcpy_s) M(strncpy_s) M(strcat_s) M(strtok_s) M(memmove_s) M(sprintf_s)
    M(sscanf_s) M(_snprintf_s) M(_controlfp_s)
    M(_CxxThrowException) M(__CxxFrameHandler3) M(_except_handler3)
    M(_except_handler4_common) M(__CppXcptFilter) M(_crt_debugger_hook)
    M(_invalid_parameter_noinfo) M(__clean_type_info_names_internal)
    M(_initterm_e) M(_beginthreadex)
#undef M
    {"msvcr80.dll", "?_type_info_dtor_internal_method@type_info@@QAEXXZ", shim_type_info_dtor_internal},

#define P(n) {"msvcp80.dll", #n, shim_##n},
    P(uncaught_exception) P(tr_length) P(tr_assign_c) P(tr_assign_w) P(tr_assign_n)
    P(tr_assign_wn) P(tr_eq) P(tr_eq_int) P(tr_eof) P(tr_compare) P(tr_find)
    P(tr_copy_s) P(tr_move_s) P(tr_xlen) P(tr_xran) P(ios_good) P(ios_flags)
#undef P
    {"msvcp80.dll", "??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@PBD@Z", shim_str_ctor_cstr},
    {"msvcp80.dll", "??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@ABV01@@Z", shim_str_ctor_copy},
    {"msvcp80.dll", "??1?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@XZ", shim_str_dtor},
    {"msvcp80.dll", "?size@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBEIXZ", shim_str_size},
    {"msvcp80.dll", "?c_str@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBEPBDXZ", shim_str_c_str},
    {"msvcp80.dll", "??1?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@QAE@XZ", shim_str_w_dtor},
    {"msvcp80.dll", "?c_str@?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@QBEPBGXZ", shim_str_wc_str},
    {"msvcp80.dll", "??0?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@QAE@PBG@Z", shim_str_w_ctor_cstr},
    {"msvcp80.dll", "?assign@?$char_traits@D@std@@SAPADPADID@Z", shim_tr_assign_n},
    {"msvcp80.dll", "?assign@?$char_traits@G@std@@SAPAGPAGIG@Z", shim_tr_assign_wn},
    {"msvcp80.dll", "?assign@?$char_traits@D@std@@SAXAADABD@Z", shim_tr_assign_c},
    {"msvcp80.dll", "?assign@?$char_traits@G@std@@SAXAAGABG@Z", shim_tr_assign_w},
    {"msvcp80.dll", "?eq@?$char_traits@D@std@@SA_NABD0@Z", shim_tr_eq},
    {"msvcp80.dll", "?eq@?$char_traits@G@std@@SA_NABG0@Z", shim_tr_weq},
    {"msvcp80.dll", "?eq_int_type@?$char_traits@D@std@@SA_NABH0@Z", shim_tr_eq_int},
    {"msvcp80.dll", "?eq_int_type@?$char_traits@G@std@@SA_NABG0@Z", shim_tr_weq_int},
    {"msvcp80.dll", "?eof@?$char_traits@D@std@@SAHXZ", shim_tr_eof},
    {"msvcp80.dll", "?eof@?$char_traits@G@std@@SAGXZ", shim_tr_weof},
    {"msvcp80.dll", "?length@?$char_traits@D@std@@SAIPBD@Z", shim_tr_length},
    {"msvcp80.dll", "?length@?$char_traits@G@std@@SAIPBG@Z", shim_tr_wlength},
    {"msvcp80.dll", "?compare@?$char_traits@D@std@@SAHPBD0I@Z", shim_tr_compare},
    {"msvcp80.dll", "?find@?$char_traits@D@std@@SAPBDPBDIABD@Z", shim_tr_find},
    {"msvcp80.dll", "?_Copy_s@?$char_traits@D@std@@SAPADPADIPBDI@Z", shim_tr_copy_s},
    {"msvcp80.dll", "?_Copy_s@?$char_traits@G@std@@SAPAGPAGIPBGI@Z", shim_tr_copy_s},
    {"msvcp80.dll", "?_Move_s@?$char_traits@D@std@@SAPADPADIPBDI@Z", shim_tr_move_s},
    {"msvcp80.dll", "?_Move_s@?$char_traits@G@std@@SAPAGPAGIPBGI@Z", shim_tr_move_s},
    {"msvcp80.dll", "?_Xlen@_String_base@std@@SAXXZ", shim_tr_xlen},
    {"msvcp80.dll", "?_Xran@_String_base@std@@SAXXZ", shim_tr_xran},
    {"msvcp80.dll", "?good@ios_base@std@@QBE_NXZ", shim_ios_good},
    {"msvcp80.dll", "?flags@ios_base@std@@QBEHXZ", shim_ios_flags},
    {"msvcp80.dll", "?width@ios_base@std@@QBEHXZ", shim_ios_width_get},
    {"msvcp80.dll", "?width@ios_base@std@@QAEHH@Z", shim_ios_width_set},
    {"msvcp80.dll", "?setstate@?$basic_ios@DU?$char_traits@D@std@@@std@@QAEXH_N@Z", shim_ios_setstate},
    {"msvcp80.dll", "?clear@?$basic_ios@DU?$char_traits@D@std@@@std@@QAEXH_N@Z", shim_ios_clear},
    {"msvcp80.dll", "?epsilon@?$numeric_limits@M@std@@SAMXZ", shim_num_epsilon},

#define WS(n) {"ws2_32.dll", #n, shim_##n},
    WS(WSAStartup) WS(WSACleanup) WS(WSAGetLastError) WS(socket) WS(closesocket)
    WS(connect) WS(bind) WS(listen) WS(accept) WS(send) WS(recv) WS(sendto) WS(recvfrom)
    WS(select) WS(setsockopt) WS(getsockopt) WS(ioctlsocket) WS(shutdown)
    WS(gethostbyname) WS(gethostbyaddr) WS(gethostname) WS(getaddrinfo) WS(freeaddrinfo)
    WS(getsockname) WS(inet_addr) WS(inet_ntoa) WS(htonl) WS(htons) WS(ntohl) WS(ntohs)
    WS(WSAAsyncSelect) WS(__WSAFDIsSet)
#undef WS

#define H(n) {"winhttp.dll", #n, shim_##n},
    H(WinHttpOpen) H(WinHttpConnect) H(WinHttpOpenRequest) H(WinHttpSendRequest)
    H(WinHttpReceiveResponse) H(WinHttpQueryHeaders) H(WinHttpReadData)
    H(WinHttpQueryDataAvailable) H(WinHttpWriteData) H(WinHttpCloseHandle)
    H(WinHttpSetOption) H(WinHttpAddRequestHeaders) H(WinHttpSetTimeouts)
    H(WinHttpSetStatusCallback) H(WinHttpCrackUrl)
    H(WinHttpGetIEProxyConfigForCurrentUser) H(WinHttpGetDefaultProxyConfiguration)
#undef H
    {"wininet.dll", "InternetSetCookieW", shim_InternetSetCookieW},
    {"wininet.dll", "InternetGetCookieW", shim_InternetGetCookieW},

#define S2(n) {"secur32.dll", #n, shim_##n},
    S2(AcquireCredentialsHandleA) S2(FreeCredentialsHandle) S2(InitializeSecurityContextA)
    S2(DeleteSecurityContext) S2(QueryContextAttributesA) S2(EncryptMessage)
    S2(DecryptMessage) S2(ApplyControlToken) S2(FreeContextBuffer)
#undef S2
#define C2(n) {"crypt32.dll", #n, shim_##n},
    C2(CertGetCertificateChain) C2(CertFreeCertificateChain) C2(CertFreeCertificateContext)
    C2(CertVerifyCertificateChainPolicy)
#undef C2
#define A2(n) {"advapi32.dll", #n, shim_##n},
    A2(CryptAcquireContextA) A2(CryptReleaseContext) A2(CryptCreateHash)
    A2(CryptHashData) A2(CryptGetHashParam) A2(CryptDestroyHash) A2(CryptGenRandom)
#undef A2
    {"iphlpapi.dll", "GetAdaptersInfo", shim_GetAdaptersInfo},
    {"psapi.dll", "GetPerformanceInfo", shim_GetPerformanceInfo},
    {"rpcrt4.dll", "UuidToStringA", shim_UuidToStringA},
    {"rpcrt4.dll", "RpcStringFreeA", shim_RpcStringFreeA},
    {"ole32.dll", "CoCreateGuid", shim_CoCreateGuid},
    {"ole32.dll", "CreateStreamOnHGlobal", shim_CreateStreamOnHGlobal},
#define GP(n) {"gdiplus.dll", #n, shim_##n},
    GP(GdiplusStartup) GP(GdiplusShutdown) GP(GdipAlloc) GP(GdipFree)
    GP(GdipCloneImage) GP(GdipDisposeImage) GP(GdipCreateBitmapFromStream)
    GP(GdipCreateHBITMAPFromBitmap)
#undef GP
#define DB(n) {"dbghelp.dll", #n, shim_##n},
    DB(SymInitialize) DB(SymCleanup) DB(SymGetOptions) DB(SymSetOptions)
    DB(SymSetSearchPath) DB(SymLoadModule) DB(SymUnloadModule) DB(SymGetModuleInfo)
    DB(SymGetModuleInfo64) DB(SymGetSymFromAddr) DB(SymGetSymFromAddr64)
    DB(SymGetLineFromAddr) DB(SymGetLineFromAddr64) DB(SymFindFileInPath)
    DB(SymFunctionTableAccess64) DB(StackWalk64) DB(EnumerateLoadedModules)
    DB(EnumerateLoadedModules64)
#undef DB
    {"eax.dll", "#6", shim_EAXDirectSoundCreate8},
    {"dsound.dll", "#2", shim_DirectSoundEnumerateA},
    {"winmm.dll", "waveOutGetDevCapsA", shim_waveOutGetDevCapsA},
#define K2(n) {"kernel32.dll", #n, shim_##n},
    K2(OpenThread) K2(GetThreadContext) K2(GetSystemPowerStatus)
    K2(SetThreadPriorityBoost) K2(SetEnvironmentVariableW) K2(GetPrivateProfileStringW)
    K2(InterlockedExchangeAdd) K2(CreateNamedPipeA) K2(ConnectNamedPipe)
    K2(DisconnectNamedPipe) K2(GetNamedPipeHandleStateA) K2(SetNamedPipeHandleState)
    K2(PeekNamedPipe) K2(ReadDirectoryChangesW) K2(DeviceIoControl) K2(LoadResource)
    K2(SizeofResource) K2(FindResourceA) K2(LockResource)
#undef K2
    {NULL, NULL, NULL}
};

/* Imports the first boot of the game was observed to need (docs/RECON.md,
 * "Known gaps"): registered separately so the original table above stays a
 * readable record of what the shared runtime already covered. */
static const ShimDef extra[] = {
#define M(n) {"msvcr80.dll", #n, shim_##n},
    M(_configthreadlocale) M(_invoke_watson) M(memchr) M(_itoa_s)
#undef M
#define U(n) {"user32.dll", #n, shim_##n},
    U(wsprintfA) U(GetKeyState) U(PostMessageA) U(SetWindowTextA) U(UpdateLayeredWindow)
    U(EnumDisplayDevicesA) U(ToAsciiEx) U(IsCharAlphaNumericW)
    U(OpenClipboard) U(CloseClipboard) U(EmptyClipboard) U(GetClipboardData)
    U(SetClipboardData) U(IsClipboardFormatAvailable)
#undef U
#define K(n) {"kernel32.dll", #n, shim_##n},
    K(lstrlenA) K(lstrlenW) K(GetCurrentDirectoryA) K(CreateDirectoryW) K(GetFileAttributesW)
    K(SetFileAttributesW) K(GetLocalTime) K(VirtualQuery) K(GetDiskFreeSpaceExA)
    K(GetLogicalDriveStringsA) K(GetThreadLocale) K(OpenMutexA) K(PulseEvent)
    K(SuspendThread) K(DuplicateHandle) K(GetPrivateProfileStringA) K(WritePrivateProfileStringA)
#undef K
#define G(n) {"gdi32.dll", #n, shim_##n},
    G(CreateSolidBrush) G(GetStockObject)
#undef G
    {"shell32.dll", "SHGetFolderPathW", shim_SHGetFolderPathW},
    {"advapi32.dll", "RegCreateKeyExA", shim_RegCreateKeyExA},
    { "msvcr80.dll", "??0exception@std@@QAE@XZ", shim_exc_ctor },
    { "msvcr80.dll", "??0exception@std@@QAE@ABQBD@Z", shim_exc_ctor_msg },
    { "msvcr80.dll", "??0exception@std@@QAE@ABV01@@Z", shim_exc_ctor_copy },
    { "msvcr80.dll", "??1exception@std@@UAE@XZ", shim_exc_dtor },
    { "msvcr80.dll", "?what@exception@std@@UBEPBDXZ", shim_exc_what },
    { "msvcp80.dll", "?uncaught_exception@std@@YA_NXZ", shim_std_uncaught_exception },
    { "msvcp80.dll", "?compare@?$char_traits@G@std@@SAHPBG0I@Z", shim_tr_compare_w },
    { "msvcp80.dll", "?find@?$char_traits@G@std@@SAPBGPBGIABG@Z", shim_tr_find_w },
    {NULL, NULL, NULL}
};

/* MSVCP80 iostream/stringstream: registered with NULL implementations so
 * the import resolves and the first call stops with its mangled name
 * (rt_call: "unimplemented import ..."). */
static const ShimDef msvcp_traps[] = {
#define ST80(n) {"msvcp80.dll", n, NULL},
    ST80("??_D?$basic_stringstream@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@QAEXXZ")
    ST80("?str@?$basic_stringstream@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@QBE?AV?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@2@@2@XZ")
    ST80("??0?$basic_stringstream@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@QAE@H@Z")
    ST80("??6?$basic_ostream@GU?$char_traits@G@std@@@std@@QAEAAV01@K@Z")
    ST80("??6?$basic_ostream@GU?$char_traits@G@std@@@std@@QAEAAV01@J@Z")
    ST80("?rdbuf@?$basic_ios@DU?$char_traits@D@std@@@std@@QBEPAV?$basic_streambuf@DU?$char_traits@D@std@@@2@XZ")
    ST80("?rdbuf@?$basic_ios@GU?$char_traits@G@std@@@std@@QBEPAV?$basic_streambuf@GU?$char_traits@G@std@@@2@XZ")
    ST80("?_Lock@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEXXZ")
    ST80("?_Unlock@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEXXZ")
    ST80("?_Lock@?$basic_streambuf@GU?$char_traits@G@std@@@std@@QAEXXZ")
    ST80("?_Unlock@?$basic_streambuf@GU?$char_traits@G@std@@@std@@QAEXXZ")
    ST80("?flush@?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV12@XZ")
    ST80("?flush@?$basic_ostream@GU?$char_traits@G@std@@@std@@QAEAAV12@XZ")
    ST80("?tie@?$basic_ios@DU?$char_traits@D@std@@@std@@QBEPAV?$basic_ostream@DU?$char_traits@D@std@@@2@XZ")
    ST80("?tie@?$basic_ios@GU?$char_traits@G@std@@@std@@QBEPAV?$basic_ostream@GU?$char_traits@G@std@@@2@XZ")
    ST80("?_Osfx@?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEXXZ")
    ST80("?_Osfx@?$basic_ostream@GU?$char_traits@G@std@@@std@@QAEXXZ")
    ST80("?setstate@?$basic_ios@GU?$char_traits@G@std@@@std@@QAEXH_N@Z")
    ST80("?sputn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEHPBDH@Z")
    ST80("?sputn@?$basic_streambuf@GU?$char_traits@G@std@@@std@@QAEHPBGH@Z")
    ST80("?sputc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEHD@Z")
    ST80("?sputc@?$basic_streambuf@GU?$char_traits@G@std@@@std@@QAEGG@Z")
    ST80("?fill@?$basic_ios@GU?$char_traits@G@std@@@std@@QBEGXZ")
    ST80("??_D?$basic_ostringstream@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEXXZ")
    ST80("?str@?$basic_ostringstream@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBE?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@2@XZ")
    ST80("?str@?$basic_ostringstream@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEXABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@2@@Z")
    ST80("??0?$basic_ostringstream@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@H@Z")
    ST80("??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@K@Z")
    ST80("??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@G@Z")
    ST80("??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@P6AAAV01@AAV01@@Z@Z")
    ST80("??$?6DU?$char_traits@D@std@@V?$allocator@D@1@@std@@YAAAV?$basic_ostream@DU?$char_traits@D@std@@@0@AAV10@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@@Z")
    ST80("?endl@std@@YAAAV?$basic_ostream@DU?$char_traits@D@std@@@1@AAV21@@Z")
    ST80("??_D?$basic_istringstream@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEXXZ")
    ST80("??5?$basic_istream@DU?$char_traits@D@std@@@std@@QAEAAV01@AAK@Z")
    ST80("??0?$basic_istringstream@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@1@H@Z")
#undef ST80
    {NULL, NULL, NULL}
};

void fc2_shims_init(void) {
    rt_register_shims(defs);
    rt_register_shims(extra);
    msvcrt_register_as_msvcr80();

    rt_register_shims(msvcp_traps);

    LOG("fc2_shims: %d entries (MSVCR80/MSVCP80, offline Win32, SEH traps)",
        (int)(sizeof defs / sizeof defs[0] - 1));
}

/* Called from the crash handler (runtime/main.c) after rt_dump_guest():
 * guest registers and memory are already on the log, so add the two things
 * only this layer knows: guest heap usage and which imports answered with a
 * documented failure value. Nothing here needs the game image to be sane. */
void fc2_dump_state(void) {
    extern void gheap_dump(void);
    gheap_dump();
    if (!g_once_n) {
        fprintf(stderr, "fc2: shim fallbacks hit: none\n");
        return;
    }
    fprintf(stderr, "fc2: shim fallbacks hit (%d):\n", g_once_n);
    for (int i = 0; i < g_once_n; i++)
        fprintf(stderr, "fc2:   %s\n", g_once_seen[i]);
}
