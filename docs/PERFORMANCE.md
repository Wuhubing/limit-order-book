# Performance report

All claims below are backed by recorded experiments. Raw data:
`results/stageC/` (baseline dataset, 14 JSONs), `results/stageD/matrix/`
(ablation matrix, 56 batch + 56 latency JSONs across 4 variants × 7 loads),
`results/stageD/RESULTS.md` (LOB-005 before/after), `results/stageD/ABLATION.md`
(summary table). Regenerate figures/tables with `scripts/plot_results.py`.

## 1. Environment (as recorded in every result JSON)

| | |
|---|---|
| CPU | Apple M4 Pro (14 cores; single-threaded bench) |
| OS | macOS 26.4.1 (25E253), arm64 |
| Compiler | Apple clang 16.0.0 (clang-1600.0.26.6), C++20 |
| Flags | Release `-O2` (upstream default, kept), ninja, cmake 4.4.3 |
| googletest | v1.14.0 (FetchContent) |
| Engine commit (baseline) | `05b4273` (engine v1, pre-optimization) |
| Engine commits (opt A / B) | `f7e8b9c` (pools) / `9f25029` (cached heights) |

## 2. Methodology (what the numbers mean)

- **Loads** (7, defined in `bench/workloads/*.json`): 300,000 requests each,
  seed 20260907, canonical Add/Mkt/Cxl/Mod text corpus, degenerateRate 0.
  Generated once per load; corpus sha256 pinned; identical corpus across all
  variants (deterministic generator, `lob_bench gen`).
- **Two modes** per load per variant:
  - *batch*: whole-corpus replay on a fresh `Book` per round, timed with
    `steady_clock`; fill sink attached to a pre-reserved vector; fill hash
    folded after the round (outside the timed region). Throughput denominator =
    **all** requests including cancels (`reqs_per_sec_all_ops`).
  - *latency*: per-request `steady_clock` around each engine call, bucketed by
    op type; p50/p95/p99; clock overhead measured (~41 ns/pair) and reported.
- 1 warmup round; batch reports 7 measured rounds, latency 3. Timed region =
    **engine service time only**: corpus parse/generation, random number
    generation, checksumming, and state inspection are excluded. No network, no
    queueing — this is the matching core, not an exchange endpoint.
- Both modes attach the fill sink and consume the events (hash) so output cost
  is identical across variants and no dead-code elimination can occur.

## 3. Profiling evidence (why these two experiments)

`sample` (macOS, 6 s, Release -O2) on the pre-optimization engine:

| Load | hot spot | share of samples |
|---|---|---|
| large_low_density (~30k price levels) | `Book::getLimitHeight` recursion (from `balance`/`insert`) | 59% |
| add_cancel_dense (realistic mixed) | same | 42% |
| both | `operator new`/free paths | < 0.5% |

The AVL balance path recomputed subtree heights by full recursion on **every**
rebalance step → O(subtree) per tree mutation. Allocation was real but
secondary. Experiment B (cached heights) targets the former; experiment A
(slab pools) targets the latter with an honest expectation of a modest gain.

## 4. Experiment A — address-stable Order/Limit slab pools (LOB-005, commit f7e8b9c)

- Change: chunked slab pools (4096 → doubling), intrusive free list, all 15
  construction/destruction sites routed through the pool; remaining allocations
  documented (map nodes, sink vector, chunk growth).
- Correctness: 123 + 18/20 + 19 suites green (Release + UBSan, differential
  incl. I1–I7); grep-clean of direct new/delete in the engine.
- Result (matched env, 7 rounds; from `results/stageD/RESULTS.md`):

| load | req/s before → after | alloc count | alloc bytes |
|---|---|---|---|
| fill_dense | 1.91M → 1.96M (+2.8%) | −50% | −41% |
| add_cancel_dense | 1.17M → 1.23M (+4.5%) | −50% | −39% |
| level_churn | 5.24M → 5.80M (+10.6%) | −50% | −38% |

- Reading: allocation was secondary (as profiled); pool gain is small alone but
  additive (+3–19%) once experiment B removes the tree bottleneck. Memory
  cost: pool chunk memory is retained for the Book lifetime (never returned to
  the OS mid-run) — the price of O(1) reuse; bounded by peak concurrency.

## 5. Experiment B — cached AVL node heights (LOB-006, commit 9f25029)

