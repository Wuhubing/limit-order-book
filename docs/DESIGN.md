# Design notes (engine v1 + Stage D optimizations)

Corresponds to the code on `main`. Behavior spec: `docs/semantics.md` (authoritative).
Issue-by-issue history: `docs/issues-register.md`.

## 1. Data structures

Each side of the book (buy, sell) keeps **one AVL tree of price levels**; a
level (`Limit`) is both a BST node and the anchor of a **doubly-linked FIFO
queue** of orders (`Order`). Three hash maps give O(1) lookup: `orderMap`
(id -> live Order, covering limit + stop orders), `limitBuyMap`/`limitSellMap`
(price -> level), `stopMap` (stop price -> stop level, shared by both sides —
upstream limitation kept, out of scope). `highestBuy`/`lowestSell` are cached
edge pointers to the inside of the book; stop trees have their own edges.

Matching consumes the best opposite level's **head** order (price-time
priority); a partially filled head keeps priority. Levels that empty are removed
from tree + maps immediately. Aggressive orders (market, crossing limit, or a
modify that crosses) fill at the resting maker's price, level by level.

### What changed vs. upstream (semantics-neutral or fixed)

- `Limit::~Limit` is inert. All tree surgery lives in
  `Book::deleteLimit`/`deleteStopLevel` (classic AVL delete: in-order successor
  splice, rebalance walk from the successor's original parent — and from the
  successor itself when it is the deleted node's direct right child — up to the
  root). Upstream deleted nodes *in the destructor* without rebalancing the
  splice path, which could silently break AVL shape and mislead the cached edge
  pointers.
- `Book::~Book` destroys orders via `orderMap` and levels via per-tree
  post-order `deleteTree`, then the pools free their chunks. No destructor
  touches tree pointers.
- `modifyLimitOrder` = cancel-and-replace, and if the new limit crosses the
  book it executes aggressively like an add (remainder rests at the tail of the
  new level). Same-price modify deliberately **loses** FIFO position
  (upstream cancel-and-append semantics, kept and documented).
- Duplicate live ids, non-positive qty/price, unknown-id cancel/modify: silent
  no-op rejects. Market orders never rest: unfilled remainder is dropped.
- All `search*` helpers are print-free.

## 2. Ownership & object lifecycle

`Book` owns every `Order` and `Limit` through **`Pool`** (`Limit_Order_Book/Pool.hpp`),
an address-stable chunked slab pool:

- chunks double (4096, 8192, ...); a live object's address never changes —
  required because orders/levels are linked by raw pointers;
- dead slots join an intrusive free list (the first word of the dead object);
  `construct()`/`destroy()` are O(1) placement-new/destructor calls;
- chunk growth is the only pool allocation and is amortized/bulk;
- single-threaded, no atomics.

Lifecycle rules (each enforced by the differential suite's I2/I3 + UBSan):
an Order is constructed at add time (resting remainder) and destroyed exactly
once — when it is removed from `orderMap` **and** unlinked from its queue
(cancel, full fill, modify full-fill, stop conversion). A Limit is constructed
when a new price level appears and destroyed exactly once when it empties and is
removed from maps + tree. Remaining allocations (documented, not hidden):
`unordered_map` node churn in the four maps, fill-sink vector growth, pool chunk
growth, benchmark structures. **The engine is not zero-alloc.**

## 3. AVL balance with cached heights (LOB-006)

Upstream stored no balance metadata: every `balance()` recomputed subtree
heights by full recursion (`getLimitHeight`, O(subtree)) — measured at 42–59%
of CPU samples and O(subtree) per rebalance step. Now each `Limit` caches
`height` (leaf = 1, null = 0); `limitHeightDifference()` is O(1). The cache is
maintained at every mutation point:

- `insert()`/`insertStop()`: refresh the node bottom-up before `balance()`;
- all rotations (ll/rr/lr/rl × limit/stop): refresh lower node first, then the
  new subtree root;
- `deleteLimit()`/`deleteStopLevel()` + `rebalanceUpward(Stop)`: refresh before
  each `balance()` along the upward walk (starting at the successor's original
  parent, or at the successor in the direct-right-child case).

`Book::getLimitHeight` is retained unchanged: the differential suite's **I6**
invariant recomputes true heights recursively and compares left/right balance
after every request, so any cache drift fails the suite — the cache cannot rot
silently.

## 4. Operation complexity (engine v1 → current)

| Operation | before | after (current) |
|---|---|---|
| add resting order | O(log n) tree + **O(subtree) rebalance** | O(log n), O(1) balance checks |
| cancel (incl. level delete) | O(1) unlink + **O(subtree)-ish rebalance walk** | O(log n) with O(1) balance |
| market / aggressive sweep | O(k · levels consumed) | unchanged (k = fills) |
| order allocation | malloc/free per Order/Limit | O(1) pool reuse |
| best bid/offer | O(1) cached edges | unchanged |

n = number of price levels. The pathological case was large numbers of levels
with sparse occupancy: tree ops degenerated toward O(n) per balance step.

## 5. Key trade-offs (decisions + why)

1. **AVL + raw pointers + maps (upstream shape) kept, not rewritten to
   std::map/skip-list**: preserves upstream lineage, keeps O(1) level lookup and
   the FIFO-within-level layout that the two experiments target; the measured
   fix (cached heights) removes the actual bottleneck with minimal surface.
2. **Price-time priority via append-at-tail; modify loses priority**: matches
   upstream semantics, keeps cancel O(1) anywhere in the queue. A
   priority-preserving modify would require a timestamp per order — not needed
   for correctness here and would change FIFO semantics for the differential
   oracle.
3. **Snapshot()/fill sink added to the engine core** (not a wrapper): the
   differential and benchmark harnesses need the true internal state; the sink
   is a single null-check per fill when disabled. Snapshot is O(book) and only
   used in verification/replay, never in the timed region.
4. **Pools over per-object malloc**: allocation was secondary in profiles;
   pools still halve allocation count and add locality once the tree cost is
   gone (A+B measured additive). Chunk growth means worst-case one bulk
   allocation per 4096 objects — bounded and documented.
5. **Stops kept upstream-identical**: re-specifying stop semantics would expand
   scope without portfolio value; they share the allocator/trees and are pinned
   by the 123 upstream tests (no differential coverage — documented).
6. **Validation rejects are silent no-ops**: matches a matching engine that has
   no rejection channel in its hot path; the differential suite proves both
   sides behave identically. (A production engine would emit reject events —
   noted as a design extension.)

## 6. Functions to read first

- `Book::marketOrderHelper` — the fill loop (full-consumption + single partial
  fill per level; fill events emitted here).
- `Book::addLimitOrder` / `Book::modifyLimitOrder` — aggressive-then-rest
  transition and the no-cross invariant.
- `Book::deleteLimit` / `Book::deleteStopLevel` / `rebalanceUpward` — AVL
  deletion + cache refresh (the riskiest code; I6 guards it).
- `Book::insert` + the rotate family — cached-height maintenance.
- `Book::snapshot()` + `reference/ReferenceBook.cpp` — the two sides of the
  differential harness.
- `Pool::construct/destroy` — object lifecycle.
