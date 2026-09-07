#include "../Limit_Order_Book/Book.hpp"
#include "../Limit_Order_Book/Limit.hpp"
#include "../Limit_Order_Book/Order.hpp"

#include <gtest/gtest.h>

struct EngineCorrectnessTests : public ::testing::Test
{
    Book* book;

    void SetUp() override
    {
        book = new Book();
    }

    void TearDown() override
    {
        delete book;
    }
};

// Issue #1 (semantics [FIX]): a duplicate live order id must be rejected with no
// state change and no leak. Upstream silently emplace-fails but still appends the
// new order to the limit queue, corrupting the book.
TEST_F(EngineCorrectnessTests, DuplicateIdAddIsRejected)
{
    book->addLimitOrder(1, true, 10, 100);
    book->addLimitOrder(1, true, 20, 101);

    EXPECT_EQ(book->searchOrderMap(1)->getShares(), 10);
    EXPECT_EQ(book->searchLimitMaps(101, true), nullptr);
}

// Issue #1: a rejected id becomes reusable once its previous order is dead.
TEST_F(EngineCorrectnessTests, DuplicateIdReusableAfterCancel)
{
    book->addLimitOrder(1, true, 10, 100);
    book->addLimitOrder(1, true, 20, 101);

    EXPECT_EQ(book->searchOrderMap(1)->getShares(), 10);

    book->cancelLimitOrder(1);
    EXPECT_EQ(book->searchOrderMap(1), nullptr);

    book->addLimitOrder(1, true, 20, 101);
    EXPECT_EQ(book->searchOrderMap(1)->getShares(), 20);
    EXPECT_EQ(book->searchOrderMap(1)->getLimit(), 101);
}

// Issue #3 (semantics [FIX]): a modify whose new limit crosses the book must
// execute aggressively like an AddLimit (full fill => order dead, no cross).
TEST_F(EngineCorrectnessTests, ModifyBuyToCrossingExecutesFullFill)
{
    book->addLimitOrder(1, false, 10, 100); // sell 10 @ 100
    book->addLimitOrder(2, true, 5, 90);    // buy 5 @ 90 (rests)

    book->modifyLimitOrder(2, 5, 105);      // buy 5 @ 105 crosses the sell

    EXPECT_EQ(book->searchOrderMap(1)->getShares(), 5); // sell partially filled
    EXPECT_EQ(book->searchOrderMap(2), nullptr);        // buy fully executed
    EXPECT_EQ(book->getHighestBuy(), nullptr);          // no crossed buy resting
    EXPECT_EQ(book->getLowestSell()->getLimitPrice(), 100);
}

// Issue #3 (semantics [FIX]): remainder rests at the new limit after crossing.
TEST_F(EngineCorrectnessTests, ModifyToCrossingLeavesRemainderResting)
{
    book->addLimitOrder(1, false, 10, 100); // sell 10 @ 100
    book->addLimitOrder(2, true, 5, 90);

    book->modifyLimitOrder(2, 12, 105);     // buy 12 @ 105 crosses the sell

    EXPECT_EQ(book->searchOrderMap(1), nullptr);           // sell fully consumed
    EXPECT_EQ(book->searchOrderMap(2)->getShares(), 2);    // remainder rests
    EXPECT_EQ(book->searchOrderMap(2)->getLimit(), 105);
    EXPECT_EQ(book->getLowestSell(), nullptr);             // no sells left
    EXPECT_EQ(book->getHighestBuy()->getLimitPrice(), 105);// rests at new limit
}

// Issue #13 (documented upstream behavior, kept): a same-price modify re-appends
// at the tail and therefore loses time priority.
TEST_F(EngineCorrectnessTests, SamePriceModifyLosesFifoPriority)
{
    book->addLimitOrder(1, true, 10, 100);
    book->addLimitOrder(2, true, 20, 100);

    book->modifyLimitOrder(1, 15, 100); // same price, size change

    EXPECT_EQ(book->getHighestBuy()->getHeadOrder()->getOrderId(), 2);
    EXPECT_EQ(book->getHighestBuy()->getHeadOrder()->getNextOrder()->getOrderId(), 1);
}

