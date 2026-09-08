#ifndef REFERENCE_DIFFERENTIALHARNESS_HPP
#define REFERENCE_DIFFERENTIALHARNESS_HPP

// Differential harness: drives the engine Book and the reference model from the
// same command stream, compares FillEvent streams and full logical book state
// after EVERY request, runs the I1-I7 invariant checker against the engine, and
// shrinks any mismatch to a minimal failing corpus.
//
// This header is the one place that is allowed to include the engine headers; it
// is NOT part of the reference model (which remains engine-free).

#include "Command.hpp"
#include "ReferenceBook.hpp"

#include "../Limit_Order_Book/Book.hpp"
#include "../Limit_Order_Book/Limit.hpp"
#include "../Limit_Order_Book/Order.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace lobdiff {

struct Divergence {
    bool found = false;
    int requestIndex = -1; // 0-based command index where divergence first appeared
    std::string message;
};

inline void applyToEngine(Book& book, const lobref::Command& c)
{
    switch (c.op) {
    case lobref::Command::Op::Add:
        book.addLimitOrder(static_cast<int>(c.id), c.side, static_cast<int>(c.qty),
                           static_cast<int>(c.price));
        break;
    case lobref::Command::Op::Mkt:
        book.marketOrder(static_cast<int>(c.id), c.side, static_cast<int>(c.qty));
        break;
    case lobref::Command::Op::Cxl:
        book.cancelLimitOrder(static_cast<int>(c.id));
        break;
    case lobref::Command::Op::Mod:
        book.modifyLimitOrder(static_cast<int>(c.id), static_cast<int>(c.qty),
                              static_cast<int>(c.price));
        break;
    }
}

inline void applyToReference(lobref::ReferenceBook& rb, const lobref::Command& c)
{
    switch (c.op) {
    case lobref::Command::Op::Add:
        rb.addLimitOrder(c.id, c.side, c.qty, c.price);
        break;
    case lobref::Command::Op::Mkt:
        rb.marketOrder(c.id, c.side, c.qty);
        break;
    case lobref::Command::Op::Cxl:
        rb.cancelLimitOrder(c.id);
        break;
    case lobref::Command::Op::Mod:
        rb.modifyLimitOrder(c.id, c.qty, c.price);
        break;
    }
}

inline bool fillsEqual(const std::vector<Book::FillEvent>& a,
                       const std::vector<lobref::ReferenceBook::FillEvent>& b,
                       std::string* diff)
{
    if (a.size() != b.size()) {
        if (diff) {
            *diff = "fill count " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
        }
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].aggressorId != b[i].aggressorId || a[i].restingId != b[i].restingId ||
            a[i].price != b[i].price || a[i].qty != b[i].qty ||
            a[i].aggressorBuy != b[i].aggressorBuy || a[i].seq != b[i].seq) {
            if (diff) {
                *diff = "fill[" + std::to_string(i) + "] diverges: engine(" +
                        std::to_string(a[i].aggressorId) + "," + std::to_string(a[i].restingId) +
                        "," + std::to_string(a[i].price) + "," + std::to_string(a[i].qty) + "," +
                        (a[i].aggressorBuy ? "buy" : "sell") + "," + std::to_string(a[i].seq) +
                        ") vs ref(" + std::to_string(b[i].aggressorId) + "," +
                        std::to_string(b[i].restingId) + "," + std::to_string(b[i].price) + "," +
                        std::to_string(b[i].qty) + "," + (b[i].aggressorBuy ? "buy" : "sell") +
                        "," + std::to_string(b[i].seq) + ")";
            }
            return false;
        }
    }
    return true;
}

inline bool stateEqual(const Book& book, const lobref::ReferenceBook& rb, std::string* diff)
{
    const std::vector<Book::LevelState> a = book.snapshot();
    const std::vector<lobref::ReferenceBook::LevelState> b = rb.snapshot();
    if (a.size() != b.size()) {
        if (diff) {
            *diff = "level count " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
        }
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].price != b[i].price || a[i].side != b[i].side ||
            a[i].totalVolume != b[i].totalVolume || a[i].orders.size() != b[i].orders.size()) {
            if (diff) {
                *diff = "level[" + std::to_string(i) + "] diverges";
            }
            return false;
        }
        for (std::size_t j = 0; j < a[i].orders.size(); ++j) {
            if (a[i].orders[j].id != b[i].orders[j].id || a[i].orders[j].qty != b[i].orders[j].qty) {
                if (diff) {
                    *diff = "level[" + std::to_string(i) + "].order[" + std::to_string(j) +
                            "] diverges: engine(id=" + std::to_string(a[i].orders[j].id) +
                            ",qty=" + std::to_string(a[i].orders[j].qty) + ") vs ref(id=" +
                            std::to_string(b[i].orders[j].id) +
                            ",qty=" + std::to_string(b[i].orders[j].qty) + ")";
                }
                return false;
            }
        }
    }
    return true;
}

