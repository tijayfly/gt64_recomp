#!/usr/bin/env bash
# Drive a running headless game: scripts/press.sh <out.png> BUTTON... (each pressed 0.3 s then released, 1.2 s apart).
# Uses the GT_INPUT file at $GT_INPUT_FILE. Screenshot taken 1.5 s after the last press.
out=$1; shift
for b in "$@"; do echo "$b" > "$GT_INPUT_FILE"; sleep 0.3; : > "$GT_INPUT_FILE"; sleep 1.2; done
sleep 1.5; DISPLAY=:99 import -window root "$out"