// Semantics (documented upstream behavior, kept): a market order's remainder is
// dropped on insufficient liquidity (market orders never rest).
TEST_F(EngineCorrectnessTests, MarketOrderRemainderDropped)
{
    book->addLimitOrder(1, false, 10, 100); // sell 10 @ 100
    book->marketOrder(2, true, 15);         // buy 15

    EXPECT_EQ(book->searchOrderMap(1), nullptr);
    EXPECT_EQ(book->getLowestSell(), nullptr);
    EXPECT_EQ(book->getHighestBuy(), nullptr); // nothing rests
}

// Cancel of the head order at a level (level survives).
TEST_F(EngineCorrectnessTests, CancelHeadOrder)
{
    book->addLimitOrder(1, true, 10, 100);
    book->addLimitOrder(2, true, 20, 100);

    book->cancelLimitOrder(1);

    EXPECT_EQ(book->searchOrderMap(1), nullptr);
    EXPECT_EQ(book->getHighestBuy()->getHeadOrder()->getOrderId(), 2);
    EXPECT_EQ(book->searchLimitMaps(100, true)->getSize(), 1);
}

// Cancel of a middle order keeps head/tail chain intact.
TEST_F(EngineCorrectnessTests, CancelMiddleOrder)
{
    book->addLimitOrder(1, true, 10, 100);
    book->addLimitOrder(2, true, 20, 100);
    book->addLimitOrder(3, true, 30, 100);

    book->cancelLimitOrder(2);

    EXPECT_EQ(book->searchOrderMap(2), nullptr);
    EXPECT_EQ(book->getHighestBuy()->getHeadOrder()->getOrderId(), 1);
    EXPECT_EQ(book->getHighestBuy()->getHeadOrder()->getNextOrder()->getOrderId(), 3);
    EXPECT_EQ(book->getHighestBuy()->getHeadOrder()->getNextOrder()->getNextOrder(), nullptr);
    EXPECT_EQ(book->searchLimitMaps(100, true)->getSize(), 2);
    EXPECT_EQ(book->searchLimitMaps(100, true)->getTotalVolume(), 40);
}

// Cancel of the tail order.
TEST_F(EngineCorrectnessTests, CancelTailOrder)
{
    book->addLimitOrder(1, true, 10, 100);
    book->addLimitOrder(2, true, 20, 100);

    book->cancelLimitOrder(2);

    EXPECT_EQ(book->searchOrderMap(2), nullptr);
    EXPECT_EQ(book->getHighestBuy()->getHeadOrder()->getOrderId(), 1);
    EXPECT_EQ(book->getHighestBuy()->getHeadOrder()->getNextOrder(), nullptr);
}

// Cancel of the last order at a level removes the level and its map entry.
TEST_F(EngineCorrectnessTests, CancelLastOrderRemovesLevel)
{
    book->addLimitOrder(1, true, 10, 100);

    book->cancelLimitOrder(1);

    EXPECT_EQ(book->searchOrderMap(1), nullptr);
    EXPECT_EQ(book->getHighestBuy(), nullptr);
    EXPECT_EQ(book->searchLimitMaps(100, true), nullptr);
}

// A partial fill keeps the head order in place with the remaining qty.
TEST_F(EngineCorrectnessTests, PartialFillKeepsHeadOrderInPlace)
{
    book->addLimitOrder(1, false, 10, 100);
    book->addLimitOrder(2, false, 20, 100);

    book->marketOrder(3, true, 5);

    EXPECT_EQ(book->getLowestSell()->getHeadOrder()->getOrderId(), 1);
    EXPECT_EQ(book->getLowestSell()->getHeadOrder()->getShares(), 5);
    EXPECT_EQ(book->getLowestSell()->getTotalVolume(), 25);
}

