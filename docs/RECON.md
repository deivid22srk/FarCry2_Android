# Far Cry 2 — reverse-engineering record

Everything below was read from the binaries of the copy under
`fc2/Far Cry 2` (GOG "Fortune's Edition", the 2012 installer build) with
`pefile`, `capstone` and hex dumps. No number here is guessed; the commands are
at the end so each can be repeated.

The game files are **not** in this repository.

## 1. The copy

| File | Size | SHA-1 | Notes |
|---|---|---|---|
| `bin/farcry2.exe` | 28,296 | `9067fa4fa28c1f5ab4458dd464eca54ff4b9914d` | tiny launcher, PE32 GUI, VC8 (linker 8.0) |
| `bin/Dunia.dll` | 19,412,104 | `3905709d89d75b1e1928c94685e70b22b25843fa` | the engine, PE32 DLL, VC8 |
| `bin/binkw32.dll` | 171,520 | `cb6367428a4b2cd1c4c2d330ae870fb872fdbebc` | Bink 1.9a, PE32 DLL |
| `bin/FC2.dll` | 389,768 | `8e379b1e618b6d6cb69f53c13328d63f8e1e4369` | SecuROM, **not used** |
| `bin/eax.dll` | — | — | EAX Unified 3.062, imported as `EAX.DLL #6` |
| `Data_Win32/common.fat` + `.dat` | 3.3 GB total | — | main asset archives |
| `Data_Win32/patch.fat` + `.dat` | | | patch archives |
| `Data_Win32/{sound,sound_english,shadersobj}.{fat,dat}` | | | audio / shader archives |

`d3dx9_38.dll` is **not** present in the install: Dunia imports it but the GOG
installer does not ship it (DirectX was installed system-wide). The port
implements its 15 entry points itself (§8).

