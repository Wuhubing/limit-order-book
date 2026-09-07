# Engine Semantics v1 (research scope)

Authoritative behavior spec for the FIXED single-instrument, single-threaded matching
core. Upstream = brprojects/Limit-Order-Book @ af6e534. [FIX] marks deliberate,
documented deviations from upstream (each backed by a reproducing test first).
The reference model (LOB-003) and the differential harness implement THIS document
exactly. Any change to this file requires re-running the whole correctness suite.

## Scope
- Order types in scope: LIMIT (add/modify/cancel), MARKET. Stop / stop-limit code
  remains in the engine (upstream lineage, regression-tested) but is NOT modeled by
  the reference model; generated corpora never issue stop commands, so the stop
  trigger path is inert during differential/benchmark runs.
- Single instrument, single thread, integer tick prices, int32 qty.
- Corpus bounds (documented, enforced by generator): price in [1, 1_000_000],
  qty in [1, 100_000], per-level live volume kept < 2^31 - 1 (int safety margin).

## Types & identity
- Order id: caller-supplied int. Must be > 0 and not already LIVE (resting) when an
  order is added. An id may be reused after its previous order is dead
  (executed / canceled / rejected).
- An order is LIVE iff it is present in the orderMap and linked in exactly one price
  level queue (limit side) — never both.

## Operations

### AddLimit(id, side, qty, limit)
1. Validate: id>0, qty>0, limit>0, id not live. Any violation => REJECT: no state
   change, no events. [FIX: upstream silently corrupted/leaked on duplicate id]
2. Aggressive phase: while the order crosses the opposite book, trade at the
   RESTING (maker) order's limit price, always against the best opposite level
   (lowest ask for buys, highest bid for sells), FIFO within the level (head first).
   - Full consumption of an order => order dead, level emptied if last, remove level.
   - Partial fill of the head order: head keeps FIFO priority with remaining qty.
   - Emit one FILL event per trade, in execution order.
3. Resting phase: if qty remains after no more crossing liquidity, the remainder
   rests at `limit`, appended at the TAIL of that price level (FIFO by arrival).
4. Trade price rule: every trade executes at the resting order's price (no price
   improvement; aggressor does not impose its limit price on fills). [upstream rule]

### Market(id, side, qty)
1. Validate qty>0 (id is not stored for market orders; if given, only qty matters).
   Violation => REJECT, no state change.
2. Aggressive phase identical to AddLimit, at best opposite level, FIFO.
3. On liquidity exhaustion with qty remaining: the remainder is DROPPED (market
   orders never rest, never become reject events). [upstream behavior, documented]
4. Empty/one-sided book => nothing fills, nothing rests, no events.

### CancelLimit(id)
1. Live resting order => remove from its level queue wherever it sits
   (head/middle/tail/last). Level emptied => remove level from tree+maps.
   Emits no FILL events.
2. Unknown / dead / never-existed id => silent no-op, no state change, no events.
   [FIX: upstream printed to stdout on every miss; engine is print-free]

### ModifyLimit(id, newQty, newLimit)
Defined as cancel-and-replace with the SAME id (single transition, no gap where the
id is dead):
1. Validate id live, newQty>0, newLimit>0. Violation => silent no-op (order keeps
   its old state).
2. Remove from current level (FIFO position is NOT preserved: like upstream, the
   replaced order is re-inserted at the TAIL of its new level; a same-price modify
   therefore loses old time priority — documented upstream behavior, kept).
3. If newLimit crosses the current book => aggressive phase exactly as AddLimit
   with this id (fills emit FILL events, aggressor id = order id).
4. Remainder rests at newLimit (tail). [FIX: upstream left the modified order
   resting CROSSED — no crossing liquidity — violating the no-cross invariant]

## Invariants (checked after EVERY request by the harness)
- I1 quantities non-negative everywhere (order qty > 0 while live; level volume =
   sum of member order qtys; totals never negative).
- I2 orderMap index == set of live orders exactly (no stale, no missing, no
   unmapped-but-linked order).
- I3 level queues consistent: each level's head/tail/next/prev chain matches its
   size; each live order is linked in exactly one level; level volume == sum qty.
- I4 price-time priority: within a level, execution order is head-to-tail; after a
   partial fill the head is unchanged.
- I5 no immediate cross: after any operation there is no live buy level at price >=
   min live sell level, and no live sell level at price <= max live buy level.
   (Applies to the limit book; stop levels are inert in the corpus.)
- I6 AVL: each limit/stop tree is a valid BST; heights of child subtrees differ by
   at most 1 (checked via the same recursive height used by the engine).
- I7 no dangling storage: deleting a Book frees every live order and level exactly
   once (ASan/UBSan clean; teardown is traversal-ordered, not map-iteration
   [FIX for upstream destructor UB]).
- I8 deterministic replay: same command sequence => identical FILL event stream and
   identical logical state summary (see Stage E).

## Events
FillEvent { aggressorId:int, restingId:int, price:int, qty:int, aggressorBuy:bool,
            seq:uint64 }
- Emitted through an optional sink (std::vector<FillEvent>* set on the engine; null
  => zero-cost disabled). One event per trade, in execution order. seq = global
  monotonically increasing counter across the engine lifetime (for replay
  ordering).
- Resting, cancellation, modification, and rejections emit no events.
- Stop-triggered executions (out of corpus) would emit events with aggressorId 0
  (system aggressor) — documented, not exercised.

## Rejected / out-of-research decisions
- Stop & stop-limit semantics: kept as upstream, NOT re-specified here; regression
  coverage via the original unit suite only.
- int32 volumes with corpus bounds above (no overflow in any exercised state).
- Upstream `modifyOrder`-on-stop and generator internals: untouched.