// A multi-level aggressive sweep fills each level at its own price, FIFO within
// each level, and handles exact-boundary exhaustion.
TEST_F(EngineCorrectnessTests, MultiLevelAggressiveSweep)
{
    book->addLimitOrder(1, false, 10, 100);
    book->addLimitOrder(2, false, 20, 100);
    book->addLimitOrder(3, false, 30, 101);

    book->addLimitOrder(4, true, 60, 102); // crosses all three sells exactly

    EXPECT_EQ(book->searchOrderMap(1), nullptr);
    EXPECT_EQ(book->searchOrderMap(2), nullptr);
    EXPECT_EQ(book->searchOrderMap(3), nullptr);
    EXPECT_EQ(book->searchOrderMap(4), nullptr); // fully consumed
    EXPECT_EQ(book->getLowestSell(), nullptr);
}

// Empty-book operations are safe no-ops.
TEST_F(EngineCorrectnessTests, EmptyBookOpsAreNoops)
{
    book->marketOrder(1, true, 10);
    book->marketOrder(2, false, 10);
    book->cancelLimitOrder(999);
    book->modifyLimitOrder(999, 10, 100);

    EXPECT_EQ(book->getHighestBuy(), nullptr);
    EXPECT_EQ(book->getLowestSell(), nullptr);
    EXPECT_TRUE(book->snapshot().empty());
}

// One-sided book operations are safe no-ops.
TEST_F(EngineCorrectnessTests, OneSidedBookOpsAreNoops)
{
    book->addLimitOrder(1, true, 10, 100); // only a buy side

    book->marketOrder(2, true, 10);        // buy against empty sell side

    EXPECT_EQ(book->getHighestBuy()->getTotalVolume(), 10);
    EXPECT_EQ(book->getHighestBuy()->getHeadOrder()->getOrderId(), 1);
    EXPECT_EQ(book->getLowestSell(), nullptr);
}

// Every validation rejection leaves state and the event stream untouched.
TEST_F(EngineCorrectnessTests, ValidationRejectsLeaveStateUntouched)
{
    std::vector<Book::FillEvent> events;
    book->setFillSink(&events);

    book->addLimitOrder(0, true, 10, 100);  // id <= 0
    book->addLimitOrder(1, true, 0, 100);   // qty <= 0
    book->addLimitOrder(1, true, 10, 0);    // price <= 0
    book->marketOrder(1, true, 0);          // qty <= 0
    book->marketOrder(1, true, -5);         // qty < 0

    EXPECT_TRUE(book->snapshot().empty());
    EXPECT_TRUE(events.empty());

    // A live order then survives a bad modify unchanged.
    book->addLimitOrder(7, true, 10, 100);
    book->modifyLimitOrder(7, 0, 100);   // newQty <= 0
    book->modifyLimitOrder(7, 10, 0);    // newLimit <= 0
    book->modifyLimitOrder(999, 10, 100); // unknown id

    EXPECT_EQ(book->searchOrderMap(7)->getShares(), 10);
    EXPECT_EQ(book->searchOrderMap(7)->getLimit(), 100);
    EXPECT_TRUE(events.empty());
}

