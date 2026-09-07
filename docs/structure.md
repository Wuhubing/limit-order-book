# Engine Structure — Stage A

Short structural explanation of the matching engine under `Limit_Order_Book/`
(`Book`, `Limit`, `Order`). This describes the code as-is at upstream
`af6e534`; it is descriptive, not a proposal to change it.

## Class roles

- **`Order`** (`Order.hpp`/`Order.cpp`) — a single order, and a node in a
  doubly-linked FIFO queue. Fields: `idNumber`, `buyOrSell`, `shares`, `limit`
  (limit price; `0` for a stop-market order), `nextOrder`/`prevOrder` (queue links),
  `parentLimit` (the `Limit` price level it rests at). `execute()` unlinks the head,
  `cancel()` unlinks any node, `partiallyFillOrder()` reduces `shares` and the level's
  `totalVolume`.
- **`Limit`** (`Limit.hpp`/`Limit.cpp`) — a price level; simultaneously a node in a
  binary search tree and the head/tail anchor of a FIFO queue of `Order`s. Fields:
  `limitPrice`, `size` (order count), `totalVolume` (sum of shares),
  `buyOrSell`, `parent`/`leftChild`/`rightChild` (tree links), `headOrder`/`tailOrder`
  (queue). `append()` adds an order at the tail (price-time priority). Its destructor
  splices the node out of the BST.
- **`Book`** (`Book.hpp`/`Book.cpp`) — the order book; owns all levels and orders,
  exposes the order operations (add/cancel/modify for limit, stop, stop-limit, plus
  market), the AVL rotations/rebalance, and the search/print helpers.

## Tree + map + list interplay

Four in-memory structures cooperate per side:

1. **Tree** — each side's resting levels are organized into an **AVL tree** keyed by
   `limitPrice`: `buyTree`/`sellTree` (limit orders) and `stopBuyTree`/`stopSellTree`
   (stop & stop-limit orders). In-order traversal yields prices sorted. `Book` keeps
   `highestBuy`/`lowestSell` (and `lowestStopBuy`/`highestStopSell`) as cached **edge
   pointers** to the best level for O(1) access.
2. **Map** — hash maps (`unordered_map`) for O(1) lookup:
   - `orderMap`: `orderId -> Order*` (cancel/modify by id).
   - `limitBuyMap` / `limitSellMap`: `limitPrice -> Limit*`.
   - `stopMap`: `stopPrice -> Limit*` (shared by buy **and** sell stop levels — a
     keying caveat, see issue register #9).
3. **List** — each `Limit` holds a **doubly-linked FIFO queue** (`headOrder`/`tailOrder`)
   of `Order`s at that price. Execution takes from the head (price-time priority);
   `append()` adds to the tail.

A resting limit add therefore does: (a) `orderMap.emplace(id, order)`, (b) look up or
create the level via the side's `limitMap`/`addLimit`, (c) `append` to the level's
queue. Cancel looks up the order in `orderMap`, unlinks it from its level's queue, and
deletes the level (from tree + map) if it emptied.

## Ownership model

- `Book` **owns everything** via raw pointers. `~Book` deletes all `Order`s by
  iterating `orderMap`, then deletes all `Limit`s by iterating `limitBuyMap`,
  `limitSellMap`, `stopMap`.
- An `Order` is reachable from both `orderMap` and its level's queue; `Limit` is
  reachable from a side map and from the tree. These are aliasing pointers into
  single ownership — the destructor walks the maps to `delete` each object once.
- `Limit::~Limit` performs BST pointer surgery (parent/child relinking, in-order
  successor splice for two-child cases) without rebalancing; `deleteLimit`
  re-balances only the deleted node's ancestors. (See issue register #2 / #6 for the
  AVL-invariant and teardown-UB caveats.)
- There are no smart pointers and no explicit rejection paths; duplicated order ids
  are silently accepted by `orderMap.emplace` while the object is still appended
  (issue register #1).

## Operation complexities (as implemented)

Nominal, assuming the AVL tree stays balanced:

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| `searchOrderMap` / `searchLimitMaps` / `searchStopMap` | O(1) avg | `unordered_map` lookup; **prints to stdout on miss** |
| `addLimitOrder` (resting) | O(log n) amortized | BST insert + AVL rebalance; market-crossing portion walks opposite levels |
| `cancelLimitOrder` | O(1) + O(log n) | map lookup, queue unlink, tree delete + rebalance |
| `modifyLimitOrder` | O(log n) | implemented as cancel + re-place (loses FIFO position, see #13) |
| `marketOrder` | O(k) | k = number of levels consumed; each level head executes in O(1), emptied levels deleted from tree |
| `getLimitHeight` | O(n) | full recursion over the subtree |

Important caveat on the tree ops: AVL **balance factors are not cached**. Every
`balance()` call recomputes subtree heights via `getLimitHeight` (full recursion,
O(subtree size)), so each insert/delete rebalance step is O(subtree) rather than the
O(1) of a stored-balance-factor AVL. With many price levels the insert/delete paths
are effectively O(height · subtree) and can degrade toward O(n) (issue register #8).

`executedOrdersCount` and `AVLTreeBalanceCount` are instance counters reset at the
start of each public operation (single-threaded, per-call stats — issue register #12).