// ---- Invariant checker (I1-I7) against the engine ----
// I7 (no dangling storage) is validated by the ASan/UBSan build, not here.

inline int avlHeightCheck(Limit* node, std::vector<std::string>* problems)
{
    if (node == nullptr) {
        return 0;
    }
    const int lh = avlHeightCheck(node->getLeftChild(), problems);
    const int rh = avlHeightCheck(node->getRightChild(), problems);
    if (lh - rh > 1 || rh - lh > 1) {
        problems->push_back("I6: AVL height imbalance at price " +
                            std::to_string(node->getLimitPrice()));
    }
    return std::max(lh, rh) + 1;
}

inline void bstOrderCheck(Limit* node, long long& prev, bool& first, std::vector<std::string>* problems)
{
    if (node == nullptr) {
        return;
    }
    bstOrderCheck(node->getLeftChild(), prev, first, problems);
    if (!first && node->getLimitPrice() <= prev) {
        problems->push_back("I6: BST order violation at price " +
                            std::to_string(node->getLimitPrice()));
    }
    prev = node->getLimitPrice();
    first = false;
    bstOrderCheck(node->getRightChild(), prev, first, problems);
}

inline std::vector<std::string> checkInvariants(const Book& book)
{
    std::vector<std::string> problems;
    const std::vector<Book::LevelState> snap = book.snapshot();

    // I1 + I2 + I3 via snapshot cross-checked against the public getters.
    for (const auto& ls : snap) {
        long long sum = 0;
        int n = 0;
        for (const auto& os : ls.orders) {
            if (os.qty <= 0) {
                problems.push_back("I1: non-positive order qty at level " + std::to_string(ls.price));
            }
            sum += os.qty;
            ++n;
            Order* o = book.searchOrderMap(os.id);
            if (o == nullptr) {
                problems.push_back("I2: snapshot order " + std::to_string(os.id) +
                                   " missing from orderMap");
            } else {
                if (o->getShares() != os.qty) {
                    problems.push_back("I2: orderMap qty mismatch for id " + std::to_string(os.id));
                }
                if (o->getLimit() != ls.price) {
                    problems.push_back("I2: orderMap limit mismatch for id " + std::to_string(os.id));
                }
                if (o->getBuyOrSell() != ls.side) {
                    problems.push_back("I2: orderMap side mismatch for id " + std::to_string(os.id));
                }
                if (o->getParentLimit() != nullptr &&
                    o->getParentLimit()->getLimitPrice() != ls.price) {
                    problems.push_back("I3: parentLimit price mismatch for id " + std::to_string(os.id));
                }
            }
        }
        if (sum != ls.totalVolume) {
            problems.push_back("I1: level volume " + std::to_string(ls.totalVolume) +
                               " != sum " + std::to_string(sum) + " at " + std::to_string(ls.price));
        }
        if (ls.totalVolume < 0) {
            problems.push_back("I1: negative level volume at " + std::to_string(ls.price));
        }

        Limit* level = book.searchLimitMaps(static_cast<int>(ls.price), ls.side);
        if (level == nullptr) {
            problems.push_back("I3: level " + std::to_string(ls.price) + " missing from limitMap");
        } else {
            if (level->getSize() != n) {
                problems.push_back("I3: level size " + std::to_string(level->getSize()) +
                                   " != order count " + std::to_string(n));
            }
            if (level->getTotalVolume() != static_cast<int>(ls.totalVolume)) {
                problems.push_back("I3: level totalVolume mismatch");
            }
            int k = 0;
            for (Order* o = level->getHeadOrder(); o != nullptr; o = o->getNextOrder()) {
                if (k >= static_cast<int>(ls.orders.size()) ||
                    o->getOrderId() != ls.orders[k].id) {
                    problems.push_back("I3: queue chain mismatch at level " +
                                       std::to_string(ls.price));
                    break;
                }
                ++k;
            }
            if (k != static_cast<int>(ls.orders.size())) {
                problems.push_back("I3: queue chain length mismatch at level " +
                                   std::to_string(ls.price));
            }
        }
    }

    // I5: no immediate cross.
    bool hasBuy = false;
    bool hasSell = false;
    long long bestBuy = 0;
    long long bestSell = 0;
    for (const auto& ls : snap) {
        if (ls.side) {
            if (!hasBuy || ls.price > bestBuy) {
                hasBuy = true;
                bestBuy = ls.price;
            }
        } else {
            if (!hasSell || ls.price < bestSell) {
                hasSell = true;
                bestSell = ls.price;
            }
        }
    }
    if (hasBuy && hasSell && bestBuy >= bestSell) {
        problems.push_back("I5: crossed book (bestBuy " + std::to_string(bestBuy) +
                           " >= bestSell " + std::to_string(bestSell) + ")");
    }

    // I6: AVL balance + BST order on the buy and sell trees.
    avlHeightCheck(book.getBuyTree(), &problems);
    avlHeightCheck(book.getSellTree(), &problems);
    {
        long long prev = 0;
        bool first = true;
        bstOrderCheck(book.getBuyTree(), prev, first, &problems);
    }
    {
        long long prev = 0;
        bool first = true;
        bstOrderCheck(book.getSellTree(), prev, first, &problems);
    }

    return problems;
}

