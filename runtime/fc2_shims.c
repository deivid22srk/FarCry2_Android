/* Far Cry 2 (PC, 2008) runtime shims.
 *
 * Everything here exists because Dunia.dll / farcry2.exe import it and the
 * shared runtime does not cover it. Three groups:
 *
 *   1. MSVCR80.dll - the VC8 C runtime. The C half is aliased to the shared
 *      msvcrt table (runtime/msvcrt.c + runtime/crt_shims.c); this file adds
 *      the VC8 entry points that table does not have.
 *   2. MSVCP80.dll - the VC8 C++ library. std::basic_string<char/wchar_t> and
 *      char_traits are implemented against the documented VC8 layout (16-byte
 *      inline buffer, size, capacity), and so is the iostream/stringstream
 *      subset Far Cry 2 uses (GStream: a file-backed streambuf plus the
 *      operator<< / operator>> / getline entry points it reaches). Anything
 *      outside that subset still fails loudly with its mangled name, so a run
 *      that needs it stops at one identifiable import instead of corrupting
 *      memory. See docs/RECON.md, "Known gaps".
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
#include "hcom.h"   /* guest COM objects (IStream, GpBitmap) */
#include "art.h"    /* png_decode for the GDI+ image path */
#include "u16.h"    /* guest wide strings are UTF-16, not the host's wchar_t */
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

#define THIS c->r[1] /* ecx: the this pointer of a __thiscall member */

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
SHIM(wcslen) { RETC(u16_len(GPTR(ARG(0)))); }
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
    uint32_t d = ARG(1);
    int32_t v = (int32_t)ARG(0);
    char tmp[16];
    int k = 0, neg = v < 0;
    uint32_t uv = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    if (!d) RETC(d);
    do { tmp[k++] = (char)('0' + uv % 10); uv /= 10; } while (uv);
    if (neg) tmp[k++] = '-';
    for (int i = 0; i < k; i++) M16(d + 2u * (uint32_t)i) = (uint16_t)tmp[k - 1 - i];
    M16(d + 2u * (uint32_t)k) = 0;
    RETC(d);
}
SHIM(_wtoi) {
    const uint8_t *p = (const uint8_t *)GPTR(ARG(0));
    if (!p) RETC(0);
    uint32_t i = 0;
    while (u16_at(p, i) == ' ' || u16_at(p, i) == '\t') i++;
    int neg = 0;
    if (u16_at(p, i) == '-') { neg = 1; i++; }
    else if (u16_at(p, i) == '+') i++;
    int32_t v = 0;
    for (;;) {
        uint16_t ch = u16_at(p, i);
        if (ch < '0' || ch > '9') break;
        v = v * 10 + (int32_t)(ch - '0');
        i++;
    }
    RETC((uint32_t)(neg ? -v : v));
}
SHIM(_wcslwr) {
    uint32_t a = ARG(0);
    for (uint32_t i = 0; M16(a + 2u * i); i++) {
        uint16_t ch = M16(a + 2u * i);
        if (ch >= 'A' && ch <= 'Z') M16(a + 2u * i) = (uint16_t)(ch + 32);
    }
    RETC(a);
}
SHIM(towupper) { RETC(towupper((wint_t)ARG(0))); }
SHIM(towlower) { RETC(towlower((wint_t)ARG(0))); }
SHIM(wcsstr) {
    const void *h = GPTR(ARG(0)), *n = GPTR(ARG(1));
    if (!h || !n) RETC(0);
    uint32_t hl = u16_len(h), nl = u16_len(n);
    if (nl > hl) RETC(0);
    for (uint32_t i = 0; i + nl <= hl; i++)
        if (u16_cmp_n((const uint8_t *)h + 2u * i, n, nl) == 0)
            RETC((uint32_t)((uintptr_t)h - (uintptr_t)MEM) + 2u * i);
    RETC(0);
}
SHIM(wcsncpy) {
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2);
    uint32_t i = 0;
    for (; i < n; i++) {
        uint16_t ch = M16(s + 2u * i);
        M16(d + 2u * i) = ch;
        if (!ch) break;
    }
    for (; i < n; i++) M16(d + 2u * i) = 0;
    RETC(d);
}
SHIM(wcscspn) {
    const void *s = GPTR(ARG(0)), *set = GPTR(ARG(1));
    uint32_t i = 0;
    for (; u16_at(s, i); i++) {
        int found = 0;
        for (uint32_t j = 0; u16_at(set, j); j++)
            if (u16_at(s, i) == u16_at(set, j)) { found = 1; break; }
        if (found) break;
    }
    RETC(i);
}
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
/* The `ptr - MEM` idiom only produces a valid guest address when the pointer
 * already points into guest memory.  For the few CRT calls that return
 * pointers into host-side statics (struct tm, strerror's table) the result has
 * to be copied into guest memory instead. */
static uint32_t g_tm_g;
static uint32_t tm_to_guest(const struct tm *t) {
    if (!t) return 0;
    if (!g_tm_g) g_tm_g = gmalloc(64);
    memcpy(MEM + g_tm_g, t, 36); /* VC8 struct tm: 9 ints */
    return g_tm_g;
}
SHIM(_localtime64) { RETC(tm_to_guest(localtime((const time_t *)GPTR(ARG(0))))); }
SHIM(_gmtime64) { RETC(tm_to_guest(gmtime((const time_t *)GPTR(ARG(0))))); }
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
SHIM(strerror) {
    static uint32_t g;
    const char *s = strerror(ARG(0));
    if (!g) g = gmalloc(160);
    snprintf((char *)MEM + g, 160, "%s", s ? s : "");
    RETC(g);
}

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
SHIM(type_info_dtor_internal) { RET(0, 0); } /* ?_type_info_dtor_internal_method@type_info@@QAEXXZ */
SHIM(_initterm_e) {
    for (uint32_t p = ARG(0); p < ARG(1); p += 4) {
        uint32_t fn = M32(p);
        if (fn) { uint32_t dummy = 0; rt_invoke(c, fn, 0, &dummy); }
    }
    RETC(0);
}

/* _beginthreadex: the CRT's thread entry.  Same guest-thread machinery and
 * handle table as CreateThread (the engine waits on and resumes the handle it
 * gets back), different return convention (uintptr_t handle, 0 on failure). */
SHIM(_beginthreadex) {
    uint32_t tid = 0;
    uint32_t h = rt_thread_create(ARG(2), ARG(3), ARG(4), &tid);
    if (ARG(5)) M32(ARG(5)) = tid;
    RETC(h);
}
/* _endthreadex: ends the calling thread.  The runtime already terminates the
 * thread slot when the start routine returns, so unwinding to that is enough;
 * not returning would leave no way back. */
SHIM(_endthreadex) { RETC(0); }

/* ==================================================================== */
/* MSVCP80: VC8 std::string ABI                                          */
/* ==================================================================== */
/* Far Cry 2's VC8 build lays basic_string out as
 *
 *   +0x00  _Alval / container proxy   (4 bytes; the engine stores its shared
 *          empty-string sentinel 0x10F239D1 here)
 *   +0x04  union { char _Buf[16]; char *_Ptr; char *_Alias[16]; } _Bx
 *   +0x14  size_t _Mysize
 *   +0x18  size_t _Myres              sizeof == 28
 *
 * i.e. _Bx is 4 bytes into the object, not at offset 0.  Verified against the
 * engine's own inlined helpers: `cmp dword ptr [reg+0x18], 0x10` occurs 749
 * times in Dunia (e.g. 0x10002750, 0x100492F0) and is followed by
 * `mov edx,[reg+0x14]` (size) plus `mov eax,[reg+4]` (long) / `add eax,4`
 * (inline), and Dunia's own std::string::assign at 0x10002D40 reads _Myres
 * from [ecx+0x18] and takes `lea ebx,[ecx+4]` as the inline buffer.
 *
 * Inline when _Myres < 16 (8 for wchar_t), otherwise _Bx is a guest heap
 * pointer. */
