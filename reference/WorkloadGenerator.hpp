#ifndef REFERENCE_WORKLOADGENERATOR_HPP
#define REFERENCE_WORKLOADGENERATOR_HPP

#include "Command.hpp"

namespace lobref {

// Parameters for the seeded workload generator. All fields are independent; the
// generator is fully deterministic for a fixed seed.
struct GeneratorParams {
    std::size_t num_requests = 2000;
    std::uint32_t seed = 0;

    // Relative op-mix weights (Add / Mod / Cxl / Mkt). Zero disables an op.
    int weightAdd = 50;
    int weightMod = 20;
    int weightCxl = 20;
    int weightMkt = 10;

    // Price model: uniform in [center - spread, center + spread], clamped to
    // [1, 1_000_000].
    std::int64_t priceCenter = 100000;
    std::int64_t priceSpread = 1000;

    // Qty range [qtyMin, qtyMax] for resting/aggressive orders.
    std::int64_t qtyMin = 1;
    std::int64_t qtyMax = 1000;

    // Target active-size band (number of live resting orders).
    std::int64_t activeMin = 20;
    std::int64_t activeMax = 60;

    // Fraction of requests (>= 1%) deliberately generated as semantically-valid
    // degenerate ops (duplicate live id, qty<=0, price<=0, cancel/modify unknown
    // id, market on empty side) so reject/no-op semantics are diffed too.
    double degenerateRate = 0.02;

    // Probability a non-forced Add is generated as a crossing (aggressive) order.
    double crossingRate = 0.25;
};

} // namespace lobref

#endif
