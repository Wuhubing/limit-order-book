# Stage D, Experiment B — Cached node heights in the AVL balance path (LOB-006)

> This directory holds both Stage D experiments. `RESULTS.md` is Experiment A
> (LOB-005, order/lifetime pools). This file is Experiment B (LOB-006, cached
> AVL heights). Raw JSONs: `baseline/` = pre-change engine on this machine;
> top-level `<load>.batch.json` / `<load>.latency.json` = post-change engine.

## What changed

The engine stored no per-node height or balance factor; every `balance()` call
recomputed subtree heights via `Book::getLimitHeight` — a full recursive walk
over each subtree — on every insert/delete rebalance step, making each step
O(subtree). With tens of thousands of levels this dominated the profile
(Hermes/sample: `getLimitHeight` = 59% of samples on `large_low_density`, 42% on
`add_cancel_dense`).

Fix: each `Limit` now carries a cached `int height` (`Limit.hpp`, set to 1 in
the constructor; `nullptr` child ⇒ height 0). Two O(1) helpers in `Book.cpp`
(`heightOf`, `refresh`) maintain it, and `limitHeightDifference()` reads the
cache instead of recursing. The cache is refreshed at every mutation point:

- `insert()` / `insertStop()`: bottom-up `refresh(root)` after each child insert
  (before `balance()`);
- all eight rotations (`ll/rr/lr/rl` + the `Stop` twins): refresh the two rotated
  nodes, children first;
- `deleteLimit()` / `deleteStopLevel()`: the successor splice and child relink
  are covered by the existing `rebalanceUpward` walk, which now `refresh(node)`
  before every `balance()` — this covers the successor's old-parent chain, the
  replacement node, and every ancestor up to the root.

`Book::getLimitHeight` is unchanged (public API, still recursive — used by the
harness/tests to recompute true structure). No engine hot path calls it any more.

## Correctness

- `balance()`/`limitHeightDifference()`/rebalance paths no longer recurse
  (`grep getLimitHeight Book.cpp` → only the definition + its own two recursive
  calls remain).
- All three suites green under Release and UBSan, including the full differential
  fuzz (20 seeds × 2000 requests) whose I6 invariant recomputes true heights
  recursively and independently — the cache-drift backstop.
- Fill-event hashes are bit-identical between baseline and after on every load,
  and match the committed Stage C hashes; `alloc_count` is unchanged
  (2,463,329 on `large_low_density` before and after) — the change is pure
  compute, no new allocations (the +4-byte field rides inside the pooled `Limit`).
- Two new height-cache regression tests in `EngineCorrectnessTests`
  (`HeightCacheMatchesTrueHeightAcrossChurn`, `HeightCacheMatchesTrueHeightStopTrees`)
  assert cached height == independently recomputed true height at every node
  after add/cancel/modify churn, root deletion, the successor==direct-right-child
  delete case, single-child splice, and stop-tree churn (I6 does not cover stop
  trees).

## Measurement method

Same corpora as Stage C (regenerated here with identical sha256: `fill_dense`
`210cad9e…`, `add_cancel_dense` `89083b4f…`, `level_churn` `dc06f535…`,
`large_low_density` `4ccfeb7e…`, `large_high_density` `44582f6a…`).
Baseline = pre-change commit (this worktree HEAD before the edit) and after =
the cached-height commit, both built Release `-O2` with AppleClang 16 on this
machine (Apple M4 Pro, macOS 25.4). `lob_bench` batch (7 measured rounds + 1
warmup) and latency (7 rounds). Rounds are the run-to-run variance source; they
were stable (e.g. `large_low_density` after: 61–70 ms across the 7 rounds).

## Results (measured, per load; delta = after vs baseline)

| workload | mode | metric | baseline | after | delta |
|---|---|---|---|---|---|
| fill_dense | batch | req/s | 1,973,602 | 8,042,657 | +307.5% |
| fill_dense | latency | all p50 ns | 500 | 125 | −75.0% |
| fill_dense | latency | all p99 ns | 1,916 | 417 | −78.2% |
| add_cancel_dense | batch | req/s | 1,201,048 | 8,219,015 | +584.3% |
| add_cancel_dense | latency | all p50 ns | 917 | 125 | −86.4% |
| add_cancel_dense | latency | all p99 ns | 3,709 | 459 | −87.6% |
| level_churn | batch | req/s | 5,777,659 | 11,150,563 | +93.0% |
| level_churn | latency | all p50 ns | 42 | 42 | 0.0% |
| level_churn | latency | all p99 ns | 875 | 375 | −57.1% |
| large_low_density | batch | req/s | 64,796 | 4,510,207 | +6860.6% |
| large_low_density | latency | all p50 ns | 15,459 | 208 | −98.7% |
| large_low_density | latency | all p99 ns | 51,666 | 666 | −98.7% |
| large_high_density | batch | req/s | 956,332 | 7,186,587 | +651.5% |
| large_high_density | latency | all p50 ns | 1,042 | 125 | −88.0% |
| large_high_density | latency | all p99 ns | 4,958 | 500 | −89.9% |

`large_low_density` per-op latency (Add p50 15,125 → 208 ns, Cxl p50
17,292 → 250 ns, Mod p50 33,125 → 416 ns, Mkt p50 14,750 → 167 ns) all improve
~98–99%.

## Honest reading

- **The pathological load collapses**: `large_low_density` (~30k price levels)
  went from ~65k to ~4.5M req/s (~69×) and its p50 Add latency from ~15 µs to
  ~208 ns. This is exactly the load the profiling flagged (full-subtree
  recursion per balance step), and the win is the expected, headline effect.
- **Every load improved; no regression measured.** The smallest gain is
  `level_churn` p50 latency, which is flat (42 ns → 42 ns): at 1–2k levels and
  tiny trees the balance work was already negligible, so removing it doesn't
  move p50 — but p99 still drops ~57% and batch throughput rises ~93%.
- **Throughput ceilings have moved**: after the change the four smaller loads
  cluster around 8–11M req/s and the two large loads around 4.5–7.2M req/s. The
  remaining spread is dominated by per-load structure (queue depth, fill volume,
  level churn), not by AVL height recomputation.
- **Not claimed**: zero-alloc is out of scope here; allocation behaviour is
  unchanged from LOB-005 (`alloc_count` identical before/after). The AVL
  balance-factor cache is maintained at every mutation point and is guarded by
  the I6 true-height recompute in the differential fuzz plus the new
  EngineCorrectnessTests height-consistency checks.
