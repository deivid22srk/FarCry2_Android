# What was actually tested

This is the honest record. "Tested" means a command was run and its output
observed. "Not tested" means exactly that — most of the port is in that column.

Local machine for everything below: Ubuntu 24.04 dev container, x86_64,
4 cores / 16 GB, clang 18.1.3, Python 3.12, capstone 5.0.7, pefile 2024.8.26.
The APK is only ever built by GitHub Actions (NDK 27.2.12479018), never here.

## Tested and working

| What | How | Result |
|---|---|---|
| PE facts for `farcry2.exe`, `Dunia.dll`, `binkw32.dll`, `FC2.dll` | `pefile` walk of headers, sections, imports, exports, relocations, version resources; `sha1sum` | every number in `docs/RECON.md` §1–§6 |
| DRM classification | imports, section names, entropy, overlay, PDB paths | `FC2.dll` = SecuROM Activate & Play 1.0.1.10; nothing imports it; GOG boot path is DRM-free |
| Boot path of `farcry2.exe` | `capstone` 32-bit disassembly of `.text` | entry `0x4014ec` → cookie init `0x401840` → CRT startup `0x40122b` → `0x4011b0`; 15 `AddFunctionCB` registrations (12 distinct stubs, names in `.rdata`); `Dunia!RunGame` at `0x4011c5` (`docs/RECON.md` §3) |
| D3DX9 / D3D9 / DInput / DSound / XInput / Bink imports | import-table extraction | 15 D3DX9 entries, `Direct3DCreate9`, `DirectInput8Create`, DSOUND `#11`/`#2`, `EAX.DLL #6`, Bink 1.9a |
| Function discovery, `Dunia.dll` (14.1 MB of code) | `python3 tools/discover.py .../Dunia.dll analysis/functions_dunia.json 10000000` | 82,403 functions, 857 jump tables, 1,512 unresolved indirect jumps, 6,423 bad data roots, 95.5 % coverage, 724 s wall, 2,075 MB peak RSS |
| Function discovery, `binkw32.dll` | same | 447 functions, 14 jump tables, 42 indirect jumps |
| Function discovery, `farcry2.exe` | same | 52 functions |
| Lifting, `Dunia.dll` | `LIFT_FILES=384 python3 tools/lift.py ...` | 82,149 lifted, 254 rejected |
| Lifting, `binkw32.dll` | `LIFT_FILES=24 ...` | 442 lifted, 5 rejected |
| Lifting, `farcry2.exe` | `LIFT_FILES=256 ...` | 52 lifted, 0 rejected |
| Lifted C is valid C | `clang -DGAME_FC2 -std=gnu11 -fsyntax-only -Iruntime -Igen -w` on **all 427** `gen/*.c` (384 dunia + 24 bink + 16 fc2 + 3 tables) | 0 failures (log ended with `ALLDONE`, empty failure list) |
| Lifted C compiles at the CI flags | `clang -DGAME_FC2 -std=gnu11 -O1 -g0 -fno-strict-aliasing -fwrapv -fsigned-char -w -fPIC -c` on 8 files including the largest (`gen/dunia_329.c`) | all succeed; largest file 34 s, medium 25 s, small 0–5 s on this 4-core x86 host |
| Runtime C compiles for `GAME_FC2` | `clang -DGAME_FC2 -std=gnu11 -O1 -fsyntax-only` with the vendored DX VK/SDL include dirs on all `runtime/*.c` | 0 errors |
| Runtime C++ compiles for `GAME_FC2` | `clang++ -DGAME_FC2 -DDXVK_NATIVE -std=c++17 -fsyntax-only` on all `runtime/*.cpp` | 0 errors |
| The lifted table matches the runtime's expectations | `gen/*_table.c` + `funcs_*.h` consumed by `runtime/modules.c`; the app-only stubs in `ci/gen_stub/` follow the same shape | compiles; only real runs will exercise them |

Note the `-mtune=cortex-a73` in the CI compile options is not valid for the
local x86 clang, so the local sample pass omits it; the NDK clang used in CI
accepts it.

## Implemented but not tested

| What | Why not |
|---|---|
| The whole runtime under `GAME_FC2` | compiles, but nothing has run it |
| Host D3DX9 (`runtime/d3dx9_host.cpp`, 15 entry points) | compiles; no game run has requested a texture |
| D3D9 bridge over DXVK, DirectSound over SDL2, DInput/XInput, touch overlay | compiles; no device |
| `fc2_shims.c` MSVCR80/MSVCP80 shims beyond `basic_string` | compiles; the game has not been observed calling most of them |
| Android launcher (folder validation, size/SHA-1 check, options UI) | Java is compiled only by the CI Android build; never installed |
| `farcry2.exe` → `Dunia!RunGame` on a device | the point of the whole port; not reached |
| App-only CI APK | the workflow is written; the run is pending |
| Full APK | needs `gen/` in a private repo; the run is pending |

## Not implemented / blocked

| What | Reason |
|---|---|
| **SEH and C++ exceptions** | `_except_handler3`, `_except_handler4_common`, `__CxxFrameHandler3`, `_CxxThrowException` are trapped with an abort. Any structured exception or `throw` stops the game. Top risk for the main-menu target. |
| **MSVCP80 iostreams / stringstreams** | 40+ entry points registered as `NULL`; the first call aborts with its mangled name. |
| **Network** | Winsock/WinHTTP/WinINet/Secur32/Crypt32 are offline stubs. |
| **DXT mip filtering** | `D3DXFilterTexture` creates the chain without filtering (logged). |
| **`D3DXSaveTextureToFileInMemory`** | returns `E_NOTIMPL`. |
| **1,512 unresolved indirect jumps in Dunia** | calls through them abort at the target address; each one is a possible gap. |
| **Running on an arm64 device** | no device was available; nothing in this port has executed a frame of the game. |
| **Building the APK locally** | deliberately not done: no SDK/NDK/Gradle is installed here, and the project rule is that the APK comes only from Actions. |
| **Publishing the APK** | requires the private `GEN_REPO` setup described in the README; not done at the time of writing. |
| **`d3dx9_38.dll`, `FC2.dll` redistribution** | neither is shipped or recompiled (`FC2.dll` is DRM; `d3dx9_38` is Microsoft's). |
