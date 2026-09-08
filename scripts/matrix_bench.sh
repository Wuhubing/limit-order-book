#!/bin/bash
# Stage D ablation matrix runner.
# Variants: base (pre-optimization), a-only (pools), b-only (cached heights), ab (both).
# Usage: matrix_bench.sh <corpus_dir> <out_dir> <variant_worktree>... (each arg: NAME=PATH)
set -u
CORPUS_DIR="$1"; OUT_DIR="$2"; shift 2
export PATH="/opt/homebrew/bin:$PATH"
LOADS=(add_cancel_dense fill_dense level_churn small_low_density small_high_density large_low_density large_high_density)
mkdir -p "$OUT_DIR"
for spec in "$@"; do
  NAME="${spec%%=*}"; WT="${spec#*=}"
  mkdir -p "$OUT_DIR/$NAME"
  for w in "${LOADS[@]}"; do
    corpus="$CORPUS_DIR/$w.txt"
    [ -f "$corpus" ] && continue
    (cd "$WT" && ./build/bench/lob_bench gen --workload "bench/workloads/$w.json" --out "$corpus" > /dev/null 2>&1 && echo "gen $w ok") || echo "gen $w FAILED"
  done
done
for spec in "$@"; do
  NAME="${spec%%=*}"; WT="${spec#*=}"
  echo "=== variant $NAME ==="
  (cd "$WT" && cmake --build build -j8 > /dev/null 2>&1) || { echo "build $NAME FAILED"; continue; }
  for w in "${LOADS[@]}"; do
    corpus="$CORPUS_DIR/$w.txt"
    [ -f "$corpus" ] || { echo "skip $w (no corpus)"; continue; }
    (cd "$WT" && ./build/bench/lob_bench run --workload "bench/workloads/$w.json" --corpus "$corpus" --rounds 7 --mode batch --out "$OUT_DIR/$NAME/$w.batch.json" > /dev/null 2>&1 && echo "  $w batch ok") || echo "  $w batch FAILED"
    (cd "$WT" && ./build/bench/lob_bench run --workload "bench/workloads/$w.json" --corpus "$corpus" --rounds 3 --mode latency --out "$OUT_DIR/$NAME/$w.latency.json" > /dev/null 2>&1 && echo "  $w latency ok") || echo "  $w latency FAILED"
  done
done
echo "MATRIX DONE -> $OUT_DIR"
