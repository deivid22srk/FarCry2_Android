#!/bin/sh
# Headless run of the desktop harness: Xvfb + Mesa lavapipe + DXVK's native
# d3d9, so no display or GPU is needed. Same binary as desktop/run.sh.
# usage: desktop/run_headless.sh "<game dir>" [save dir]
# Needs: xvfb, mesa-vulkan-drivers (lavapipe), and build/lb (desktop/build.sh).
# Logging goes to stderr (LB_LOG=1 is the default here).
set -e
cd "$(dirname "$0")/.."
GAME=${1:?usage: desktop/run_headless.sh "<game dir>" [save dir]}
SAVE=${2:-"$GAME/port"}
mkdir -p "$SAVE"

export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-"${TMPDIR:-/tmp}/lb-xdg"}
mkdir -p "$XDG_RUNTIME_DIR"
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-x11}
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}
export LB_LOG=${LB_LOG:-1}

exec xvfb-run -a -s "-screen 0 1280x720x24" ./build/lb "$GAME" "$SAVE"