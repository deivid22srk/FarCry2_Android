#!/usr/bin/env python3
"""Cross-check shim argument counts against the game's own call sites.

The runtime's shims declare how many stack arguments they pop: `RET(v, n)` for a
`__stdcall`/`__thiscall` import (`n` excludes a `this` pointer passed in ecx) and
`RETC(v)` when the caller cleans up.  A wrong count is invisible until the guest
stack silently drifts and then shows up much later as a wild pointer - e.g.
CryptGenRandom registered with the length and the buffer swapped, which memset
500 MB starting at a stack address.

This walks the import tables of the game images, counts the dwords each caller
pushes before `call dword ptr [IAT]`, and compares that with the count parsed out
of the runtime sources.  Decoding starts at the enclosing function's entry, taken
from analysis/functions_*.json, so the instruction stream is exact.

Limits, deliberately: arguments set up in an earlier basic block (after a jump)
are undercounted, and a register pushed right before the call (a save, not an
argument) is overcounted, so a "0 push" or "N+1 push" line needs a look rather
than an immediate fix.  Imports reached through a register (GetProcAddress
results) are not visible statically at all.

Usage:
    tools/shim_arity.py <game dir> [runtime dir] [--analysis DIR] [--all]

The default module list is what the port loads for Far Cry 2; `--all` checks
every PE in <game dir>/bin (and then needs no function lists).
"""
import bisect
import glob
import itertools
import json
import os
import re
import sys

try:
    import capstone
    import pefile
except ImportError:
    sys.exit("needs pefile and capstone: python3 -m pip install pefile capstone")

# game module -> analysis/functions_<key>.json
FC2_MODULES = {"Dunia.dll": "dunia", "farcry2.exe": "fc2",
               "binkw32.dll": "bink", "systemdetection.dll": "sdet"}

TERMINATORS = {"call", "jmp", "ret", "retn", "retf", "leave", "int", "int3",
               "iret", "hlt", "ud2", "sysenter", "sysexit"}


def shim_pops(runtime_dir):
    """{shim name: (pop count or None, cdecl flag)} parsed from the sources."""
    out = {}
    for path in sorted(glob.glob(os.path.join(runtime_dir, "*.c")) +
                       glob.glob(os.path.join(runtime_dir, "*.cpp"))):
        src = open(path, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"SHIM\((\w+)\)\s*\{", src):
            name, i, depth, end = m.group(1), m.end() - 1, 0, None
            for j in range(i, len(src)):
                if src[j] == "{":
                    depth += 1
                elif src[j] == "}":
                    depth -= 1
                    if depth == 0:
                        end = j
                        break
            if end is None:
                continue
            hits = list(re.finditer(r"\b(RETC?)\s*\(\s*([^,()]*?)\s*(?:,\s*(\d+)\s*)?\)",
                                    src[i:end + 1]))
            if not hits:
                out[name] = (None, None)
                continue
            kind, _, n = hits[-1].groups()
            out[name] = (int(n) if n else 0, kind == "RETC")
    return out


def registrations(runtime_dir):
    """[(dll, import name, shim name or None)] from the textual ShimDef tables.

    The tables are built with one-letter macros that get redefined and reused
    inside a file, so this walks each file in order and expands a use with the
    definition in force at that line.
    """
    out = []
    for path in sorted(glob.glob(os.path.join(runtime_dir, "*.c")) +
                       glob.glob(os.path.join(runtime_dir, "*.cpp"))):
        src = open(path, encoding="utf-8", errors="replace").read()
        for dll, nm, fn in re.findall(
                r'\{\s*"([^"]+\.dll)"\s*,\s*"([^"]+)"\s*,\s*(NULL|shim_\w+)\s*\}', src):
            out.append((dll.lower(), nm, None if fn == "NULL" else fn[5:]))
        cur = {}
        for line in src.splitlines():
            m = re.match(r'#define\s+(\w+)\s*\(([^)]*)\)\s*'
                         r'\{\s*"([^"]+\.dll)"\s*,\s*#?(\w+)\s*,\s*(?:shim_##)?(\w+)\s*\}\s*,?',
                         line)
            if m and m.group(2).strip():
                params = [p.strip() for p in m.group(2).split(",")]
                try:
                    npos = params.index(m.group(4)) + 1
                except ValueError:
                    continue
                if m.group(5) == "NULL":
                    fpos = 0
                else:
                    try:
                        fpos = params.index(m.group(5)) + 1
                    except ValueError:
                        continue
                cur[m.group(1)] = (m.group(3).lower(), npos, fpos)
                continue
            u = re.match(r"\s*#undef\s+(\w+)", line)
            if u:
                cur.pop(u.group(1), None)
                continue
            if re.match(r"\s*#", line):
                continue
            for use in re.finditer(r"\b(\w+)\s*\(\s*(\w+)(?:\s*,\s*(\w+)\s*)?\)", line):
                mac, a, b = use.groups()
                if mac not in cur:
                    continue
                dll, npos, fpos = cur[mac]
                arg = {0: None, 1: a, 2: b}
                if arg[npos] is None:
                    continue
                out.append((dll, arg[npos], arg[fpos]))
    return out


def pushed_dwords(run):
    """Argument dwords pushed between the previous branch and the call."""
    n = 0
    for ins in reversed(run):
        m, ops = ins.mnemonic, ins.op_str
        if m in TERMINATORS:
            break
        if m == "push":
            n += 1
        elif m == "pop":
            n -= 1
        elif m in ("sub", "add") and ops.replace(" ", "").startswith("esp,"):
            break               # frame bookkeeping, or another call's cleanup
    return n