- Change: `Limit.height` cache; `limitHeightDifference` O(1); refresh at every
  insert/rotate/delete-rebalance point (children-first, bottom-up). No public
  API change; `getLimitHeight` kept as the independent I6 audit path.
- Correctness: all suites green incl. the 20-seed differential fuzz whose I6
  invariant recomputes **true** heights after every request; 2 new regression
  tests (root delete, successor==right-child, single-child, stop trees).
- Spot-verified independently by the reviewer (batch 5 rounds): level_churn
  10.4M/s, large_low_density 4.26M/s.

## 6. Ablation matrix (matched environment — canonical dataset)

Same machine, same corpora, 4 builds. Batch mode, 7 rounds/cell:
`results/stageD/matrix/<variant>/<load>.batch.json`.

| load | base | +A pools | +B heights | A+B | vs base |
|---|---|---|---|---|---|
| add_cancel_dense | 1,158,271 | 1,176,542 | 6,992,492 | 7,885,612 | 6.8× |
| fill_dense | 1,862,430 | 1,984,380 | 6,957,197 | 7,747,236 | 4.2× |
| level_churn | 5,057,764 | 5,454,283 | 9,172,153 | 10,943,789 | 2.2× |
| small_low_density | 3,140,214 | 3,234,262 | 6,900,794 | 8,060,983 | 2.6× |
| small_high_density | 7,279,340 | 7,781,222 | 10,671,312 | 11,447,227 | 1.6× |
| large_low_density | 61,302 | 63,605 | 4,072,608 | 4,379,115 | **71×** |
| large_high_density | 914,181 | 937,890 | 6,285,296 | 7,223,661 | 7.9× |

Per-request service latency p50 (all ops, ns) — `results/stageD/matrix/<v>/<load>.latency.json`:

| load | base | +A | +B | A+B |
|---|---|---|---|---|
| add_cancel_dense | 958 | 958 | 125 | 125 |
| fill_dense | 541 | 500 | 125 | 125 |
| large_low_density | 16,167 | 15,666 | 250 | 208 |

p95/p99 and per-op-type tables are in the raw JSONs (e.g. large_low_density base
Add p50 ≈ 15.3 µs → A+B ≈ 208 ns).

### Reading
1. B is the dominant optimization (1.5×–66× by load): removing O(subtree)
   balance recomputation turns tree mutations into true O(log n).
2. A alone is modest (+1.6%–+7.8%) — matches the profiling prediction; its
   value shows as an additive +3%–+19% over B (pool locality once the tree cost
   is gone).
3. No load regressed in any variant. Fill hashes are identical across variants
   (semantic identity), and the differential suite passed at every commit.

## 7. Unfavorable scenarios and limits of the optimizations

- **Map node churn remains**: with pools active, per-op allocation is not zero —
  `orderMap`/limit-map `unordered_map` nodes still allocate on insert/erase. A
  further experiment could pool or flat-map those; we did not, and do not claim
  a zero-alloc engine.
- **The height cache adds 4 B/level** and one branch per balance decision; on
  tiny books (small_high_density) the win is only 1.6× — the floor is then
  hash-map + fill-push cost (~125 ns p50), not the tree.
- **Pool memory is retained** (never shrinks mid-run) — fine for a long-lived
  matching core, wrong for a process that creates/destroys Books at high rate
  with low peak concurrency.
- **Not measured**: multi-threaded throughput, network/queue latency, real
  market-data feeds. Engine service time only. Absolute numbers are
  M4-Pro-specific; relative deltas are the robust claim.
- The two large-* workloads take ~4 min to generate (reference-model
  bookkeeping in the generator) — a tooling cost, excluded from measurements.

## 8. Reproducing this report

```sh
bash scripts/run_bench.sh          # regenerates corpora (sha-skip), runs 7 loads,
                                   # writes results/stageC/*.json
bash scripts/matrix_bench.sh <corpus-dir> <out-dir> \
    "base=<wt-base>" "a=<wt-a>" "b=<wt-b>" "ab=<repo>"   # ablation matrix
python3 scripts/plot_results.py results/stageD/matrix results/figures   # figures + CSVs
```
Exact variant worktrees/commits and commands: `docs/REPRODUCE.md`. No perf gates
in CI — this dataset is a local, single-machine artifact by design.
