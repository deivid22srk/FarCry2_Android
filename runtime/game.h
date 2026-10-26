/* Per-game names: this runtime is shared by several static-recompilation
 * ports. Build with -DGAME_FC2 for Far Cry 2 (PC, 2008, GOG/retail 1.0.0.1).
 *
 * Verified module set (see docs/RECON.md):
 *   farcry2.exe  - 28,296 bytes, tiny launcher stub at 0x00400000; registers
 *                  game callbacks and calls Dunia.dll!RunGame.
 *   Dunia.dll    - 19,412,104 bytes, the whole engine at 0x10000000 (14.1 MB
 *                  of code, 404 exports).
 *   binkw32.dll  - 171,520 bytes, Bink 1.9a video/audio. Preferred base
 *                  0x18000000 collides with the guest TEB, so it is rebased
 *                  to 0x09000000 (tools/regen_gen.sh and runtime/modules.c).
 *   d3dx9_38.dll - Microsoft D3DX (June 2008), 15 imports. Not shipped with
 *                  the game; the user copies it next to the exe or the port
 *                  uses the host implementations in runtime/d3dx9_host.cpp.
 *
 * NOT part of the port: FC2.dll. It is SecuROM "Activate & Play"
 * (SONY DADC), version 1.0.1.10; nothing imports it and the GOG build does
 * not load it. It is never read or recompiled.
 */
#pragma once
#if defined(GAME_FC2)
#define GAME_EXE       "farcry2.exe"
#define GAME_EXE_DIR   "bin"
#define DLL_D3DX9      "d3dx9_38.dll"
#define DLL_EXTRA      "binkw32.dll"
#define GAME_WIN_DIR   "C:\\FarCry2"
#define GAME_WIN_PREFIX "\\farcry2\\"
#define GAME_TITLE     "Far Cry 2"
#define GAME_PKG_DIR   "com.deivid22srk.fc2"
#define GAME_FC2 1
#elif defined(GAME_USM)
#define GAME_EXE       "USM.exe"
#define GAME_EXE_DIR   ""
#define DLL_D3DX9      "binkw32.dll"
#define DLL_EXTRA      ""
#define GAME_WIN_DIR   "C:\\USM"
#define GAME_WIN_PREFIX "\\usm\\"
#define GAME_TITLE     "Ultimate Spider-Man"
#define GAME_PKG_DIR   "com.deivid22srk.usm"
#define GAME_USM 1
#else
#define GAME_EXE       "LEGOBatman.exe"
#define GAME_EXE_DIR   ""
#define DLL_D3DX9      "d3dx9_35.dll"
#define DLL_EXTRA      "binkw32.dll"
#define GAME_WIN_DIR   "C:\\LEGOBatman"
#define GAME_WIN_PREFIX "\\legobatman\\"
#define GAME_TITLE     "LEGO Batman"
#define GAME_PKG_DIR   "com.tj.legobatman"
#define GAME_BATMAN 1
#endif
