#!/usr/bin/env python3
"""Convert a LOBSTER NASDAQ message file into the canonical corpus format.

Mapping (documented in testdata/real/README.md):
  LOBSTER type 1 (new limit order)  -> Add <id> <side> <qty> <price>
  LOBSTER type 2 (partial cancel)   -> Mod <id> <remaining_qty> <price>
  LOBSTER type 3 (full deletion)    -> Cxl <id>
  LOBSTER type 4 (visible execution)-> engine has no external-aggressor fills:
                                       apply the executed size as a same-price
                                       qty reduction (Mod), or Cxl at zero.
  LOBSTER type 5 (hidden execution) -> dropped (no displayed-book change).

SIDE: LOBSTER Direction 1 = buy -> 1 (buy); -1 = sell -> 0 (sell).
Price: LOBSTER integer cents*100 ($ x 1e-4); used as integer ticks verbatim.
Qty: LOBSTER Size (shares), verbatim.

Known divergences from the real tape (cannot be validated):
  - queue POSITION order differs (engine Mod = cancel-and-replace at tail);
    level totals are preserved, order identity within a level is not.
  - an Add that would rest crossed executes aggressively in the engine
    (real tapes rarely contain these); such fills are counted in the stats.
  - type-5 hidden-order executions are ignored.

Usage:
  lobster_to_corpus.py --message FILE.csv --out corpus.txt [--limit N]
Prints conversion statistics to stdout. --limit N keeps the first N events.
"""
import argparse
import csv
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--message", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    remaining = {}          # order id -> shares still in the (displayed) book
    counts = {"1": 0, "2": 0, "3": 0, "4": 0, "5": 0}
    emitted = {"Add": 0, "Cxl": 0, "Mod": 0}
    skipped = {"unknown": 0, "nonpos": 0, "type5": 0, "anomaly": 0}
    min_p, max_p, min_q, max_q = 10**30, 0, 10**30, 0

    with open(args.message) as fin, open(args.out, "w") as fout:
        for row in csv.reader(fin):
            if len(row) < 6:
                continue
            typ, oid, size, price = row[1], int(row[2]), int(row[3]), int(row[4])
            direction = int(row[5])
            counts[typ] = counts.get(typ, 0) + 1
            if args.limit and sum(counts.values()) > args.limit:
                break
            if price <= 0 or size <= 0:
                skipped["nonpos"] += 1
                continue
            min_p, max_p = min(min_p, price), max(max_p, price)
            min_q, max_q = min(min_q, size), max(max_q, size)

            if typ == "1":
                side = 1 if direction == 1 else 0
                remaining[oid] = size
                fout.write(f"Add {oid} {side} {size} {price}\n")
                emitted["Add"] += 1
            elif typ == "2":
                if oid not in remaining:
                    skipped["unknown"] += 1
                    continue
                remaining[oid] -= size
                if remaining[oid] <= 0:
                    skipped["anomaly"] += 1
                    remaining.pop(oid, None)
                    continue
                fout.write(f"Mod {oid} {remaining[oid]} {price}\n")
                emitted["Mod"] += 1
            elif typ == "3":
                if oid not in remaining:
                    skipped["unknown"] += 1
                    continue
                remaining.pop(oid)
                fout.write(f"Cxl {oid}\n")
                emitted["Cxl"] += 1
            elif typ == "4":
                if oid not in remaining:
                    skipped["unknown"] += 1
                    continue
                remaining[oid] -= size
                if remaining[oid] > 0:
                    fout.write(f"Mod {oid} {remaining[oid]} {price}\n")
                    emitted["Mod"] += 1
                else:
                    remaining.pop(oid, None)
                    fout.write(f"Cxl {oid}\n")
                    emitted["Cxl"] += 1
            elif typ == "5":
                skipped["type5"] += 1

    print(f"input events      : {sum(counts.values())}  (types: {counts})")
    print(f"emitted ops       : {emitted}  (total {sum(emitted.values())})")
    print(f"skipped           : {skipped}")
    print(f"price range (ticks): {min_p}..{max_p}; qty range {min_q}..{max_q}")
    print(f"open orders at end: {len(remaining)}")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
