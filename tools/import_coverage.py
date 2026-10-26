#!/usr/bin/env python3
"""Report how many of each module's imports the runtime actually implements.

For every DLL the given PE imports, this checks whether the runtime registers
an implementation (a `ShimDef` entry, in any of its macro form or explicit
form), whether a *recompiled* module exports the name (dunia/binkw32/farcry2,
from analysis/functions_*.json), or whether the import will fall through to the
runtime's abort stub at run time.

Deliberate loud traps (ShimDef with a NULL function, e.g. the MSVCP80 iostreams)
are counted separately: they stop with the mangled name, which is intended.

Usage: tools/import_coverage.py <game dir> [analysis dir]

The macros used in the runtime tables all have the shape
    #define X(n) {"dll.dll", #n, shim_n},
    #define X(n) {"dll.dll", #n, NULL},
or the ordinal form
    {"dll.dll", "#6", shim_...},
so parsing the sources is enough; no need to build anything.
"""
import collections
import glob
import json
import os
import re
import sys

try:
    import pefile
except ImportError:
    sys.exit("needs pefile: python3 -m pip install pefile")

# CRT tables are shared: msvcrt's table is re-registered under the VC7 and VC8
# names at startup (runtime/msvcrt.c: msvcrt_register_as_msvcr71/80), and
# crt_shims.c aliases its msvcr71 entries to msvcr80.
ALIAS = {"msvcrt": ("msvcrt", "msvcr71", "msvcr80"),
         "msvcr71": ("msvcr71", "msvcr80")}

RECOMPILED = {"dunia": "dunia", "binkw32": "bink", "farcry2": "fc2"}


def base(dll):
    return dll.replace("\\", "/").split("/")[-1].replace(".dll", "")


def parse_tables(root):
    impl, traps = collections.defaultdict(set), collections.defaultdict(set)
    one = re.compile(r'#define\s+(\w+)\s*\(\w+\)\s*\{\s*"([^"]+)"\s*,\s*'
                     r'(?:#\w+|\w+)\s*,\s*([^},]+?)\s*\}')
    two = re.compile(r'#define\s+(\w+)\s*\(\s*\w+\s*,\s*\w+\s*\)\s*\{\s*"([^"]+)"\s*,\s*'
                     r'\w+\s*,\s*([^},]+?)\s*\}')
    for path in glob.glob(os.path.join(root, "runtime", "*.c")) + \
                glob.glob(os.path.join(root, "runtime", "*.cpp")):
        txt = open(path, encoding="utf-8", errors="replace").read()
        # Macro definitions, in source order. The same letter is reused for
        # several DLLs in one file (user32.c defines S for USER32 and again for
        # GDI32), so a use belongs to the definition preceding it.
        defs = []
        for rx in (one, two):
            for m in rx.finditer(txt):
                defs.append((m.start(), m.end(), m.group(1), m.group(2).lower(),
                             m.group(3).strip() != "NULL"))
        defs.sort()
        for i, (_, dend, name, dll, has_fn) in enumerate(defs):
            end = len(txt)
            for _, s2, n2, _, _ in defs[i + 1:]:
                if n2 == name:
                    end = s2
                    break
            target = impl if has_fn else traps
            for mm in re.finditer(r'\b%s\s*\(([^()]*)\)' % re.escape(name), txt[dend:end]):
                for n in re.split(r"[\s,]+", mm.group(1).strip()):
                    n = n.strip().strip('"')
                    if n and n != "n":
                        target[dll].add(n)
        # explicit entries
        for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*([^}]+?)\s*\}', txt):
            has_fn = m.group(3).strip() != "NULL"
            (impl if has_fn else traps)[m.group(1).lower()].add(m.group(2))
        # data imports registered directly
        for m in re.finditer(r'rt_register_data\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,', txt):
            impl[m.group(1).lower()].add(m.group(2))

    # fold aliases: every table is reachable under its own name and, for the
    # CRT, under the VC7/VC8 names it is re-registered as
    folded_impl, folded_traps = collections.defaultdict(set), collections.defaultdict(set)
    for dll, names in impl.items():
        b = base(dll)
        for t in set(ALIAS.get(b, ())) | {b}:
            folded_impl[t] |= names
    for dll, names in traps.items():
        b = base(dll)
        for t in set(ALIAS.get(b, ())) | {b}:
            folded_traps[t] |= names
    return folded_impl, folded_traps


def recompiled_exports(analysis, keys):
    out = collections.defaultdict(set)
    for module, short in RECOMPILED.items():
        path = os.path.join(analysis, "functions_%s.json" % short)
        if not os.path.exists(path):
            continue
        for e in json.load(open(path)).get("exports", []):
            out[module].add(e["name"] if isinstance(e, dict) else str(e))
    return out


def report(exe, impl, traps, exports):
    p = pefile.PE(exe, fast_load=True)
    p.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    total = have = trapn = miss = 0
    missing = collections.Counter()
    for e in p.DIRECTORY_ENTRY_IMPORT:
        dll = base(e.dll.decode().lower())
        for imp in e.imports:
            name = imp.name.decode() if imp.name else "#%d" % imp.ordinal
            total += 1
            if name in impl.get(dll, set()) or name in exports.get(dll, set()):
                have += 1
            elif name in traps.get(dll, set()):
                trapn += 1
            else:
                miss += 1
                missing[dll] += 1
    print("%-14s %4d imports | %4d implemented | %3d loud traps | %3d abort stubs"
          % (os.path.basename(exe), total, have, trapn, miss))
    if missing:
        print("               stubs: " + ", ".join("%s %d" % kv for kv in missing.most_common()))
    return miss


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    game = sys.argv[1]
    analysis = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "analysis")
    impl, traps = parse_tables(root)
    exports = recompiled_exports(analysis, RECOMPILED)
    if not any(exports.values()):
        print("note: analysis/functions_*.json not found; recompiled exports are not counted")
    for name in ("farcry2.exe", "Dunia.dll", "binkw32.dll"):
        path = os.path.join(game, "bin", name)
        if os.path.exists(path):
            report(path, impl, traps, exports)


if __name__ == "__main__":
    main()
