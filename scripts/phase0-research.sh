#!/usr/bin/env bash
# Phase 0 research spike: establish, empirically, how pocl's CPU device compiles and
# schedules work-items, and whether LLVM's DWARF survives into the object pocl dlopen()s.
# Outputs land in docs/phase0/ so the findings are reviewable outside the container.
set -euo pipefail
OUT=${OUT:-/work/docs/phase0}
mkdir -p "$OUT" /tmp/b
gcc -g -O0 /work/tests/phase0/probe.c -o /tmp/b/probe -lOpenCL

collect() { # name, build-options, [env...]
  local name="$1" opts="$2"; shift 2
  local cache="/tmp/cache_$name"
  rm -rf "$cache"; mkdir -p "$cache"
  env POCL_CACHE_DIR="$cache" "$@" /tmp/b/probe /work/kernels/vecadd.cl "$opts" \
      > "$OUT/$name.run.txt" 2>&1 || { echo "run failed: $name"; cat "$OUT/$name.run.txt"; return 1; }
  local so; so=$(find "$cache" -name '*.so' | head -1)
  cp "$so" "$OUT/$name.so"
  {
    echo "### build options: $opts   extra env: $*"
    echo "### object: ${so#$cache/}"
    echo; echo "=== ELF sections ==="; readelf -S "$so" | grep -E 'debug|\.text|Name'
    echo; echo "=== symbols ==="; nm "$so"
    echo; echo "=== source line -> number of distinct machine addresses ==="
    llvm-dwarfdump-18 --debug-line "$so" | awk '/^0x/ {print $2}' | sort -n | uniq -c
    echo; echo "=== DIE tree ==="; llvm-dwarfdump-18 --debug-info "$so"
    echo; echo "=== location lists ==="; llvm-dwarfdump-18 --debug-loc "$so"
    echo; echo "=== line table ==="; llvm-dwarfdump-18 --debug-line "$so"
    echo; echo "=== disassembly ==="; objdump -d --no-show-raw-insn "$so" | sed -n '/Disassembly/,$p'
  } > "$OUT/$name.dwarf.txt" 2>&1
  echo "wrote $OUT/$name.dwarf.txt"
}

collect optimized      "-g"
collect optdisabled    "-g -cl-opt-disable"
collect loops          "-g -cl-opt-disable" POCL_WORK_GROUP_METHOD=loops
echo "done"