typedef struct { uint32_t hdr; uint32_t bx; uint32_t pad[3]; uint32_t size, res; } Vc8Str;
#define VC8_EMPTY 0x10F239D1u /* engine's shared empty-string sentinel */

/* `res >= 16` means _Bx holds a *guest* offset; host code must add MEM to
 * reach the bytes.  (It used to reinterpret the offset as a host pointer,
 * which either read unmapped memory or, via the `ptr - MEM` idiom, handed the
 * engine an address that was not the string at all.) */
static char *vc8_ptr(Vc8Str *s) { return s->res >= 16 ? (char *)MEM + s->bx : (char *)&s->bx; }
static uint16_t *vc8_wptr(Vc8Str *s) { return s->res >= 8 ? (uint16_t *)(MEM + s->bx) : (uint16_t *)&s->bx; }
/* c_str() hands the engine a guest address. */
static uint32_t vc8_data_g(Vc8Str *s) { return (uint32_t)((uint8_t *)vc8_ptr(s) - MEM); }
static uint32_t vc8_wdata_g(Vc8Str *s) { return (uint32_t)((uint8_t *)vc8_wptr(s) - MEM); }
static void vc8_set_c(Vc8Str *s, const char *p, uint32_t n) {
    s->hdr = VC8_EMPTY;
    if (n < 16) {
        s->res = 15;
        memcpy(&s->bx, p, n);
        ((char *)&s->bx)[n] = 0;
    } else {
        uint32_t d = gmalloc(n + 1);
        memcpy(MEM + d, p, n);
        M8(d + n) = 0;
        s->bx = d;
        s->res = n;
    }
    s->size = n;
}
static void vc8_set_w(Vc8Str *s, const void *p, uint32_t n) {
    s->hdr = VC8_EMPTY;
    if (n < 8) {
        s->res = 7;
        if (n) memcpy(&s->bx, p, 2u * n);
        ((uint16_t *)&s->bx)[n] = 0;
    } else {
        uint32_t d = gmalloc(2 * (n + 1));
        memcpy(MEM + d, p, 2 * n);
        M16(d + 2 * n) = 0;
        s->bx = d;
        s->res = n;
    }
    s->size = n;
}

/* thiscall: RET(v, nargs) leaves `this` for the caller, as MSVC ret N does.
 * `this` is ecx (THIS); the stack arguments start at ARG(0) - these shims used
 * to read ARG(0) as `this`, which is the return address. */
SHIM(str_ctor_cstr) {
    Vc8Str *s = GPTR(THIS);
    const char *p = GSTR(ARG(0));
    vc8_set_c(s, p ? p : "", p ? (uint32_t)strlen(p) : 0);
    RET(0, 1);
}
SHIM(str_ctor_copy) {
    Vc8Str *s = GPTR(THIS), *o = GPTR(ARG(0));
    vc8_set_c(s, o ? vc8_ptr(o) : "", o ? o->size : 0);
    RET(0, 1);
}
SHIM(str_dtor) {
    Vc8Str *s = GPTR(THIS);
    if (!s) RET(0, 0);
    if (s->res >= 16) gfree(s->bx);
    s->res = 15;
    s->size = 0;
    s->hdr = VC8_EMPTY;
    ((char *)&s->bx)[0] = 0;
    RET(0, 0);
}
SHIM(str_size) { Vc8Str *s = GPTR(THIS); RET(s ? s->size : 0, 0); }
SHIM(str_c_str) { Vc8Str *s = GPTR(THIS); RET(s ? vc8_data_g(s) : 0, 0); }
SHIM(str_wc_str) { Vc8Str *s = GPTR(THIS); RET(s ? vc8_wdata_g(s) : 0, 0); }
SHIM(str_w_ctor_cstr) {
    Vc8Str *s = GPTR(THIS);
    const void *p = GPTR(ARG(0));
    vc8_set_w(s, p, u16_len(p));
    RET(0, 1);
}
SHIM(str_w_dtor) {
    Vc8Str *s = GPTR(THIS);
    if (!s) RET(0, 0);
    if (s->res >= 8) gfree(s->bx);
    s->res = 7;
    s->size = 0;
    s->hdr = VC8_EMPTY;
    ((uint16_t *)&s->bx)[0] = 0;
    RET(0, 0);
}

/* char_traits: static members are __cdecl. */
SHIM(tr_length) { RETC((uint32_t)strlen(GSTR(ARG(0)))); }
SHIM(tr_wlength) { RETC(u16_len(GPTR(ARG(0)))); }
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
/* char_traits<unsigned short> counts elements, not bytes (VC8's _Copy_s/_Move_s
 * multiply the count by sizeof(_Elem) = 2). Sharing the narrow shims here used to
 * copy half of every wide string, so every std::wstring longer than the 8-element
 * small-string buffer came out with a stale tail. */
SHIM(tr_copy_s_w) {
    memcpy(GPTR(ARG(0)), GPTR(ARG(2)), 2u * ARG(3));
    RETC(ARG(0));
}
SHIM(tr_move_s_w) {
    memmove(GPTR(ARG(0)), GPTR(ARG(2)), 2u * ARG(3));
    RETC(ARG(0));
}
SHIM(tr_xlen) { rt_fatal("std::string length_error (_Xlen) reached"); }
SHIM(tr_xran) { rt_fatal("std::string out_of_range (_Xran) reached"); }

/* ==================================================================== */
/* MSVCP80: VC8 iostream / stringstream subset                           */
/* ==================================================================== */
/* Dunia uses basic_ostringstream<char>, basic_istringstream<char> and
 * basic_stringstream<wchar_t> only through out-of-line MSVCP80 entry
 * points (the rest of the stream machinery is inlined into Dunia).  Rather
 * than guess MSVC's stream object layout, the contents live on the host,
 * keyed by the guest address the constructor was handed, so the guest
 * object layout never has to match MSVC's.
 *
 * A handful of entry points (ios_base::width/flags, basic_ios::rdbuf/tie/
 * setstate/clear, basic_streambuf::sputc/sputn) are reached through the
 * virtual-base adjustor - the engine does `mov ecx,[obj] ; mov ecx,[ecx+4]
 * ; add ecx,obj` - so their `this` is a small constant offset from the
 * stream object.  Lookup therefore falls back to the nearest live stream
 * within +/-0x800 bytes.  basic_ios::rdbuf returns the stream's own key, so
 * a subsequent sputc/sputn through it maps back exactly. */
#define GSTREAM_MAX 96
typedef struct {
    int used, wide;
    uint32_t self;
    char *c; uint32_t cn, cc;       /* narrow contents */
    uint16_t *w; uint32_t wn, wc;   /* wide contents */
    uint32_t rdpos;                 /* istringstream read position */
    int32_t width, flags;
    uint16_t fill;
    int good;
} GStream;
static GStream g_streams[GSTREAM_MAX];
static uint32_t g_stream_rotor;


