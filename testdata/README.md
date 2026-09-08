# Test datasets catalog

Every dataset below is deterministic (fixed seed or literal file) and validated by
replaying it through BOTH the engine and the independent reference model — identity
is required (fills + state after every request). Validation tool: `tools/diff_fuzz`.

## Canonical corpus format
One command per line, plain ASCII, no comments, no blank lines:
```
Add <id> <side> <qty> <price>     side: 0 = sell, 1 = buy
Mkt <id> <side> <qty>
Cxl <id>
Mod <id> <qty> <price>
```
Parser: `reference/Command.hpp` (`parseCorpus`); generation: `reference/WorkloadGenerator.hpp`
(`generateWorkload`). Malformed lines are parse errors — datasets are generated, not hand-edited
(exception: the adversarial files below are hand-crafted scenario scripts and are intentionally
small and readable; they are still validated by full differential replay).

## Inventory

### A. Random differential corpora — `corpora/` (committed, 500 requests each)
Generated with the seeded workload generator incl. 2% degenerate ops (duplicate live
id, qty<=0, price<=0, unknown cancel/modify, market on empty side) so reject/no-op
semantics are exercised:
- `mix-heavy.txt` (seed 100), `cancel-heavy.txt` (seed 101), `market-heavy.txt` (seed 102)
Purpose: deterministic offline differential regression input for CI / replay checks.

### B. Adversarial scenario corpora — `corpora/adversarial/` (committed, hand-crafted)
Each targets one edge class (see filenames): boundary ticks (1 and 1,000,000),
qty extremes and exact-boundary exhaustion, duplicate-live-id rejection and id reuse
after death, cancel storms (head/middle/tail/last + unknown), modify storms
(same-price FIFO loss, cross-modify, invalid args), market edge cases (empty book,
one-sided, remainder drop), FIFO discipline in a 30-order deep queue, price-ladder
level churn (AVL insert/delete), id-reuse cycles, and multi-level cross sweeps on
both sides with trailing partial fills. All validated: `diff_fuzz --replay` exit 0.

### C. Benchmark workloads — `bench/workloads/*.json` + corpora under `build/benchdata/`
Seven 300,000-request load shapes (seed 20260907, degenerateRate 0) covering the
Stage-C matrix — add/cancel dense, fill dense (multi-level sweeps), level churn
(narrow price band), and {small, large} active book x {low, high} price density:
`add_cancel_dense`, `fill_dense`, `level_churn`, `small_low_density`,
`small_high_density`, `large_low_density`, `large_high_density`.
Configs are committed; corpus text is regenerated deterministically by
`lob_bench gen` (sha256-pinned; cached in build/benchdata/). Replay validity of a
workload corpus can be checked with `diff_fuzz --replay` (exit 0).

### D. Real market data (LOBSTER NASDAQ samples) — `testdata/real/`
AAPL and MSFT 2012-06-21 full-day NASDAQ order flow from LOBSTER's free sample
files, converted to the canonical corpus format (mapping + honest limitations in
`testdata/real/README.md`). Committed: 50,000-op slices + golden records; full
days (~380k/~663k ops) regenerable via the documented download+convert commands.
Validated: differential identity (engine == reference) over the full days and
byte-identical replay determinism.

## Regenerating / validating everything
```sh
# corpora A: (contents committed; regenerate with seeds 100/101/102 via the generator
#  or `tools/diff_fuzz` generate mode)
# corpora B:
build/test/diff_fuzz --replay testdata/corpora/adversarial/*.txt   # expect exit 0 each
# corpora C:
./build/bench/lob_bench gen --workload bench/workloads/<name>.json --out build/benchdata/<name>.txt
build/test/diff_fuzz --replay build/benchdata/<name>.txt            # expect exit 0
```
