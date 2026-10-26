# Far Cry 2 — Android arm64 static recompilation

An experimental native Android port of **Far Cry 2** (PC, 2008, Ubisoft Montreal /
Dunia engine) for **arm64-v8a**. The x86 game code is not emulated: the original
binaries are translated to C once, on the user's machine, and that C is compiled
into the APK for the phone.

The approach and the runtime are modeled on
[`deivid22srk/Ultimate_Spider_Man_Android`](https://github.com/deivid22srk/Ultimate_Spider_Man_Android)
and [`ttom795/Skyrim_Android`](https://github.com/ttom795/Skyrim_Android). No code
or data from those ports' games is included here — only the recompilation idea
and the shared runtime structure (which those projects also share).

**This is not a playable release yet.** See [Status](#status) and
[`docs/TESTED.md`](docs/TESTED.md) for exactly what has and has not been run.

## Status

| Area | State |
|---|---|
| PE recon of `farcry2.exe`, `Dunia.dll`, `binkw32.dll`, `systemdetection.dll` | **Done**, every fact in [`docs/RECON.md`](docs/RECON.md) read from the binaries |
| Boot path disassembly (exe → `Dunia.dll!RunGame`) | **Done**, `docs/RECON.md` §3 |
| Function discovery (`Dunia.dll`, 14.1 MB of code) | **Done**: 82,403 functions, 95.5 % coverage |
| x86 → C lifting of all four modules | **Done**: 83,940 functions lifted, 261 rejected |
| Runtime (`runtime/`, CRT/Win32/D3D9/DSound/input shims) | **Written** and executed: it boots the game on a host and reaches the render loop (see *First execution* below) |
| Import resolution of the four modules | **Done**: every import resolves (`tools/import_coverage.py`); 0 unregistered imports, 0 abort stubs |
| Host D3DX9 (`runtime/d3dx9_host.cpp`, 15 imports) | **Written**, compiles; exercised by the host run (texture creation observed) |
| Android launcher + activity | **Written**, compiles in CI |
| APK (app-only, stub tables) | **Built by GitHub Actions**, run [40338447334](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40338447334): `FarCry2-apk`, 2,106,960 bytes |
| APK (full, with `gen/`) | **Built by GitHub Actions**: run [40377549175](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40377549175) → 45,540,586 bytes, then run [40398348378](https://github.com/deivid22srk/FarCry2_Android/actions/runs/40398348378) → 45,545,214 bytes, `arm64-v8a`, `libmain.so` 109,747,328 bytes. Published as **private** release assets in [`deivid22srk/FarCry2_gen`](https://github.com/deivid22srk/FarCry2_gen) (`apk-5-20261026-1836`, `apk-6-20261026-2051`), never here |
| Game on a phone | **Partially working**: the full APK installs on a real arm64 phone (Motorola Edge 30 Fusion, Android 14, Adreno 660) and the launcher opens and accepts the game folder; `farcry2.exe` → `Dunia!RunGame` boots. The user's `logcat` is from **`apk-6`** (its `libmain.so` BuildId matches that release exactly) and shows a `SIGSEGV` in the UI init, at a `NULL` entry of a lookup table only reachable because that build does not read the shipped `Data_Win32` data at all — see below |
| Game on the host (desktop harness) | **Partially working**: boots to the engine's render loop, then dies in the UI init of the first screen (details below) |
| SEH / C++ exceptions | **Not implemented** — top blocker, see below |

The minimum acceptance target for this project is: APK installs, the launcher
validates the game folder, and the game reaches its main menu. **The first two
are met** (the user installed and ran the full APK on their own phone); the third is not.

### First execution (host, not the APK)

`desktop/build.sh` builds `build/lb`: the same runtime plus the recompiled code
as a Linux binary. `desktop/run.sh "<game dir>"` runs the game in place, and
`desktop/run_headless.sh` does the same under Xvfb + Mesa lavapipe for a machine
with no display or GPU.

What has been observed, running the user's own GOG copy:

* the boot path: `farcry2.exe` → `Dunia.dll!RunGame`, Dunia loading and
  resolving `binkw32.dll` and `systemdetection.dll`;
* `CreateWindowExA 'Dunia'`, `WM_ACTIVATEAPP`, DirectSound8 (SDL2), the D3D9
  device and swap chain through DXVK, many `CreateTexture`, `Reset 1024x576`,
  then the `PeekMessageA` / `QueryPerformanceCounter` main loop.

Where it stops (see `docs/TESTED.md` for the commands and the gdb evidence): the
engine activates its first screen, `MAINMENU_INITIAL_RATING`, and dereferences
`[screen+0x14]` — the root of that screen's UI-element tree — which is still
`NULL`. The whole constructor chain of that screen object was disassembled from
the real PE and compared byte-for-byte with the lifted C; the only instruction
anywhere in the chain that touches the field **zeroes** it, and a byte-level scan
of the entire `Dunia` `.text` plus a gdb watchpoint confirm no code in the
process ever writes a non-`NULL` value. So whatever is supposed to build the
screen's elements never runs. `f_10CA2220` then calls `f_10A86DD0` with the
`NULL` and the fault lands at guest address `0x58`.

The **phone** run stops earlier, and for a known reason: the `apk-6` build the
user has predates three fixes that are in the tree now — guest wide strings must
be UTF-16 (host `wchar_t` is 4 bytes on Android/Linux), `c:\data_win32\...`
must map to the game's `Data_Win32` rather than the save directory, and the
D3DX9 entry points are `__stdcall`. The disassembled `apk-6` `libmain.so` shows
the crash is a `NULL` slot in a 256-entry table that is only empty because the
build never opened the shipped data. With the fixes the host run gets past that
point (`f_10425CB0`: 202 calls, 0 `NULL` entries) and stops at the screen's
UI-element tree, as above.

Everything the run touched is listed in `docs/TESTED.md`; the pieces that abort
loudly (SEH/C++ unwinding, unresolved indirect jumps, Bink imports) have not been
hit yet.

## What it does

```
farcry2.exe   0x00400000  28 KB launcher stub: registers callbacks, calls RunGame
Dunia.dll     0x10000000  19.4 MB engine (14.1 MB of code, 404 exports)
binkw32.dll   0x09000000  171 KB Bink 1.9a video/audio (rebased from 0x18000000)
```

* `farcry2.exe` is lifted as a normal module (`gen/fc2_*.c`). It imports only
  `Dunia.dll`, `MSVCR80.dll` and `KERNEL32.dll`.
* `Dunia.dll` is lifted in 384 buckets (`gen/dunia_*.c`). Its exports are
  resolved from the real PE export table; the x86 code of every exported and
  internal function is C.
* `binkw32.dll` is lifted (`gen/bink_*.c`) instead of shimmed, because the game
  calls 16 Bink entry points and video playback must match the original.
* `d3dx9_38.dll` is **not** part of the game install and is **not** shipped.
  `runtime/d3dx9_host.cpp` implements the 15 D3DX functions `Dunia.dll` imports
  (texture/cube loading from memory, DDS/BMP/TGA, DXT1/3/5, plane and matrix
  maths, shader input semantics, SH projection, save-to-file).
* D3D9 goes through the vendored [DXVK](https://github.com/doitsujin/dxvk)
  `d3d9` (native build) on Vulkan; DirectSound, DInput, XInput and Win32 are
  shimmed in `runtime/`.
* `FC2.dll` is **excluded**. It is SecuROM "Activate & Play" (SONY DADC,
  1.0.1.10), nothing imports it, and the GOG build never loads it. It is never
  read, recompiled or shipped.

See [`docs/RECON.md`](docs/RECON.md) for the evidence behind every number.

## Requirements

* **Phone**: arm64-v8a, Android 9 (API 28) or newer, a Vulkan 1.1 driver.
  A flagship-class SoC is strongly recommended; the recompiled engine is large.
* **Your own copy of Far Cry 2 for PC**, GOG "Fortune's Edition" or the matching
  retail 1.0.0.1 build (the launcher checks the size and SHA-1 of
  `farcry2.exe` and `Dunia.dll`).

### Files the port needs from your copy

Copy the *contents* of the game folder (the one containing `bin/` and
`Data_Win32/`) to the phone, then point the launcher at it:

```
bin/farcry2.exe      28,296 bytes   SHA-1 9067fa4fa28c1f5ab4458dd464eca54ff4b9914d
bin/Dunia.dll        19,412,104     SHA-1 3905709d89d75b1e1928c94685e70b22b25843fa
bin/binkw32.dll      171,520        SHA-1 cb6367428a4b2cd1c4c2d330ae870fb872fdbebc
Data_Win32/common.fat, common.dat
Data_Win32/patch.fat,  patch.dat
Data_Win32/...        the rest of the assets (sound, shaders, worlds, ui)
```

`bin/FC2.dll` is not used and may be deleted. `d3dx9_38.dll` is not needed.

## Building

There are two separate steps. The first one **must be done by you**, on a PC
with Python; the result (`gen/`) is derived from the game and must never be
committed or published. The second one builds the APK and is only ever run by
GitHub Actions in this project.

### 1. Generate `gen/` from your copy

Needs Python 3.10+, `capstone==5.0.9`, `pefile==2024.8.26`:

```sh
python3 -m pip install capstone==5.0.9 pefile==2024.8.26
tools/regen_gen.sh "/path/to/Far Cry 2"
```

`regen_gen.sh` runs `tools/discover.py` on `farcry2.exe`, `Dunia.dll`,
`binkw32.dll` and `systemdetection.dll`, then `tools/lift.py` on each. On a 2026
laptop it takes roughly 15–25 minutes and produces ~288 MB of C in 692 files
(`analysis/` holds the discovery JSON). Expect **~1.1 GB of object files** and
**1–2 hours** to compile them on a CI runner with three parallel jobs.

### 2. Build the APK (GitHub Actions only)

The repository is set up so the APK is built by `.github/workflows/build.yml`
and never on a developer machine. Two modes:

| Mode | Trigger | What it produces |
|---|---|---|
| **app-only** | `GEN_REPO` repository variable unset | Launcher + runtime linked against `ci/gen_stub` (empty tables). Installs, validates the game folder, and stops with a clear message when Play is pressed. No game code, safe to publish. |
| **full** | `GEN_REPO` variable set to a **private** repo, `GEN_TOKEN` secret with `contents:write` on it | Downloads `gen.tar.zst` from that repo's release, builds the real APK, and publishes it as an asset of a **private** release there. The public repo and its artifacts never contain the APK or the recompiled code. |

Full-build setup (the recommended "strategy C"):

1. Create a private repository, e.g. `deivid22srk/FarCry2_gen`.
2. Run `tools/regen_gen.sh` locally, then pack and upload the result to a
   release of that private repo:
   ```sh
   tar --use-compress-program='zstd -T4 -3' -cf gen.tar.zst gen
   sha256sum gen.tar.zst
   gh release create gen-v1 gen.tar.zst --repo deivid22srk/FarCry2_gen
   ```
   Pack with `--use-compress-program=unzstd` extraction in mind (that is what
   `build.yml` uses); the archive must have `gen/` at its top level. Add a new
   tag (and bump `GEN_TAG`) whenever `gen/` is re-lifted.
3. In the public repo: *Settings → Secrets and variables → Actions*
   * secret `GEN_TOKEN`: a fine-grained token with `Contents: read/write` on
     `FarCry2_gen` (needed to download and to publish the APK there);
   * variable `GEN_REPO`: `deivid22srk/FarCry2_gen`;
   * variable `GEN_TAG`: `gen-v1`.
4. Run the **build** workflow. The APK is published to
   `https://github.com/deivid22srk/FarCry2_gen/releases`.

This is exactly how the maintainer's repository is wired: `GEN_REPO` =
`deivid22srk/FarCry2_gen`, `GEN_TAG` = `gen-v1`, and a `GEN_TOKEN` that lives
only as an Actions secret.

Never point `GEN_REPO` at a public repository: the APK contains recompiled
Ubisoft code and must stay private.

## Using the app

1. Install the APK (arm64 device).
2. Copy the game folder somewhere the app can read — internal storage outside
   `Android/data` is the safe place (the launcher asks for all-files access).
3. Open **Far Cry 2**: the launcher validates `bin/` and `Data_Win32/`, checks
   the size/SHA-1 of `farcry2.exe` and `Dunia.dll`, and offers touch-overlay
   options.
4. Press **Jogar**. Progress and crashes go to `<game folder>/port/log.txt`.

The touch overlay follows the reference port: left stick, right stick as mouse,
face buttons mapped to keyboard keys. It is generic and was not authored for
Far Cry 2's controls.

## Known limitations

These are real blockers or gaps, not disclaimers:

* **SEH and C++ exceptions are not implemented.** `Dunia.dll` imports
  `_except_handler3`, `_except_handler4_common`, `__CxxFrameHandler3` and
  `_CxxThrowException`. The runtime traps on the first two
  (`runtime/fc2_shims.c`), so any structured exception or `throw` stops the
  game with a message. This is the biggest risk for reaching the main menu.
* **MSVCP80 iostreams / stringstreams are not implemented.** The 40-odd
  iostream entry points are registered with `NULL` so the first call fails with
  its mangled name instead of corrupting memory. Only `basic_string` and
  `char_traits` are implemented.
* **No network.** Winsock/WinHTTP/WinINet/Secur32/Crypt32 shims are offline
  stubs; online features cannot work.
* **DXT mip filtering is a no-op** (`IDirect3D9` path logs it). Mip levels are
  built without filtering.
* **`D3DXSaveTextureToFileInMemory` returns `E_NOTIMPL`.**
* **App-only APKs cannot start the game** by design; they only validate the
  folder. A full APK needs your own `gen/`.
* The Android launcher, the touch layer and the whole runtime are **untested on
  a device**. `docs/TESTED.md` separates "compiled" from "ran".
* The `gen/` output is large: a full APK will be hundreds of MB and may be slow
  to install and to start on mid-range hardware.

## Legal

Far Cry 2 and the Dunia engine are © Ubisoft Entertainment. Bink is
© RAD Game Tools. This repository contains **no game code, data, artwork or
binaries** — only the tooling and the runtime that the user combines with their
own legally obtained copy. `gen/`, `analysis/`, the game folder and the APK are
git-ignored on purpose; do not commit, publish or redistribute them.
`FC2.dll` (SecuROM) is deliberately not used; do not redistribute it either.

## Credits

* [`deivid22srk/Ultimate_Spider_Man_Android`](https://github.com/deivid22srk/Ultimate_Spider_Man_Android)
  and [`ttom795/Skyrim_Android`](https://github.com/ttom795/Skyrim_Android) for
  the static-recompilation runtime this port is built on.
* [DXVK](https://github.com/doitsujin/dxvk) (zlib), [SDL2](https://libsdl.org)
  (zlib), [adrenotools](https://github.com/bylaws/libadrenotools) (BSD-2) —
  vendored under `android/app/jni/`.
* [Capstone](https://www.capstone-engine.org) and
  [pefile](https://github.com/erocarrera/pefile) for the tooling.
