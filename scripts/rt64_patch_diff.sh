#!/usr/bin/env bash
# Write the diff of lib/rt64's current <files> against lib/rt64 with the earlier patches applied, as <out.patch>.
#   scripts/rt64_patch_diff.sh <out.patch> <earlier.patch ...> -- <file ...>
# For patches that touch files other patches also change: each patch must apply on top of the ones before it.
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(realpath -m "$1"); shift
pre=(); while [[ $1 != -- ]]; do pre+=("$(realpath "$1")"); shift; done; shift
tmp=$(mktemp -d)
git -C lib/rt64 worktree add --detach "$tmp/wt" HEAD -q
for p in "${pre[@]}"; do git -C "$tmp/wt" apply "$p"; done
git -C "$tmp/wt" add -A; git -C "$tmp/wt" -c user.name=x -c user.email=x commit -qm base
for f in "$@"; do mkdir -p "$tmp/wt/$(dirname "$f")"; cp "lib/rt64/$f" "$tmp/wt/$f"; done
git -C "$tmp/wt" add -A "$@"
git -C "$tmp/wt" diff --cached -- "$@" > "$out"
git -C lib/rt64 worktree remove --force "$tmp/wt"; rm -rf "$tmp"
echo "wrote $out"
