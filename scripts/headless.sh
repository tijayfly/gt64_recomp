#!/usr/bin/env bash
# Run the game headless (Xvfb + any Vulkan driver, e.g. lavapipe) for N seconds and save screenshots.
# Usage: scripts/headless.sh <seconds> [shot interval seconds] [out dir]
# Env: GT_INPUT=<file> injects controller 1 input (see main.cpp), other GT_* debug variables pass through.
set -uo pipefail
cd "$(dirname "$0")/.."
secs=${1:-20}; every=${2:-5}; out=${3:-shots}
mkdir -p "$out"
export DISPLAY=:99
pgrep -f "Xvfb :99" > /dev/null || { Xvfb :99 -screen 0 1280x720x24 > /dev/null 2>&1 & sleep 1; }
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} SDL_VIDEODRIVER=x11
GT_ROM="${GT_ROM:-rom/GT64 - Championship Edition (USA).z64}" build/app/GT64Recompiled > "$out/run.log" 2>&1 &
pid=$!
t=0
while (( t < secs )) && kill -0 $pid 2> /dev/null; do
    sleep "$every"; t=$((t + every))
    import -window root "$out/t$(printf %03d $t).png" 2> /dev/null
done
kill $pid 2> /dev/null; sleep 1; kill -9 $pid 2> /dev/null
echo "log: $out/run.log"; ls "$out"
