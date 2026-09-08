# Stage D, Experiment A — Order/Lifetime memory pools (LOB-005)

## What changed

Added an address-stable, chunked slab pool (`Limit_Order_Book/Pool.hpp` +
`Pool.cpp`) and routed every Order/Limit creation and destruction site in the
engine through it. `Book` now owns two pools (`poolOrder_`, `poolLimit_`); the
previous `new Order`/`delete order`/`new Limit`/`delete limit` churn is gone
(`grep -E "new (Order|Limit)|delete (order|limit|headOrder|newOrder)"` over
`Limit_Order_Book/*.cpp` is empty).

Pool properties (see `Pool.hpp` for full documentation):

- **Address stability**: slots are carved from fixed chunks that are never
  reallocated or compacted, so raw pointers between Orders/Limits stay valid.
- **Chunked growth**: first chunk = 4096 objects, each subsequent chunk doubles
  (4096, 8192, ...). Growth happens only on exhaustion and is a bulk allocation.
- **Intrusive free list**: a dead slot's first word stores the next-free
  pointer; no per-object header. `construct`/`destroy` are O(1).
- **Teardown**: frees chunks only; the engine destroys each object exactly once
  before the pools' members run.
- **Single-threaded** (engine is single-threaded): no atomics.

No public API, semantics, fill events, or snapshot output changed. All three
test suites pass under Release and UBSan (differential incl. I1–I7 invariants
against the pooled engine).

## Measurement method

Same corpora as Stage C (regenerated here with identical sha256:
`fill_dense` = `210cad9e…`, `add_cancel_dense` = `89083b4f…`,
`level_churn` = `dc06f535…`). Baseline = this branch's engine with the pool
reverted (stash), pooled = with the pool, both built `-O2` Release on Apple
Clang 16. `lob_bench` batch (7 measured rounds) + latency. The bench links a
global `operator new/delete` override, so the alloc counters below count every
allocation in the measured region.

## Results (measured, per load)

| workload | mode | metric | baseline | pooled | delta |
|---|---|---|---|---|---|
| fill_dense | batch | req/s | 1,906,710 | 1,959,917 | **+2.79%** |
| fill_dense | batch | alloc_count | 4,660,713 | 2,330,529 | **−50.0%** |
| fill_dense | batch | alloc_bytes | 262,027,640 | 154,676,792 | −41.0% |
| fill_dense | latency | p50 ns | 541 | 500 | −7.6% |
| fill_dense | latency | p99 ns | 1,958 | 1,875 | −4.2% |
| add_cancel_dense | batch | req/s | 1,174,534 | 1,227,367 | **+4.50%** |
| add_cancel_dense | batch | alloc_count | 4,251,841 | 2,126,105 | **−50.0%** |
| add_cancel_dense | batch | alloc_bytes | 244,633,784 | 148,287,480 | −39.4% |
| add_cancel_dense | latency | p50 ns | 875 | 875 | 0.0% |
| add_cancel_dense | latency | p99 ns | 3,458 | 3,334 | −3.6% |
| level_churn | batch | req/s | 5,240,097 | 5,795,917 | **+10.61%** |
| level_churn | batch | alloc_count | 3,862,209 | 1,931,273 | **−50.0%** |
| level_churn | batch | alloc_bytes | 227,324,344 | 141,977,848 | −37.5% |
| level_churn | latency | p50 ns | 83 | 42 | −49.4% |
| level_churn | latency | p99 ns | 875 | 792 | −9.5% |

## Honest reading

- **Per-op allocation count halves** on every load: exactly the Order/Limit
  object churn (one object per add/cancel/modify/market step) disappeared. This
  is the headline, expected, and reproducible effect.
- **Throughput**: a small-to-moderate win — +2.8% (fill_dense), +4.5%
  (add_cancel_dense), +10.6% (level_churn). `level_churn` benefits most because
  it is the most level-create/delete-heavy workload, so chunk-adjacent Limit
  objects improve cache locality during AVL walks; p50 latency falls ~49%.
- **Not a headline gain**: allocation was already a secondary cost (profiling
  showed `getLimitHeight` recursion dominating; operator new/free <0.5%), so the
  pool removes that secondary cost without touching the real bottleneck. This is
  consistent with the experiment's prior expectation.

## Remaining allocations (not eliminated, and not claimed to be)

- `std::unordered_map` node allocations in `orderMap`, `limitBuyMap`,
  `limitSellMap`, `stopMap` on insert/erase (≈ half of the remaining count).
- `fillSink` vector growth in `marketOrderHelper`.
- Pool chunk growth (bulk, amortised; grows only on exhaustion).
- The benchmark's own structures (corpus parse, sink reserve, stats).

The engine is **not** zero-alloc; the pool removes only the Order/Limit
object-lifetime churn.
