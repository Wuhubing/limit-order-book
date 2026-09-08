// Differential test suite (LOB-003).
//
// Sections:
//  (1) Reference-model unit scenarios (engine-free) mirroring semantics.md.
//  (2) Differential fuzz: >=20 seeds x >=2000 requests, engine vs reference,
//      comparing FillEvent streams AND full state after every request, plus the
//      I1-I7 invariant checker after every request.
//  (3) Determinism self-check: same corpus -> two fresh engine instances are
//      identical.
//  (4) Corpus parser <-> generator round-trip + generator degenerate-injection.
//
// On any differential mismatch the failing corpus is dumped and minimised under
// .agent/results/failures/ and the gtest failure names the seed, request index,
// and first divergence. No test here is disabled.

#include "../Limit_Order_Book/Book.hpp"
#include "../reference/Command.hpp"
#include "../reference/DifferentialHarness.hpp"
#include "../reference/ReferenceBook.hpp"
#include "../reference/WorkloadGenerator.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using lobref::Command;
using lobref::ReferenceBook;

// Canonical one-line dump of the reference book for compact scenario assertions.
std::string dumpRef(const ReferenceBook& rb)
{
    std::string s;
    for (const auto& ls : rb.snapshot()) {
        s += ls.side ? "B" : "S";
        s += std::to_string(ls.price);
        s += "[";
        for (std::size_t i = 0; i < ls.orders.size(); ++i) {
            if (i) {
                s += ",";
            }
            s += std::to_string(ls.orders[i].id) + ":" + std::to_string(ls.orders[i].qty);
        }
        s += "] ";
    }
    s += "fills{";
    for (const auto& f : rb.fills()) {
        s += std::to_string(f.aggressorId) + ">" + std::to_string(f.restingId) + "@" +
             std::to_string(f.price) + "x" + std::to_string(f.qty) + (f.aggressorBuy ? "B" : "S") +
             "#" + std::to_string(f.seq) + ";";
    }
    s += "}";
    return s;
}

bool snapshotsEqual(const std::vector<Book::LevelState>& a, const std::vector<Book::LevelState>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].price != b[i].price || a[i].side != b[i].side ||
            a[i].totalVolume != b[i].totalVolume || a[i].orders.size() != b[i].orders.size()) {
            return false;
        }
        for (std::size_t j = 0; j < a[i].orders.size(); ++j) {
            if (a[i].orders[j].id != b[i].orders[j].id || a[i].orders[j].qty != b[i].orders[j].qty) {
                return false;
            }
        }
    }
    return true;
}

bool fillsEqual(const std::vector<Book::FillEvent>& a, const std::vector<Book::FillEvent>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].aggressorId != b[i].aggressorId || a[i].restingId != b[i].restingId ||
            a[i].price != b[i].price || a[i].qty != b[i].qty ||
            a[i].aggressorBuy != b[i].aggressorBuy || a[i].seq != b[i].seq) {
            return false;
        }
    }
    return true;
}

struct EngineTrace {
    std::vector<std::vector<Book::FillEvent>> fills;
    std::vector<std::vector<Book::LevelState>> snaps;
};

EngineTrace runEngine(const std::vector<Command>& commands)
{
    Book book;
    std::vector<Book::FillEvent> sink;
    book.setFillSink(&sink);
    EngineTrace trace;
    for (const auto& c : commands) {
        lobdiff::applyToEngine(book, c);
        trace.fills.push_back(sink);
        trace.snaps.push_back(book.snapshot());
    }
    return trace;
}

void writeTextFile(const std::filesystem::path& path, const std::string& content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

// Save full + minimized corpus on mismatch and emit the failure text.
void reportMismatch(std::uint32_t seed, const std::vector<Command>& commands,
                    const lobdiff::Divergence& div)
{
    const std::string dir = ".agent/results/failures";
    std::filesystem::create_directories(dir);

    const std::string fullName = dir + "/seed_" + std::to_string(seed) + ".txt";
    writeTextFile(fullName, lobref::serializeCorpus(commands));

    const std::vector<Command> minimal = lobdiff::minimize(commands);
    const std::string minName = dir + "/seed_" + std::to_string(seed) + ".min.txt";
    writeTextFile(minName, lobref::serializeCorpus(minimal));

    ADD_FAILURE() << "differential mismatch: seed=" << seed
                  << " requestIndex=" << div.requestIndex
                  << " firstDivergence={" << div.message << "}"
                  << "\nfull corpus saved to " << fullName
                  << "\nminimal corpus (" << minimal.size() << " commands) saved to " << minName
                  << "\nminimal corpus:\n"
                  << lobref::serializeCorpus(minimal);
}

} // namespace