// Replay `commands` on fresh engine + reference instances, comparing FillEvent
// streams and full state after EVERY request, and checking invariants after every
// request. Stops at the first divergence. Returns true if identical.
inline bool replayAndCompare(const std::vector<lobref::Command>& commands, Divergence* div)
{
    if (div != nullptr) {
        *div = Divergence{};
    }

    Book book;
    std::vector<Book::FillEvent> engineFills;
    book.setFillSink(&engineFills);

    lobref::ReferenceBook ref;

    for (std::size_t i = 0; i < commands.size(); ++i) {
        applyToEngine(book, commands[i]);
        applyToReference(ref, commands[i]);

        std::string diff;
        if (!fillsEqual(engineFills, ref.fills(), &diff)) {
            if (div != nullptr) {
                div->found = true;
                div->requestIndex = static_cast<int>(i);
                div->message = "fill divergence after request " + std::to_string(i) + ": " + diff;
            }
            return false;
        }
        if (!stateEqual(book, ref, &diff)) {
            if (div != nullptr) {
                div->found = true;
                div->requestIndex = static_cast<int>(i);
                div->message = "state divergence after request " + std::to_string(i) + ": " + diff;
            }
            return false;
        }
        const std::vector<std::string> problems = checkInvariants(book);
        if (!problems.empty()) {
            if (div != nullptr) {
                div->found = true;
                div->requestIndex = static_cast<int>(i);
                div->message = "invariant violation after request " + std::to_string(i) + ": " +
                               problems.front();
            }
            return false;
        }
    }
    return true;
}

// Shortest failing prefix length via bisection over prefix length.
inline int findShortestFailingPrefix(const std::vector<lobref::Command>& commands)
{
    int lo = 1;
    int hi = static_cast<int>(commands.size());
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        std::vector<lobref::Command> sub(commands.begin(), commands.begin() + mid);
        if (!replayAndCompare(sub, nullptr)) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

// Greedy minimisation: (a) shortest failing prefix, then (b) repeatedly try
// removing each remaining command, keeping removals that still fail.
inline std::vector<lobref::Command> minimize(const std::vector<lobref::Command>& commands)
{
    const int prefixLen = findShortestFailingPrefix(commands);
    std::vector<lobref::Command> work(commands.begin(), commands.begin() + prefixLen);

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i < work.size();) {
            std::vector<lobref::Command> cand;
            cand.reserve(work.size() - 1);
            for (std::size_t j = 0; j < work.size(); ++j) {
                if (j != i) {
                    cand.push_back(work[j]);
                }
            }
            if (!replayAndCompare(cand, nullptr)) {
                work = cand;
                changed = true;
            } else {
                ++i;
            }
        }
    }
    return work;
}

} // namespace lobdiff

#endif