static void st_reset(GStream *s) {
    if (s->c) { free(s->c); s->c = NULL; }
    if (s->w) { free(s->w); s->w = NULL; }
    memset(s, 0, sizeof *s);
}
static GStream *st_exact(uint32_t self) {
    for (int i = 0; i < GSTREAM_MAX; i++)
        if (g_streams[i].used && g_streams[i].self == self) return &g_streams[i];
    return NULL;
}
static GStream *st_find(uint32_t self) {
    GStream *best = NULL;
    uint32_t bd = 0x801;
    for (int i = 0; i < GSTREAM_MAX; i++) {
        uint32_t d;
        if (!g_streams[i].used) continue;
        d = self > g_streams[i].self ? self - g_streams[i].self : g_streams[i].self - self;
        if (d < bd) { bd = d; best = &g_streams[i]; }
    }
    return best;
}
static GStream *st_open(uint32_t self, int wide) {
    GStream *s = st_exact(self);
    if (s) st_reset(s);
    else {
        for (int i = 0; i < GSTREAM_MAX; i++)
            if (!g_streams[i].used) { s = &g_streams[i]; break; }
        if (!s) { s = &g_streams[g_stream_rotor++ % GSTREAM_MAX]; st_reset(s); }
    }
    memset(s, 0, sizeof *s);
    s->used = 1; s->self = self; s->wide = wide;
    s->flags = 0x0201; /* skipws | dec, the VC8 default */
    s->fill = ' ';
    s->good = 1;
    return s;
}
/* MSVCP80's constructor would have set the object's vftable pointer at [+0]
 * and its virtual-base pointer at [+4].  Dunia also contains inlined library
 * code that reads those two fields directly, e.g. 0x1021425D:
 *     mov ecx,[esi+4]      ; vbptr
 *     mov edx,[ecx+4]      ; offset of the ios_base virtual base
 *     lea ecx,[edx+esi+4]  ; &ios_base   -> ?clear / ?width / ...
 * With [+4] left zero that walk faults on guest address 4.  Any small positive
 * offset works: every basic_ios/ios_base/ostream entry point is one of our
 * shims and they find the stream by proximity.  The vftable's slots all point
 * at a logging stub so a virtual call the engine happens to make inlined is
 * reported rather than jumping to address 0. */
SHIM(st_vcall) {
    once("MSVCP80 stream virtual call (absorbed - see st_obj_init)");
    RET(0, 0);
}
static uint32_t st_vtable_g, st_vbtable_g;
static void st_obj_init(uint32_t self) {
    if (!st_vtable_g) {
        uint32_t stub = rt_thunk(shim_st_vcall, "MSVCP80 stream virtual call");
        st_vbtable_g = gmalloc(8);
        if (st_vbtable_g) { M32(st_vbtable_g) = 0; M32(st_vbtable_g + 4) = 0x0c; }
        st_vtable_g = gmalloc(8 + 16 * 4);
        if (st_vtable_g) {
            uint32_t vt = st_vtable_g + 4;   /* RTTI slot (used by dynamic_cast) at -4 */
            M32(st_vtable_g) = 0;
            for (int i = 0; i < 16; i++) M32(vt + 4 * i) = stub;
            st_vtable_g = vt;
        }
    }
    if (st_vtable_g && self) M32(self) = st_vtable_g;
    if (st_vbtable_g && self) M32(self + 4) = st_vbtable_g;
}
static void st_close(uint32_t self) {
    GStream *s = st_exact(self);
    if (s) st_reset(s);
}

static void st_rc(GStream *s, uint32_t need) {
    if (s->cc < need) {
        uint32_t nc = s->cc ? s->cc : 32;
        while (nc < need) nc *= 2;
        s->c = realloc(s->c, nc);
        s->cc = nc;
    }
}
static void st_rw(GStream *s, uint32_t need) {
    if (s->wc < need) {
        uint32_t nc = s->wc ? s->wc : 32;
        while (nc < need) nc *= 2;
        s->w = realloc(s->w, 2 * nc);
        s->wc = nc;
    }
}
static void st_putc(GStream *s, int ch) {
    if (s->wide) { st_rw(s, s->wn + 1); s->w[s->wn++] = (uint16_t)ch; }
    else { st_rc(s, s->cn + 1); s->c[s->cn++] = (char)ch; }
}
static void st_write(GStream *s, const void *p, uint32_t n) {
    if (!n || !p) return;
    if (s->wide) {
        st_rw(s, s->wn + n);
        for (uint32_t i = 0; i < n; i++) s->w[s->wn + i] = ((const uint16_t *)p)[i];
        s->wn += n;
    } else {
        st_rc(s, s->cn + n);
        memcpy(s->c + s->cn, p, n);
        s->cn += n;
    }
}
/* Integer insertion honouring the base/width/fill the engine set through
 * ios_base (fmtflags: showbase 0x8, showpos 0x20, left 0x40, dec/oct/hex
 * in 0x0E00, uppercase 0x4). */
static void st_num(GStream *s, uint64_t v, int neg) {
    char buf[80], tmp[80];
    int n = 0, base = 10, i;
    const char *digs = "0123456789abcdef";
    int32_t w, pad;
    if ((s->flags & 0x0E00) == 0x0800) base = 16;
    else if ((s->flags & 0x0E00) == 0x0400) base = 8;
    if ((s->flags & 0x0004) && base == 16) digs = "0123456789ABCDEF";
    if (!v) buf[n++] = '0';
    while (v) { buf[n++] = digs[v % base]; v /= base; }
    if (neg) buf[n++] = '-';
    else if (s->flags & 0x0020) buf[n++] = '+';
    if ((s->flags & 0x0008) && base == 16) { buf[n++] = (digs[10] == 'a') ? 'x' : 'X'; buf[n++] = '0'; }
    for (i = 0; i < n; i++) tmp[i] = buf[n - 1 - i];
    w = s->width;
    pad = (w > n) ? w - n : 0;
    if (!(s->flags & 0x0040))
        while (pad-- > 0) st_putc(s, s->fill);
    st_write(s, tmp, (uint32_t)n);
    pad = (w > n) ? w - n : 0;
    if (s->flags & 0x0040)
        while (pad-- > 0) st_putc(s, s->fill);
    s->width = 0; /* width resets after every insertion */
}
/* Copy the host contents into a guest basic_string (VC8 layout above). */
static void st_to_str(GStream *s, uint32_t dst) {
    Vc8Str *o = GPTR(dst);
    if (!o) return;
    if (s && s->wide) {
        uint32_t n = s->wn, i;
        if (n < 8) {
            o->res = 7;
            for (i = 0; i < n; i++) ((uint16_t *)&o->bx)[i] = s->w[i];
            ((uint16_t *)&o->bx)[n] = 0;
        } else {
            uint32_t d = gmalloc(2 * (n + 1));
            for (i = 0; i < n; i++) M16(d + 2 * i) = s->w[i];
            M16(d + 2 * n) = 0;
            o->bx = d;
            o->res = n;
        }
        o->size = n;
    } else {
        uint32_t n = s ? s->cn : 0;
        vc8_set_c(o, (s && s->c) ? s->c : "", n);
    }
    o->hdr = VC8_EMPTY;
}

/* ---- constructors / destructors ------------------------------------- */
/* The engine pushes two dwords to the ostringstream/stringstream ctor and
 * three to the istringstream ctor (both extra arguments are ignored). */
SHIM(io_os_ctor) { st_obj_init(THIS); st_open(THIS, 0); RET(THIS, 2); }
SHIM(io_ws_ctor) { st_obj_init(THIS); st_open(THIS, 1); RET(THIS, 2); }
SHIM(io_is_ctor) {
    GStream *s;
    st_obj_init(THIS);
    s = st_open(THIS, 0);
    Vc8Str *v = GPTR(ARG(0));
    if (v) st_write(s, vc8_ptr(v), v->size);
    RET(THIS, 3);
}
SHIM(io_os_dtor) { st_close(THIS); RET(0, 0); }
SHIM(io_ws_dtor) { st_close(THIS); RET(0, 0); }
SHIM(io_is_dtor) { st_close(THIS); RET(0, 0); }

/* ---- ?str (returns basic_string by value through a hidden pointer) --- */
SHIM(io_os_str_get) { st_to_str(st_find(THIS), ARG(0)); RET(ARG(0), 1); }
SHIM(io_ws_str_get) { st_to_str(st_find(THIS), ARG(0)); RET(ARG(0), 1); }
SHIM(io_os_str_set) {
    GStream *s = st_open(THIS, 0);
    Vc8Str *v = GPTR(ARG(0));
    if (v) st_write(s, vc8_ptr(v), v->size);
    RET(0, 1);
}

