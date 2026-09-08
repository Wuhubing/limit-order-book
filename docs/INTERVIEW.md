# Interview materials

Everything here is grounded in the actual code and recorded experiments in this
repo. Read `docs/PERFORMANCE.md` and `docs/DESIGN.md` first; re-run any claim
you plan to make with the commands in `docs/REPRODUCE.md`.

## 1. Two-minute project pitch

> "I took an open-source, single-threaded C++ limit-order-book matching engine —
> AVL price trees with FIFO order queues — and ran it through a full
> correctness-and-performance engineering cycle, because for a matching engine
> you cannot optimize what you cannot verify.
>
> First I fixed the correctness layer. The upstream engine had real defects —
> duplicate order IDs silently corrupted state, modifying an order to a crossing
> price left a crossed book, the AVL tree deleted nodes inside the destructor
> without rebalancing, so the tree could silently degrade and the cached best
> bid/offer could point at the wrong level. Every fix landed with a reproducing
> test committed first, and I wrote an independent reference model in pure
> std::map/deque code — no shared logic — plus a differential harness that
> replays identical command streams through both engines and compares the fill
> events and the full book state after every request, checking seven invariants
> including AVL balance. Twenty seeds times two thousand requests: zero
> divergence. That pinned the engine's semantics before I touched performance.
>
> Then I built a reproducible benchmark: seven deterministic 300k-request
> workloads spanning add/cancel-dense, fill-dense, level churn, and small/large
> books at low/high price density, with batch throughput and per-request latency
> measured separately, engine service time only. Profiling showed the AVL
> rebalance recomputed subtree heights by full recursion — 59% of cycles on a
> 30-thousand-level book. I cached node heights: that workload went from 61
> thousand to 4.4 million requests per second, 71x. A second experiment —
> address-stable slab pools for orders and levels — halved allocation count and
> added another 3 to 19 percent on top. Every claim is in committed raw data
> with a reproduction guide, and the final engine still passes the differential
> suite: same semantics, 125-to-250-nanosecond p50 service latency across all
> workloads."

## 2. Ten-minute technical talk outline

1. **Problem & scope** (1 min): single-instrument single-thread matching core;
   price-time priority, partial fills, cancels; why correctness gates perf.
2. **Upstream architecture** (1.5 min): AVL tree of Limit nodes = price levels,
   each holding a doubly-linked FIFO of Orders; three hash maps; cached
   best-bid/offer edges; raw-pointer ownership. Op complexities.
3. **Semantics spec** (1 min): trade price = maker price; market remainder
   dropped; modify = cancel-and-replace (FIFO position lost); silent rejects for
   invalid input — written down before any test (`docs/semantics.md`).
4. **Correctness fixes with repro-first commits** (2 min): duplicate-id
   rejection; modify-to-crossing executes; destructor tree surgery moved into
   Book-side classic AVL delete; safe teardown; print-free engine.
5. **Independent verification** (2 min): reference model design; per-request
   fill-stream + state comparison; I1–I7 invariants (I6 = true-height AVL
   audit); seeded fuzz with degenerate-input injection; minimizer; sanitizers —
   and the honest ASan-on-macOS limitation, closed in Linux CI.
6. **Measurement system** (1 min): corpus format shared by tests/bench/replay;
   seven workloads; batch vs per-op latency; clock-overhead accounting;
   allocation counters; fill-hash determinism guard; same-corpus across
   variants.
7. **Optimizations** (1.5 min): profile evidence → cached AVL heights (the 71x
   story) and slab pools (the −50% allocations story); ablation matrix
   baseline/A/B/A+B; no-regression evidence.
8. **Engineering delivery** (1 min): deterministic replay + golden traces, CI
   (Release/UBSan/ASan+LSan), staged commits, full documentation.
9. **Limits & what I'd do next** (0.5 min): stop orders out of differential
   scope; remaining hash-map allocations; next: flat-map price index, allocator
   for map nodes, time-priority-preserving modify, multi-instrument.

## 3. Ten deep questions (with grounded answers)

**Q1. Why did you trust the upstream tests when the engine had bugs?**
Upstream's 123 tests all passed at baseline — the defects live in its blind
spots: duplicate IDs (one test even *relied* on the bug, reusing an id for a
second order), and modify-to-crossing (never tested). That's exactly why
self-tests aren't sufficient — the differential oracle with injected degenerate
inputs is what covers reject/no-op paths, and the I5 no-cross invariant is
checked after *every* request. Two upstream tests that depended on duplicate-id
acceptance were adapted to unique ids with the intent preserved (commits
`9e9c827`), and the change was deliberate semantics (`docs/semantics.md`).

**Q2. Why an AVL tree of price levels at all, instead of a std::map or a
sorted array?** Upstream chose it; my job was to keep lineage and fix real cost.
std::map gives O(log n) level ops too but allocates a node per level *and* on
every insert/erase, with worse cache behavior than the pool-allocated AVL. A
contiguous price array is only viable for dense tick ranges and imposes a price
bound — I'd need to publish those assumptions and would lose sparse-level
efficiency; the large_low_density workload (~30k sparse levels over a 60k-tick
window) would waste memory. Measured: with cached heights the AVL is no longer
the bottleneck on any load (p50 125–250 ns floor is hash-map + fill-push).

