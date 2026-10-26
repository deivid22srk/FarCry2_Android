#!/bin/sh
# Runs the desktop test against a Far Cry 2 install, in place.
# usage: desktop/run.sh "<game dir>"      (LB_LOG=1 for logging)
# The game dir must contain bin/ and Data_Win32/ (the folder the launcher
# would be pointed at). Output goes to "<game dir>/port/log.txt".
cd "$(dirname "$0")/.."
GAME=${1:?game dir}
exec ./build/lb "$GAME"
