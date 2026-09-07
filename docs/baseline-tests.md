# Baseline Tests — Stage A Run Results

Run date: 2026-09-07. Binary: `build/test/LimitOrderBookTests` (Release, Apple clang
16.0.0, `-O2`, C++20). Command and environment in `environment.md`.

## Summary

- Total: **123 tests across 2 suites — all reported PASSED (0 failures, 0 errors)**
  by googletest, run completed normally, exit code `0` (no crash/abort/signal).
- `LimitOrderBookTests`: **120 / 120 passed** (genuine engine-state assertions).
- `ExampleOrdersTests`: **3 / 3 "passed"** but all three are **environment-stubbed**
  — they perform no real work because they hard-code a Windows absolute path
  (`C:/Users/benja/Documents/Limit_order_book/...`) that does not exist here, so the
  file opens fail and the code returns early. See below.

Raw output: `.agent/results/LOB-001-unit.log` (stdout+stderr),
`.agent/results/LOB-001-unit.xml` (gtest XML). Both gitignored, not committed.

## Per-suite detail

### LimitOrderBookTests (120 tests) — PASSED

The real unit suite (`test/LimitOrderBookTests.cpp`). Exercises add/cancel/modify/
market/stop/stop-limit paths against `Book` state via `searchOrderMap`,
`searchLimitMaps`, `searchStopMap` and getters. All 120 tests passed with zero
assertion failures. No crashes, aborts, or hangs.

Expected stdout noise is present: `search*` print to stdout on every miss
(`"No order number N"`, `"No buy limit at P"`, `"No sell limit at P"`,
`"No stop level at P"`), 21 such lines total in this run. This is the known
print-on-miss behavior (see `docs/issues-register.md` item #5) and is not a
failure.

### ExampleOrdersTests (3 tests) — environment-stubbed, reported PASSED trivially

`test/ExampleOrdersTests.cpp` has no `EXPECT`/`ASSERT`; each test "passes" merely by
returning. What actually happened:

- `CreateInitialOrdersTest` — calls `generateOrders->createInitialOrders(10000, 300)`.
  That function opens an `ofstream` on the hard-coded path
  `C:/Users/benja/Documents/Limit_order_book/initialOrders.txt` (GenerateOrders.cpp:343).
  On macOS this path does not resolve to a writable location, `is_open()` is false,
  it prints `Error opening file for writing!` and returns **before generating any
  orders**. No orders were added to the book.
- `ProcessInitialOrdersTest` — calls
  `orderPipeline->processOrdersFromFile("C:/Users/benja/Documents/Limit_order_book/initialOrders.txt")`.
  `processOrdersFromFile` (OrderPipeline.cpp:29-31) cannot open the file, prints
  `Error opening file: C:/Users/benja/Documents/Limit_order_book/initialOrders.txt`
  and returns early. No orders were processed.
- `CreateOrdersTest` — first calls `processOrdersFromFile` on the same missing path
  (prints the same "Error opening file" and returns), then calls
  `generateOrders->createOrders(100000)`, which opens an `ofstream` on
  `C:/Users/benja/Documents/Limit_order_book/orders.txt` (GenerateOrders.cpp:278),
  fails, prints `Error opening file for writing!` and returns early. No orders were
  generated.

Exact stderr lines emitted by these three tests (verbatim from the log):

```
Error opening file for writing!
Error opening file: C:/Users/benja/Documents/Limit_order_book/initialOrders.txt
Error opening file: C:/Users/benja/Documents/Limit_order_book/initialOrders.txt
Error opening file for writing!
```

These tests were NOT edited or deleted. They are classified environment-stubbed
because the required input/output files live at a Windows absolute path that does
not exist in this environment.

## Not run

- The `LimitOrderBook` executable (`build/LimitOrderBook`) was **not run**:
  `main.cpp` reads `./initialOrders.txt` and `./Orders.txt` from the working
  directory, which are not present in the repo (only `Generate_Orders/initialOrders.txt`
  exists, and the pipeline targets `./Orders.txt`). Running it is out of scope here.

## Honesty notes

- Every "passed" above is a pass that googletest actually reported at run time.
- The three ExampleOrdersTests pass only because they contain no assertions and
  short-circuit on a missing file; they do not exercise the engine.
- No assertion failures were observed in `LimitOrderBookTests`; there is therefore
  no failure text to record for this suite.