/* ---- insertions ----------------------------------------------------- */
SHIM(io_os_putk) { GStream *s = st_find(THIS); if (s) st_num(s, ARG(0), 0); RET(THIS, 1); }
SHIM(io_os_putg) { GStream *s = st_find(THIS); if (s) st_num(s, ARG(0) & 0xFFFFu, 0); RET(THIS, 1); }
SHIM(io_ws_putk) { GStream *s = st_find(THIS); if (s) st_num(s, ARG(0), 0); RET(THIS, 1); }
SHIM(io_ws_putj) {
    GStream *s = st_find(THIS);
    int32_t v = (int32_t)ARG(0);
    if (s) {
        if (v < 0) st_num(s, (uint64_t)(-(int64_t)v), 1);
        else st_num(s, (uint64_t)v, 0);
    }
    RET(THIS, 1);
}
SHIM(io_os_putmanip) {
    static uint32_t endl_thunk;
    GStream *s;
    if (!endl_thunk)
        endl_thunk = rt_import("msvcp80.dll",
                               "?endl@std@@YAAAV?$basic_ostream@DU?$char_traits@D@std@@@1@AAV21@@Z");
    s = st_find(THIS);
    if (s && (ARG(0) == endl_thunk || ARG(0) == 0x10D7A334u)) st_putc(s, '\n');
    RET(THIS, 1);
}
SHIM(io_endl) { /* std::endl, __cdecl */
    GStream *s = st_find(ARG(0));
    if (s) st_putc(s, '\n');
    RETC(ARG(0));
}
SHIM(io_put_str) { /* operator<<(ostream&, const basic_string&), __cdecl */
    GStream *s = st_find(ARG(0));
    Vc8Str *v = GPTR(ARG(1));
    if (s && v) st_write(s, vc8_ptr(v), v->size);
    RETC(ARG(0));
}

/* ---- stream / ios_base plumbing (mostly no-ops) --------------------- */
SHIM(io_flush) { RET(THIS, 0); }
SHIM(io_wflush) { RET(THIS, 0); }
SHIM(io_osfx) { RET(0, 0); }
SHIM(io_wosfx) { RET(0, 0); }
SHIM(io_lock) { RET(0, 0); }
SHIM(io_unlock) { RET(0, 0); }
SHIM(io_wlock) { RET(0, 0); }
SHIM(io_wunlock) { RET(0, 0); }
SHIM(io_rdbuf) { GStream *s = st_find(THIS); RET(s ? s->self : 0, 0); }
SHIM(io_wrdbuf) { GStream *s = st_find(THIS); RET(s ? s->self : 0, 0); }
SHIM(io_tie) { RET(0, 0); }
SHIM(io_wtie) { RET(0, 0); }
SHIM(io_setstate) { GStream *s = st_find(THIS); if (s) s->good = (ARG(0) == 0); RET(0, 2); }
SHIM(io_wsetstate) { GStream *s = st_find(THIS); if (s) s->good = (ARG(0) == 0); RET(0, 2); }
SHIM(io_putc) { GStream *s = st_find(THIS); if (s) st_putc(s, (int)(int8_t)ARG(0)); RET(ARG(0), 1); }
SHIM(io_wputc) { GStream *s = st_find(THIS); if (s) st_putc(s, ARG(0) & 0xFFFFu); RET(ARG(0), 1); }
SHIM(io_putn) { GStream *s = st_find(THIS); if (s) st_write(s, GPTR(ARG(0)), ARG(1)); RET(ARG(1), 2); }
SHIM(io_wputn) { GStream *s = st_find(THIS); if (s) st_write(s, GPTR(ARG(0)), ARG(1)); RET(ARG(1), 2); }
SHIM(io_fill) { GStream *s = st_find(THIS); RET(s ? s->fill : ' ', 0); }
SHIM(io_is_getk) { /* istream& operator>>(unsigned long&) */
    GStream *s = st_find(THIS);
    uint32_t v = 0;
    if (s && !s->wide) {
        while (s->rdpos < s->cn && (s->c[s->rdpos] == ' ' || s->c[s->rdpos] == '\t')) s->rdpos++;
        while (s->rdpos < s->cn && s->c[s->rdpos] >= '0' && s->c[s->rdpos] <= '9')
            v = v * 10 + (uint32_t)(s->c[s->rdpos++] - '0');
    }
    if (ARG(0)) M32(ARG(0)) = v;
    RET(THIS, 1);
}

/* ios_base / basic_ios (thiscall).  `this` is the sub-object the engine
 * reaches through the virtual-base adjustor. */
SHIM(ios_good) { GStream *s = st_find(THIS); RET(s ? (uint32_t)(s->good != 0) : 1u, 0); }
SHIM(ios_flags) { GStream *s = st_find(THIS); RET(s ? (uint32_t)s->flags : 0x201u, 0); }
SHIM(ios_width_get) { GStream *s = st_find(THIS); RET(s ? (uint32_t)s->width : 0u, 0); }
SHIM(ios_width_set) {
    GStream *s = st_find(THIS);
    uint32_t old = s ? (uint32_t)s->width : 0u;
    if (s) s->width = (int32_t)ARG(1);
    RET(old, 1);
}
SHIM(ios_setstate) { GStream *s = st_find(THIS); if (s) s->good = (ARG(1) == 0); RET(0, 2); }
SHIM(ios_clear) { GStream *s = st_find(THIS); if (s) s->good = (ARG(0) == 0); RET(0, 2); }

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
SHIM(gethostbyname) {
    /* Offline but *successful*: something has to resolve, or the engine's
     * sockaddr_in::set() walk (inet_addr -> resolve -> inet_ntoa ->
     * inet_addr) never terminates.  Answer with 127.0.0.1, which is what a
     * loopback-only host would return. */
    static uint32_t hostent_g, name_g, alias_g, list_g, addr_g;
    if (!hostent_g) {
        name_g = gmalloc(16);
        memcpy(MEM + name_g, "localhost", 10);
        alias_g = gmalloc(4);
        M32(alias_g) = 0;
        addr_g = gmalloc(4);
        M32(addr_g) = 0x0100007Fu; /* 127.0.0.1, network order */
        list_g = gmalloc(8);
        M32(list_g) = addr_g;
        M32(list_g + 4) = 0;
        hostent_g = gmalloc(16);
        M32(hostent_g + 0x00) = name_g;   /* h_name */
        M32(hostent_g + 0x04) = alias_g;  /* h_aliases */
        M16(hostent_g + 0x08) = 2;        /* h_addrtype = AF_INET */
        M16(hostent_g + 0x0a) = 4;        /* h_length */
        M32(hostent_g + 0x0c) = list_g;   /* h_addr_list */
    }
    once("WS2_32.gethostbyname (offline: resolves to 127.0.0.1)");
    RET(hostent_g, 1);
}
SHIM(gethostbyaddr) { RET(0, 3); }
SHIM(gethostname) {
    const char *h = "localhost";
    uint32_t buf = ARG(0), n = ARG(1);
    if (buf && n) {
        size_t cl = strlen(h);
        if (cl > n - 1) cl = n - 1;
        memcpy(MEM + buf, h, cl);
        M8(buf + cl) = 0;
    }
    RET(0, 2);
}
SHIM(getaddrinfo) { RET(11001, 4); }            /* WSAHOST_NOT_FOUND */
SHIM(freeaddrinfo) { RET(0, 1); }
SHIM(getsockname) { RET(0xFFFFFFFFu, 3); }
/* Real dotted-quad parsing/formatting: stub versions made the engine's
 * resolver loop forever (see gethostbyname above). */
