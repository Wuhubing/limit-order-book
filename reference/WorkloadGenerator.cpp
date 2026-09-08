#include "WorkloadGenerator.hpp"
#include "ReferenceBook.hpp"

#include <algorithm>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_set>

namespace lobref {

namespace {

constexpr std::int64_t kPriceMin = 1;
constexpr std::int64_t kPriceMax = 1000000;

std::int64_t clampPrice(std::int64_t v)
{
    if (v < kPriceMin) {
        return kPriceMin;
    }
    if (v > kPriceMax) {
        return kPriceMax;
    }
    return v;
}

struct LiveOrder {
    std::int64_t id;
    bool side;
    std::int64_t price;
};

// Collect the live resting orders in deterministic (snapshot) order.
std::vector<LiveOrder> liveOrders(const ReferenceBook& shadow)
{
    std::vector<LiveOrder> out;
    for (const auto& ls : shadow.snapshot()) {
        for (const auto& os : ls.orders) {
            out.push_back({os.id, ls.side, ls.price});
        }
    }
    return out;
}

void applyShadow(ReferenceBook& shadow, const Command& c)
{
    switch (c.op) {
    case Command::Op::Add:
        shadow.addLimitOrder(c.id, c.side, c.qty, c.price);
        break;
    case Command::Op::Mkt:
        shadow.marketOrder(c.id, c.side, c.qty);
        break;
    case Command::Op::Cxl:
        shadow.cancelLimitOrder(c.id);
        break;
    case Command::Op::Mod:
        shadow.modifyLimitOrder(c.id, c.qty, c.price);
        break;
    }
}

struct Best {
    bool hasBuy = false;
    bool hasSell = false;
    std::int64_t bestBuy = 0;
    std::int64_t bestSell = 0;
};

Best bestPrices(const std::vector<LiveOrder>& live)
{
    Best b;
    for (const auto& o : live) {
        if (o.side) {
            if (!b.hasBuy || o.price > b.bestBuy) {
                b.hasBuy = true;
                b.bestBuy = o.price;
            }
        } else {
            if (!b.hasSell || o.price < b.bestSell) {
                b.hasSell = true;
                b.bestSell = o.price;
            }
        }
    }
    return b;
}

} // namespace

std::vector<Command> generateWorkload(const GeneratorParams& p)
{
    std::mt19937 rng(p.seed);
    std::vector<Command> out;
    out.reserve(p.num_requests);

    ReferenceBook shadow;

    std::int64_t nextId = 1;
    std::vector<std::int64_t> freeIds; // dead ids legal to reuse

    std::uniform_real_distribution<double> unit(0.0, 1.0);

    for (std::size_t i = 0; i < p.num_requests; ++i) {
        const std::vector<LiveOrder> live = liveOrders(shadow);
        const std::int64_t total = static_cast<std::int64_t>(live.size());
        const std::int64_t buyCount = shadow.liveBuyCount();
        const std::int64_t sellCount = shadow.liveSellCount();
        const Best best = bestPrices(live);

        auto rqty = [&]() {
            std::uniform_int_distribution<std::int64_t> d(p.qtyMin, p.qtyMax);
            return d(rng);
        };
        auto rprice = [&]() {
            std::uniform_int_distribution<std::int64_t> d(-p.priceSpread, p.priceSpread);
            return clampPrice(p.priceCenter + d(rng));
        };
        auto rside = [&]() {
            return unit(rng) < 0.5;
        };

        // Choose an id for a (valid) Add: mostly fresh, occasionally reuse a
        // dead id (legal reuse-after-death).
        auto freshId = [&]() {
            std::int64_t id;
            if (!freeIds.empty() && unit(rng) < 0.3) {
                std::uniform_int_distribution<std::size_t> d(0, freeIds.size() - 1);
                std::size_t idx = d(rng);
                id = freeIds[idx];
                freeIds[idx] = freeIds.back();
                freeIds.pop_back();
            } else {
                id = nextId++;
            }
            return id;
        };
        auto deadId = [&]() {
            // A guaranteed-not-live id (never assigned within this corpus).
            std::uniform_int_distribution<std::int64_t> d(1000000000LL, 2000000000LL);
            return d(rng);
        };

        Command c;

        if (unit(rng) < p.degenerateRate) {
            std::vector<int> kinds;
            if (!live.empty()) {
                kinds.push_back(0); // duplicate live id add
                kinds.push_back(6); // modify live id with bad qty/price
            }
            kinds.push_back(1); // add with id <= 0
            kinds.push_back(2); // add with qty <= 0
            kinds.push_back(3); // add with price <= 0
            kinds.push_back(4); // cancel unknown id
            kinds.push_back(5); // modify unknown id
            kinds.push_back(7); // market with qty <= 0
            if (sellCount == 0 || buyCount == 0) {
                kinds.push_back(8); // market on empty side
            }

            std::uniform_int_distribution<std::size_t> kd(0, kinds.size() - 1);
            const int kind = kinds[kd(rng)];

            switch (kind) {
            case 0: { // duplicate live id
                std::uniform_int_distribution<std::size_t> d(0, live.size() - 1);
                c = {Command::Op::Add, live[d(rng)].id, rside(), rqty(), rprice()};
                break;
            }
            case 1: { // id <= 0
                std::uniform_int_distribution<std::int64_t> d(-100, 0);
                c = {Command::Op::Add, d(rng), rside(), rqty(), rprice()};
                break;
            }
            case 2: { // qty <= 0
                std::uniform_int_distribution<std::int64_t> d(-100, 0);
                c = {Command::Op::Add, freshId(), rside(), d(rng), rprice()};
                break;
            }
            case 3: { // price <= 0
                std::uniform_int_distribution<std::int64_t> d(-100, 0);
                c = {Command::Op::Add, freshId(), rside(), rqty(), d(rng)};
                break;
            }
            case 4: { // cancel unknown
                c = {Command::Op::Cxl, deadId(), false, 0, 0};
                break;
            }
            case 5: { // modify unknown
                c = {Command::Op::Mod, deadId(), false, rqty(), rprice()};
                break;
            }
            case 6: { // modify live with bad qty/price
                std::uniform_int_distribution<std::size_t> d(0, live.size() - 1);
                std::int64_t id = live[d(rng)].id;
                std::uniform_int_distribution<std::int64_t> bad(-100, 0);
                if (unit(rng) < 0.5) {
                    c = {Command::Op::Mod, id, false, bad(rng), rprice()};
                } else {
                    c = {Command::Op::Mod, id, false, rqty(), bad(rng)};
                }
                break;
            }
            case 7: { // market qty <= 0
                std::uniform_int_distribution<std::int64_t> bad(-100, 0);
                c = {Command::Op::Mkt, nextId++, rside(), bad(rng), 0};
                break;
            }
            default: { // market on empty side
                if (sellCount == 0) {
                    c = {Command::Op::Mkt, nextId++, true, rqty(), 0};
                } else {
                    c = {Command::Op::Mkt, nextId++, false, rqty(), 0};
                }
                break;
            }
            }
        } else if (total == 0) {
            // Empty book: must inject resting liquidity.
            c = {Command::Op::Add, freshId(), rside(), rqty(), rprice()};
        } else if (buyCount == 0 || sellCount == 0) {
            // One side empty: inject resting liquidity on the empty side.
            const bool sideBuy = (buyCount == 0);
            std::int64_t price;
            if (sideBuy && best.hasSell) {
                std::uniform_int_distribution<std::int64_t> d(1, p.priceSpread + 1);
                price = clampPrice(best.bestSell - d(rng));
            } else if (!sideBuy && best.hasBuy) {
                std::uniform_int_distribution<std::int64_t> d(1, p.priceSpread + 1);
                price = clampPrice(best.bestBuy + d(rng));
            } else {
                price = rprice();
            }
            c = {Command::Op::Add, freshId(), sideBuy, rqty(), price};
        } else if (total < p.activeMin) {
            // Below the active band: add.
            c = {Command::Op::Add, freshId(), rside(), rqty(), rprice()};
        } else if (total >= p.activeMax) {
            // Above the active band: consume (cancel / modify / market), no add.
            std::uniform_int_distribution<std::size_t> d(0, live.size() - 1);
            const LiveOrder o = live[d(rng)];
            std::uniform_int_distribution<int> kd(0, 2);
            const int k = kd(rng);
            if (k == 0) {
                c = {Command::Op::Cxl, o.id, false, 0, 0};
            } else if (k == 1) {
                std::uniform_int_distribution<std::int64_t> dd(-p.priceSpread, p.priceSpread);
                c = {Command::Op::Mod, o.id, false, rqty(), clampPrice(o.price + dd(rng))};
            } else {
                c = {Command::Op::Mkt, nextId++, rside(), rqty(), 0};
            }
        } else {
            // In-band: weighted pick over Add / Cxl / Mod / Mkt.
            std::vector<Command::Op> ops;
            std::vector<int> weights;
            ops.push_back(Command::Op::Add);
            weights.push_back(p.weightAdd);
            ops.push_back(Command::Op::Cxl);
            weights.push_back(p.weightCxl);
            ops.push_back(Command::Op::Mod);
            weights.push_back(p.weightMod);
            ops.push_back(Command::Op::Mkt);
            weights.push_back(p.weightMkt);

            int wsum = 0;
            for (int w : weights) {
                wsum += w;
            }
            std::uniform_int_distribution<int> pd(0, wsum - 1);
            int pick = pd(rng);
            Command::Op op = ops.back();
            for (std::size_t k = 0; k < ops.size(); ++k) {
                if (pick < weights[k]) {
                    op = ops[k];
                    break;
                }
                pick -= weights[k];
            }

            if (op == Command::Op::Add) {
                bool side = rside();
                std::int64_t price = rprice();
                if (unit(rng) < p.crossingRate) {
                    if (side && best.hasSell) {
                        std::uniform_int_distribution<std::int64_t> d(0, p.priceSpread);
                        price = clampPrice(best.bestSell + d(rng));
                    } else if (!side && best.hasBuy) {
                        std::uniform_int_distribution<std::int64_t> d(0, p.priceSpread);
                        price = clampPrice(best.bestBuy - d(rng));
                    }
                }
                c = {Command::Op::Add, freshId(), side, rqty(), price};
            } else if (op == Command::Op::Cxl) {
                std::uniform_int_distribution<std::size_t> d(0, live.size() - 1);
                c = {Command::Op::Cxl, live[d(rng)].id, false, 0, 0};
            } else if (op == Command::Op::Mod) {
                std::uniform_int_distribution<std::size_t> d(0, live.size() - 1);
                const LiveOrder o = live[d(rng)];
                std::uniform_int_distribution<std::int64_t> dd(-p.priceSpread, p.priceSpread);
                c = {Command::Op::Mod, o.id, false, rqty(), clampPrice(o.price + dd(rng))};
            } else {
                c = {Command::Op::Mkt, nextId++, rside(), rqty(), 0};
            }
        }

        applyShadow(shadow, c);
        out.push_back(c);

        // Recover ids of orders that died under this command for legal reuse.
        std::unordered_set<std::int64_t> before;
        for (const auto& o : live) {
            before.insert(o.id);
        }
        for (const auto& o : liveOrders(shadow)) {
            before.erase(o.id);
        }
        for (std::int64_t dead : before) {
            freeIds.push_back(dead);
        }
    }

    return out;
}

} // namespace lobref
