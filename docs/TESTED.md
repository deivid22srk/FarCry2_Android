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
| Host run, where it stops | same, plus `gdb` (`break f_10425CB0` / `break f_10A86DD0`, reading the guest registers and `MEM` directly) | with the three fixes below, the run gets **past** the phone's crash point: `f_10425CB0` is entered **202 times with 0 `NULL` table entries**. It then stops later, in the same UI init as before: the engine activates `MAINMENU_INITIAL_RATING` and reads `[screen+0x14]` (the UI-element root) as `NULL`; `f_10CA2220` → `f_10A86DD0` faults at guest `0x58` |
| Guest wide strings are UTF-16, not host `wchar_t` | `runtime/u16.h` (`u16_at`/`u16_put`/`u16_len`/`u16_cmp_n`) replaces every `wcs*`/`wchar_t` use on guest data in `runtime/fc2_shims.c`; host `wchar_t` is 4 bytes on Linux/Android, so `wcslen` counted half the units and `swprintf`/`towlower` wrote 4 bytes per element into 2-byte buffers | fixed; the host run no longer creates the truncated-name side directories (`D`, `Docume`, ...) the half-width bug produced |
| `c:\data_win32\...` maps to the game's `Data_Win32` | `runtime/kernel32.c` `rt_host_path` (`#if defined(GAME_FC2)`) | fixed; the host run now opens the shipped `Data_Win32` data (`common.*`, `patch.*`, `sound`, `shaders`, `shadersobj`, ...) instead of looking under the save directory |
| D3DX9 entry points are `__stdcall` | all 15 shims in `runtime/d3dx9_host.cpp` now return with `RET(v, nargs)` (callee pops the arguments) instead of `RETC` | fixed; the D3DX calls no longer leave the guest stack unbalanced |
| The full APK on a real phone | the user installed the **`apk-6-20261026-2051`** private release (run [40398348378](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40398348378)) on their Motorola Edge 30 Fusion (Android 14, Adreno 660) and ran it; the `logcat` they pasted names `libmain.so` BuildId `76c65daf9e007948f6c56ad6e9e260c72e6a702f`, which is **exactly** the `lib/arm64-v8a/libmain.so` inside `apk-6` (apk-5's is `263e6cfaa549d3022ab2cde021d17c9d3b1eb4a7`), so the build is identified beyond doubt | the launcher opened and accepted the game folder, and `farcry2.exe` → `Dunia!RunGame` booted on the phone; then `SIGSEGV` / `SEGV_ACCERR` at fault address `0x6dade00088`, i.e. `MEM` base (`0x6dade00000`) + `0x88`. Disassembling `apk-6`'s own `libmain.so` shows the faulting instruction at `f_10425CB0+108` is `ldar w16,[x16]` with `x16 = MEM + (w15 + 0x88)`, `w15 = 0`: a 256-entry lookup table at `[this+0x14]` had a `NULL` slot. **That build predates the three fixes above** (`runtime/u16.h`, the `data_win32` mapping, the D3DX9 `__stdcall` change), and it never reads `Data_Win32` at all |
| A build with these three fixes is published for testing | run [40403855986](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40403855986), `gh release list` | private release **`apk-7-20261026-2135`** in `deivid22srk/FarCry2_gen` (sha256 `0a9b719a611a10d9d186fe8acc372c1cc42acb6e2eed5724a377a46b21f9af9c`). **It has not been run on a phone yet** — the host harness is the only execution evidence so far |
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