// Fill events carry the correct price/qty/order ids, are emitted in execution
// order, and carry a monotonically increasing engine-lifetime seq.
TEST_F(EngineCorrectnessTests, FillEventsCorrect)
{
    std::vector<Book::FillEvent> events;
    book->setFillSink(&events);

    book->addLimitOrder(1, false, 10, 100);
    book->addLimitOrder(2, false, 20, 100);
    book->addLimitOrder(3, false, 30, 101);

    book->marketOrder(4, true, 55);

    ASSERT_EQ(events.size(), 3);

    EXPECT_EQ(events[0].aggressorId, 4);
    EXPECT_EQ(events[0].restingId, 1);
    EXPECT_EQ(events[0].price, 100);
    EXPECT_EQ(events[0].qty, 10);
    EXPECT_TRUE(events[0].aggressorBuy);

    EXPECT_EQ(events[1].aggressorId, 4);
    EXPECT_EQ(events[1].restingId, 2);
    EXPECT_EQ(events[1].price, 100);
    EXPECT_EQ(events[1].qty, 20);
    EXPECT_TRUE(events[1].aggressorBuy);

    EXPECT_EQ(events[2].aggressorId, 4);
    EXPECT_EQ(events[2].restingId, 3);
    EXPECT_EQ(events[2].price, 101);
    EXPECT_EQ(events[2].qty, 25);
    EXPECT_TRUE(events[2].aggressorBuy);

    EXPECT_EQ(events[0].seq, 0u);
    EXPECT_EQ(events[1].seq, 1u);
    EXPECT_EQ(events[2].seq, 2u);

    // The resting order 3 is left with its remainder.
    EXPECT_EQ(book->searchOrderMap(3)->getShares(), 5);

    // A sell aggressor produces aggressorBuy == false and seq keeps increasing.
    book->addLimitOrder(5, true, 10, 100);
    book->marketOrder(6, false, 4);

    ASSERT_EQ(events.size(), 4);
    EXPECT_EQ(events[3].aggressorId, 6);
    EXPECT_EQ(events[3].restingId, 5);
    EXPECT_EQ(events[3].price, 100);
    EXPECT_EQ(events[3].qty, 4);
    EXPECT_FALSE(events[3].aggressorBuy);
    EXPECT_EQ(events[3].seq, 3u);
}

// The snapshot oracle interface mirrors state asserted through public getters.
TEST_F(EngineCorrectnessTests, SnapshotMatchesState)
{
    book->addLimitOrder(1, true, 10, 100);
    book->addLimitOrder(2, true, 20, 99);
    book->addLimitOrder(3, false, 5, 105);
    book->addLimitOrder(4, false, 7, 110);
    book->addLimitOrder(5, false, 9, 105); // append at 105, tail of that level

    auto snap = book->snapshot();

    ASSERT_EQ(snap.size(), 4);

    // Buy levels ascending: 99 then 100.
    EXPECT_EQ(snap[0].price, 99);
    EXPECT_EQ(snap[0].side, true);
    EXPECT_EQ(snap[0].totalVolume, 20);
    ASSERT_EQ(snap[0].orders.size(), 1);
    EXPECT_EQ(snap[0].orders[0].id, 2);
    EXPECT_EQ(snap[0].orders[0].qty, 20);

    EXPECT_EQ(snap[1].price, 100);
    EXPECT_EQ(snap[1].side, true);
    EXPECT_EQ(snap[1].totalVolume, 10);
    ASSERT_EQ(snap[1].orders.size(), 1);
    EXPECT_EQ(snap[1].orders[0].id, 1);
    EXPECT_EQ(snap[1].orders[0].qty, 10);

    // Sell levels ascending: 105 then 110. 105 has FIFO head->tail (3 then 5).
    EXPECT_EQ(snap[2].price, 105);
    EXPECT_EQ(snap[2].side, false);
    EXPECT_EQ(snap[2].totalVolume, 14);
    ASSERT_EQ(snap[2].orders.size(), 2);
    EXPECT_EQ(snap[2].orders[0].id, 3);
    EXPECT_EQ(snap[2].orders[0].qty, 5);
    EXPECT_EQ(snap[2].orders[1].id, 5);
    EXPECT_EQ(snap[2].orders[1].qty, 9);

    EXPECT_EQ(snap[3].price, 110);
    EXPECT_EQ(snap[3].side, false);
    EXPECT_EQ(snap[3].totalVolume, 7);
    ASSERT_EQ(snap[3].orders.size(), 1);
    EXPECT_EQ(snap[3].orders[0].id, 4);
    EXPECT_EQ(snap[3].orders[0].qty, 7);

    // Cross-check against the public getters.
    EXPECT_EQ(snap[1].price, book->getHighestBuy()->getLimitPrice());
    EXPECT_EQ(snap[2].price, book->getLowestSell()->getLimitPrice());
}

// A disabled sink (nullptr) emits nothing and is safe.
TEST_F(EngineCorrectnessTests, NullFillSinkIsSafe)
{
    book->addLimitOrder(1, false, 10, 100);
    book->marketOrder(2, true, 5);
    EXPECT_EQ(book->searchOrderMap(1)->getShares(), 5);
}