static uint32_t ipv4_parse(const char *s) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int n = 0, d = 0;
        if (!isdigit((unsigned char)*s)) return 0xFFFFFFFFu;
        while (isdigit((unsigned char)*s)) {
            n = n * 10 + (*s - '0');
            if (n > 255) return 0xFFFFFFFFu;
            s++;
            d++;
        }
        if (d > 3) return 0xFFFFFFFFu;
        v |= (uint32_t)n << (8 * i);
        if (i < 3) {
            if (*s != '.') return 0xFFFFFFFFu;
            s++;
        }
    }
    if (*s) return 0xFFFFFFFFu;
    return v;
}
static char g_ntoa_host[24];
static uint32_t g_ntoa_g; /* guest copy handed to the engine */
SHIM(inet_addr) {
    const char *s = GSTR(ARG(0));
    RET(s ? ipv4_parse(s) : 0xFFFFFFFFu, 1);
}
SHIM(inet_ntoa) {
    uint32_t ip = ARG(0);
    size_t n;
    if (!g_ntoa_g) g_ntoa_g = gmalloc(24);
    snprintf(g_ntoa_host, sizeof g_ntoa_host, "%u.%u.%u.%u", ip & 0xFFu, (ip >> 8) & 0xFFu,
             (ip >> 16) & 0xFFu, (ip >> 24) & 0xFFu);
    n = strlen(g_ntoa_host);
    memcpy(MEM + g_ntoa_g, g_ntoa_host, n + 1);
    RET(g_ntoa_g, 1);
}
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
/* InitializeSecurityContextA takes 12 parameters; registering 9 left 12 bytes
 * of arguments on the guest stack at every call. */
SEC0(InitializeSecurityContextA, 12) SEC0(DeleteSecurityContext, 1)
SEC0(QueryContextAttributesA, 3) SEC0(EncryptMessage, 4) SEC0(DecryptMessage, 4)
SEC0(ApplyControlToken, 2) SEC0(FreeContextBuffer, 1)
SHIM(CertGetCertificateChain) { RET(0, 8); }   /* 8 parameters, not 9 */
SHIM(CertFreeCertificateChain) { RET(0, 1); }
SHIM(CertFreeCertificateContext) { RET(1, 1); }
SHIM(CertVerifyCertificateChainPolicy) { RET(0, 4); }

/* ---- Crypt32/Advapi hashing (online auth): accepts, returns fixed bytes. */
SHIM(CryptAcquireContextA) { if (ARG(0)) M32(ARG(0)) = 1; RET(1, 5); }
SHIM(CryptReleaseContext) { RET(1, 2); }
/* CryptCreateHash(hProv, Algid, hKey, dwFlags, phHash) - phHash is argument 4
 * of 5; the old code wrote through argument 5, which is not part of the call. */
SHIM(CryptCreateHash) { if (ARG(4)) M32(ARG(4)) = 2; RET(1, 5); }
SHIM(CryptHashData) { RET(1, 4); }
/* CryptGetHashParam(hProv, dwParam, pbData, pdwDataLen, dwFlags): the caller
 * passes its buffer size in *pdwDataLen, so report an empty result rather than
 * clearing ARG(3) bytes starting at the stack slot that held the size. */
SHIM(CryptGetHashParam) { if (ARG(3)) M32(ARG(3)) = 0; RET(1, 5); }
SHIM(CryptDestroyHash) { RET(1, 1); }
/* CryptGenRandom(hProv, dwLen, pbBuffer): the buffer is the third parameter
 * and the length the second - swapped here before, so the engine's memset of
 * a 32-byte nonce wrote 500 MB at the stack address ARG(2) while reading the
 * length from it.  Offline but functional: cheap pseudo-random bytes. */
SHIM(CryptGenRandom) {
    uint32_t len = ARG(1), buf = ARG(2);
    uint8_t *p = GPTR(buf);
    static uint32_t seed;
    once("Advapi32.CryptGenRandom (offline: pseudo-random bytes)");
    if (len > 0x100000u) len = 0x100000u;
    if (!seed) seed = (uint32_t)time(NULL) | 1u;
    if (p)
        for (uint32_t i = 0; i < len; i++) {
            seed = seed * 1103515245u + 12345u;
            p[i] = (uint8_t)(seed >> 16);
        }
    RET(1, 3);
}

/* ---- IP Helper / PSAPI ---- */
SHIM(GetAdaptersInfo) { if (ARG(1)) M32(ARG(1)) = 0; RET(232, 2); } /* ERROR_NO_DATA */
SHIM(GetPerformanceInfo) {
    /* PERFORMANCE_INFORMATION is 52 bytes on Win32; honour the caller's cb. */
    uint32_t n = ARG(1);
    if (n > 52) n = 52;
    if (ARG(0)) memset(GPTR(ARG(0)), 0, n);
    RET(1, 2);
}

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
/* ---- ole32: a real in-memory IStream over an HGLOBAL -------------------- *
 * The engine wraps its own image bytes in a stream and hands it to GDI+.
 * Returning E_FAIL *without writing *ppstm* is what caused the first null
 * dereference of a real run: the engine's caller initialises its IStream* to
 * NULL, and after the failure it Releases it unguarded (guest 0x100063F6
 * `mov ecx,[eax]` with eax = the untouched local), so the guest touched
 * address 0.  The stream is therefore real: it reads the HGLOBAL through the
 * same guest heap the engine allocated it from.
 *
 * HGLOBAL in this runtime *is* the guest pointer (GlobalLock returns it as
 * is), so the size comes from gsize() and fDeleteOnRelease is honoured by
 * leaving the block to the engine: the engine passes FALSE here and frees it
 * with GlobalFree after the last Release. */
typedef struct { uint32_t base, size, pos; } MemStream;
static HComClass k_istream;

static MemStream *ms_of(CPU *c) { return (MemStream *)hcom_host(ARG(0)); }

SHIM(istream_QueryInterface) { hcom_addref(ARG(0)); if (ARG(2)) M32(ARG(2)) = ARG(0); RET(0, 3); }
SHIM(istream_AddRef) { RET(hcom_addref(ARG(0)), 1); }
SHIM(istream_Release) {
    MemStream *m = ms_of(c);
    uint32_t left;
    int died = hcom_release(ARG(0), &left);
    if (died) free(m);
    RET(left, 1);
}
SHIM(istream_Read) {
    MemStream *m = ms_of(c);
    uint32_t pv = ARG(1), cb = ARG(2), pcb = ARG(3), n = 0;
    if (m && pv && cb) {
        n = m->size > m->pos ? m->size - m->pos : 0;
        if (n > cb) n = cb;
        if (n) memcpy(MEM + pv, MEM + m->base + m->pos, n);
        m->pos += n;
    }
    if (pcb) M32(pcb) = n;
    RET(0, 4);
}
SHIM(istream_Write) { if (ARG(3)) M32(ARG(3)) = 0; RET(E_NOTIMPL_, 4); }
SHIM(istream_Seek) {   /* LARGE_INTEGER (2 dwords), DWORD, ULARGE_INTEGER* */
    MemStream *m = ms_of(c);
    int64_t move = (int64_t)((uint64_t)ARG(1) | (uint64_t)ARG(2) << 32), np = 0;
    uint32_t origin = ARG(3), pnew = ARG(4);
    if (m) {
        if (origin == 0) np = move;
        else if (origin == 1) np = (int64_t)m->pos + move;
        else np = (int64_t)m->size + move;
        if (np < 0) np = 0;
        if (np > (int64_t)m->size) np = m->size;
        m->pos = (uint32_t)np;
    }
    if (pnew) { M32(pnew) = (uint32_t)np; M32(pnew + 4) = (uint32_t)((uint64_t)np >> 32); }
    RET(0, 5);
}
SHIM(istream_SetSize) { RET(E_NOTIMPL_, 3); }
SHIM(istream_CopyTo) { RET(E_NOTIMPL_, 6); }
SHIM(istream_Commit) { RET(0, 2); }
SHIM(istream_Revert) { RET(E_NOTIMPL_, 1); }
SHIM(istream_LockRegion) { RET(E_NOTIMPL_, 6); }
SHIM(istream_UnlockRegion) { RET(E_NOTIMPL_, 6); }
SHIM(istream_Stat) {   /* STATSTG: type at 4, cbSize at 8, grfMode at 0x28 */
    MemStream *m = ms_of(c);
    uint32_t p = ARG(1);
    if (p) {
        memset(MEM + p, 0, 0x48);
        M32(p + 4) = 2;                                    /* STGTY_STREAM */
        M32(p + 8) = m ? m->size : 0;
        M32(p + 0x28) = 0;                                 /* STGM_READ */
    }
    RET(0, 3);
}
SHIM(istream_Clone) { RET(E_NOTIMPL_, 2); }