// ---------------------------------------------------------------------------
// (1) Reference-model unit scenarios (standalone, no engine).
// ---------------------------------------------------------------------------

TEST(ReferenceScenarios, FifoOrderAndPartialFillKeepsHeadPriority)
{
    ReferenceBook rb;
    rb.addLimitOrder(1, false, 10, 100);
    rb.addLimitOrder(2, false, 20, 100);
    rb.addLimitOrder(3, true, 5, 100);

    EXPECT_EQ(dumpRef(rb), "S100[1:5,2:20] fills{3>1@100x5B#0;}");
}

TEST(ReferenceScenarios, PricePriorityAcrossLevels)
{
    ReferenceBook rb;
    rb.addLimitOrder(1, false, 10, 100);
    rb.addLimitOrder(2, false, 20, 101);
    rb.marketOrder(3, true, 15);

    EXPECT_EQ(dumpRef(rb), "S101[2:15] fills{3>1@100x10B#0;3>2@101x5B#1;}");
}

TEST(ReferenceScenarios, MultiLevelSweepWithPerLevelPrices)
{
    ReferenceBook rb;
    rb.addLimitOrder(1, false, 10, 100);
    rb.addLimitOrder(2, false, 20, 100);
    rb.addLimitOrder(3, false, 30, 101);
    rb.addLimitOrder(4, true, 60, 102);

    EXPECT_EQ(dumpRef(rb), "fills{4>1@100x10B#0;4>2@100x20B#1;4>3@101x30B#2;}");
}

TEST(ReferenceScenarios, CancelHeadMiddleTailLastAndLevelCleanup)
{
    ReferenceBook rb;
    rb.addLimitOrder(1, true, 10, 100);
    rb.addLimitOrder(2, true, 20, 100);
    rb.addLimitOrder(3, true, 30, 100);

    rb.cancelLimitOrder(1); // head
    EXPECT_EQ(dumpRef(rb), "B100[2:20,3:30] fills{}");

    rb.cancelLimitOrder(2); // middle -> now 3 alone
    EXPECT_EQ(dumpRef(rb), "B100[3:30] fills{}");

    rb.cancelLimitOrder(3); // last -> level removed
    EXPECT_EQ(dumpRef(rb), "fills{}");
}

TEST(ReferenceScenarios, ModifySamePriceReinsertsAtTail)
{
    ReferenceBook rb;
    rb.addLimitOrder(1, true, 10, 100);
    rb.addLimitOrder(2, true, 20, 100);
    rb.modifyLimitOrder(1, 15, 100);

    EXPECT_EQ(dumpRef(rb), "B100[2:20,1:15] fills{}");
}

TEST(ReferenceScenarios, ModifyToCrossingLeavesRemainderResting)
{
    ReferenceBook rb;
    rb.addLimitOrder(1, false, 10, 100);
    rb.addLimitOrder(2, true, 5, 90);
    rb.modifyLimitOrder(2, 12, 105);

    EXPECT_EQ(dumpRef(rb), "B105[2:2] fills{2>1@100x10B#0;}");
}

TEST(ReferenceScenarios, ModifyToCrossingFullFillDeletesOrder)
{
    ReferenceBook rb;
    rb.addLimitOrder(1, false, 10, 100);
    rb.addLimitOrder(2, true, 5, 90);
    rb.modifyLimitOrder(2, 5, 105);

    EXPECT_EQ(dumpRef(rb), "S100[1:5] fills{2>1@100x5B#0;}");
}

TEST(ReferenceScenarios, MarketRemainderDropped)
{
    ReferenceBook rb;
    rb.addLimitOrder(1, false, 10, 100);
    rb.marketOrder(2, true, 15);

    EXPECT_EQ(dumpRef(rb), "fills{2>1@100x10B#0;}");
}

TEST(ReferenceScenarios, EmptyAndOneSidedBookAreNoops)
{
    ReferenceBook rb;
    rb.marketOrder(1, true, 10);
    rb.addLimitOrder(2, true, 10, 100);
    rb.marketOrder(3, true, 5); // buy against empty sell side
    rb.cancelLimitOrder(999);
    rb.modifyLimitOrder(999, 10, 100);

    EXPECT_EQ(dumpRef(rb), "B100[2:10] fills{}");
}

