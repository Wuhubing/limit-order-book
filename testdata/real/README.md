# Real market-data extension — LOBSTER NASDAQ samples

Real order-flow test datasets derived from **LOBSTER** (lobsterdata.com) sample
files: NASDAQ TotalView-ITCH reconstructions for **AAPL** and **MSFT** on
**2012-06-21** (09:30–16:00 ET), downloaded from LOBSTER's registration-free
sample page. Prices are integer ticks ($ × 10⁻⁴); sizes are shares.

**Files**: `tools/lobster_to_corpus.py` (converter), `tools/lobster_book_check.py`
(end-of-day cross-check — see caveats), `corpus_aapl_50k.txt` /
`corpus_msft_50k.txt` (committed 50,000-op slices for offline use). Full-day
corpora (~380k / ~663k ops) are regenerated locally with the commands below.

## Source & download

```sh
# sample files (registration-free direct links, ~7–9 MB each):
curl -L -o AAPL.zip "https://php.lobsterdata.com/info/sample/LOBSTER_SampleFile_AAPL_2012-06-21_10.zip"
curl -L -o MSFT.zip "https://php.lobsterdata.com/info/sample/LOBSTER_SampleFile_MSFT_2012-06-21_10.zip"
unzip AAPL.zip -d aapl; unzip MSFT.zip -d msft
```

## Conversion (documented mapping)

| LOBSTER message | corpus op |
|---|---|
| 1 new limit order | `Add <id> <side> <qty> <price>` (dir 1=buy→1, −1=sell→0) |
| 2 partial cancellation | `Mod <id> <remaining> <price>` (same price, tracker-based) |
| 3 full deletion | `Cxl <id>` |
| 4 execution of visible order | size reduction at same price: `Mod` to remaining, or `Cxl` at zero |
| 5 execution of hidden order | **dropped** (no displayed-book change) |

Unknown-id events (orders alive at 09:30 whose add predates the sample) are
dropped at conversion and counted in the stats. Converter:

```sh
python3 tools/lobster_to_corpus.py --message aapl/*_message_10.csv --out corpus_aapl.txt
```

## What is validated on real data — and what is NOT

**Validated (ran on the full-day corpora, AAPL 380,678 ops / MSFT 663,288 ops):**
1. **Differential consistency**: engine ≡ independent reference model over the
   whole real order flow — `./build/test/diff_fuzz --replay corpus_*.txt` exit 0
   for both tickers (per-request fills + state + I1–I7 invariants).
2. **Determinism**: `./build/tools/replay` records byte-identical across runs.
3. **Realistic flow**: op mix, price ticks, and order sizes come from real
   NASDAQ order flow rather than a synthetic model.

**NOT validated (honest limitations):**
- The engine starts with an **empty book at 09:30**; orders alive before the
  sample (overnight book) are not modeled, so the engine's end-of-day state does
  NOT equal the real displayed book — `lobster_book_check.py` exists but its
  level-volume comparison is only meaningful as a *divergence measurement*:
  because of the missing overnight state, some type-1 adds that were
  non-marketable in reality rest crossed in the engine (AAPL 13,298 fills,
  MSFT 4,258 fills across the day ≈ 2–3.5% of adds). Every such divergence is
  identical in engine and reference (differential still exit 0), but the
  engine's book is not a faithful reconstruction of the real market.
- Queue order within a level is not reconstructed (engine `Mod` =
  cancel-and-replace at the tail); level totals only.
- Trade prices/aggressors are not modeled: type-4/5 executions are applied as
  resting-size reductions, not as fills.

## Committed artifacts & re-runs

```sh
# slices (committed): replay + differential must stay green
./build/test/diff_fuzz --replay testdata/real/corpus_aapl_50k.txt   # exit 0
./build/tools/replay --check-golden testdata/real/corpus_aapl_50k.golden   # exit 0
# full-day: regenerate corpus locally, then the same two checks
```
Full-day conversion stats (recorded):
- AAPL: 400,391 events → Add 191,015 / Mod 8,733 / Cxl 180,930; dropped: 11,332
  hidden-exec, 8,381 unknown-id; price range 5,773,500–5,883,200 ticks;
  qty 1–15,000; engine open orders at 16:00: 10,085.
- MSFT: 668,765 events → Add 329,566 / Mod 11,075 / Cxl 322,647; dropped: 3,616
  hidden-exec, 1,861 unknown-id; price range 299,700–312,100; qty 1–200,000;
  engine open orders at 16:00: 6,919.