SHIM(CreateStreamOnHGlobal) {
    uint32_t h = ARG(0), del = ARG(1), out = ARG(2);
    if (!out) RET(0x80070057u /* E_INVALIDARG */, 3);
    MemStream *m = calloc(1, sizeof *m);
    m->base = h;
    m->size = h ? gsize(h) : 0;
    m->pos = 0;
    (void)del;   /* the engine owns the HGLOBAL (it passes FALSE) */
    M32(out) = hcom_new(&k_istream, m);
    LOG("ole32.CreateStreamOnHGlobal(%08x, delete=%u) -> %u bytes at %08x", h, del, m->size, M32(out));
    RET(0, 3);
}

/* ---- gdiplus ------------------------------------------------------------ *
 * Dunia gives GDI+ exactly one job at startup: an IStream of image bytes in,
 * a GpBitmap out, then GdipCreateHBITMAPFromBitmap, and the HBITMAP only ever
 * goes into this port's fake DCs (GDI here is a set of no-op handles, there is
 * no BitBlt).  What matters is that every call hands back a *valid* object, so
 * the engine never holds a NULL GpBitmap: the flat API's own status is
 * returned, and the pixels are decoded when the format is recognised (BMP,
 * PNG).  The engine only imports these eight entry points. */
typedef struct { int w, h; uint8_t *px; } GpBitmap;
static HComClass k_gpbitmap;

SHIM(gpbitmap_vcall) { once("gdiplus GpBitmap vtable call (unexpected)"); RET(E_NOTIMPL_, 1); }

SHIM(GdiplusStartup) { if (ARG(0)) M32(ARG(0)) = 1; RET(0, 3); }
SHIM(GdiplusShutdown) { RET(0, 1); }
SHIM(GdipAlloc) { RET(gmalloc(ARG(0)), 1); }
SHIM(GdipFree) { gfree(ARG(0)); RET(0, 1); }

static uint32_t gdi16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t gdi32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/* 24/32-bit BI_RGB, bottom-up or top-down; NULL for anything else. */
static uint8_t *bmp_decode(const uint8_t *b, size_t n, int *ow, int *oh) {
    if (n < 54 || b[0] != 'B' || b[1] != 'M') return NULL;
    uint32_t off = gdi32(b + 10);
    int32_t w = (int32_t)gdi32(b + 18), h = (int32_t)gdi32(b + 22);
    uint32_t bpp = gdi16(b + 28), comp = gdi32(b + 30);
    int topdown = h < 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || comp != 0 || (bpp != 24 && bpp != 32)) return NULL;
    uint32_t stride = ((uint32_t)w * bpp / 8 + 3) & ~3u;
    if ((uint64_t)off + (uint64_t)stride * (uint32_t)h > n) return NULL;
    uint8_t *out = malloc((size_t)w * h * 4);
    if (!out) return NULL;
    for (int y = 0; y < h; y++) {
        const uint8_t *src = b + off + (size_t)stride * (topdown ? y : h - 1 - y);
        uint8_t *dst = out + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            dst[0] = src[x * bpp / 8 + 2];
            dst[1] = src[x * bpp / 8 + 1];
            dst[2] = src[x * bpp / 8 + 0];
            dst[3] = bpp == 32 ? src[x * 4 + 3] : 0xFF;
        }
    }
    *ow = w;
    *oh = h;
    return out;
}

SHIM(GdipCreateBitmapFromStream) {
    uint32_t st = ARG(0), out = ARG(1);
    GpBitmap *gb = calloc(1, sizeof *gb);
    gb->w = gb->h = 1;
    gb->px = calloc(4, 1);
    gb->px[0] = gb->px[1] = gb->px[2] = gb->px[3] = 0xFF;
    uint32_t base = 0, size = 0;
    if (st && M32(st) == k_istream.vtbl) {
        MemStream *m = hcom_host(st);
        if (m) { base = m->base; size = m->size; }
    }
    if (base && size) {
        const uint8_t *bytes = MEM + base;
        int w = 0, h = 0;
        uint8_t *rgba = NULL;
        if (size > 8 && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8)) rgba = png_decode(bytes, size, &w, &h);
        else if (size > 2 && bytes[0] == 'B' && bytes[1] == 'M') rgba = bmp_decode(bytes, size, &w, &h);
        LOG("gdiplus.GdipCreateBitmapFromStream: %u bytes, magic %02x%02x%02x%02x", size, bytes[0], bytes[1],
            size > 2 ? bytes[2] : 0, size > 3 ? bytes[3] : 0);
        if (rgba) {
            free(gb->px);
            gb->px = rgba;
            gb->w = w;
            gb->h = h;
        } else {
            once("gdiplus.GdipCreateBitmapFromStream: unrecognised image (blank 1x1 bitmap)");
        }
    } else {
        once("gdiplus.GdipCreateBitmapFromStream: not one of our streams (blank 1x1 bitmap)");
    }
    if (out) M32(out) = hcom_new(&k_gpbitmap, gb);
    RET(0, 2);
}
SHIM(GdipCreateHBITMAPFromBitmap) {   /* (GpBitmap*, HBITMAP*, ARGB) */
    if (ARG(1)) M32(ARG(1)) = 0x30007;
    RET(0, 3);
}
SHIM(GdipCloneImage) {                /* (GpImage*, GpImage**) */
    uint32_t src = ARG(0), out = ARG(2);
    if (src && M32(src) == k_gpbitmap.vtbl && out) {
        GpBitmap *s = hcom_host(src), *d = calloc(1, sizeof *d);
        size_t n = (size_t)s->w * s->h * 4;
        d->w = s->w;
        d->h = s->h;
        d->px = malloc(n ? n : 4);
        memcpy(d->px, s->px, n);
        M32(out) = hcom_new(&k_gpbitmap, d);
    } else if (out) {
        M32(out) = src;
    }
    RET(0, 2);
}
SHIM(GdipDisposeImage) {
    uint32_t g = ARG(0);
    if (g && M32(g) == k_gpbitmap.vtbl) {
        GpBitmap *gb = hcom_host(g);
        if (gb) { free(gb->px); free(gb); }
        uint32_t left;
        hcom_release(g, &left);
    }
    RET(0, 1);
}

/* ---- dbghelp: the engine's own crash reporter. No symbols, no walking. ---- */
SHIM(SymInitialize) { RET(1, 3); }
SHIM(SymCleanup) { RET(1, 1); }
SHIM(SymGetOptions) { RET(0, 0); }
SHIM(SymSetOptions) { RET(ARG(0), 1); }
SHIM(SymSetSearchPath) { RET(1, 2); }
SHIM(SymLoadModule) { RET(0, 6); }
SHIM(SymUnloadModule) { RET(1, 2); }   /* (hProcess, BaseOfDll) */
SHIM(SymGetModuleInfo) { RET(0, 3); }
SHIM(SymGetModuleInfo64) { RET(0, 3); }
SHIM(SymGetSymFromAddr) { RET(0, 4); }
SHIM(SymGetSymFromAddr64) { RET(0, 4); }
SHIM(SymGetLineFromAddr) { RET(0, 4); }
SHIM(SymGetLineFromAddr64) { RET(0, 4); }
/* (hProcess, SearchPath, FileName, id, two, three, flags, FoundFile,
 * callback, context) - ten parameters, not eight. */
