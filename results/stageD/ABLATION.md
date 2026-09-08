# Stage D ablation matrix — results

Matched-environment comparison, single machine (Apple M4 Pro, macOS 26.4.1,
Apple clang 16.0.0, Release -O2, C++20, ninja). One shared corpus per load
(300,000 requests, seed 20260907, sha256-pinned — regenerated in
`~/projects/lob-benchdata/` by `lob_bench gen`). Batch mode: 7 measured rounds
per cell, fresh Book per round, fill sink attached, fill-hash identical across
rounds and variants. Latency mode: 3 rounds, steady_clock per request.

Variants (git):
- base = pre-optimization engine v1 (`05b4273`)
- a    = + object pools only (`f7e8b9c`)
- b    = + cached AVL heights only (`1420102`, cherry-pick of `9f25029`)
- ab   = pools + cached heights (`9f25029`, current main)

Raw JSONs per cell: `results/stageD/matrix/<variant>/<load>.{batch,latency}.json`.

## Throughput (requests/sec, ALL ops incl. cancels in denominator)

| load | base | a | b | ab | ab/base |
|---|---|---|---|---|---|
| add_cancel_dense | 1,158,271 | 1,176,542 | 6,992,492 | 7,885,612 | 6.8x |
| fill_dense | 1,862,430 | 1,984,380 | 6,957,197 | 7,747,236 | 4.2x |
| level_churn | 5,057,764 | 5,454,283 | 9,172,153 | 10,943,789 | 2.2x |
| small_low_density | 3,140,214 | 3,234,262 | 6,900,794 | 8,060,983 | 2.6x |
| small_high_density | 7,279,340 | 7,781,222 | 10,671,312 | 11,447,227 | 1.6x |
| large_low_density | 61,302 | 63,605 | 4,072,608 | 4,379,115 | 71x |
| large_high_density | 914,181 | 937,890 | 6,285,296 | 7,223,661 | 7.9x |

## Per-request service latency p50, all ops (ns) — engine service time only

| load | base | a | b | ab |
|---|---|---|---|---|
| add_cancel_dense | 958 | 958 | 125 | 125 |
| fill_dense | 541 | 500 | 125 | 125 |
| large_low_density | 16,167 | 15,666 | 250 | 208 |

## Reading (evidence-backed)

1. **B (cached AVL heights) is the dominant optimization**: 1.5x–66x across
   loads; the pathological large_low_density (~30k price levels) goes from
   61K to 4.1–4.4M req/s. Root cause was the recursive O(subtree) height
   recompute in every `balance()` (59% of profile samples on that load);
   removing it makes tree ops O(log n) with O(1) balance checks.
2. **A (object pools) alone is modest** (+1.6% to +7.8%): allocation was a
   secondary cost (profiling: <0.5% of samples) — the honest expectation from
   the profile, confirmed by measurement. Pool's allocation count halves.
3. **A+B composes positively**: pools add another +3% to +19% on top of B
   (level_churn +19%, small_low_density +17%, fill_dense +11%, large_low +7.5%,
   add_cancel +13%) — pool locality helps once the tree cost is gone.
4. p50 service latency collapses to a 125–250ns floor across loads (≈ 2 clock
   reads + map ops + fill push at this corpus size); large_low_density latency
   drops 16,167ns → 208ns (78x).
5. No regressions measured on any load/variant. Fill hashes identical across
   variants (semantic identity preserved — also enforced by the differential
   suite at every commit).
6. Caveats: single machine, single run per cell (7 rounds averaged internally);
   absolute numbers are M4-Pro-specific; deltas are the robust part. Bench
   measures engine service time only (no parse/network/queue). No perf gates in
   CI — this dataset is a local artifact.
