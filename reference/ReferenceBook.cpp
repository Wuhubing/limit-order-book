#include "ReferenceBook.hpp"

namespace lobref {

ReferenceBook::Level* ReferenceBook::bestOppositeLevel(bool aggressorBuy)
{
    if (aggressorBuy) {
        return sellLevels_.empty() ? nullptr : &sellLevels_.begin()->second;
    }
    return buyLevels_.empty() ? nullptr : &buyLevels_.rbegin()->second;
}

void ReferenceBook::removeLevel(bool side, std::int64_t price)
{
    auto& levels = side ? buyLevels_ : sellLevels_;
    levels.erase(price);
}

void ReferenceBook::emit(std::int64_t aggressorId, std::int64_t restingId, std::int64_t price,
                         std::int64_t qty, bool aggressorBuy)
{
    fills_.push_back({aggressorId, restingId, price, qty, aggressorBuy, seq_++});
}

// Execute an aggressive order against the best opposite level, FIFO within the
// level. Mirrors the engine's marketOrderHelper: full head executions while the
// head's qty is <= remaining, then at most one partial fill. Any remainder after
// liquidity exhaustion is dropped (callers that must rest handle that themselves).
void ReferenceBook::matchMarket(std::int64_t id, bool side, std::int64_t qty)
{
    while (qty > 0) {
        Level* best = bestOppositeLevel(side);
        if (best == nullptr) {
            break; // remainder dropped
        }
        Level& level = *best;
        Order& head = level.orders.front();
        if (head.qty <= qty) {
            const std::int64_t tradePrice = level.price;
            const std::int64_t restingId = head.id;
            const std::int64_t tradeQty = head.qty;
            qty -= tradeQty;
            level.orders.pop_front();
            level.totalVolume -= tradeQty;
            if (level.orders.empty()) {
                removeLevel(!side, level.price);
            }
            index_.erase(restingId);
            emit(id, restingId, tradePrice, tradeQty, side);
        } else {
            const std::int64_t tradePrice = level.price;
            const std::int64_t restingId = head.id;
            head.qty -= qty;
            level.totalVolume -= qty;
            emit(id, restingId, tradePrice, qty, side);
            qty = 0;
        }
    }
}

// Aggressive phase for a limit order: cross while the opposite best level's price
// is within the limit. Returns the qty that still rests (0 = fully consumed).
// Mirrors the engine's limitOrderAsMarketOrder exactly.
std::int64_t ReferenceBook::matchLimit(std::int64_t id, bool side, std::int64_t qty, std::int64_t limit)
{
    if (side) {
        while (qty > 0) {
            Level* best = bestOppositeLevel(true);
            if (best == nullptr || best->price > limit) {
                break;
            }
            if (qty <= best->totalVolume) {
                matchMarket(id, true, qty);
                return 0;
            }
            qty -= best->totalVolume;
            matchMarket(id, true, best->totalVolume);
        }
        return qty;
    }
    while (qty > 0) {
        Level* best = bestOppositeLevel(false);
        if (best == nullptr || best->price < limit) {
            break;
        }
        if (qty <= best->totalVolume) {
            matchMarket(id, false, qty);
            return 0;
        }
        qty -= best->totalVolume;
        matchMarket(id, false, best->totalVolume);
    }
    return qty;
}

void ReferenceBook::rest(std::int64_t id, bool side, std::int64_t qty, std::int64_t limit)
{
    auto& levels = side ? buyLevels_ : sellLevels_;
    auto it = levels.find(limit);
    if (it == levels.end()) {
        Level lvl;
        lvl.price = limit;
        lvl.side = side;
        lvl.totalVolume = 0;
        it = levels.emplace(limit, std::move(lvl)).first;
    }
    Level& level = it->second;
    level.orders.push_back({id, qty});
    level.totalVolume += qty;
    index_[id] = {side, limit};
}

// Unlink a live order from its level and erase the level if it empties.
void ReferenceBook::removeOrder(std::int64_t id)
{
    auto it = index_.find(id);
    if (it == index_.end()) {
        return;
    }
    const bool side = it->second.side;
    const std::int64_t price = it->second.price;
    index_.erase(it);

    auto& levels = side ? buyLevels_ : sellLevels_;
    auto lit = levels.find(price);
    if (lit == levels.end()) {
        return; // inconsistent; treat as no-op
    }
    Level& level = lit->second;
    for (auto oit = level.orders.begin(); oit != level.orders.end(); ++oit) {
        if (oit->id == id) {
            level.totalVolume -= oit->qty;
            level.orders.erase(oit);
            break;
        }
    }
    if (level.orders.empty()) {
        levels.erase(lit);
    }
}

void ReferenceBook::addLimitOrder(std::int64_t id, bool side, std::int64_t qty, std::int64_t limit)
{
    if (id <= 0 || qty <= 0 || limit <= 0) {
        return; // reject: no state change, no events
    }
    if (index_.count(id) != 0) {
        return; // reject duplicate live id
    }
    qty = matchLimit(id, side, qty, limit);
    if (qty > 0) {
        rest(id, side, qty, limit);
    }
}

void ReferenceBook::marketOrder(std::int64_t id, bool side, std::int64_t qty)
{
    if (qty <= 0) {
        return; // reject
    }
    matchMarket(id, side, qty); // remainder dropped; market orders never rest
}

void ReferenceBook::cancelLimitOrder(std::int64_t id)
{
    removeOrder(id); // unknown/dead id => silent no-op
}

void ReferenceBook::modifyLimitOrder(std::int64_t id, std::int64_t newQty, std::int64_t newLimit)
{
    auto it = index_.find(id);
    if (it == index_.end() || newQty <= 0 || newLimit <= 0) {
        return; // silent no-op; order keeps old state
    }
    const bool side = it->second.side;
    removeOrder(id);
    const std::int64_t remaining = matchLimit(id, side, newQty, newLimit);
    if (remaining > 0) {
        rest(id, side, remaining, newLimit);
    }
}

std::vector<ReferenceBook::LevelState> ReferenceBook::snapshot() const
{
    std::vector<LevelState> result;
    for (const auto& [price, level] : buyLevels_) {
        LevelState ls;
        ls.price = price;
        ls.side = true;
        ls.totalVolume = level.totalVolume;
        for (const auto& order : level.orders) {
            ls.orders.push_back({order.id, order.qty});
        }
        result.push_back(std::move(ls));
    }
    for (const auto& [price, level] : sellLevels_) {
        LevelState ls;
        ls.price = price;
        ls.side = false;
        ls.totalVolume = level.totalVolume;
        for (const auto& order : level.orders) {
            ls.orders.push_back({order.id, order.qty});
        }
        result.push_back(std::move(ls));
    }
    return result;
}

std::int64_t ReferenceBook::liveOrderCount() const
{
    return static_cast<std::int64_t>(index_.size());
}

std::int64_t ReferenceBook::liveBuyCount() const
{
    std::int64_t n = 0;
    for (const auto& [id, entry] : index_) {
        (void)id;
        if (entry.side) {
            ++n;
        }
    }
    return n;
}

std::int64_t ReferenceBook::liveSellCount() const
{
    std::int64_t n = 0;
    for (const auto& [id, entry] : index_) {
        (void)id;
        if (!entry.side) {
            ++n;
        }
    }
    return n;
}

} // namespace lobref