SHIM(SymFindFileInPath) { RET(0, 10); }
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
/* GetPrivateProfileStringW lives next to the ANSI version below: it is the
 * call that supplies the engine language (see the comment there). */
SHIM(InterlockedExchangeAdd) { RET(__atomic_fetch_add((uint32_t *)GPTR(ARG(0)), ARG(1), __ATOMIC_SEQ_CST), 2); }

/* Named pipes: the engine creates one for its console/remote log. Refuse
 * cleanly; the callers treat it as "not available". Each of these pops as many
 * arguments as the API takes: CreateNamedPipeA has eight (not seven),
 * GetNamedPipeHandleStateA seven (not five), PeekNamedPipe six (not five) and
 * ReadDirectoryChangesW eight (not six). */
SHIM(CreateNamedPipeA) { once("kernel32.CreateNamedPipeA (no pipes)"); RET(0xFFFFFFFFu, 8); }
SHIM(ConnectNamedPipe) { RET(0, 2); }
SHIM(DisconnectNamedPipe) { RET(0, 1); }
SHIM(GetNamedPipeHandleStateA) { RET(0, 7); }
SHIM(SetNamedPipeHandleState) { RET(0, 4); }
SHIM(PeekNamedPipe) { RET(0, 6); }
SHIM(ReadDirectoryChangesW) { RET(0, 8); }
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
    FILE *f;
    if (!app) app = "";
    if (!key) key = "";
    f = fopen(host, "r");
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
    FILE *f;
    if (!app) app = "";
    if (!key) key = "";
    if (!val) val = "";
    f = fopen(host, "rb");
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
    static int seen;
    uint32_t cap = ARG(4), buf = ARG(3);
    const char *file = GSTR(ARG(5));
    if (!seen++)
        LOG("fc2: GetPrivateProfileStringA(app='%s' key='%s' def='%s' file='%s')",
            GSTR(ARG(0)) ? GSTR(ARG(0)) : "(null)", GSTR(ARG(1)) ? GSTR(ARG(1)) : "(null)",
            GSTR(ARG(2)) ? GSTR(ARG(2)) : "(null)", file ? file : "(null)");
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

/* Wide->ANSI for the profile APIs: only the ASCII range is meaningful for
 * section/key/value names, so fold anything else to '?'. */
static void w16_to_a(const void *w, char *out, size_t cap) {
    size_t n = 0;
    if (!cap) return;
    if (w) {
        for (; u16_at(w, n) && n + 1 < cap; n++) {
            uint16_t ch = u16_at(w, n);
            out[n] = ch < 0x80 ? (char)ch : '?';
        }
    }
    out[n < cap ? n : cap - 1] = 0;
}

/* Far Cry 2 obtains its engine language through the *wide* entry point:
 *
 *   GetPrivateProfileStringW(L"FC2_INIT", L"language", L"english", buf, 256,
 *                            <profile dir>\FC2Init.ini)
 *
 * (f_10004690, Dunia.dll 0x10004818-0x10004827; the literals are at
 * 0x10d839bc/0x10d839d0/0x10d839e4/0x10d839f4.) The result becomes
 * Language+0x34 (magma::Language, built by f_104C0ED0/f_1029B6E0), and that
 * name is the first component of every localized data path - "ui\localized
 * \<lang>\...". While this was a stub returning 0 without writing the buffer,
 * f_1029B6E0 copied the string's *stale* inline bytes as the language name
 * (one 0x10 byte), so every localized lookup was unmatchable: the UI package
 * documents never registered and the first page request faulted on the NULL
 * document. Return the value when the INI is present and lpDefault otherwise,
 * which is what Windows does - the game's own default is L"english". */
