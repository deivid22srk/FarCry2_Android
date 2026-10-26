/* Recompiled modules linked into this build. */
#include "game.h"
#include "rt_table.h"

#if defined(GAME_FC2)
/* Far Cry 2. Verified load map (docs/RECON.md):
 *   farcry2.exe  native 0x00400000, relocs stripped, 3 imports from Dunia
 *   Dunia.dll    native 0x10000000, 26 MB image (0x192a000), 404 exports
 *   binkw32.dll  rebased 0x18000000 -> 0x09000000 to clear the guest TEB
 * d3dx9_38.dll is not a module: runtime/d3dx9_host.cpp implements its 15
 * imports on top of the D3D9 bridge (the DLL is not shipped with the game).
 * FC2.dll (SecuROM Activate & Play) is never loaded. */
extern const FnEntry g_fn_table_fc2[];
extern const unsigned g_fn_count_fc2;
extern const FnEntry g_fn_table_dunia[];
extern const unsigned g_fn_count_dunia;
extern const FnEntry g_fn_table_bink[];
extern const unsigned g_fn_count_bink;

const FnModule g_modules[] = {
    {GAME_EXE, g_fn_table_fc2, 0},
    {"Dunia.dll", g_fn_table_dunia, 0},
    {"binkw32.dll", g_fn_table_bink, 0},
};
const unsigned g_nmodules = sizeof g_modules / sizeof g_modules[0];

unsigned rt_module_count(unsigned m) {
    switch (m) {
    case 0: return g_fn_count_fc2;
    case 1: return g_fn_count_dunia;
    case 2: return g_fn_count_bink;
    default: return 0;
    }
}

const char *const g_dll_modules[] = {"Dunia.dll", "binkw32.dll", NULL};
const uint32_t g_dll_bases[] = {0x10000000u, 0x09000000u, 0};

#else
/* LEGO Batman / USM (kept so the shared runtime still builds for the
 * reference target). */
#define g_fn_table_bink g_fn_table_d3dx9
#define g_fn_count_bink g_fn_count_d3dx9
extern const FnEntry g_fn_table_exe[];
extern const unsigned g_fn_count_exe;
extern const FnEntry g_fn_table_d3dx9[];
extern const unsigned g_fn_count_d3dx9;
extern const FnEntry g_fn_table_bink[];
extern const unsigned g_fn_count_bink;

const FnModule g_modules[] = {
    {GAME_EXE, g_fn_table_exe, 0},
    {DLL_D3DX9, g_fn_table_d3dx9, 0},
    {DLL_EXTRA, g_fn_table_bink, 0},
};
const unsigned g_nmodules = sizeof g_modules / sizeof g_modules[0];
unsigned rt_module_count(unsigned m) {
    switch (m) {
    case 0: return g_fn_count_exe;
    case 1: return g_fn_count_d3dx9;
    case 2: return g_fn_count_bink;
    default: return 0;
    }
}
const char *const g_dll_modules[] = {DLL_D3DX9, DLL_EXTRA, NULL};
const uint32_t g_dll_bases[] = {0x08000000u, 0x09000000u, 0};
#endif
