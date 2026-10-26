#!/bin/sh
# Regenerates gen/ (recompiled C) from your own copy of Far Cry 2 (PC, 2008,
# GOG "Fortune's Edition" or the matching retail 1.0.0.1 build).
#
# Usage: tools/regen_gen.sh GAME_DIR
#
# GAME_DIR is the install folder, the one that contains bin/ and Data_Win32/
# (GOG installs it under "Far Cry 2", retail on the DVD root). Nothing is
# copied into the repository: gen/ and analysis/ are git-ignored and derived
# from your game, so they must never be redistributed.
#
# FC2.dll is NOT recompiled: it is SecuROM "Activate & Play", nothing imports
# it, and the GOG build never loads it. It is not read here.
#
# Needs Python 3.10+: pip install "capstone>=5.0.7" pefile==2024.8.26
# (unicorn==2.0.1.post1 is only needed by tools/difftest.py)
set -e
G=$(cd "${1:?usage: tools/regen_gen.sh GAME_DIR}" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
find_file() { for f in "$G"/bin/* "$G"/*; do
    [ "$(echo "${f##*/}" | tr 'A-Z' 'a-z')" = "$1" ] && { echo "$f"; return; }; done
    echo "missing $1 in $G or $G/bin" >&2; exit 1; }
EXE=$(find_file farcry2.exe)
DUNIA=$(find_file dunia.dll)
BINK=$(find_file binkw32.dll)
mkdir -p analysis gen
export LIFT_GAME=fc2
# discover.py imports peimg.py from tools/, so run it from there.
(cd tools && python3 discover.py "$EXE" ../analysis/functions_fc2.json)
(cd tools && python3 discover.py "$DUNIA" ../analysis/functions_dunia.json 10000000)
(cd tools && python3 discover.py "$BINK" ../analysis/functions_bink.json 09000000)
# The engine is 14 MB of code; many small buckets keep each compile unit small
# (one bucket is one cc1/clang process, and Android runners have little RAM).
LIFT_FILES=256 python3 tools/lift.py "$EXE" analysis/functions_fc2.json gen fc2
LIFT_FILES=384 python3 tools/lift.py "$DUNIA" analysis/functions_dunia.json gen dunia 10000000
LIFT_FILES=24 python3 tools/lift.py "$BINK" analysis/functions_bink.json gen bink 09000000
echo "gen/ is ready: $(ls gen/*.c | wc -l) files, $(du -sh gen | cut -f1)"
