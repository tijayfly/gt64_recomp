#!/usr/bin/env bash
# Regenerate syms.toml from the splat disassembly and run N64Recomp.
set -euo pipefail
cd "$(dirname "$0")/.."

python3 -I scripts/gen_syms.py syms.toml symbols/libultra.txt,symbols/manual.txt
rm -rf RecompiledFuncs && mkdir -p RecompiledFuncs
./build/n64recomp/N64Recomp gt64.us.toml > recomp.log 2>&1 || { tail -30 recomp.log; exit 1; }
grep -i error recomp.log && exit 1
echo "OK: $(grep -c '{ name =' syms.toml) functions recompiled into RecompiledFuncs/"

mkdir -p rsp
./build/n64recomp/RSPRecomp n_aspMain.us.toml
echo "OK: rsp/n_aspMain.cpp"
