#!/usr/bin/env python3
"""Cross-check the engine's end-of-day book against the real LOBSTER book file.

Usage:
  lobster_book_check.py --corpus corpus.txt --book BOOK.csv [--replay BIN]
    BOOK.csv: LOBSTER *_orderbook_10.csv; the LAST row is the end-of-day book.
    BIN: path to the built tools/replay binary (default ./build/tools/replay).

Runs the corpus through the engine (tools/replay), parses the deterministic
final-state block, and compares the top-10 price levels per side (volume per
level) against the real displayed book. Reports matched levels, volume deltas,
spread/mid, and the engine fill count from the record header (crossing adds =
known divergence source). Exit 0 when the check itself ran (mismatches are
reported, not fatal — this is a measurement, see testdata/real/README.md).
"""
import argparse
import subprocess
import sys


def parse_record(record: str):
    lines = record.splitlines()
    header = lines[0].split()
    assert header[0] == "replay", header
    fills = int(header[2])
    state = {}
    for ln in lines[1:]:
        if ln.startswith("level "):
            _, side, price, vol, n = ln.split()[:5]
            side = int(side)
            state.setdefault(side, {})[int(price)] = int(vol)
    return fills, state


def parse_book_last_row(book_csv: str):
    # columns: AskP1,AskS1,BidP1,BidS1, AskP2,AskS2,BidP2,BidS2, ... (10 levels)
    import csv
    with open(book_csv) as f:
        rows = list(csv.reader(f))
    last = rows[-1]
    asks, bids = {}, {}
    for i in range(10):
        base = i * 4
        ap, asz = int(last[base]), int(last[base + 1])
        bp, bsz = int(last[base + 2]), int(last[base + 3])
        if asz > 0:
            asks[ap] = asz
        if bsz > 0:
            bids[bp] = bsz
    return asks, bids


def topk(d, k=10, reverse=True):
    return dict(sorted(d.items(), key=lambda kv: kv[0], reverse=reverse)[:k])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--book", required=True)
    ap.add_argument("--replay", default="./build/tools/replay")
    args = ap.parse_args()

    out = subprocess.run([args.replay, "--replay", args.corpus],
                         capture_output=True, text=True, check=True)
    fills, state = parse_record(out.stdout)
    real_asks, real_bids = parse_book_last_row(args.book)

    eng_asks = topk(state.get(0, {}), 10, reverse=False)   # sell = lowest asks
    eng_bids = topk(state.get(1, {}), 10, reverse=True)

    print(f"engine fills in replay (crossing adds): {fills}")
    for label, real, eng in (("ASK", real_asks, eng_asks), ("BID", real_bids, eng_bids)):
        rp = set(real); ep = set(eng)
        common = rp & ep
        only_real = rp - ep
        only_eng = ep - rp
        vol_real = sum(real[p] for p in rp)
        vol_eng = sum(eng.get(p, 0) for p in rp)
        match_vol = sum(real[p] for p in common if real[p] == eng.get(p))
        print(f"{label}: real levels={len(rp)} engine={len(ep)} "
              f"common={len(common)} real-only={sorted(only_real)[:5]} "
              f"eng-only={len(only_eng)}")
        print(f"  matched-vol levels: {sum(1 for p in common if real[p] == eng.get(p))}/{len(common)}")
        print(f"  top10 real vol={vol_real} engine(real prices) vol={vol_eng} "
              f"delta={vol_eng - vol_real:+d} ({100 * (vol_eng - vol_real) / max(1, vol_real):+.2f}%)")
    if real_asks and real_bids and state.get(0) and state.get(1):
        ra = min(real_asks); rb = max(real_bids)
        ea = min(state[0]); eb = max(state[1])
        print(f"spread real={ra - rb} engine={ea - eb} | mid real={(ra + rb) // 2} engine={(ea + eb) // 2}")


if __name__ == "__main__":
    main()
