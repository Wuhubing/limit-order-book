#ifndef REFERENCE_REFERENCEBOOK_HPP
#define REFERENCE_REFERENCEBOOK_HPP

#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

// Independent reference implementation of the docs/semantics.md matching core.
//
// This is the ORACLE for the differential harness: it is written from the
// semantics document alone, uses only standard-library containers, and MUST NOT
// include or mirror any Limit_Order_Book engine header or data-structure design.
// Its observable behavior (FillEvent stream + logical book state) is what the
// engine is diffed against, request by request.

namespace lobref {

class ReferenceBook {
public:
    struct FillEvent {
        std::int64_t aggressorId;
        std::int64_t restingId;
        std::int64_t price;
        std::int64_t qty;
        bool aggressorBuy;
        std::uint64_t seq;
    };

    // Shape-compatible with the engine's snapshot() LevelState: buy levels in
    // ascending price, then sell levels in ascending price, each level's orders
    // in FIFO head->tail order.
    struct OrderState {
        std::int64_t id;
        std::int64_t qty;
    };
    struct LevelState {
        std::int64_t price;
        bool side;
        std::int64_t totalVolume;
        std::vector<OrderState> orders;
    };

    ReferenceBook() = default;

    void addLimitOrder(std::int64_t id, bool side, std::int64_t qty, std::int64_t limit);
    void marketOrder(std::int64_t id, bool side, std::int64_t qty);
    void cancelLimitOrder(std::int64_t id);
    void modifyLimitOrder(std::int64_t id, std::int64_t newQty, std::int64_t newLimit);

    const std::vector<FillEvent>& fills() const { return fills_; }
    std::vector<LevelState> snapshot() const;

    // Lightweight introspection for the workload generator's shadow book.
    bool isLive(std::int64_t id) const { return index_.count(id) != 0; }
    std::int64_t liveOrderCount() const;
    std::int64_t liveBuyCount() const;
    std::int64_t liveSellCount() const;

private:
    struct Order {
        std::int64_t id;
        std::int64_t qty;
    };
    struct Level {
        std::int64_t price;
        bool side;
        std::int64_t totalVolume = 0;
        std::deque<Order> orders;
    };
    struct IndexEntry {
        bool side;
        std::int64_t price;
    };

    std::map<std::int64_t, Level> buyLevels_;   // ascending price
    std::map<std::int64_t, Level> sellLevels_;  // ascending price
    std::unordered_map<std::int64_t, IndexEntry> index_;

    std::vector<FillEvent> fills_;
    std::uint64_t seq_ = 0;

    Level* bestOppositeLevel(bool aggressorBuy);
    void removeLevel(bool side, std::int64_t price);
    void emit(std::int64_t aggressorId, std::int64_t restingId, std::int64_t price,
              std::int64_t qty, bool aggressorBuy);
    void matchMarket(std::int64_t id, bool side, std::int64_t qty);
    std::int64_t matchLimit(std::int64_t id, bool side, std::int64_t qty, std::int64_t limit);
    void rest(std::int64_t id, bool side, std::int64_t qty, std::int64_t limit);
    void removeOrder(std::int64_t id);
};

} // namespace lobref

#endif
