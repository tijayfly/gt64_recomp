#!/usr/bin/env bash
# Start a headless run and drive it into a Free Run race (Championship round 1), holding A to accelerate.
#   scripts/torace.sh <out dir> [extra seconds in race]   (env passes through to the game, e.g. GT_VPLOG=1)
# Screenshots: race0.png ~8 s into the race, race1.png <extra> s later.
cd "$(dirname "$0")/.."
out=$(realpath -m "$1"); extra=${2:-10}
rm -rf "$out"; mkdir -p "$out"; export GT_INPUT_FILE="$out/in.txt"; : > "$GT_INPUT_FILE"
(GT_MODELOG=1 GT_INPUT="$GT_INPUT_FILE" scripts/headless.sh $((300 + extra)) ${SHOT_EVERY:-1000} "$out" > /dev/null 2>&1 &)
mode() { grep "^\[mode\]" "$out/run.log" 2>/dev/null | tail -1 | cut -d' ' -f2; }
press() { echo "$1" > "$GT_INPUT_FILE"; sleep 0.3; : > "$GT_INPUT_FILE"; sleep 1.2; }
sleep 20
for i in $(seq 1 40); do [[ "$(mode)" == 8000416C ]] && break; press START; [[ "$(mode)" == 8000416C ]] && break; press A; done
sleep 4; press DD; press A
for i in $(seq 1 40); do [[ "$(mode)" == 800046C4 ]] && break; sleep 1; done
echo A > "$GT_INPUT_FILE"; sleep 12
DISPLAY=:99 import -window root "$out/race0.png"; sleep "$extra"
DISPLAY=:99 import -window root "$out/race1.png"
: > "$GT_INPUT_FILE"
pkill -f "^bash scripts/headless"; pkill -f "[b]uild/app/GT64Recompiled"; sleep 1; pkill -9 -f "[b]uild/app/GT64Recompiled"
