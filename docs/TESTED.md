# What was actually tested

This is the honest record. "Tested" means a command was run and its output
observed. "Not tested" means exactly that — most of the port is in that column.

Local machine for everything below: Ubuntu 24.04 dev container, x86_64,
4 cores / 16 GB, clang 18.1.3, Python 3.12, capstone 5.0.7, pefile 2024.8.26.
The APK is only ever built by GitHub Actions (NDK 27.2.12479018), never here.

The host runs use the same runtime and the same recompiled code as the APK:
`desktop/build.sh` produces `build/lb`, and `desktop/run_headless.sh` runs it
under Xvfb with Mesa lavapipe (no GPU) and DXVK's native `d3d9`.

## Tested and working

| What | How | Result |
|---|---|---|
| PE facts for `farcry2.exe`, `Dunia.dll`, `binkw32.dll`, `systemdetection.dll`, `FC2.dll` | `pefile` walk of headers, sections, imports, exports, relocations, version resources; `sha1sum` | every number in `docs/RECON.md` §1–§6 |
| DRM classification | imports, section names, entropy, overlay, PDB paths | `FC2.dll` = SecuROM Activate & Play 1.0.1.10; nothing imports it; GOG boot path is DRM-free |
| Boot path of `farcry2.exe` | `capstone` 32-bit disassembly of `.text` | entry `0x4014ec` → cookie init `0x401840` → CRT startup `0x40122b` → `0x4011b0`; 15 `AddFunctionCB` registrations (12 distinct stubs, names in `.rdata`); `Dunia!RunGame` at `0x4011c5` (`docs/RECON.md` §3) |
| D3DX9 / D3D9 / DInput / DSound / XInput / Bink imports | import-table extraction | 15 D3DX9 entries, `Direct3DCreate9`, `DirectInput8Create`, DSOUND `#11`/`#2`, `EAX.DLL #6`, Bink 1.9a |
| Function discovery, `Dunia.dll` (14.1 MB of code) | `python3 tools/discover.py .../Dunia.dll analysis/functions_dunia.json 10000000` | 82,403 functions, 857 jump tables, 1,512 unresolved indirect jumps, 6,423 bad data roots, 95.5 % coverage, 724 s wall, 2,075 MB peak RSS |
| Function discovery, `binkw32.dll` | same | 447 functions, 14 jump tables, 42 indirect jumps |
| Function discovery, `farcry2.exe` | same | 52 functions |
| Function discovery, `systemdetection.dll` | same, rebased `12000000` | 1,299 functions, 46 jump tables, 11 indirect jumps, 368 bad data roots, 173,534 of 179,264 bytes covered (**96.8 %**) |
| Lifting, `Dunia.dll` | `LIFT_FILES=384 python3 tools/lift.py ...` | 82,149 lifted, 254 rejected |
| Lifting, `binkw32.dll` | `LIFT_FILES=24 ...` | 442 lifted, 5 rejected |
| Lifting, `farcry2.exe` | `LIFT_FILES=256 ...` | 52 lifted, 0 rejected, 46 of 256 files non-empty |
| Lifting, `systemdetection.dll` | `LIFT_FILES=24 ...` | 1,297 lifted, 2 rejected |
| Lifted C is valid C | `clang -DGAME_FC2 -std=gnu11 -fsyntax-only -fno-strict-aliasing -fwrapv -fsigned-char -Iruntime -Igen -w` on **all 692** `gen/*.c` (current set: 384 dunia + 256 fc2 + 24 bink + 24 sdet + 4 tables) | 0 failures (log contained only `ALLDONE`) |
| The lifted code references only symbols the runtime provides | `grep -rhoE '\brt_[A-Za-z0-9_]+' gen/ | sort -u`, each checked against `runtime/` | 9 symbols (`rt_call`, `rt_cpuid`, `rt_int3`, `rt_movsd`, `rt_rdtsc`, `rt_setjmp3`, `rt_setjmp_slot`, `rt_unimpl`, plus the `rt_table.h` include) — all defined; the temporary `rt_dbg` debug helper is gone, which would otherwise have failed the link |
| Lifted C compiles at the CI flags | `clang -DGAME_FC2 -std=gnu11 -O1 -g0 -fno-strict-aliasing -fwrapv -fsigned-char -w -fPIC -c` on 8 files including the largest (`gen/dunia_329.c`) | all succeed; largest file 34 s, medium 25 s, small 0–5 s on this 4-core x86 host |
| Runtime C compiles for `GAME_FC2` | `clang -DGAME_FC2 -std=gnu11 -O1 -fsyntax-only` with the vendored DX VK/SDL include dirs on all `runtime/*.c` | 0 errors |
| Runtime C++ compiles for `GAME_FC2` | `clang++ -DGAME_FC2 -DDXVK_NATIVE -std=c++17 -fsyntax-only` on all `runtime/*.cpp` | 0 errors |
| The lifted table matches the runtime's expectations | `gen/*_table.c` + `funcs_*.h` consumed by `runtime/modules.c`; the app-only stubs in `ci/gen_stub/` follow the same shape | compiles; only real runs will exercise them |
| Every import of all four modules resolves | `python3 tools/import_coverage.py "…/Far Cry 2"` (reads the PE import tables and the runtime shim tables, including the CRT aliasing and ordinal imports) | `farcry2.exe` 44 = 44; `Dunia.dll` 602 = 602; `binkw32.dll` 91 = 91; `systemdetection.dll` 140 = 140 — 0 loud traps, 0 abort stubs |
| The private-profile INI reader/writer | extracted `ini_get`/`ini_write` into a host test; create section, replace a value, insert into an existing section, second section, cross-section reads | all pass; file content as expected |
| App-only APK | GitHub Actions run [40339818075](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40339818075), `.github/workflows/build.yml` with `gen/` absent (stub tables) | **success**; artifact `FarCry2-apk`, 2,114,082 bytes |
| `gen.tar.zst` round trip | built with `tar --use-compress-program='zstd -T4 -3' -cf gen.tar.zst gen`, published as a release asset of the private `deivid22srk/FarCry2_gen`, then downloaded and extracted exactly as `build.yml` does (`gh release download gen-v1 --pattern gen.tar.zst` + `tar --use-compress-program=unzstd -xf ... -C .`) | 25,370,685 bytes, sha256 `a767d069…ca21`, 692 `.c` + 4 `funcs_*.h`, 288 MB extracted, `sha256sum -c` OK |
| Full APK (with the real `gen/`) | GitHub Actions run [40377549175](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40377549175) (`GEN_REPO`/`GEN_TAG`/`GEN_TOKEN` set) | **success** in 11m49s; step log `recompiled files: 692`, `gen/ present: building the full APK (692 C files)`; `FarCry2.apk` 45,540,586 bytes, sha256 `39b15558…24ed` |
| The full APK contains the recompiled code and only `arm64-v8a` | downloaded the private release asset, `unzip -l`, `file`, `strings` | `lib/arm64-v8a/libmain.so` 109,747,328 bytes, ELF aarch64 (stripped); module names `Dunia.dll`, `binkw32.dll`, `systemdetection.dll` present; **no game data** in the APK |
| The APK is published privately, not here | run log + the public repo's artifact list | release `apk-5-20261026-1836` in `deivid22srk/FarCry2_gen`; the public repo has no APK artifact for this run |
| Host boot of the game | `desktop/build.sh` then `desktop/run_headless.sh "<game dir>"` (Xvfb + Mesa lavapipe + DXVK's native d3d9) on the user's own GOG copy | executes `farcry2.exe` → `Dunia.dll!RunGame`, loads `Dunia.dll`/`binkw32.dll`/`systemdetection.dll`, `CreateWindowExA 'Dunia'`, `WM_ACTIVATEAPP`, DirectSound8, D3D9 device + swap chain (DXVK), many `CreateTexture`, `Reset 1024x576`, then the `PeekMessageA`/`QueryPerformanceCounter` loop |
| Host run, where it stops | same, plus `gdb` (`break f_10425CB0` / `break f_10A86DD0`, reading the guest registers and `MEM` directly) | with the three fixes below, the run got **past** the phone's crash point: `f_10425CB0` is entered **202 times with 0 `NULL` table entries**. It then stopped later, in the same UI init as before: the engine activates `MAINMENU_INITIAL_RATING` and reads `[screen+0x14]` (the UI-element root) as `NULL`; `f_10CA2220` → `f_10A86DD0` faults at guest `0x58`. **That stop is now gone too** — see the four fixes below; the run reached 1,308 presented frames and no longer faults in guest code |
| The engine language is read with `GetPrivateProfileStringW` | gdb trace of `GetPrivateProfileStringW` / `f_10004690` / `f_104C0ED0` / `f_1029B6E0` / `f_10547090` (`analysis/trace_v7.py`), plus `capstone` + a `.text` scan for `push imm32` literals | Dunia `f_10004690` calls `GetPrivateProfileStringW(L"FC2_INIT", L"language", L"english", buf, 256, <profile dir>\FC2Init.ini)` at `0x10004818`–`0x10004827` (literals `0x10d839bc`/`0x10d839d0`/`0x10d839e4`/`0x10d839f4`, wide). The runtime's shim was `SHIM(GetPrivateProfileStringW) { RET(0, 6); }` — it returned 0 and wrote nothing, so the engine's language came back empty and `f_1029B6E0` copied the *stale inline bytes* of the result string as the name: the trace shows `LANGMGR name='\x10'`, and every one of the 54 localized path builds (`f_10547090`) used that one-byte name |
| `GetPrivateProfileStringW` implemented | new shim in `runtime/fc2_shims.c` (wide→ANSI via `u16_at`, `ini_get` on `rt_host_path(file)`, the shipped `bin/FC2Init.ini` as the fallback for the profile-dir path the engine asks for, `lpDefault` when neither has the key — which is what Windows does) | the first call now logs `fc2: GetPrivateProfileStringW(app='FC2_INIT' key='language' def='english' file='C:\FarCry2\FC2Init.ini')` and the language resolves, so the localized UI data at `ui\localized\pc\...` loads; the `f_10A86DD0(NULL)` fault at guest `0x58` **no longer occurs** |
| `D3DXGetImageInfoFromFileInMemory` argument order | gdb (`analysis/trace_dds.py`) stopped at the fault with the arguments and the frame's locals | the shim read `ARG(0)` as `D3DXIMAGE_INFO*`, `ARG(1)` as the data and `ARG(2)` as the size. The real signature is `(LPCVOID pSrcData, UINT SrcDataSize, D3DXIMAGE_INFO *pImageInfo)`. Observed at the fault: `info=313022f0 data=00001080 size=541063948` — with the correct order that is `pSrcData=0x313022f0` (heap), `SrcDataSize=0x1080` (4224), `pImageInfo=0x203FF98C` (stack). Reading it as data/size sent `0x1080` in as the buffer, inside the `PROT_NONE` guard at the bottom of `MEM`, and the first UI texture load took `SIGSEGV`. Fixed; the load now parses the DDS header and returns the real dimensions |
| Imports of a module that has not been mapped yet | gdb backtrace on the abort + `runtime/main.c` | `load_pe()` bound each module's imports as it mapped it. `Dunia.dll` is mapped before `binkw32.dll` but imports it, so those imports bound to the abort stub and the first movie died with `FATAL: unimplemented import binkw32!_BinkSetMemory@8 (called from 10419815)`. Binding was split into `bind_imports(base)` and `main()` now runs a second pass over every module once all are mapped (`rt_import()` checks the registered export table before its thunk cache, so the second pass takes effect). The abort no longer happens |
| `SetWindowPos` / `MoveWindow` were no-ops | gdb backtrace on `IDirect3DDevice9::Reset` (`analysis/trace_reset.py`) + the run counters | every `Reset` came from the engine's display-mode switch (`f_104147F0`, reached `f_10330A00` ← `f_1033EE60` ← `f_1031F9B0` ← `f_104C0A10`), which sizes its window (`SetWindowPos(..., SWP_NOMOVE\|SWP_NOZORDER\|SWP_NOOWNERZORDER)` at `0x104148d4`, `MoveWindow`) and then checks the client area through `GetClientRect`. Both shims just returned 1 without doing anything, so the reported client never changed and the switch never converged: **1,430 `Reset` calls, all byte-identical `1024x576`**, each one rebuilding DXVK's swap chain. `platform_resize_window()` now resizes the SDL window and updates the reported client space (FC2 only; the other ports are untouched). After the fix the run shows **0 resets** and `platform: window resized to 1152x648` |
| Host run, current stop | `desktop/run_headless.sh "<game dir>"`, `LB_LOG=1`, full log kept | the run reaches **1,308 presented frames** (`watchdog 30s: frames 1308`, i.e. ~43 fps), with `0` `Reset` calls and no guest fault, and then `SIGSEGV`s **inside the host graphics stack**: `DXVK 1.9.2 DxvkDescriptorPool::alloc` → `libvulkan_lvp.so` `vkAllocateDescriptorSets`, on DXVK's command-stream thread (`DxvkCsThread::threadFunc` → `DxvkContext::draw`). No frame in the backtrace is guest code. Mesa lavapipe here is Vulkan 1.4.318 (LLVM 20.1.2) against DXVK 1.9.2 (2021); this is not reproducible on a phone GPU and is **not** claimed to be a guest bug. Whether the game reaches the main menu is therefore still **unverified** |
| Guest wide strings are UTF-16, not host `wchar_t` | `runtime/u16.h` (`u16_at`/`u16_put`/`u16_len`/`u16_cmp_n`) replaces every `wcs*`/`wchar_t` use on guest data in `runtime/fc2_shims.c`; host `wchar_t` is 4 bytes on Linux/Android, so `wcslen` counted half the units and `swprintf`/`towlower` wrote 4 bytes per element into 2-byte buffers | fixed; the host run no longer creates the truncated-name side directories (`D`, `Docume`, ...) the half-width bug produced |
| `c:\data_win32\...` maps to the game's `Data_Win32` | `runtime/kernel32.c` `rt_host_path` (`#if defined(GAME_FC2)`) | fixed; the host run now opens the shipped `Data_Win32` data (`common.*`, `patch.*`, `sound`, `shaders`, `shadersobj`, ...) instead of looking under the save directory |
| D3DX9 entry points are `__stdcall` | all 15 shims in `runtime/d3dx9_host.cpp` now return with `RET(v, nargs)` (callee pops the arguments) instead of `RETC` | fixed; the D3DX calls no longer leave the guest stack unbalanced |
| The full APK on a real phone | the user installed the **`apk-6-20261026-2051`** private release (run [40398348378](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40398348378)) on their Motorola Edge 30 Fusion (Android 14, Adreno 660) and ran it; the `logcat` they pasted names `libmain.so` BuildId `76c65daf9e007948f6c56ad6e9e260c72e6a702f`, which is **exactly** the `lib/arm64-v8a/libmain.so` inside `apk-6` (apk-5's is `263e6cfaa549d3022ab2cde021d17c9d3b1eb4a7`), so the build is identified beyond doubt | the launcher opened and accepted the game folder, and `farcry2.exe` → `Dunia!RunGame` booted on the phone; then `SIGSEGV` / `SEGV_ACCERR` at fault address `0x6dade00088`, i.e. `MEM` base (`0x6dade00000`) + `0x88`. Disassembling `apk-6`'s own `libmain.so` shows the faulting instruction at `f_10425CB0+108` is `ldar w16,[x16]` with `x16 = MEM + (w15 + 0x88)`, `w15 = 0`: a 256-entry lookup table at `[this+0x14]` had a `NULL` slot. **That build predates the three fixes above** (`runtime/u16.h`, the `data_win32` mapping, the D3DX9 `__stdcall` change), and it never reads `Data_Win32` at all |
| A build with these three fixes is published for testing | run [40403855986](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40403855986), `gh release list` | private release **`apk-7-20261026-2135`** in `deivid22srk/FarCry2_gen` (sha256 `0a9b719a611a10d9d186fe8acc372c1cc42acb6e2eed5724a377a46b21f9af9c`). **It has not been run on a phone yet** — the host harness is the only execution evidence so far |
| Full APK with the four fixes | GitHub Actions run [40495170415](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40495170415), `build.yml` on `70e2d2a` | **success** in 8m10s; `recompiled files: 692`, `gen/ present: building the full APK (692 C files)`; `FarCry2.apk` 45,547,078 bytes, sha256 `38a8222f8e73c7d6947a55468d4012e5b50b6dee2920e71a42bb2b6976c7f9d8`, published to the private release **`apk-8-20261027-1126`** in `deivid22srk/FarCry2_gen`. **Not run on a phone yet** |
| adrenotools' generated `bcenabler_patch.h` | assembled `src/bcenabler_patch.s` with `clang --target=aarch64-linux-gnu` + `llvm-objcopy` and compared with upstream | 264 bytes, byte-identical |

Note the `-mtune=cortex-a73` in the CI compile options is not valid for the
local x86 clang, so the local sample pass omits it; the NDK clang used in CI
accepts it.

## CI history (all from the public repo)

| Run | Result | What it found |
|---|---|---|
| [40337075327](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40337075327) | failure | `Fc2LauncherActivity.java`: `driverList` used without a field declaration |
| [40337581868](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40337581868) | failure | `adrenotools/src/bcenabler.cpp:10`: `gen/bcenabler_patch.h` not found |
| [40338447334](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40338447334) | **success** | app-only APK built and uploaded (`FarCry2-apk`, 2,106,960 bytes) |
| [40339818075](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40339818075) | **success** | app-only APK built after the remaining-import shims (`FarCry2-apk`, 2,114,082 bytes) |
| [40377549175](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40377549175) | **success** | first **full** APK: downloaded `gen.tar.zst` (692 C files) from the private `FarCry2_gen` release, built `FarCry2.apk` (45,540,586 bytes) and published it as a private release there |
| [40398348378](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40398348378) | **success** | full APK after the wide-string fixes; `FarCry2.apk` 45,545,214 bytes, published as the private release `apk-6-20261026-2051` — **this is the build the user tested on the phone** (the log's `libmain.so` BuildId matches its `lib/arm64-v8a/libmain.so` exactly) |
| [40403855986](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40403855986) | **success** | full APK carrying the three fixes for the phone crash (`runtime/u16.h`, the `data_win32` mapping, the D3DX9 `__stdcall` change); `recompiled files: 692`, `FarCry2.apk` 45,544,734 bytes, sha256 `0a9b719a...af9c`, published as the private release `apk-7-20261026-2135` |

## Phone run of 2026-10-27 (user's `port/log.txt` + screenshots, apk-7)

The user's own log and three screenshots show the game past the UI init that
`apk-6` died in: the Ubisoft logos, the "Press any key" title screen, and — in
the later screenshot — the main menu. That is the furthest the port has ever
run. The log and the screenshots also pin down three defects, all addressed in
`runtime/` the same day:

| Evidence | Reading | Change |
|---|---|---|
| The title screen's game area is a solid green box (1370x648 inside 2400x1080), and the later screenshot shows the menu in the same box | The game picked the 648-height mode the bridge's own `bridge_scaled_mode` list offers (648 at the window's aspect = 1370x648), sized its window for it and created the device with it. `pp_fix_size` then overrode the back buffer to the window's 2285x1080 — but the engine went on laying out and clearing for 1370x648, so the frame sat boxed in the corner of a bigger back buffer. (The `CreateTexture ... -> 00000000` lines in the same log are **successes**: `00000000` is `D3D_OK`, the log prints the HRESULT.) | `mode_count()` reports a single mode — the window's own size — for `GAME_FC2`, so the game's pick can only land on the native resolution; and `pp_fix_size` no longer overrides the game's request for FC2: dxvk scales whatever the engine asks for to the window (plain D3D9 windowed Present), which fills the screen instead of boxing it |
| `window deactivated` / `window activated` every ~300 frames; the run ends with a `window deactivated` that is never followed by an activation, `watchdog 30s: frames 1506`, thread 0 idling in `USER32!PeekMessageA` with 10,392,867 calls | The phone delivers `SDL_WINDOWEVENT_FOCUS_LOST/GAINED` in bursts while the app is in the foreground. Each FOCUS_LOST posts `WM_ACTIVATEAPP(0)`, the engine stops presenting and idles in its message pump, and when one burst ends on a LOST it never resumes | On `__ANDROID__`, focus events no longer touch the game's activation; only `SDL_APP_WILLENTERBACKGROUND`/`DIDENTERFOREGROUND` (a real backgrounding) pause/resume it |
| Intro shows only the "Ubisoft presents …" text, title background is green; the log never mentions Bink | The title/intro picture is a Bink video; a surface whose YUV planes are never filled samples as green. Nothing logged what the movie system does | `runtime/binkdiag.c` (new) wraps all 16 `_Bink*@N` entry points with rate-limited logging (`BinkOpen` logs the movie path and the returned struct's w/h/frames); `rt_register_data` overrides in place so the wrappers replace the lifted exports for the second import pass; `CreateFileA` logs `.bik` opens at log level 1 |

What the next phone log should therefore show: no more `window deactivated`
churn mid-title, a back buffer equal to the window's size (or, if the engine
sticks to a profile-stored size, a stretched full-screen frame instead of a
boxed one), and — for the videos — either working `BinkOpen`/`BinkDoFrame`
lines or the precise point where the movie system gives up.

## Implemented but not tested

| What | Why not |
|---|---|
| Most of the runtime under `GAME_FC2` | the host run boots the game, but it stops at the first screen, so anything past the UI init is unexercised |
| Host D3DX9 (`runtime/d3dx9_host.cpp`, 15 entry points) | texture creation is observed, but no entry point's *output* has been compared against the original |
| D3D9 bridge over DXVK, DirectSound over SDL2 | exercised on the host and, on the phone, up to the same crash; what the device actually rendered was never captured or compared |
| DInput/XInput, touch overlay | not exercised on the host or on the phone |
| Some `fc2_shims.c` shims (`VirtualQuery`, the file-search W calls, `wsprintfA`, `GetKeyState`, …) | compile; the host run is not known to have called them |
| Android launcher options UI (resolution, extras) | the install and the folder validation ran on the phone (see above); the options screen itself was not exercised |
| Any frame on the phone past the first screen | the device run dies in the same UI init as the host, so the menus and everything after are still unexercised |
| Android-only paths in `runtime/kernel32.c` (`/proc` affinity, `singlecore.txt`) | `#ifdef __ANDROID__` code the host build never compiles |

## Not implemented / blocked

| What | Reason |
|---|---|
| **The screen's UI-element tree is never built** | the current blocker for the main-menu target. `f_10CA2220` (slot 2 of the screen class's vtable) reads `[screen+0x10]` and `[screen+0x14]` and calls `f_10A86DD0(screen+0x14)`, which dereferences `[this+0x58]`; both fields are `0`. The whole constructor chain of the screen (`CFCXRatingPage`, `MAINMENU_INITIAL_RATING`, vtable `0x10E28098`) was disassembled from the real PE and compared byte-for-byte with the lifted C — `0x108425B0` → `0x10841DB0` → `0x108B71B0` → `0x10CA2100` → `0x101888B0` → `0x10108040`; the lifted C matches, and the *only* writer of `+0x10`/`+0x14` anywhere in the chain is the root base constructor `0x10108040`, which **zeroes** them. A byte-level scan of the whole `Dunia` `.text` plus a gdb watchpoint confirm no lifted code ever sets them to a non-`NULL` value, so the missing write is either in one of the rejected/unresolved functions or reached only through a code path the port does not yet take. Root cause not identified yet |
| **SEH and C++ exceptions** | `_except_handler3`, `_except_handler4_common`, `__CxxFrameHandler3`, `_CxxThrowException` are trapped with an abort. Any structured exception or `throw` stops the game. |
| **MSVCP80 entry points outside the implemented subset** | the iostream/stringstream subset the engine uses is implemented; anything else still aborts with its mangled name. |
| **Network** | Winsock/WinHTTP/WinINet/Secur32/Crypt32 are offline stubs. |
| **DXT mip filtering** | `D3DXFilterTexture` creates the chain without filtering (logged). |
| **`D3DXSaveTextureToFileInMemory`** | returns `E_NOTIMPL`. |
| **1,512 unresolved indirect jumps in Dunia** | calls through them abort at the target address; each one is a possible gap. Same for `binkw32.dll` (42) and `systemdetection.dll` (11). |
| **Eight documented fallbacks** | `UpdateLayeredWindow`, `WSAStartup`, `socket`, `gethostbyname`, `AcquireCredentialsHandleA`, `CryptGenRandom`, `EnumDisplayDevicesA`, `EAXDirectSoundCreate8` log once and return a fixed failure value. |
| **`systemdetection.dll`'s WMI path** | `CLSID_WbemLocator` is not provided; the DxDiag/D3D9 path is what answers. |
| **Bink imports from Dunia** | `binkw32.dll` loads after its importer, so Dunia's IAT slots for it are bound to abort stubs; no `_Bink*` call has been hit yet, i.e. video playback has not been reached. |
| **Reaching the main menu anywhere** | neither the host nor the phone run gets past the first screen's UI init, so the minimum acceptance target is still unmet. |
| **Building the APK locally** | deliberately not done: the project rule is that the APK comes only from Actions. |
| **`d3dx9_38.dll`, `FC2.dll` redistribution** | neither is shipped or recompiled (`FC2.dll` is DRM; `d3dx9_38` is Microsoft's). |

## The device actually reaches the main menu (user report, 2026-10-27)

The user's third screenshot (`Screenshot_20261027-100011.png`, `2400x1080`) shows the
**main menu rendered in full** — the notebook, "FAR CRY 2", Story Mode / Multiplayer
/ Options / Credits / Additional Content / Quit, and "Accept". That screenshot is from
a build newer than `apk-8` and is the first evidence that the minimum acceptance target
is met on a phone; `docs/` had not been updated to say so. The same run, and the
`port/log.txt` written at 13:01, show the frame counter reaching `frame 3000` with the
watchdog recovering after a stall, so the "Press any key" freeze recorded from the
`apk-6` run (the `pastebin` log, frozen at `frames 1506`) is no longer the current stop.

Two defects visible in that log and in `Screenshot_20261027-083357.png` (the green
"Press any key" screen) are fixed in this branch. Both are **written and syntax-checked
locally (`clang++ -fsyntax-only`), not yet run on a phone** — the APK comes from Actions
and no device run has used this branch yet.

| What | Evidence | Change |
|---|---|---|
| D3DX texture creation bypasses BC emulation | `port/log.txt` lines 510–605: `DxvkError: D3D9: Cannot create texture ... D3D9Format::DXT1 ... Layers: 6` followed by `D3DX: CreateCubeTexture -> 8876017c` (D3DXERR_INVALIDDATA) five times. Adreno 660 reports `textureCompressionBC: 0`, so the raw `dev->CreateCubeTexture` in `runtime/d3dx9_host.cpp` fails and the guest is handed `NULL`. The bridge (`d3d9_bcemu.cpp`) already emulates DXT as A8R8G8B8 for `B_IDirect3DDevice9_Create*Texture`, but the D3DX host shims call the device directly and never went through it | `d3d9_bcemu.cpp` exposes `bc_host_format()` / `bc_register_texture()`; `create_tex()` and the cube-map shim create as A8R8G8B8, decode the DDS into it and register the guest DXT format so `GetLevelDesc`/`GetLockRect` still report DXT. Inactive (a no-op) when BC is supported |
| The frame is a 1370x648 box in the corner of a 2285x1080 back buffer | `port/log.txt` line 261: `back buffer 1370x648 is not the window's aspect: using 2285x1080 (mode 0)`; the screenshots show the menu occupying the top-left ~1370x648 with the rest black. `pp_fix_size` required an *exact* `bw*wh == bh*ww`, but 1370x648 against the window's 2285x1080 differs by 0.07 % (1,080 out of 1,479,600) — pure integer rounding of the same aspect. The override then left the game rendering its own 1370x648 viewport inside the bigger back buffer | `runtime/d3d9_bridge.cpp` accepts a back buffer within 1 % of the window's aspect, so the near-window size is kept and dxvk scales it the fraction of a pixel it needs; a genuinely different aspect (e.g. 4:3) is still overridden as before |

Not addressed here: the Bink videos, SEH/C++ exceptions, and the `watchdog ... (no new
frames)` stall (which in the 13:01 log recovers — `frame 2400`/`2700`/`3000` follow it).

## The nested-lock regression in the D3DX paths (apk-9 log, 2026-10-27)

`port/3log.txt` (13:58, the run behind `apk-9-20261027-1352`) confirms the
back-buffer fix — `touch: back buffer 1370x648, window 2285x1080, aspect 2.1157`,
`Presenter: Buffer size: 2285x1080` — and reaches `frame 3300`. It also shows
`D3DX: LockRect failed` **exactly 234 times**: 6 faces x (8+8+8+8+7) mip levels of
the five DXT cube maps, i.e. one failure per subresource the BC emulation had just
started creating.

Cause: every DDS `LockRect` site in `runtime/d3dx9_host.cpp` locked the surface
itself and then, in the non-raw branch, called `blit_argb()`, which locks the same
surface again. `D3D9DeviceEx::LockImage` refuses the nested lock (*"Don't allow
multiple lockings"*, `D3DERR_INVALIDCALL`), so the inner lock failed and the DDS
bytes never reached the texture. Before the BC-emulation change that branch was not
taken for DXT, so the bug was latent; afterwards it silently blanked every emulated
DDS and cube map. The raw-copy and `blit_argb()` cases are now exclusive in both the
2-D and cube paths, and the failure log prints the HRESULT and the rectangle.

`runtime/binkdiag.c` (from the title-render branch) also had a format bug: its
per-entry argument macros expanded to several printf arguments, so `(%s)` consumed
the format string as the value and the actual result and call count were left
unused — the wrapper logged pointers where the result should be. Each entry point
now formats its arguments into a buffer first, so the log prints
`bink: BinkOpen(name="...", flags=...) -> <handle> (call N)` as intended.

Both are **written and syntax-checked only**; the APK still comes from Actions and
no phone run has used them yet.

## The green video blocks and the story-mode black screen (`6log.txt` + the gameplay recording, 2026-10-27)

The run behind the `fix/bink-plane-probe` probe build (the user's `6log.txt` and
the 71 s screen recording) gets **all the way into the story mode**: the title
video plays, the main menu renders in full, "New Story" starts the intro, and
the intro's bar/savanna scenes render. Three defects are visible in it.

### 1. Green macroblocks in every video

The recording shows the Ubisoft logo video playing with 16x16-block-shaped
green patches everywhere nothing moves, and the same green bleeding through
the title screen. Green is what the YUV shader produces for a chroma value of
zero, so the displayed planes contain zeroed blocks — Bink's *inter frames*
wrote only the blocks that changed since the previous frame, and everything
else stayed at whatever the buffer held.

What the log shows, step by step:

* the engine locks its plane textures every frame — the *decode* set with
  `D3DLOCK_READONLY` (`flags 10`, handed to Bink through the
  BINKFRAMEBUFFERS entries) and the *display* set with plain read-write locks;
* the bridge hands every lock a **fresh, zeroed guest shadow**, so each new
  lock erased the previous frame's content from the buffer Bink was about to
  apply deltas onto. On Windows a `D3DPOOL_MANAGED` lock returns the *same*
  sysmem pointer with the content of the last write — the rotating plane sets
  keep their chains, which is what Bink's model requires;
* the decoded frame itself is fine (`bink: thumb` shows real picture data
  every 30 frames), the engine copies it into its display textures itself —
  the unlock census prints `nonzero 1024000` for a display luma plane *before*
  the bridge's old fallback touched it — so the holes in the decode targets
  are what reached the screen.

Two workarounds that had been added earlier (`bink_publish` after every
`BinkDoFrame`, and the unlock-time "publish the current frame's plane" copy in
the bridge) made it worse: each set is a rotating buffer whose content must
survive as the reference for its own next decode, so copying the newest frame
over the *other* set desynchronised every following inter frame — and when the
current set's chroma happened to be empty (decode 2 in the log: Y written,
U/V skipped because nothing changed), the copies spread zeros around.

**Change** (all in `runtime/`):

* `runtime/d3d9_bridge.cpp`: the big single-plane A8 MANAGED textures (the
  Bink planes; 600x600 movies clear the 128 KiB floor) now lock through the
  persistent-guest-copy path — one guest buffer per (texture, level), kept
  across locks, guest copy authoritative, uploaded on unlock exactly like the
  SYSTEMMEM path already did. That reproduces the Windows MANAGED behaviour
  the engine was built against.
* the unlock-time cross-set copy is gone (the census log stays, so the next
  run still shows per-plane content);
* `runtime/binkdiag.c`: `bink_publish()` is removed with it.

### 2. The story mode goes black after the intro

After the intro movies close, the story world load starts (save-game scans,
`downloadcontent`/`worlds` probes, the loading-screen DXT3 texture, the first
compressed vertex formats — types 5/6/7/9/10, the world's packed streams) and
the frame counter keeps advancing, but the screen stays black. Nothing faults;
the render thread eventually just idles.

One concrete defect fits: **`ReadFile` used to return whatever one `read(2)`
gave it.** Over Android's FUSE storage (`/storage/emulated/0`) a single read
is silently capped at the connection's `max_read` (128 KiB there), and it
comes back as *success*. The engine's pack/world loaders issue multi-megabyte
reads and trust the count, so everything past the first 128 KiB of every big
read was garbage — small reads (intro, menus) worked, the world load did not.
`runtime/kernel32.c` now loops each `ReadFile` until the buffer is full, EOF
or a real error, which is what a Windows disk read does; read failures are
logged (rate-limited) instead of silently becoming EOF.

Two log gaps that hid the story-load path are closed as well: `CreateFileA`
failures under `data_win32` are now LOG-level (rate-limited), and
`FindFirstFileW` says when a *file* pattern (worlds.dat, dominos.dat, ...)
matched nothing — the folder-missing case was already visible, the
file-missing case was silent.

### 3. Audio is silent end to end

The recording's audio track is digital silence, and the mixer census says
`0 playing` for the whole run: the engine created 194 DirectSound buffers and
destroyed all 194, with exactly **one** `Play` call in the entire log (the
startup blip). The XAudio2 shim is never reached (no `CoCreateInstance` of
either CLSID). Whatever gates the engine's decision to play — most likely
something in the EAX-off fallback path or the 3D-buffer LOCDEFER handling —
needs its own investigation; nothing was changed here in this round.

### What the next phone log should show

* video plane unlocks whose `nonzero` count stays high for *every* plane
  (no more `nonzero 0` chroma), no `filled` column (the copy is gone);
* green gone from the title screen and the intro videos;
* `ReadFile ... failed` lines if any data read actually fails;
* `CreateFileA failed ... data_win32` / `FindFirstFileW ... no match` lines
  naming exactly which story-load file is missing, if that is the stall.

## Round 5 (2026-10-28): the adrenotools integration, done properly

The launcher had a GPU-driver import and selection screen, but "what's in the
app doesn't work" per the user: selecting an imported Turnip pack either did
nothing visible (game kept running on the Qualcomm driver) or failed somewhere
invisible. An audit of the whole chain against the official example
(darksylinc/AdrenoToolsTest) and current upstream bylaws/libadrenotools found
the vendored library, the hook libraries and the build packaging were all
correct and current — the APK ships `libmain_hook.so`, `libhook_impl.so`,
`libfile_redirect_hook.so` and `libgsl_alloc_hook.so`, links adrenotools into
`libmain.so`, and extracts native libs (`useLegacyPackaging` +
`extractNativeLibs`) so `nativeLibraryDir` really holds the hooks. What was
missing was every piece that decides *whether it worked*:

* **No tmpLibDir.** `adrenotools_open_libvulkan` was called with `nullptr`
  where the official example passes a writable folder; below API 29 memfd may
  not exist and the port's minSdk is 28. It now gets `<save>/driver-tmp`.
* **No RTLD_LOCAL** on the loader open (the example uses `RTLD_NOW |
  RTLD_LOCAL`); harmless but now aligned.
* **Silent failures.** Every failure inside the hook goes to logcat
  (`hook_impl` tag), so log.txt only ever said "keeping the system driver"
  with no reason. `usm_driver_open` now mirrors every decision to logcat
  (tag `fc2-driver`) and log.txt, and logs `dlerror()` when the open fails.
* **No verification.** A driver for the wrong architecture, or one whose ICD
  never comes up, used to be accepted and then produce a black screen or a
  silent fallback. `usm_driver_open` now (1) checks the user's `libvulkan.so`
  is really a 64-bit arm64 ELF before opening it, and (2) *probes* the loaded
  loader with a throwaway `VkInstance`: it enumerates physical devices through
  the custom loader and only points DXVK and SDL at it when at least one
  device answers. The verdict is logged as `driver: ACTIVE <device name>
  (vulkan x.y.z, driver x.y.z)` — the launcher reads exactly this line.
* **Fragile SDL wiring.** `SDL_Vulkan_SetExternalLoader` dropped the request
  silently if the video device did not exist yet. It now remembers the
  request and applies it at the next `Vulkan_LoadLibrary` as well, so the
  custom loader can never be lost to call order.
* **No feedback in the launcher.** The DRIVER DE GPU card now shows "Última
  execução: …" parsed from `port/log.txt` (green for ACTIVE/system, amber for
  any fallback reason), and the import rejects packs whose `libvulkan.so` is
  not an arm64 ELF ("não é arm64 (aarch64)" instead of a silent fallback).

What the next phone log should show with a driver selected: `driver: ACTIVE
...` naming the imported ICD (Turnip on a working pack), then `platform: DXVK
and SDL now resolve Vulkan through the custom GPU driver`, and DXVK's device
banner reporting the custom driver instead of `Adreno (TM) 660 / 512.530.0`.
If the pack is unusable, log.txt now says exactly which step refused it.

## Round 6 (2026-10-28): driver import hardened and self-diagnosing

User report: "Erro ao importa driver" with a logcat capture. The capture was
PID-filtered (only the launcher's own process) and the importer logged
nothing — it validated by Toast and the reason died with the toast. What the
log did show: the SAF picker opened ~4s after launch and a toast appeared at
+10s, i.e. our own validation rejecting the file, with the cause invisible.

The importer in `Fc2LauncherActivity` was rewritten:

* **MIME filter removed** from the SAF picker (`EXTRA_MIME_TYPES` zips only).
  Managers that tag downloads `application/octet-stream` made driver files
  unselectable and there was nothing to diagnose.
* **One copy, magic-byte sniffing.** The picked file is copied once to cache
  (exact size, first 8 bytes), then routed by content: `PK` → zip,
  `7z¼¯'` → "o arquivo é um .7z — o app só aceita .zip", `Rar!` → same for
  .rar, ELF → import the bare .so, anything else → "formato desconhecido".
  A truncated/corrupt zip no longer prints a raw `ZipException` dump.
* **Pack variants accepted.** Any `libvulkan*.so` at any depth (renamed to
  `libvulkan.so` on install), one nested-zip level unwrapped when the outer
  pack has no lib, and a last-resort fallback to the biggest arm64 `.so`
  over 2 MB for packs with odd names. Multi-ABI packs pick the arm64 lib.
* **ELF reasons** name the actual problem: "é ELF 32-bit — o driver precisa
  ser arm64-v8a (64-bit)", "não é arm64/aarch64 (é x86-64)", "é muito
  pequeno (N KB) para ser um driver Vulkan".
* **Failures are visible without adb.** Every step is logged to logcat (tag
  `fc2-driver`, same tag the native runtime uses — the user's captures are
  PID-filtered, so these lines now actually appear) and appended to
  `Android/data/com.deivid22srk.fc2/files/driver-import.log` (openable from
  any file manager). A failure also stays written on the DRIVER card ("Falha
  ao importar: <motivo> / Detalhes: <path>") instead of flashing by.
* **Post-install verification**: the installed `drivers/<name>/libvulkan.so`
  must exist before the pack is announced and auto-selected; a concurrent
  second import is refused ("Uma importação já está em andamento") because
  both would race in the same cache dir.
