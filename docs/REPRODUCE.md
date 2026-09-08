# Reproduction guide — from clean checkout to the numbers in the report

Every command below was executed on this machine (Apple M4 Pro, macOS 26.4.1,
Apple clang 16.0.0). `cmake`/`ninja` live at `/opt/homebrew/bin` — prepend PATH
where needed (`export PATH="/opt/homebrew/bin:$PATH"`).

## 0. Clean checkout

```sh
git clone https://github.com/brprojects/Limit-Order-Book.git lob
cd lob
# upstream pinned at af6e534; our work is on top of main. Check out our branch:
git fetch origin main && git checkout main      # or clone this repo directly
```

## 1. Build (Release)

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```
googletest v1.14.0 is fetched automatically when `googletest/` is absent
(CMakeLists.txt bootstrap — the only build-system change vs upstream).

## 2. Correctness suites

```sh
./build/test/LimitOrderBookTests     # 123 (upstream incl. 3 env-stubbed)  -> PASSED
./build/test/EngineCorrectnessTests  # 20 engine-v1 regression/invariant     -> PASSED
./build/test/DifferentialTests       # 19: ref model + 20 seeds x 2000 fuzz  -> PASSED
```
UBSan config: `cmake -B build-ubsan -G Ninja -DCMAKE_BUILD_TYPE=Debug
-DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer"` then run the
same binaries from `build-ubsan/test/`.
ASan+LSan: cannot run on this macOS host (Apple clang 16 ASan init bug,
probe-verified) — run the same three suites in CI (`.github/workflows/ci.yml`,
ubuntu-latest, `-fsanitize=address,undefined`, `ASAN_OPTIONS=detect_leaks=1`).

## 3. Differential replay & adversarial corpora

```sh
./build/test/diff_fuzz --replay testdata/corpora/mix-heavy.txt        # exit 0
for f in testdata/corpora/adversarial/*.txt; do
  ./build/test/diff_fuzz --replay "$f" || echo "FAIL $f"
done
```
Exit codes: 0 identical (engine == reference model), 1 divergence (saved +
minimized corpus), 2 parse error.

## 4. Deterministic replay records (golden)

```sh
./build/tools/replay --replay testdata/corpora/mix-heavy.txt           # record to stdout
for g in testdata/golden/*.golden; do
  ./build/tools/replay --check-golden "$g" || echo "FAIL $g"
done
```
(Regenerate a golden file with
`./build/tools/replay --emit-golden <corpus> --out <golden-file>`.)

## 5. Benchmarks — baseline dataset (results/stageC)

```sh
bash scripts/run_bench.sh        # gen 7 corpora (sha-skip via sidecar), 7 loads
                                 # batch + latency -> results/stageC/<load>.{batch,latency}.json
```
Corpus generation for `large_*` loads takes ~4 min each (once; cached after).

## 6. Ablation matrix (results/stageD/matrix)

Four engine variants, identical corpora, one machine:

```sh
BASE=05b4273            # engine v1 (pre-optimization)
A=f7e8b9c               # + object pools (LOB-005)
# B-only variant: cherry-pick the B commit onto the base:
git worktree add /tmp_b-wt b-only-branch $BASE   # create branch, then:
git -C /tmp_b-wt cherry-pick 9f25029             # cached heights WITHOUT pools
AB=9f25029              # both (current main)

git worktree add /wt/base  $BASE
git worktree add /wt/a    $A
git worktree add /wt/b    b-only-branch

bash scripts/matrix_bench.sh /shared-corpus-dir /out-dir \
    "base=/wt/base" "a=/wt/a" "b=/wt/b" "ab=/repo/main-checkout"
# -> /out-dir/<variant>/<load>.{batch,latency}.json
```
(simplified: `scripts/matrix_bench.sh` configures nothing — run
`cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` in each worktree first, or
let the script's `--build` step handle an existing configure.)

## 7. Tables & figures

```sh
python3 scripts/plot_results.py results/stageD/matrix results/figures
# -> throughput.csv, latency.csv, throughput.png, latency_p50.png
```

## 8. Reproducing a single number by hand

```sh
./build/bench/lob_bench gen --workload bench/workloads/large_low_density.json \
    --out build/benchdata/large_low_density.txt      # sha256 pinned
./build/bench/lob_bench run --workload bench/workloads/large_low_density.json \
    --corpus build/benchdata/large_low_density.txt --rounds 7 --mode batch --out r.json
python3 -c "import json;print(json.load(open('r.json'))['reqs_per_sec_all_ops'])"
# expect ~4.3-4.5M on an M4 Pro (ab variant / main HEAD); ~61K at BASE.
```

## Variance notes
- Round-to-round noise on this machine was small (<2% between the reported
  matrix and reviewer spot-checks); the large effects (2×–71×) dwarf it.
- Single machine, single benchmark binary config; treat absolute numbers as
  M4-Pro-specific, deltas as the robust result.
- CI intentionally runs no benchmarks (shared-host noise) — correctness +
  sanitizers only.
