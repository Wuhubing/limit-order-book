#ifndef BENCH_ALLOCSTATS_HPP
#define BENCH_ALLOCSTATS_HPP

// Allocation counters for the Stage C benchmark harness.
//
// NOTE ON MEASUREMENT HONESTY: these counters are linked into the lob_bench
// binary ONLY (never the engine library or the test binaries). They override
// the global operator new/delete, so every allocation made while the benchmark
// process runs is counted. The counters therefore add a small constant cost to
// EVERY allocation in the measured region (the header write + counter bumps).
// They do not count themselves: the counter storage is a static struct and the
// tracking header lives in the malloc'd slack, so neither is charged. The
// overhead is constant-per-allocation and is documented here rather than hidden.

#include <cstdint>

namespace bench {

struct AllocSnapshot {
    std::uint64_t allocCount = 0;
    std::uint64_t freeCount = 0;
    std::uint64_t allocBytes = 0;
    std::uint64_t freeBytes = 0;
};

AllocSnapshot allocSnapshot();

} // namespace bench

#endif // BENCH_ALLOCSTATS_HPP