TEST(ReferenceScenarios, ValidationRejectsLeaveStateUntouched)
{
    ReferenceBook rb;
    rb.addLimitOrder(0, true, 10, 100);
    rb.addLimitOrder(1, true, 0, 100);
    rb.addLimitOrder(1, true, 10, 0);
    rb.marketOrder(1, true, 0);
    rb.marketOrder(1, true, -5);
    rb.addLimitOrder(7, true, 10, 100);
    rb.addLimitOrder(7, true, 20, 101); // duplicate live id
    rb.modifyLimitOrder(7, 0, 100);
    rb.modifyLimitOrder(7, 10, 0);
    rb.modifyLimitOrder(999, 10, 100);

    EXPECT_EQ(dumpRef(rb), "B100[7:10] fills{}");
}

TEST(ReferenceScenarios, FillEventOrderAndSeq)
{
    ReferenceBook rb;
    rb.addLimitOrder(1, false, 10, 100);
    rb.addLimitOrder(2, false, 20, 100);
    rb.addLimitOrder(3, false, 30, 101);
    rb.marketOrder(4, true, 55);

    ASSERT_EQ(rb.fills().size(), 3u);
    EXPECT_EQ(rb.fills()[0].aggressorId, 4);
    EXPECT_EQ(rb.fills()[0].restingId, 1);
    EXPECT_EQ(rb.fills()[0].price, 100);
    EXPECT_EQ(rb.fills()[0].qty, 10);
    EXPECT_TRUE(rb.fills()[0].aggressorBuy);
    EXPECT_EQ(rb.fills()[0].seq, 0u);

    EXPECT_EQ(rb.fills()[1].restingId, 2);
    EXPECT_EQ(rb.fills()[1].qty, 20);
    EXPECT_EQ(rb.fills()[1].seq, 1u);

    EXPECT_EQ(rb.fills()[2].restingId, 3);
    EXPECT_EQ(rb.fills()[2].price, 101);
    EXPECT_EQ(rb.fills()[2].qty, 25);
    EXPECT_EQ(rb.fills()[2].seq, 2u);

    // sell aggressor -> aggressorBuy == false, seq keeps increasing.
    rb.addLimitOrder(5, true, 10, 100);
    rb.marketOrder(6, false, 4);

    ASSERT_EQ(rb.fills().size(), 4u);
    EXPECT_EQ(rb.fills()[3].aggressorId, 6);
    EXPECT_EQ(rb.fills()[3].restingId, 5);
    EXPECT_EQ(rb.fills()[3].price, 100);
    EXPECT_EQ(rb.fills()[3].qty, 4);
    EXPECT_FALSE(rb.fills()[3].aggressorBuy);
    EXPECT_EQ(rb.fills()[3].seq, 3u);
}

TEST(ReferenceScenarios, IdReuseAfterDeath)
{
    ReferenceBook rb;
    rb.addLimitOrder(1, true, 10, 100);
    rb.cancelLimitOrder(1);
    rb.addLimitOrder(1, true, 20, 101);

    EXPECT_EQ(dumpRef(rb), "B101[1:20] fills{}");
}

// ---------------------------------------------------------------------------
// (2) Differential fuzz: engine vs reference.
// ---------------------------------------------------------------------------

