/* Import resolution: maps dll!name to host shims (or to a stub that aborts
 * with the import's name when it is first called). */
#include "rt.h"
#include <string.h>
#include <strings.h>

#define MAX_DEFS 2048
/* Cache keys are "dll!name", and MSVC's mangled C++ import names run long
 * (the longest in Dunia.dll is 140 bytes).  Truncating one silently turns the
 * matching shim into an abort stub, so the buffers get real headroom and
 * rt_import() says so if even these are not enough. */
#define IMP_KEY 1024
static const ShimDef *g_defs[MAX_DEFS];
static int g_ndefs;
static struct { char key[IMP_KEY]; uint32_t thunk; } g_imports[MAX_DEFS];
static int g_nimports;

void rt_register_shims(const ShimDef *defs) {
    for (; defs->name; defs++) {
        if (g_ndefs >= MAX_DEFS) rt_fatal("too many shims");
        g_defs[g_ndefs++] = defs;
    }
}

/* Register the same implementations under another DLL name. USM.exe imports
 * the VC7.1 CRT as msvcr71.dll, which is the same API as msvcrt.dll. */
void rt_register_shims_alias(const ShimDef *defs, const char *dll) {
    for (; defs->name; defs++) {
        ShimDef *d = malloc(sizeof *d);
        d->dll = dll;
        d->name = defs->name;
        d->fn = defs->fn;
        if (g_ndefs >= MAX_DEFS) rt_fatal("too many shims");
        g_defs[g_ndefs++] = d;
    }
}

static const char *dll_base(const char *dll, char *buf, size_t n) {
    /* "C:\\x\\KERNEL32.DLL" -> "kernel32" */
    const char *p = strrchr(dll, '\\');
    p = p ? p + 1 : dll;
    snprintf(buf, n, "%s", p);
    char *dot = strrchr(buf, '.');
    if (dot) *dot = 0;
    return buf;
}

static ShimFn find_shim(const char *want, const char *name) {
    char have[64];
    for (int i = 0; i < g_ndefs; i++) {
        if (strcmp(g_defs[i]->name, name)) continue;
        if (strcasecmp(dll_base(g_defs[i]->dll, have, sizeof have), want)) continue;
        return g_defs[i]->fn;
    }
    return NULL;
}

/* Data imports (variables) and exports of recompiled DLLs resolve to plain
 * guest addresses rather than thunks. */
#define MAX_ADDRS 4096
static struct { char key[IMP_KEY]; uint32_t addr; } g_addrs[MAX_ADDRS];
static int g_naddrs;

/* "dll!name" cache key, with a hard failure rather than a silent truncation. */
static void key_build(char *out, const char *want, const char *name) {
    int n = snprintf(out, IMP_KEY, "%s!%s", want, name);
    if (n >= IMP_KEY) rt_fatal("import name too long (%d bytes): %s", n, name);
}

void rt_register_data(const char *dll, const char *name, uint32_t addr) {
    char want[64];
    dll_base(dll, want, sizeof want);
    char key[IMP_KEY];
    key_build(key, want, name);
    /* Re-registering a known key overrides it in place. load_pe() registers
     * every export of a recompiled module once; the one legitimate second
     * call is binkdiag's, which re-points a binkw32 export at a logging
     * wrapper after the module is loaded (rt_import() reads g_addrs first,
     * so the update is what the next import pass binds). */
    for (int i = 0; i < g_naddrs; i++)
        if (!strcasecmp(g_addrs[i].key, key)) { g_addrs[i].addr = addr; return; }
    if (g_naddrs >= MAX_ADDRS) rt_fatal("too many data imports");
    key_build(g_addrs[g_naddrs].key, want, name);
    g_addrs[g_naddrs++].addr = addr;
}

/* GetProcAddress semantics: NULL when we have no implementation. */
uint32_t rt_import_opt(const char *dll, const char *name) {
    char want[64];
    dll_base(dll, want, sizeof want);
    char key[IMP_KEY];
    key_build(key, want, name);
    for (int i = 0; i < g_naddrs; i++)
        if (!strcasecmp(g_addrs[i].key, key)) return g_addrs[i].addr;
    if (!find_shim(want, name)) return 0;
    return rt_import(dll, name);
}

uint32_t rt_import(const char *dll, const char *name) {
    char want[64], have[64], key[IMP_KEY];
    dll_base(dll, want, sizeof want);
    key_build(key, want, name);
    for (int i = 0; i < g_naddrs; i++)
        if (!strcasecmp(g_addrs[i].key, key)) return g_addrs[i].addr;
    for (int i = 0; i < g_nimports; i++)
        if (!strcasecmp(g_imports[i].key, key)) return g_imports[i].thunk;
    ShimFn fn = NULL;
    for (int i = 0; i < g_ndefs; i++) {
        if (strcmp(g_defs[i]->name, name)) continue;
        if (strcasecmp(dll_base(g_defs[i]->dll, have, sizeof have), want)) continue;
        fn = g_defs[i]->fn;
        break;
    }
    if (!fn) LOG("import %s has no shim yet", key);
    if (g_nimports >= MAX_DEFS) rt_fatal("too many imports");
    snprintf(g_imports[g_nimports].key, sizeof g_imports[0].key, "%s", key);
    char *stored = strdup(key);
    g_imports[g_nimports].thunk = rt_thunk(fn, stored);
    return g_imports[g_nimports++].thunk;
}

void k32_init(void);
void user32_init(void);
void misc_init(void);
void input_init(void);
void d3d9_init(void);
void dinput_init(void);
void dsound_init(void);
void msvcrt_init(void);
void xaudio2_init(void);
void crt_shims_init(void);
void fc2_shims_init(void);
void d3dx9_init(void);

void rt_init_shims(void) {
    k32_init();
    user32_init();
    misc_init();
    input_init();
    d3d9_init();
    dinput_init();
    dsound_init();
    msvcrt_init();
    crt_shims_init();
    xaudio2_init();
    fc2_shims_init();
    d3dx9_init();
}
