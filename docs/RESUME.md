# Resume bullets (English)

Honest framing: this is an **extension and optimization of an open-source
project** (brprojects/Limit-Order-Book, MIT) — say so; do not claim a from-scratch
exchange. All numbers below are measured on this repo's committed benchmark
dataset (see `docs/PERFORMANCE.md`). Use at most two to three bullets.

---

**Option 1 — one strong bullet + one systems bullet + one verification bullet**

- Extended an open-source C++20 limit-order-book matching engine into a
  verified + optimized core: fixed latent correctness defects (duplicate-id
  state corruption, crossed-book modifies, AVL-delete imbalance, destructor
  use-after-free) with repro-first commits, then **measured 1.6–71× throughput
  gains across seven 300k-request workloads** (61K → 4.4M orders/s on a
  30k-price-level book; p50 service latency 16.2 µs → 208 ns) on Apple M4 Pro.

- Root-caused the bottleneck via sampling profiles (59% of cycles in recursive
  AVL height recomputation) and delivered two independently-verified
  optimizations — cached AVL node heights and address-stable slab pools for
  orders/levels (allocation count −50%) — validated by an ablation matrix
  (baseline/A/B/A+B) with no regressions and bit-identical fill streams across
  variants.

- Built an independent reference matching model and differential test harness
  (per-request fill-stream + full-book-state comparison, 7 invariants incl. AVL
  audit, 20-seed × 2000-request fuzz, input minimization): **zero divergence**;
  shipped a reproducible benchmark system (batch + per-op p50/p95/p99 latency,
  allocation counters, clock-overhead accounting), deterministic replay with
  golden traces, and CI running Release/UBSan/ASan+LSan — 162 tests green.

**Option 2 — compact 2-bullet variant**

- Engineered a verified single-threaded C++20 matching engine on top of an
  open-source base: independent reference model + differential fuzzing
  (20×2000 requests, per-request state+fill comparison, I1–I7 invariants) with
  zero divergence, after fixing duplicate-id corruption, crossed-book modify,
  and destructor-based AVL deletion with repro-first tests.

- Optimized it from profiling evidence: cached AVL heights (removing O(subtree)
  rebalance, 59% of sampled cycles) plus slab pools (−50% allocations) →
  **1.6–71× throughput across 7 workloads** (up to 4.4M orders/s; p50 latency
  16.2 µs → 208 ns); reproducible bench harness, deterministic replay +
  golden traces, CI with ASan/LSan — all results in committed raw data.

---

Do NOT use numbers you cannot reproduce from `results/stageD/matrix/` and
`docs/PERFORMANCE.md`; if asked, walk through `docs/REPRODUCE.md` on the spot.