TEST(DifferentialFuzz, TwentySeedsByTwoThousandRequestsMatch)
{
    constexpr std::uint32_t kSeeds = 20;
    constexpr std::size_t kRequests = 2000;

    for (std::uint32_t seed = 0; seed < kSeeds; ++seed) {
        SCOPED_TRACE("seed=" + std::to_string(seed));

        lobref::GeneratorParams p;
        p.seed = seed;
        p.num_requests = kRequests;

        const std::vector<Command> commands = lobref::generateWorkload(p);
        ASSERT_EQ(commands.size(), kRequests);

        lobdiff::Divergence div;
        if (!lobdiff::replayAndCompare(commands, &div)) {
            reportMismatch(seed, commands, div);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// (3) Determinism self-check.
// ---------------------------------------------------------------------------

TEST(DifferentialDeterminism, SameCorpusTwoEngineInstancesAreIdentical)
{
    lobref::GeneratorParams p;
    p.seed = 12345;
    p.num_requests = 2000;
    const std::vector<Command> commands = lobref::generateWorkload(p);

    const EngineTrace a = runEngine(commands);
    const EngineTrace b = runEngine(commands);

    ASSERT_EQ(a.fills.size(), b.fills.size());
    ASSERT_EQ(a.snaps.size(), b.snaps.size());
    for (std::size_t i = 0; i < commands.size(); ++i) {
        EXPECT_TRUE(fillsEqual(a.fills[i], b.fills[i])) << "fills diverge at request " << i;
        EXPECT_TRUE(snapshotsEqual(a.snaps[i], b.snaps[i])) << "state diverges at request " << i;
    }
}

TEST(DifferentialDeterminism, SameCorpusReferenceIsDeterministic)
{
    lobref::GeneratorParams p;
    p.seed = 777;
    p.num_requests = 1000;
    const std::vector<Command> commands = lobref::generateWorkload(p);

    ReferenceBook a;
    ReferenceBook b;
    for (const auto& c : commands) {
        lobdiff::applyToReference(a, c);
        lobdiff::applyToReference(b, c);
    }
    EXPECT_EQ(dumpRef(a), dumpRef(b));
}

// ---------------------------------------------------------------------------
// (4) Corpus round-trip + generator properties.
// ---------------------------------------------------------------------------

TEST(CorpusRoundTrip, ParseEqualsGeneratedStream)
{
    for (std::uint32_t seed = 0; seed < 5; ++seed) {
        lobref::GeneratorParams p;
        p.seed = seed;
        p.num_requests = 500;
        const std::vector<Command> generated = lobref::generateWorkload(p);
        const std::string text = lobref::serializeCorpus(generated);
        const std::vector<Command> parsed = lobref::parseCorpus(text);

        ASSERT_EQ(parsed.size(), generated.size());
        for (std::size_t i = 0; i < generated.size(); ++i) {
            EXPECT_EQ(parsed[i].op, generated[i].op);
            EXPECT_EQ(parsed[i].id, generated[i].id);
            EXPECT_EQ(parsed[i].side, generated[i].side);
            EXPECT_EQ(parsed[i].qty, generated[i].qty);
            EXPECT_EQ(parsed[i].price, generated[i].price);
        }
    }
}

TEST(CorpusParser, MalformedLinesThrow)
{
    EXPECT_THROW(lobref::parseCorpus("Add 1 1 10 100\nBogus 2\n"),
                 std::runtime_error);
    EXPECT_THROW(lobref::parseCorpus("Add 1 1 10\n"), std::runtime_error);       // missing price
    EXPECT_THROW(lobref::parseCorpus("Mkt 1 1\n"), std::runtime_error);          // missing qty
    EXPECT_THROW(lobref::parseCorpus("Add 1 2 10 100\n"), std::runtime_error);   // bad side
    EXPECT_THROW(lobref::parseCorpus("Cxl\n"), std::runtime_error);              // missing id
}

TEST(WorkloadGenerator, InjectsClearlyDegenerateCommands)
{
    bool sawBadQty = false;
    bool sawBadPrice = false;
    bool sawBadId = false;
    for (std::uint32_t seed = 0; seed < 10 && !(sawBadQty && sawBadPrice && sawBadId); ++seed) {
        lobref::GeneratorParams p;
        p.seed = seed;
        p.num_requests = 2000;
        for (const auto& c : lobref::generateWorkload(p)) {
            if (c.qty <= 0) {
                sawBadQty = true;
            }
            if (c.price <= 0) {
                sawBadPrice = true;
            }
            if (c.id <= 0) {
                sawBadId = true;
            }
        }
    }
    EXPECT_TRUE(sawBadQty);
    EXPECT_TRUE(sawBadPrice);
    EXPECT_TRUE(sawBadId);
}

// ---------------------------------------------------------------------------
// (5) Invariant checker positive control: a valid book must be clean.
// ---------------------------------------------------------------------------

TEST(InvariantChecker, ValidBookReportsNoViolations)
{
    Book book;
    book.addLimitOrder(1, false, 10, 100);
    book.addLimitOrder(2, false, 20, 100);
    book.addLimitOrder(3, false, 30, 101);
    book.addLimitOrder(4, true, 60, 102); // sweeps, no cross remains
    book.addLimitOrder(5, true, 7, 99);
    book.addLimitOrder(6, true, 3, 99);
    book.marketOrder(7, true, 4);

    EXPECT_TRUE(lobdiff::checkInvariants(book).empty());
}