**Q3. Explain the cached-height change and why it was safe.**
`balance()` previously recomputed subtree heights recursively — O(subtree) per
rebalance step, 42–59% of profile samples. Now each Limit stores its height;
rotations and insert/delete paths refresh affected nodes bottom-up, making
balance checks O(1). Safety: the differential suite's I6 invariant recomputes
*true* heights independently after every request across 40k fuzz requests, so a
stale cache fails CI — plus two targeted regression tests (root delete,
successor-is-direct-right-child, stop trees). `getLimitHeight` stays as the
audit function.

**Q4. Why were pools a "small" win, and why keep them?**
Profiling said allocation was <0.5% of samples — the honest expectation was
modest, and measurement confirmed: +3–11% alone. But the ablation matrix shows
pools add +3–19% *on top of* the height fix (level_churn +19%): once the tree
cost is gone, allocation locality matters. Also they halve allocation count and
remove per-op malloc variance — attractive for tail latency even where mean
throughput barely moves. This is the experiment where "no effect would also be
a recorded result" applied; we kept them because A+B > B everywhere with no
regression and documented the memory cost (chunks retained for Book lifetime).

**Q5. What allocations remain after pooling, and why not remove them?**
`unordered_map` nodes for orderMap/limit maps (allocated per insert/erase),
fill-sink vector growth, pool chunk growth, benchmark structures. The engine is
not zero-alloc and the report says so. Removing map-node churn would mean a
flat/id-based map or an allocator for the map nodes — a further experiment with
its own trade-offs (id space assumptions, iterator invalidation); we scoped it
out and documented it as the next candidate.

**Q6. How do you know the optimized engine still matches the *old* semantics?**
Three independent pins: (1) every optimization commit passes the differential
suite (engine vs independent reference model, per-request fills + state + I1–I7)
under Release and UBSan; (2) benchmark fill hashes are bit-identical across
baseline/A/B/A+B for every workload — the event streams are the same; (3)
deterministic replay records are byte-identical across runs and pinned by
committed golden files. So the speedups are not semantic drift in disguise.

**Q7. What does "engine service time" exclude, and why is that the right
measure?** It excludes corpus parsing/generation, RNG, checksumming, and state
inspection — everything outside `Book`'s public ops with the fill sink attached
inside the timed region. That isolates the matching core: the thing the
portfolio claims to optimize. It deliberately excludes network, queueing, and
market-data latency — a production endpoint would report end-to-end numbers
separately. Clock overhead (~41 ns per timing pair) is measured and reported so
per-op latencies are honest at the 125 ns floor.

**Q8. Why is the no-crossed-book invariant (I5) important, and which upstream
bug violated it?** A resting buy at ≥ the best ask means the book contains
executable liquidity that the engine refuses to match — inconsistent with any
market model and exploitable. Upstream violated it via
`modifyLimitOrder`: cancel-and-replace skipped the crossing check that
`addLimitOrder` performs, so modifying an order to a crossing price left it
resting crossed. Fix: modify executes aggressively like an add when it crosses,
resting only the remainder. The differential harness checks I5 after every
request, and the adversarial modify_storm corpus targets it.

**Q9. How did you make benchmarking fair across variants and runs?**
Fixed corpus per load (seeded generator, sha256-pinned, shared across
variants); fresh Book per round (identical initial state); warmup + 7 measured
rounds; batch and latency measured separately; fill sink attached and events
hashed in all variants so output work is identical; allocation counters live
only in the bench binary. The ablation matrix ran all four variants on one
machine back-to-back; effects are large enough (2×–71×) that machine drift is a
second-order concern, and the report says absolute numbers are
M4-Pro-specific while deltas are robust. No perf gates in CI by design.

**Q10. If you had two more weeks, what would you do?**
(1) Attack the remaining per-op allocations: flat map keyed by order id with a
free list, or a custom allocator for map nodes — measure whether the 125–250 ns
floor moves; (2) extend the reference model to stop/stop-limit orders so the
differential net covers the full upstream surface (it is currently pinned only
by upstream tests); (3) time-priority-preserving modify with explicit order
timestamps (semantics change, so it needs the full oracle re-run); (4) ingest a
public LOB tape (e.g., LOBSTER) mapped onto our corpus format to validate
against real-world order flow — the corpus format already supports it.

## Suggested reading order for the interviewer

`README.md` → `docs/PERFORMANCE.md` (numbers) → `docs/DESIGN.md` (structure) →
`docs/semantics.md` (contract) → `Limit_Order_Book/Book.cpp::marketOrderHelper`
+ `deleteLimit` → `reference/ReferenceBook.cpp` → `results/stageD/ABLATION.md`.
