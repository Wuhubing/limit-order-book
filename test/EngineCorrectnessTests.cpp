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