SHIM(GetPrivateProfileStringW) {
    static int seen;
    uint32_t cap = ARG(4), buf = ARG(3);
    char app[256], key[256], def[512], file[1024];
    w16_to_a(ARG(0) ? GPTR(ARG(0)) : NULL, app, sizeof app);
    w16_to_a(ARG(1) ? GPTR(ARG(1)) : NULL, key, sizeof key);
    w16_to_a(ARG(2) ? GPTR(ARG(2)) : NULL, def, sizeof def);
    w16_to_a(ARG(5) ? GPTR(ARG(5)) : NULL, file, sizeof file);
    if (!seen++)
        LOG("fc2: GetPrivateProfileStringW(app='%s' key='%s' def='%s' file='%s')",
            app[0] ? app : "(null)", key[0] ? key : "(null)",
            def[0] ? def : "(null)", file[0] ? file : "(null)");
    char host[1024], val[512];
    const char *r = NULL;
    rt_host_path(file[0] ? file : "port/FC2Init.ini", host, sizeof host);
    if (ini_get(host, app, key, val, sizeof val)) r = val;
    if (!r && file[0]) {
        /* The install keeps FC2Init.ini in bin/, next to the executable; the
         * engine looks for it under its profile directory, which never holds
         * it. Fall back to the file the game actually ships. */
        const char *base = strrchr(file, '\\');
        base = base ? base + 1 : file;
        if (*base && g_game_dir[0]) {
            snprintf(host, sizeof host, "%s/bin/%s", g_game_dir, base);
            if (ini_get(host, app, key, val, sizeof val)) r = val;
        }
    }
    if (!r) r = def;
    uint32_t n = (uint32_t)strlen(r);
    if (buf && cap) {
        uint16_t *w = (uint16_t *)(MEM + buf);
        uint32_t c = n < cap - 1 ? n : cap - 1;
        for (uint32_t i = 0; i < c; i++) u16_put(w, i, (uint8_t)r[i]);
        u16_put(w, c, 0);
        n = c;
    }
    RET(n, 6);
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
SHIM(UpdateLayeredWindow) { once("user32.UpdateLayeredWindow (no layered windows)"); RET(0, 9); }
/* DISPLAY_DEVICEA = cb, DeviceName[32], DeviceString[128], StateFlags,
 * DeviceID[128], DeviceKey[128]  (0x1a8 bytes). Far Cry 2 walks this looking
 * for the primary adapter and bails out with ERROR_DISPLAY_PRIMARYDEVICE if
 * the first call fails, so report exactly one attached primary device. */
SHIM(EnumDisplayDevicesA) {
    if (ARG(1) != 0) RET(0, 4);
    uint32_t dd = ARG(2);
    if (dd) {
        static const char name[] = "\\\\.\\DISPLAY1";
        static const char desc[] = "Generic Display Adapter";
        uint32_t i;
        M32(dd) = 0x1A8u;
        for (i = 0; name[i]; i++) M8(dd + 4 + i) = (uint8_t)name[i];
        M8(dd + 4 + i) = 0;
        for (i = 0; desc[i]; i++) M8(dd + 36 + i) = (uint8_t)desc[i];
        M8(dd + 36 + i) = 0;
        M32(dd + 164) = 0x1u | 0x4u; /* ATTACHED_TO_DESKTOP | PRIMARY_DEVICE */
        M8(dd + 168) = 0;
        M8(dd + 296) = 0;
    }
    once("user32.EnumDisplayDevicesA (one synthetic primary adapter)");
    RET(1, 4);
}
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
    M(_initterm_e) M(_beginthreadex) M(_endthreadex)
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
    {"msvcp80.dll", "?_Copy_s@?$char_traits@G@std@@SAPAGPAGIPBGI@Z", shim_tr_copy_s_w},
    {"msvcp80.dll", "?_Move_s@?$char_traits@D@std@@SAPADPADIPBDI@Z", shim_tr_move_s},
    {"msvcp80.dll", "?_Move_s@?$char_traits@G@std@@SAPAGPAGIPBGI@Z", shim_tr_move_s_w},
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
    /* Dunia imports WS2_32 by ordinal (the IAT entries carry the high bit),
     * so the same offline shims are registered under ws2_32's stable
     * documented ordinals as well. */
    {"ws2_32.dll", "#1", shim_accept},        {"ws2_32.dll", "#2", shim_bind},
    {"ws2_32.dll", "#3", shim_closesocket},   {"ws2_32.dll", "#4", shim_connect},
    {"ws2_32.dll", "#6", shim_getsockname},   {"ws2_32.dll", "#7", shim_getsockopt},
    {"ws2_32.dll", "#8", shim_htonl},         {"ws2_32.dll", "#9", shim_htons},
    {"ws2_32.dll", "#10", shim_ioctlsocket},  {"ws2_32.dll", "#11", shim_inet_addr},
    {"ws2_32.dll", "#12", shim_inet_ntoa},    {"ws2_32.dll", "#13", shim_listen},
    {"ws2_32.dll", "#14", shim_ntohl},        {"ws2_32.dll", "#15", shim_ntohs},
    {"ws2_32.dll", "#16", shim_recv},         {"ws2_32.dll", "#17", shim_recvfrom},
    {"ws2_32.dll", "#18", shim_select},       {"ws2_32.dll", "#19", shim_send},
    {"ws2_32.dll", "#20", shim_sendto},       {"ws2_32.dll", "#21", shim_setsockopt},
    {"ws2_32.dll", "#22", shim_shutdown},     {"ws2_32.dll", "#23", shim_socket},
    {"ws2_32.dll", "#51", shim_gethostbyaddr}, {"ws2_32.dll", "#52", shim_gethostbyname},
    {"ws2_32.dll", "#57", shim_gethostname},  {"ws2_32.dll", "#101", shim_WSAAsyncSelect},
    {"ws2_32.dll", "#111", shim_WSAGetLastError}, {"ws2_32.dll", "#115", shim_WSAStartup},
    {"ws2_32.dll", "#116", shim_WSACleanup},  {"ws2_32.dll", "#151", shim___WSAFDIsSet},

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

/* MSVCP80 iostream/stringstream, implemented against the host-side stream
 * registry above.  (These used to be registered with NULL so the first use
 * stopped with its mangled name; see docs/RECON.md, "Known gaps".) */
static const ShimDef msvcp_streams[] = {
#define ST80(n, f) {"msvcp80.dll", n, shim_##f},
    ST80("??_D?$basic_stringstream@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@QAEXXZ", io_ws_dtor)
    ST80("?str@?$basic_stringstream@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@QBE?AV?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@2@@2@XZ", io_ws_str_get)
    ST80("??0?$basic_stringstream@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@QAE@H@Z", io_ws_ctor)
    ST80("??6?$basic_ostream@GU?$char_traits@G@std@@@std@@QAEAAV01@K@Z", io_ws_putk)
    ST80("??6?$basic_ostream@GU?$char_traits@G@std@@@std@@QAEAAV01@J@Z", io_ws_putj)
    ST80("?rdbuf@?$basic_ios@DU?$char_traits@D@std@@@std@@QBEPAV?$basic_streambuf@DU?$char_traits@D@std@@@2@XZ", io_rdbuf)
    ST80("?rdbuf@?$basic_ios@GU?$char_traits@G@std@@@std@@QBEPAV?$basic_streambuf@GU?$char_traits@G@std@@@2@XZ", io_wrdbuf)
    ST80("?_Lock@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEXXZ", io_lock)
    ST80("?_Unlock@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEXXZ", io_unlock)
    ST80("?_Lock@?$basic_streambuf@GU?$char_traits@G@std@@@std@@QAEXXZ", io_wlock)
    ST80("?_Unlock@?$basic_streambuf@GU?$char_traits@G@std@@@std@@QAEXXZ", io_wunlock)
    ST80("?flush@?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV12@XZ", io_flush)
    ST80("?flush@?$basic_ostream@GU?$char_traits@G@std@@@std@@QAEAAV12@XZ", io_wflush)
    ST80("?tie@?$basic_ios@DU?$char_traits@D@std@@@std@@QBEPAV?$basic_ostream@DU?$char_traits@D@std@@@2@XZ", io_tie)
    ST80("?tie@?$basic_ios@GU?$char_traits@G@std@@@std@@QBEPAV?$basic_ostream@GU?$char_traits@G@std@@@2@XZ", io_wtie)
    ST80("?_Osfx@?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEXXZ", io_osfx)
    ST80("?_Osfx@?$basic_ostream@GU?$char_traits@G@std@@@std@@QAEXXZ", io_wosfx)
    ST80("?setstate@?$basic_ios@GU?$char_traits@G@std@@@std@@QAEXH_N@Z", io_wsetstate)
    ST80("?sputn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEHPBDH@Z", io_putn)
    ST80("?sputn@?$basic_streambuf@GU?$char_traits@G@std@@@std@@QAEHPBGH@Z", io_wputn)
    ST80("?sputc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEHD@Z", io_putc)
    ST80("?sputc@?$basic_streambuf@GU?$char_traits@G@std@@@std@@QAEGG@Z", io_wputc)
    ST80("?fill@?$basic_ios@GU?$char_traits@G@std@@@std@@QBEGXZ", io_fill)
    ST80("??_D?$basic_ostringstream@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEXXZ", io_os_dtor)
    ST80("?str@?$basic_ostringstream@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBE?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@2@XZ", io_os_str_get)
    ST80("?str@?$basic_ostringstream@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEXABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@2@@Z", io_os_str_set)
    ST80("??0?$basic_ostringstream@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@H@Z", io_os_ctor)
    ST80("??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@K@Z", io_os_putk)
    ST80("??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@G@Z", io_os_putg)
    ST80("??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@P6AAAV01@AAV01@@Z@Z", io_os_putmanip)
    ST80("??$?6DU?$char_traits@D@std@@V?$allocator@D@1@@std@@YAAAV?$basic_ostream@DU?$char_traits@D@std@@@0@AAV10@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@@Z", io_put_str)
    ST80("?endl@std@@YAAAV?$basic_ostream@DU?$char_traits@D@std@@@1@AAV21@@Z", io_endl)
    ST80("??_D?$basic_istringstream@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEXXZ", io_is_dtor)
    ST80("??5?$basic_istream@DU?$char_traits@D@std@@@std@@QAEAAV01@AAK@Z", io_is_getk)
    ST80("??0?$basic_istringstream@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@1@H@Z", io_is_ctor)
#undef ST80
    {NULL, NULL, NULL}
};

void fc2_shims_init(void) {
    rt_register_shims(defs);
    rt_register_shims(extra);
    msvcrt_register_as_msvcr80();

    rt_register_shims(msvcp_streams);

    /* Guest COM objects: the ole32 memory stream and the GDI+ bitmap the
     * engine holds a pointer to. Both need a real vtable so the engine's
     * Release()/dispose calls land somewhere valid. */
    static const ShimFn istream_fns[] = {
        shim_istream_QueryInterface, shim_istream_AddRef,        shim_istream_Release,
        shim_istream_Read,           shim_istream_Write,         shim_istream_Seek,
        shim_istream_SetSize,        shim_istream_CopyTo,        shim_istream_Commit,
        shim_istream_Revert,         shim_istream_LockRegion,    shim_istream_UnlockRegion,
        shim_istream_Stat,           shim_istream_Clone,
    };
    static const ShimFn gpbitmap_fns[] = {shim_gpbitmap_vcall};
    hcom_class_init(&k_istream, "IStream", istream_fns, (int)(sizeof istream_fns / sizeof istream_fns[0]));
    hcom_class_init(&k_gpbitmap, "GpBitmap", gpbitmap_fns, (int)(sizeof gpbitmap_fns / sizeof gpbitmap_fns[0]));

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