def after_call(tail):
    """`add esp, N` immediately after the call, in dwords, or None."""
    for ins in tail:
        m, ops = ins.mnemonic, ins.op_str
        if m == "add" and ops.replace(" ", "").startswith("esp,"):
            try:
                return int(ops.split(",")[-1].strip(), 0) // 4
            except ValueError:
                return None
        if m == "pop":
            return 1
        return None
    return None


def function_starts(analysis_dir, key):
    path = os.path.join(analysis_dir, "functions_%s.json" % key)
    try:
        return sorted(int(k, 16) for k in json.load(open(path))["functions"])
    except (OSError, ValueError, KeyError):
        return None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    game, runtime = args[0], args[1] if len(args) > 1 else "runtime"
    analysis = "analysis"
    if "--analysis" in sys.argv:
        analysis = sys.argv[sys.argv.index("--analysis") + 1]
    check_all = "--all" in sys.argv
    pops = shim_pops(runtime)
    by_key = {}
    for dll, nm, fn in registrations(runtime):
        by_key.setdefault((dll, nm), fn)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False
    png = "*.exe" if check_all else "*"
    files = sorted(glob.glob(os.path.join(game, "bin", png)) +
                   glob.glob(os.path.join(game, "bin", "*.dll")) +
                   glob.glob(os.path.join(game, png)))
    seen = set()
    results = {}
    for pe_path in files:
        name = os.path.basename(pe_path)
        if name in seen or (not check_all and name not in FC2_MODULES):
            continue
        try:
            pe = pefile.PE(pe_path, fast_load=True)
            pe.parse_data_directories(
                directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
        except pefile.PEFormatError:
            continue
        seen.add(name)
        starts = function_starts(analysis, FC2_MODULES.get(name, ""))
        if starts is None:
            continue
        base = pe.OPTIONAL_HEADER.ImageBase
        data = pe.__data__
        # Every `call dword ptr [IAT]`, grouped by the function containing it so
        # each function is decoded exactly once.
        sites = []
        for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            dll_name = e.dll.decode().lower()
            for imp in e.imports:
                if not imp.name:
                    continue
                nm = imp.name.decode()
                fn = by_key.get((dll_name, nm))
                if fn is None:
                    fn = by_key.get((None, nm))
                if fn is None or fn not in pops:
                    continue
                nargs, cdecl = pops[fn]
                if nargs is None:
                    continue
                pat = b"\xff\x15" + imp.address.to_bytes(4, "little")
                i = 0
                while True:
                    j = data.find(pat, i)
                    if j < 0:
                        break
                    i = j + 1
                    try:
                        call_va = base + pe.get_rva_from_offset(j)
                    except Exception:
                        continue
                    k = bisect.bisect_right(starts, call_va) - 1
                    if k < 0 or call_va - starts[k] > 0x10000:
                        continue
                    sites.append((starts[k], call_va, j, imp.address,
                                  nm, fn, nargs, cdecl))
        sites.sort()
        for fn_start, group in itertools.groupby(sites, key=lambda s: s[0]):
            try:
                off = pe.get_offset_from_rva(fn_start - base)
            except Exception:
                continue
            run = list(md.disasm(data[off:off + 0x10000], fn_start))
            by_addr = {ins.address: idx for idx, ins in enumerate(run)}
            for _, call_va, j, iat, nm, fn, nargs, cdecl in group:
                idx = by_addr.get(call_va)
                if idx is None:
                    continue
                if ("[0x%x]" % iat) not in run[idx].op_str:
                    continue            # byte match that is not this call
                pushes = pushed_dwords(run[:idx])
                if pushes < 0 or pushes > 24:
                    continue
                if cdecl:
                    tail = run[idx + 1:]
                    if not tail:
                        tail = list(md.disasm(data[j + 6:j + 6 + 16], call_va + 6))
                    # cdecl: the caller cleans up, so a site with no arguments
                    # needs no `add esp` and is fine either way.
                    good = pushes == 0 or after_call(tail) == pushes
                else:
                    good = pushes == nargs
                rec = results.setdefault((name, nm, fn, nargs, cdecl), {})
                rec.setdefault(pushes, {})
                rec[pushes][bool(good)] = rec[pushes].get(bool(good), 0) + 1
    bad = 0
    skipped = 0
    for (module, nm, fn, nargs, cdecl), rec in sorted(results.items()):
        wrong = {p: c for p, c in rec.items() if not c.get(True)}
        if not wrong:
            continue
        if len(rec) > 1:
            # The same slot called with different argument counts is not the
            # imported API: Dunia stores its own function pointers in some IAT
            # slots (the GetCurrentProcess slot is called with ten arguments by
            # one wrapper), so nothing about the shim can be concluded here.
            skipped += 1
            continue
        bad += len(wrong)
        want = "cdecl" if cdecl else "%d arg" % nargs
        got = ", ".join("%d push x%d" % (p, c.get(False, 0)) for p, c in sorted(wrong.items()))
        print("%-18s %-42s shim_%-24s %-7s but %s"
              % (module, nm[:42], fn, want, got))
    if not bad:
        print("shim_arity: every consistently-called shim pops what its call sites push")
    else:
        print("shim_arity: %d mismatching call-site group(s)" % bad)
    if skipped:
        print("shim_arity: %d import(s) skipped (slot called with differing argument counts)"
              % skipped)


if __name__ == "__main__":
    main()