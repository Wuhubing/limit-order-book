#!/usr/bin/env bash
# scripts/bench_compare.sh — Stage D interleaving tool.
#
# Alternates batch runs of two lob_bench binaries (two commits/checkouts) over
# the SAME corpora: A,B,A,B,... x rounds, writing a side-by-side JSON per
# workload. Interleaving cancels thermal / frequency drift for a fair A/B.
#
# Usage:
#   scripts/bench_compare.sh DIR_A DIR_B [rounds] [outdir]
#
# DIR_A / DIR_B each resolve to a lob_bench binary by trying, in order:
#   <DIR>/lob_bench, <DIR>/bench/lob_bench, <DIR>/build/bench/lob_bench
# If DIR_A is "self", the current build's binary is used.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKLOAD_DIR="$ROOT/bench/workloads"
DATA_DIR="$ROOT/build/benchdata"
RESULTS_DIR="$ROOT/results/stageC-compare"

if [ $# -lt 2 ]; then
  echo "usage: $0 DIR_A DIR_B [rounds] [outdir]" >&2
  exit 2
fi

DIR_A="$1"; DIR_B="$2"
ROUNDS="${3:-7}"
if [ $# -ge 4 ]; then RESULTS_DIR="$4"; fi

resolve_bin() {
  local dir="$1"
  if [ "$dir" = "self" ]; then
    printf '%s' "$ROOT/build/bench/lob_bench"
    return 0
  fi
  local cand
  for cand in "$dir/lob_bench" "$dir/bench/lob_bench" "$dir/build/bench/lob_bench"; do
    if [ -x "$cand" ]; then printf '%s' "$cand"; return 0; fi
  done
  echo "error: no lob_bench binary found under $dir" >&2
  return 1
}

BIN_A="$(resolve_bin "$DIR_A")"
BIN_B="$(resolve_bin "$DIR_B")"

mkdir -p "$DATA_DIR" "$RESULTS_DIR"

for config in "$WORKLOAD_DIR"/*.json; do
  name="$(basename "$config" .json)"
  corpus="$DATA_DIR/$name.txt"

  if [ ! -f "$corpus" ]; then
    "$BIN_A" gen --workload "$config" --out "$corpus" >/dev/null
  fi

  out="$RESULTS_DIR/$name.compare.json"
  {
    printf '{\n'
    printf '  "workload": "%s",\n' "$name"
    printf '  "corpus": "%s",\n' "$corpus"
    printf '  "rounds": %s,\n' "$ROUNDS"
    printf '  "runs": [\n'
    first=1
    for ((i = 0; i < ROUNDS; i++)); do
      for bin in A B; do
        exe="$BIN_A"; [ "$bin" = "B" ] && exe="$BIN_B"
        tmp="$DATA_DIR/$name.$bin.$i.json"
        "$exe" run --workload "$config" --corpus "$corpus" --rounds 1 \
          --mode batch --out "$tmp"
        t="$(python3 -c "import json,sys;d=json.load(open(sys.argv[1]));print(d['round_times_sec'][0])" "$tmp")"
        rps="$(python3 -c "import json,sys;d=json.load(open(sys.argv[1]));print(d['reqs_per_sec_all_ops'])" "$tmp")"
        rm -f "$tmp"
        [ "$first" = "1" ] && first=0 || printf ',\n'
        printf '    { "index": %d, "binary": "%s", "round_time_sec": %s, "reqs_per_sec_all_ops": %s }' \
          "$i" "$bin" "$t" "$rps"
      done
    done
    printf '\n  ]\n}\n'
  } > "$out"
  echo "wrote $out"
done

echo "done: alternating A/B batch runs over $(ls "$WORKLOAD_DIR"/*.json | wc -l | tr -d ' ') workloads"