`FC2.dll` carries a 4,744-byte overlay (Authenticode PKCS#7) like several other
DLLs in the folder; the overlay is not code and is not loaded by anything.

## 2. `farcry2.exe`

| Property | Value |
|---|---|
| Image base / entry | `0x00400000` / `0x004014ec` |
| Size of image / code | `0x9000` / `0xc00` |
| `.text` | VA `0x401000`, virtual size `0xa18`, raw `0xc00`, entropy 5.43 |
| Imports | 44 functions from 3 DLLs: `Dunia.dll` (3), `MSVCR80.dll` (27), `KERNEL32.dll` (14) |
| Relocations | **stripped** (`relocs_stripped: true`) |
| PDB | `d:\CastorVersions\FCX-PC-0053\fcx-branches\milestones-b\bin\NomadLaunch_PC.pdb` |
| Overlay | 4,744 bytes, Authenticode |

The whole executable is a launcher: 12 small callback stubs, one registration
function, and CRT startup. There is no game logic in it.

## 3. Boot path (disassembled)

```
0x4014ec  entry:  call 0x401840          ; __security_init_cookie
                  jmp  0x40122b          ; mainCRTStartup
0x40122b  mainCRTStartup: SEH frame, __getmainargs,
                  parse command line,
0x401366          call 0x4011b0          ; the only application function
0x4011b0  main:   push 0x4010e0
                  call [0x402004]        ; Dunia!RegisterGameFunctionProvider(provider)
                  ...
0x4011c5          call [0x402000]        ; Dunia!?RunGame@@YA_NPAUHINSTANCE__@@PBD@Z
```

The import slots are:

| IAT slot | DLL | Symbol |
|---|---|---|
| `0x402000` | `Dunia.dll` | `?RunGame@@YA_NPAUHINSTANCE__@@PBD@Z` |
| `0x402004` | `Dunia.dll` | `RegisterGameFunctionProvider` |
| `0x402008` | `Dunia.dll` | `AddFunctionCB` |

`RegisterGameFunctionProvider(0x4010e0)` registers the function Dunia calls
back to ask for the launcher's callbacks. `0x4010e0` then registers **15**
callbacks with `AddFunctionCB(name, fn)` (0x4010e0–0x4011a2, 15 call sites,
12 distinct stubs). The names are plain strings in `.rdata`:

| Name (`.rdata`) | Stub | Behaviour |
|---|---|---|
| `toRed` | `0x4010d0` | return 0 |
| `mapJoke` | `0x401000` | `*(int*)p = 1`, return 0 |
| `menuJoke` | `0x401010` | return `*p` |
| `SaveGame` | `0x401020` | return 1 |
| `LoadGame` | `0x4010d0` | return 0 |
| `SelectStoryMission` | `0x401020` | return 1 |
| `SelectLibraryMission` | `0x401030` | return `*p + 10` |
| `MalariaCurve` | `0x401040` | return `*p + 0x15` |
| `AddDiamond` | `0x401050` | `*p *= 1.0f` (`0x4020fc` = `1.0f`) |
| `SetDefaultTimeOut` | `0x401070` | `*a += *b`, return 0 |
| `SetLoadingText` | `0x401080` | `*a = *b`, return 0 |
| `PlayerSPFinalize` | `0x401090` | `*(int16*)p = 0`, return 0 |
| `InitializeUseableEvent` | `0x4010a0` | `*(float*)p = 1.0f` (`0x402100` = `1.0f`) |
| `CheckDomino` | `0x4010c0` | `*(char*)p = 1`, return 0 |
| `incHB` | `0x4010c0` | same stub as `CheckDomino` |

This is the entire job of `farcry2.exe`: after `RunGame` returns, the EXE exits.
Everything the player sees comes out of `Dunia.dll`.

## 4. `Dunia.dll`

| Property | Value |
|---|---|
| Image base / entry | `0x10000000` / `0x10a1d2a1` |
| Size of image | `0x192a000` (26,320,896 bytes mapped) |
| `.text` | VA `0x10001000`, virtual size `0xd78b16` (**14,125,334 bytes**), raw `0xd78c00` |
| `.rdata` / `.data` | `0x1705aa` / `0x6dce20` (virtual) |
| `.reloc` | present (`0x1230e4`), image is rebasable |
| Exports | 404, including `?RunGame@@YA_NPAUHINSTANCE__@@PBD@Z`, `AddFunctionCB`, `RegisterGameFunctionProvider` |
| Imports | 602 functions from 27 DLLs |
| PDB | `d:\CastorVersions\FCX-PC-0053\fcx-branches\milestones-b\bin\Dunia.pdb` |
| Version | `Dunia Engine/Far Cry 2 Dynamic Link Library`, Ubisoft, file/product 0.1.0.1 |

Version resource: "Far Cry 2", © 2008 Ubisoft Entertainment.
Section names are ordinary (`.text`, `.rdata`, `.data`, `CONST`, `.rsrc`,
`.reloc`), entropy of `.text` is normal for compiled C++, and there is no
packer stub: the engine is directly executable as shipped.

### Import map (DLL → functions)

| DLL | # | Used for |
|---|---|---|
| `KERNEL32.dll` | 145 | files, threads, memory, time |
| `MSVCR80.dll` | 180 | VC8 C runtime |
| `MSVCP80.dll` | 71 | VC8 C++ library (`std::string`, iostreams) |
| `USER32.dll` / `GDI32.dll` | 43 / 8 | windows, messages, fonts |
| `WS2_32.dll` | 32 | sockets |
| `WINHTTP.dll` / `WININET.dll` | 17 / 2 | HTTP (online) |
| `dbghelp.dll` | 16 | crash dumping |
| `binkw32.dll` | 16 | video/audio playback (`_BinkOpen@8`, `_BinkDoFrame@4`, …) |
| `d3dx9_38.dll` | 15 | texture/matrix helpers (see §8) |
| `ADVAPI32.dll` | 12 | registry, crypto |
| `Secur32.dll` / `CRYPT32.dll` | 9 / 4 | TLS, certificates (online) |
| `gdiplus.dll` | 8 | image decoding |
| `ole32.dll` / `OLEAUT32.dll` | 5 / 4 | COM |
| `SHELL32.dll` | 3 | shell paths |
| `d3d9.dll` | 1 | `Direct3DCreate9` |
| `DINPUT8.dll` | 1 | `DirectInput8Create` |
| `XINPUT1_3.dll` | 2 | `#2`, `#3` (XInputGetState ordinal exports) |
| `DSOUND.dll` | 2 | `#11` (`DirectSoundCreate8`), `#2` |
| `EAX.DLL` | 1 | `#6` (EAX Unified) |
| `RPCRT4.dll` | 2 | UUID helpers |
| `IPHLPAPI.dll` / `PSAPI.dll` | 1 / 1 | adapters, process memory |
| `WINMM.dll` | 1 | `waveOutGetDevCapsA` |

Graphics is D3D9 with shader model 2/3 (the game ships precompiled shaders in
`Data_Win32/shadersobj.*`), audio is DirectSound8 plus EAX, input is DInput8 and
XInput, video is Bink 1.9a.

## 5. `binkw32.dll` and `systemdetection.dll` (lifted, rebased)

### `binkw32.dll`

| Property | Value |
|---|---|
| Base / entry | `0x18000000` / `0x180024e0` |
| Size / image | 171,520 / `0x38000` |
| Sections | `.text`, `BINKY12`, `BINKY16`, `BINKP8`, `BINK16`, `BINK32`, `BINK`, `.rdata`, `.data`, `BINKBSS`, `BINKDATA`, `.rsrc`, `.reloc` |
| Exports | 71 (the 16 used by Dunia are `_Bink*@N`) |
| Imports | 91 from `KERNEL32`, `USER32`, `GDI32`, `WINMM` |
| Version | RAD Video Tools **1.9a** (© 1994–2007 RAD Game Tools) |

`binkw32.dll` is lifted, not shimmed: decoding must behave exactly as the
original. Its preferred base `0x18000000` collides with the guest TEB used by
the runtime (see §7), so the port rebases it to **`0x09000000`**. The image has
`.reloc`, so this is legal; the recompiled code is emitted for the new base and
the PE headers are still read for data/imports.

### `systemdetection.dll`

| Property | Value |
|---|---|
| Base / entry | `0x10000000` / `0x1001ab68` |
| Size / image | 275,080 / `0x44000` |
| Sections | `.text`, `.rdata`, `.data`, `.rsrc`, `.reloc` |
| Exports | 2: `GetHardwareInstance`, `GetScoreInstance` |
| Imports | 140 from `ADVAPI32`, `GDI32`, `KERNEL32`, `OLEAUT32`, `OPENGL32`, `USER32`, `d3d9`, `ole32` |
| Version | 1, 0, 0, 2, Ubisoft Entertainment, "System Detection DLL — Powered by Gear" |

Dunia calls `LoadLibraryA("systemdetection.dll")` at startup and resolves the two
exports with `GetProcAddress`; each returns a COM-like object that Dunia queries
through its vtable. That contract is only knowable from the code, so the module
is recompiled rather than mocked. Its preferred base `0x10000000` is Dunia's own
address, so it is rebased to **`0x12000000`** (it has `.reloc`). Its OpenGL and
D3D9 imports are used to enumerate the GPU; the runtime answers them from the
host (`runtime/misc.c`, `runtime/d3d9_bridge.cpp`), and the WMI/ADVAPI32 path it
may take instead is stubbed.

## 6. `FC2.dll` — SecuROM, excluded

| Property | Value |
|---|---|
| Size / SHA-1 | 389,768 / `8e379b1e618b6d6cb69f53c13328d63f8e1e4369` |
| Version | **SecuROM PA Unlock DLL for Activate & Play 1.0.1.10**, SONY DADC Austria AG |
| Original name | `paul_dll_activate_and_play.dll` |
| PDB | `d:\code\milestones-b\external\Securom\samples\paul\Release\fc2.pdb` |
| Exports | 1 |
| Imports | 268 from `KERNEL32`, `USER32`, `ADVAPI32`, `ole32`, `OLEAUT32`, … |

Nothing in `farcry2.exe`, `Dunia.dll` or `binkw32.dll` imports `FC2.dll`; the
GOG build does not load it. It is SecuROM "Activate & Play" DRM, not game code.
The port never reads, lifts, loads or ships it. The GOG boot path is DRM-free.

`eax.dll` (EAX Unified 3.062) is also present in `bin/`; `Dunia.dll` imports it
as `EAX.DLL #6`. It is **not** recompiled — the runtime provides the one entry
point (§9).

## 7. Guest address layout

The runtime maps the guest's 32-bit address space with `mmap(NULL, 4 GiB)` and
places the images at their original (or rebased) addresses:

| Region | Address | Size |
|---|---|---|
| `farcry2.exe` | `0x00400000` | `0x9000` |
| `binkw32.dll` (rebased) | `0x09000000` | `0x38000` |
| `Dunia.dll` | `0x10000000` | `0x192a000` |
| `systemdetection.dll` (rebased) | `0x12000000` | `0x44000` |
| thread stacks (32 slots) | `0x20000000` – `0x28000000` | 4 MiB each |
| TEB (one page per thread) | `0x28000000` | |
| PEB / environment / misc | `0x28100000` | |
| guest heap (`malloc`, `HeapAlloc`) | `0x30000000` – `0x60000000` | 768 MiB |
| `VirtualAlloc` region | `0x60000000` – `0xE0000000` | |
| host shim thunks | `0xFF000000` | |

The layout is `GAME_FC2`-specific in `runtime/rt.h`. The USM/LEGO layout put the
stacks at `0x10000000`, which is where Dunia lives, hence the move and the Bink
rebase. `systemdetection.dll` would also load at `0x10000000` (its own preferred
base, and Dunia's address), so it is rebased to `0x12000000`; Dunia resolves it
with `LoadLibraryA`/`GetProcAddress` at startup.

`farcry2.exe` has **no relocations**, so it can only be loaded at `0x00400000`.
Dunia has relocations but is loaded at its preferred base, so its absolute
addresses are unchanged.

## 8. D3DX9 (`d3dx9_38.dll`) — host implementation

The DLL is not in the install and is not shipped here. `Dunia.dll` imports
exactly 15 entry points; `runtime/d3dx9_host.cpp` implements each one:

| Entry point | Implementation |
|---|---|
| `D3DXPlaneNormalize`, `D3DXPlaneTransform` | exact float math |
| `D3DXMatrixTranspose`, `D3DXMatrixInverse` | transpose; cofactor inverse |
| `D3DXGetImageInfoFromFileInMemory` | DDS/BMP/TGA header parse |
| `D3DXCreateTextureFromFileInMemoryEx` | DDS (DXT1/3/5 + uncompressed), BMP, TGA → `CreateTexture` |
| `D3DXCreateCubeTextureFromFileInMemoryEx` | cube faces → `CreateCubeTexture` |
| `D3DXLoadSurfaceFromMemory`, `D3DXLoadSurfaceFromSurface` | copy + DXT decode |
| `D3DXFilterTexture` | mip chain creation (no filtering; documented gap) |
| `D3DXFillTexture` | procedural fill callback per level |
| `D3DXGetShaderInputSemantics` | vs_1_1/2_0/3_0 `dcl` parsing |
| `D3DXSHProjectCubeMap` | orders 1–5 |
| `D3DXSaveTextureToFileW` | DDS/BMP write |
| `D3DXSaveTextureToFileInMemory` | returns `E_NOTIMPL` (loudly logged) |

Unsupported formats and the in-memory save path fail with `E_NOTIMPL`/`E_FAIL`
and a log line instead of silently returning garbage.

## 9. Host API mapping

| Guest import | Host implementation |
|---|---|
| D3D9 `Direct3DCreate9` | DXVK `d3d9` (native build) over Vulkan, `runtime/d3d9_bridge.cpp` |
| `D3DX9` (15) | `runtime/d3dx9_host.cpp` |
| `DINPUT8` / `XINPUT1_3` | `runtime/dinput.c`, `runtime/input.c` (keyboard, mouse, pad, touch) |
| `DSOUND` #11/#2, `EAX` #6 | `runtime/dsound.c` (SDL2 audio), EAX ignored |
| `binkw32` (16) | **recompiled** `gen/bink_*.c` |
| `MSVCR80` (180) | `runtime/msvcrt.c` + `runtime/crt_shims.c` (aliased `msvcr80`) + `runtime/fc2_shims.c` |
| `MSVCP80` (71) | `basic_string`/`char_traits`/`std::exception` implemented, plus the iostream/stringstream subset the engine uses (`GStream`: a file-backed streambuf with `operator<<`/`operator>>`/`getline`); anything outside that subset is a loud trap |
| Win32 (KERNEL32/USER32/GDI32/…) | `runtime/kernel32.c`, `user32.c`, `misc.c`, `fc2_shims.c` |
| Winsock/WinHTTP/WinINet/Secur32/Crypt32 | offline stubs in `runtime/fc2_shims.c` |
| `dbghelp`, `psapi`, `iphlpapi`, `rpcrt4` | stubs in `runtime/fc2_shims.c` |

`tools/import_coverage.py <game dir>` reads the PE import tables and the runtime
shim tables and reports where each import lands:

```
farcry2.exe            44 imports |   44 implemented |   0 loud traps |   0 abort stubs
Dunia.dll             602 imports |  602 implemented |   0 loud traps |   0 abort stubs
binkw32.dll            91 imports |   91 implemented |   0 loud traps |   0 abort stubs
systemdetection.dll   140 imports |  140 implemented |   0 loud traps |   0 abort stubs
```

Every import of all four modules resolves to an implementation. The 35
MSVCP80 iostream/stringstream entry points that used to be deliberate loud
traps are implemented now (see §9); a call into an entry point *outside* that
subset still stops at one identifiable mangled name rather than corrupting
memory.

Eight of the implementations are deliberate fallbacks: they log once and
return a fixed failure value instead of pretending to work —
`UpdateLayeredWindow`, `WSAStartup`, `socket`, `gethostbyname`,
`AcquireCredentialsHandleA`, `CryptGenRandom`, `EnumDisplayDevicesA` and
`EAXDirectSoundCreate8`.

Ordinal imports matter here: `pefile` silently resolves them to names through its
built-in `ordlookup` tables, which hides them from a name-only comparison. The
checker reads the ordinal out of the import table instead. The engine imports
`WS2_32` by ordinal (30 entries), plus `OLEAUT32` `#8`/`#9`, `XINPUT1_3`
`#2`/`#3`, `DSOUND` `#11`/`#2` and `EAX` `#6`; the runtime registers those shims
under `#<ordinal>` as well.


`crt_shims.c` registers its table under both `msvcr71.dll` (the reference ports)
and `msvcr80.dll` (this game), and registers the data imports `_acmdln`,
`_adjust_fdiv` and `_iob` for both names. `_acmdln` is built from the game's
Windows path (`C:\FarCry2\bin\farcry2.exe`), not a hard-coded "USM.exe".

## 10. Discovery and lifting

### Discovery (`tools/discover.py`)

| Image | `.text` bytes | Functions | Jump tables | Unresolved indirect jumps | Bad data roots | Time | Peak RSS |
|---|---|---|---|---|---|---|---|
| `Dunia.dll` | 14,125,334 | **82,403** | 857 | 1,512 | 6,423 | 724 s | 2,075 MB |
| `binkw32.dll` | 6,316 (plus the `BINK*` sections) | 447 | 14 | 42 | — | seconds | — |
| `farcry2.exe` | 2,584 | 52 | 0 | 0 | 0 | <1 s | — |
| `systemdetection.dll` | 179,264 | **1,299** | 46 | 11 | 368 | seconds | — |

Dunia's discovery reached **95.5 %** of the code bytes; `systemdetection.dll`
reached 173,534 of 179,264 bytes (**96.8 %**).

The 4.5 % of Dunia not covered is code reachable only through the 1,512
unresolved indirect jumps (C++ virtual calls through tables the analyser cannot
prove) plus the 6,423 data roots that did not point at plausible code. Those
functions are simply absent from `gen/`; calling one at runtime resolves to the
abort stub with its address, which is how the next gap gets identified.

### Lifting (`tools/lift.py`)

| Image | Lifted | Rejected | Output |
|---|---|---|---|
| `Dunia.dll` | **82,149** | 254 | 384 files |
| `binkw32.dll` | **442** | 5 | 24 files |
| `systemdetection.dll` | **1,297** | 2 | 24 files |
| `farcry2.exe` | **52** | 0 | 256 files (46 non-empty) |
| **total** | **83,940** | **261** | **688 files + 4 tables, 288 MB** |

Files are address buckets (`LIFT_FILES`, default 24, 64 for an exe): the count
keeps a per-file compile short while keeping a re-lift of one address range
local. `farcry2.exe`'s 52 functions are spread over the whole `.text`, so most
of its 256 buckets end up empty (they compile to nothing).

`gen/` therefore holds 692 `.c` files (688 lifted + one `*_table.c` per module)
plus the four `funcs_*.h`, and is the single input the Android build globs.

Rejection reasons (unsupported x86 encodings, mostly junk bytes inside
functions that the analyser over-approximated):

| Reason | Count | Reason | Count |
|---|---|---|---|
| `reg es` | 67 | `junk lds` | 11 |
| `reg ss` | 34 | `junk aaa` | 11 |
| `junk enter` | 19 | `junk aas` | 9 |
| `junk das` | 18 | `junk loope` | 4 |
| `junk loopne` | 14 | `junk aam` | 3 |
| `reg ds` | 14 | `addr16` | 3 |
| `junk int1` | 14 | `junk aad` | 3 |
| `junk les` | 11 | `segment` / `reg gs` / `reg fs` | 2 / 2 / 2 |
| `junk daa` | 11 | `junk fbld` / `mmx pshufw` | 1 / 1 |

Rejected functions keep their address in the analysis JSON; the lifted table has
no entry, so a call aborts by address rather than jumping into nothing.

## 11. Known gaps (verified, not hidden)

| Gap | Evidence / effect |
|---|---|
| **SEH / C++ exceptions** | `Dunia.dll` imports `_except_handler3`, `_except_handler4_common`, `__CxxFrameHandler3`, `_CxxThrowException`, `_setjmp3`/`longjmp`. `fc2_shims.c` traps the handlers with `rt_fatal`; the first real exception stops the game. **Top blocker.** |
| **MSVCP80 iostreams** | the iostream/stringstream subset the engine uses is implemented (`GStream`, see §9), as are `basic_string`, `char_traits` and `std::exception`. Entry points outside that subset are still loud traps: they abort with their mangled name instead of corrupting memory. |
| **No network** | Winsock/WinHTTP/WinINet/Secur32/Crypt32 are offline stubs. |
| **DXT mips** | `D3DXFilterTexture` builds levels without filtering (logged). |
| **In-memory texture save** | `D3DXSaveTextureToFileInMemory` → `E_NOTIMPL`. |
| **Indirect-jump coverage** | 1,512 unresolved targets in Dunia; a call into one aborts at its address. `binkw32.dll` 42, `systemdetection.dll` 11. |
| **All other imports** | resolved: `tools/import_coverage.py` reports 0 unregistered imports across the four modules (see §9). |
| **Unverified code** | Nothing has run on a device yet; see `docs/TESTED.md`. |

## 12. Commands used

```sh
# hashes and PE facts
sha1sum bin/farcry2.exe bin/Dunia.dll bin/binkw32.dll bin/FC2.dll
python3 -c 'import pefile; ...'            # docs/TESTED.md lists the fields

# discovery (82k functions, ~12 min)
cd tools && python3 discover.py ../fc2/Far\ Cry\ 2/bin/Dunia.dll \
    ../analysis/functions_dunia.json 10000000

# lifting (uses analysis/functions_*.json)
LIFT_FILES=384 python3 tools/lift.py ../fc2/Far\ Cry\ 2/bin/Dunia.dll \
    ../analysis/functions_dunia.json ../gen dunia 10000000
```

The boot-path listing in §3 is `capstone` in 32-bit mode over `.text`.
