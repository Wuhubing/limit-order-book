#!/usr/bin/env bash
# scripts/run_bench.sh — drive the full Stage C load matrix reproducibly.
#
# For each committed workload config under bench/workloads/:
#   1. generate the corpus (skipped if already present and its sha256 matches
#      the config, tracked via a sidecar .meta file),
#   2. run batch mode and latency mode (default rounds=7),
#   3. write results/stageC/<name>.batch.json and .latency.json.
#
# Re-runnable. Prints a one-line summary table at the end.
#
# Usage:
#   scripts/run_bench.sh            # full run, 7 rounds
#   scripts/run_bench.sh --quick    # fast smoke run, 2 rounds
#   scripts/run_bench.sh --rounds N # override round count

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH_BIN="${BENCH_BIN:-$ROOT/build/bench/lob_bench}"
WORKLOAD_DIR="$ROOT/bench/workloads"
DATA_DIR="$ROOT/build/benchdata"
RESULTS_DIR="$ROOT/results/stageC"
ROUNDS=7

while [ $# -gt 0 ]; do
  case "$1" in
    --quick) ROUNDS=2; shift ;;
    --rounds) ROUNDS="$2"; shift 2 ;;
    *) shift ;;
  esac
done

if [ ! -x "$BENCH_BIN" ]; then
  echo "error: lob_bench binary not found at $BENCH_BIN (build first)" >&2
  exit 1
fi

mkdir -p "$DATA_DIR" "$RESULTS_DIR"

gen_corpus() {
  local config="$1" corpus="$2" meta="$3"
  local config_sha corpus_sha
  config_sha="$(shasum -a 256 "$config" | awk '{print $1}')"

  if [ -f "$corpus" ] && [ -f "$meta" ]; then
    local stored_cfg stored_corpus actual
    stored_cfg="$(sed -n '1p' "$meta")"
    stored_corpus="$(sed -n '2p' "$meta")"
    if [ "$stored_cfg" = "$config_sha" ]; then
      actual="$(shasum -a 256 "$corpus" | awk '{print $1}')"
      if [ "$actual" = "$stored_corpus" ]; then
        return 0
      fi
    fi
    rm -f "$corpus" "$meta"
  fi

  local gen_log
  gen_log="$("$BENCH_BIN" gen --workload "$config" --out "$corpus")"
  corpus_sha="$(printf '%s\n' "$gen_log" | sed -n 's/^sha256: //p' | head -1)"
  if [ -z "$corpus_sha" ]; then
    echo "error: gen failed for $config" >&2
    echo "$gen_log" >&2
    exit 1
  fi
  printf '%s\n%s\n' "$config_sha" "$corpus_sha" > "$meta"
}

summary=""
summary+="$(printf '%-24s %14s %14s\n' 'workload' 'reqs_per_sec' 'p99_all(ns)')\n"

for config in "$WORKLOAD_DIR"/*.json; do
  name="$(basename "$config" .json)"
  corpus="$DATA_DIR/$name.txt"
  meta="$corpus.meta"

  gen_corpus "$config" "$corpus" "$meta"

  "$BENCH_BIN" run --workload "$config" --corpus "$corpus" --rounds "$ROUNDS" \
    --mode batch --out "$RESULTS_DIR/$name.batch.json"
  "$BENCH_BIN" run --workload "$config" --corpus "$corpus" --rounds "$ROUNDS" \
    --mode latency --out "$RESULTS_DIR/$name.latency.json"

  reqs="$(python3 -c "import json,sys;d=json.load(open(sys.argv[1]));print('%.1f' % d['reqs_per_sec_all_ops'])" "$RESULTS_DIR/$name.batch.json")"
  p99="$(python3 -c "import json,sys;d=json.load(open(sys.argv[1]));print('%.0f' % d['latency']['all']['p99'])" "$RESULTS_DIR/$name.latency.json")"
  summary+="$(printf '%-24s %14s %14s\n' "$name" "$reqs" "$p99")\n"
done

printf '%b\n' "$summary"
echo "done: $(ls "$WORKLOAD_DIR"/*.json | wc -l | tr -d ' ') workloads, $(ls "$RESULTS_DIR"/*.json 2>/dev/null | wc -l | tr -d ' ') result files in $RESULTS_DIR"
