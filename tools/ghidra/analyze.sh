#!/bin/bash
set -euo pipefail
work="$HOME/ghidra-oot"
mkdir -p "$work"
cp -u /mnt/d/Games/Emulators/recomp/build/ghidra/main.elf "$work/main.elf"
export GHIDRA_HEADLESS_MAXMEM=10G
if [ -d "$work/oot.rep" ] && [ "${1:-}" != "--reimport" ]; then
    input=(-process main.elf)
else
    input=(-import "$work/main.elf" -overwrite -loader ElfLoader -loader-imagebase 7000000000 -processor AARCH64:LE:64:v8A)
fi
if [ "${1:-}" = "--script" ]; then
    shift
    name="$1"; shift
    /opt/ghidra_12.1.4_PUBLIC/support/analyzeHeadless "$work" oot -process main.elf -noanalysis -readOnly \
        -scriptPath /mnt/d/Games/Emulators/recomp/tools/ghidra/scripts -postScript "$name" "$@" \
        -log "$work/script.log" 2>&1 | grep -v "^    /opt\|INFO  Class searcher\|INFO  Initializing\|INFO  Random\|INFO  Trust\|cache cleanup"
    exit
fi
/opt/ghidra_12.1.4_PUBLIC/support/analyzeHeadless "$work" oot "${input[@]}" \
    -max-cpu 20 \
    -log "$work/analyze.log" 2>&1 | tee "$work/console.log"
echo "Ghidra analysis finished at $(date)"
