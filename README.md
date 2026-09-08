# Limit Order Book — correctness & performance engineering portfolio project

A rigorous correctness-verification and performance-engineering study of a
single-threaded limit-order-book matching engine. Built on top of an open-source
upstream codebase, this repository adds an independent reference model +
differential test harness, a reproducible benchmark system, two measured
optimizations, deterministic replay, and CI — every claim backed by executed
tests and recorded experiments.

**Target roles: Quant Developer / Low-Latency C++.**

---

## 1. Upstream source and licensing

- Upstream: [brprojects/Limit-Order-Book](https://github.com/brprojects/Limit-Order-Book)
  by Benjamin Rienecker, **MIT License** (kept in `LICENSE`, upstream copyright
  notice preserved). Original README archived at `docs/upstream-README.md`.
- Pinned at upstream commit `af6e5349874649fe196bd6c26653d357f5a751f2`
  (2024-06-12, 48 commits). The full upstream git history is preserved on `main`;
  every change below is layered on top as separate, reviewable commits.
- Upstream claim for context: "over 1.4M transactions/second" measured on an
  Intel i5-12450H with its own pipeline; we did **not** reproduce that exact
  harness (its input files are absent and it writes per-order CSV) — see the
  performance report for our own methodology and numbers.

### What is upstream vs. ours (honest split)

| Area | Status |
|---|---|
| AVL price trees (limit + stop), FIFO queues, hash maps, matching paths | upstream design, inherited |
| Stop / stop-limit features | upstream behavior kept and regression-tested, **out of our research scope** (reference model does not model stops) |
| Correctness fixes (duplicate-id rejection, modify-to-crossing execution, inert destructor + Book-side classic AVL delete, print-free engine, input validation, safe teardown) | ours — each with a repro test committed first (`docs/issues-register.md`) |
| Fill-event sink, snapshot()/introspection API | ours (needed for verification) |
| Independent reference model + differential harness (reference/) | ours |
| Bench harness + workloads + results (bench/, scripts/, results/) | ours |
| Object pools (LOB-005) + cached AVL heights (LOB-006) | ours (measured, see §6) |
| tools/replay + golden traces + CI | ours |
| docs/semantics.md (authoritative behavior spec) | ours |

## 2. Scope

Single instrument, single thread, integer tick prices. In-scope order
operations: **limit add / cancel / modify, market** — with price-time priority,
partial fills, multi-level crosses, deterministic reject semantics for invalid
input. Structured fill records and fully inspectable book state. No networking,
no multithreaded matching, no GUI, no exchange infrastructure (per project
scope; upstream's extra order types are regression-pinned but not extended).

## 3. Repository layout

```
Limit_Order_Book/    engine (Book/Limit/Order + Pool); v1 semantics + optimizations
reference/           independent reference model + corpus machinery + differential harness
bench/               lob_bench (gen/run/selfcheck) + 7 workload configs
tools/               diff_fuzz (differential replay), replay (deterministic records)
test/                3 gtest binaries (upstream, engine correctness, differential)
testdata/            corpora catalog: random (3), adversarial (10), golden (6)
scripts/             run_bench.sh, bench_compare.sh, matrix_bench.sh, plot_results.py
results/             raw JSON datasets (stageC baseline, stageD ablation matrix)
docs/                semantics, issue register, design, performance, reproduce, interview
.github/workflows/   CI (build + suites + ASan/LSan + golden replay)
```

## 4. Quick start

```sh
# build (requires cmake>=3.29, ninja; googletest v1.14.0 auto-fetched)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8

# correctness: 123 upstream + 20 engine-correctness + 19 differential tests
./build/test/LimitOrderBookTests
./build/test/EngineCorrectnessTests
./build/test/DifferentialTests          # 20 seeds x 2000 reqs, per-request diff

# differential replay of any corpus (engine vs reference, exit 0 = identical)
./build/tools/diff_fuzz --replay testdata/corpora/mix-heavy.txt

# benchmark one workload (batch + per-op latency)
./build/bench/lob_bench gen  --workload bench/workloads/fill_dense.json --out build/benchdata/fill_dense.txt
./build/bench/lob_bench run  --workload bench/workloads/fill_dense.json \
    --corpus build/benchdata/fill_dense.txt --rounds 7 --mode batch --out r.json

# deterministic replay record + golden check
./build/tools/replay --replay testdata/corpora/mix-heavy.txt
./build/tools/replay --replay testdata/corpora/mix-heavy.txt --golden testdata/golden/mix-heavy.golden
```

Sanitizers: UBSan locally (`-DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer"`);
ASan+LSan runs in CI (macOS-host ASan is broken with Apple clang 16 / macOS 26 —
probe-verified; documented in `docs/issues-register.md`).

## 5. Core results

**Correctness** — engine v1 (fixed upstream) matches an independent reference
model with **zero divergence** across 20 seeds × 2000-request differential runs
and 10 hand-crafted adversarial corpora: per-request fill streams, full book
state, and I1–I7 invariants (incl. AVL validity and the no-crossed-book
invariant). 123 upstream + 20 engine + 19 differential tests green under Release
and UBSan; ASan/LSan green in CI.

**Performance** (Apple M4 Pro, Release -O2, 7 workloads × 300k requests, same
corpora across variants; engine service time only — no parse/network/queue):

| Workload (active book × price density) | baseline | + pools | + cached heights | **both** |
|---|---|---|---|---|
| add/cancel dense | 1.16M/s | 1.18M/s | 6.99M/s | **7.89M/s** |
| fill dense (multi-level sweeps) | 1.86M/s | 1.98M/s | 6.96M/s | **7.75M/s** |
| level churn (narrow band) | 5.06M/s | 5.45M/s | 9.17M/s | **10.94M/s** |
| small × low density | 3.14M/s | 3.23M/s | 6.90M/s | **8.06M/s** |
| small × high density | 7.28M/s | 7.78M/s | 10.67M/s | **11.45M/s** |
| **large × low density** (≈30k levels) | **61K/s** | 64K/s | 4.07M/s | **4.38M/s** |
| large × high density | 914K/s | 938K/s | 6.29M/s | **7.22M/s** |

p50 per-request service latency: 541–16,167 ns before → **125–250 ns** after
across all loads. No regressions; semantic identity across variants pinned by
identical fill hashes + the differential suite.

**What the two optimizations were** (each with profiling evidence, isolated
change, correctness verification, cost/limits — `docs/PERFORMANCE.md`):
1. **Cached AVL node heights** (LOB-006): `balance()` recomputed subtree heights
   by full recursion (42–59% of profile samples; O(subtree) per rebalance step).
   Storing per-node heights makes balance O(1) — the dominant win.
2. **Address-stable slab pools for Order/Limit** (LOB-005): removes per-op
   malloc churn (allocation count −50%). Modest alone (+3–11%), additive on top
   of the height fix (+3–19%).

**Limitations (honest)** — `docs/PERFORMANCE.md` §Caveats and §8 of this readme
sources: single machine for all measurements; engine service time excludes any
I/O/network/queueing; stop/stop-limit semantics inherited unchanged and out of
differential scope; hash-map node allocations remain (engine is not zero-alloc);
the macOS host cannot run ASan (toolchain bug) — CI covers it.

## 6. Verification methodology (why to trust this)

- **Independent oracle**: `reference/` implements `docs/semantics.md` from
  scratch (std containers), no engine code shared.
- **Per-request differential**: fill streams + full state compared after every
  request; mismatch → seed + corpus saved + greedy minimizer.
- **Invariants I1–I7** checked after every request incl. true-height AVL audit.
- **Deterministic replay**: `tools/replay` — byte-identical records across runs,
  golden files pinned.
- **Fixes precede optimizations**: semantic fixes landed as engine v1 with
  repro-first commits; all performance comparisons use the fixed engine as
  baseline (upstream's buggy behavior is not repackaged as an "optimization").
- Per-stage commits on `main`; upstream history intact.

## 7. Contribution record (interview-ready summary)

See `docs/INTERVIEW.md` (2-minute pitch, 10-minute talk outline, 10 deep Q&A),
`docs/RESUME.md` (English bullets), `docs/PERFORMANCE.md`, `docs/DESIGN.md`.

## 8. Known limitations

- All performance numbers are from one machine (Apple M4 Pro); relative deltas
  are the robust claim.
- `ExampleOrdersTests` (3) are environment-stubbed (hard-coded Windows paths in
  upstream) — documented, not fixed.
- Reference model excludes stops; bench workloads exclude degenerate inputs
  (reject paths are covered by the differential suite instead).
- Book state introspection (`snapshot()`) is O(book) — used only in
  verification/replay, never in the timed region.
