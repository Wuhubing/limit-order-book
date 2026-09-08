// bench/lob_bench.cpp — Stage C reproducible performance-measurement harness
// for the FIXED engine (engine v1). Measures ENGINE SERVICE TIME ONLY: no
// network / queue / parse / rng work is inside any timed region.
//
// Subcommands:
//   gen       --workload bench/workloads/<name>.json --out <corpus.txt>
//             Deterministically generates a corpus; writes it and prints its
//             sha256 (lowercase hex) as "sha256: <hex>".
//   run       --workload <json> --corpus <corpus.txt> --rounds N
//             --mode batch|latency --out <result.json>
//             Replays the parsed corpus on a FRESH Book each round and writes a
//             fully stamped result JSON.
//   selfcheck Runs a tiny statistical sanity check (p50<=p95<=p99), a
//             clock-overhead estimate and an empty-loop allocation check.
//
// Determinism contract: no randomness inside any timed region; identical config
// => identical corpus (sha256) and identical fill-event hash across rounds.

#include "AllocStats.hpp"
#include "Json.hpp"
#include "sha256.hpp"

#include "../Limit_Order_Book/Book.hpp"
#include "../reference/Command.hpp"
#include "../reference/WorkloadGenerator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// Compile-time stamping injected by CMake (bench/CMakeLists.txt).
#ifndef LOB_GIT_COMMIT
#define LOB_GIT_COMMIT "unknown"
#endif
#ifndef LOB_CXX_COMPILER
#define LOB_CXX_COMPILER "unknown"
#endif
#ifndef LOB_CXX_FLAGS
#define LOB_CXX_FLAGS "unknown"
#endif
#ifndef LOB_BUILD_TYPE
#define LOB_BUILD_TYPE "unknown"
#endif

namespace {

// ---- file helpers ---------------------------------------------------------

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open file: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write file: " + path.string());
    out << content;
}

// ---- engine application ---------------------------------------------------

void applyEngine(Book& book, const lobref::Command& c)
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

// ---- fill-event hash (determinism guard) ----------------------------------

// Fold every FillEvent field into a single uint64 via FNV-1a (stable across
// runs and platforms; used only to assert identical output across rounds).
std::uint64_t hashFills(const std::vector<Book::FillEvent>& fills)
{
    std::uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&h](std::uint64_t v) {
        const unsigned char* b = reinterpret_cast<const unsigned char*>(&v);
        for (int i = 0; i < 8; ++i) h = (h ^ b[i]) * 0x100000001b3ULL;
    };
    for (const auto& f : fills) {
        mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(f.aggressorId)));
        mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(f.restingId)));
        mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(f.price)));
        mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(f.qty)));
        mix(f.aggressorBuy ? 1ULL : 0ULL);
        mix(f.seq);
    }
    return h;
}

std::string toHex64(std::uint64_t v)
{
    char buf[19];
    std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

// ---- statistics -----------------------------------------------------------

// Nearest-rank percentile over a copy of the data (sorted in place). Guarantees
// p50 <= p95 <= p99 by construction.
double percentile(std::vector<double> data, double p)
{
    if (data.empty()) return 0.0;
    std::sort(data.begin(), data.end());
    std::size_t rank = static_cast<std::size_t>(std::ceil(p / 100.0 * data.size()));
    if (rank < 1) rank = 1;
    if (rank > data.size()) rank = data.size();
    return data[rank - 1];
}

double median(std::vector<double> data)
{
    if (data.empty()) return 0.0;
    std::sort(data.begin(), data.end());
    const std::size_t n = data.size();
    if (n % 2 == 1) return data[n / 2];
    return (data[n / 2 - 1] + data[n / 2]) / 2.0;
}

struct RoundStats {
    double mean = 0.0;
    double median = 0.0;
    double min = 0.0;
    double max = 0.0;
};

RoundStats roundStats(const std::vector<double>& times)
{
    RoundStats s;
    if (times.empty()) return s;
    s.mean = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
    s.median = median(times);
    s.min = *std::min_element(times.begin(), times.end());
    s.max = *std::max_element(times.begin(), times.end());
    return s;
}

// ---- clock overhead -------------------------------------------------------

volatile std::uint64_t g_clockSink = 0;

// Wall-clock cost of a steady_clock now()/now() pair, in nanoseconds. The two
// now() calls bracket empty work; the sum is folded into a volatile so the loop
// is never eliminated. `perNow` is half of this.
double clockOverheadNsPerPair(int iterations = 1000000)
{
    const auto t0 = Clock::now();
    std::uint64_t acc = 0;
    for (int i = 0; i < iterations; ++i) {
        const auto a = Clock::now();
        const auto b = Clock::now();
        acc += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    }
    const auto t1 = Clock::now();
    g_clockSink = acc;
    const double wallNs =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return wallNs / iterations;
}

// ---- workload config ------------------------------------------------------

lobref::GeneratorParams loadParams(const bench::Json& cfg)
{
    lobref::GeneratorParams p;
    p.num_requests = static_cast<std::size_t>(cfg.at("num_requests").asInt());
    p.seed = static_cast<std::uint32_t>(cfg.at("seed").asInt());
    p.weightAdd = static_cast<int>(cfg.at("weightAdd").asInt());
    p.weightMod = static_cast<int>(cfg.at("weightMod").asInt());
    p.weightCxl = static_cast<int>(cfg.at("weightCxl").asInt());
    p.weightMkt = static_cast<int>(cfg.at("weightMkt").asInt());
    p.priceCenter = cfg.at("priceCenter").asInt();
    p.priceSpread = cfg.at("priceSpread").asInt();
    p.qtyMin = cfg.at("qtyMin").asInt();
    p.qtyMax = cfg.at("qtyMax").asInt();
    p.activeMin = cfg.at("activeMin").asInt();
    p.activeMax = cfg.at("activeMax").asInt();
    p.degenerateRate = cfg.at("degenerateRate").asDouble();
    p.crossingRate = cfg.at("crossingRate").asDouble();
    return p;
}

// ---- environment stamping -------------------------------------------------

std::string hostSysctl(const char* name)
{
    char buf[512];
    std::size_t len = sizeof(buf);
    if (sysctlbyname(name, buf, &len, nullptr, 0) != 0) return "unknown";
    return std::string(buf);
}

std::string nowIso8601Utc()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

bench::Json buildEnvJson()
{
    struct utsname u{};
    uname(&u);

    int ncpu = 0;
    std::size_t len = sizeof(ncpu);
    sysctlbyname("hw.ncpu", &ncpu, &len, nullptr, 0);

    bench::Json host;
    host["sysname"] = u.sysname;
    host["nodename"] = u.nodename;
    host["release"] = u.release;
    host["version"] = u.version;
    host["machine"] = u.machine;
    host["model"] = hostSysctl("hw.model");
    host["ncpu"] = ncpu;

    bench::Json env;
    env["git_commit"] = LOB_GIT_COMMIT;
    env["compiler"] = LOB_CXX_COMPILER;
    env["compiler_id_macro"] = __VERSION__;
    env["clang_version_macro"] = __clang_version__;
    env["cxx_flags"] = LOB_CXX_FLAGS;
    env["build_type"] = LOB_BUILD_TYPE;
    env["host"] = host;
    env["date"] = nowIso8601Utc();
    env["compile_date"] = std::string(__DATE__) + " " + __TIME__;
    return env;
}

// ---- allocation reporting -------------------------------------------------

std::uint64_t ruMaxRssBytes()
{
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    // On macOS ru_maxrss is already in bytes; on Linux it would be in KiB.
    return static_cast<std::uint64_t>(ru.ru_maxrss);
}

bench::Json allocDeltasJson(const bench::AllocSnapshot& base,
                            const bench::AllocSnapshot& s)
{
    bench::Json j;
    j["alloc_count"] = static_cast<long long>(s.allocCount - base.allocCount);
    j["alloc_bytes"] = static_cast<long long>(s.allocBytes - base.allocBytes);
    j["freed_bytes"] = static_cast<long long>(s.freeBytes - base.freeBytes);
    const std::uint64_t baseLive = base.allocBytes - base.freeBytes;
    const std::uint64_t live = s.allocBytes - s.freeBytes;
    j["live_bytes"] = static_cast<long long>(live - baseLive);
    return j;
}

// ---- percentile bucket ----------------------------------------------------

bench::Json percentileBucketJson(std::vector<double> data)
{
    bench::Json j;
    j["count"] = static_cast<long long>(data.size());
    if (!data.empty()) {
        std::sort(data.begin(), data.end());
        j["p50"] = percentile(data, 50.0);
        j["p95"] = percentile(data, 95.0);
        j["p99"] = percentile(data, 99.0);
        j["min"] = data.front();
        j["max"] = data.back();
    }
    return j;
}

// ---- command dispatch -----------------------------------------------------

void usage(std::ostream& os)
{
    os << "usage:\n"
       << "  lob_bench gen --workload <json> --out <corpus.txt>\n"
       << "  lob_bench run --workload <json> --corpus <corpus.txt>\n"
       << "        [--rounds N] --mode batch|latency --out <result.json>\n"
       << "  lob_bench selfcheck\n";
}

struct RunArgs {
    std::string workload;
    std::string corpus;
    std::string out;
    std::string mode;
    int rounds = 7;
};

int doGen(const std::string& workloadPath, const std::string& outPath)
{
    const bench::Json cfg = bench::Json::parse(readFile(workloadPath));
    const lobref::GeneratorParams p = loadParams(cfg);
    const std::vector<lobref::Command> commands = lobref::generateWorkload(p);
    const std::string text = lobref::serializeCorpus(commands);
    writeFile(outPath, text);
    std::cout << "wrote " << commands.size() << " commands to " << outPath << "\n";
    std::cout << "sha256: " << bench::sha256Hex(text) << "\n";
    return 0;
}

// Replay `commands` on a fresh Book, collecting fills. The Book + sink are
// constructed before the timed region; the returned duration covers only the
// Book calls. Allocation snapshots are taken before Book construction (s0),
// after replay with the Book still alive (s1), and after destruction (s2).
struct ReplayResult {
    double seconds = 0.0;
    std::uint64_t fillHash = 0;
    std::size_t fillCount = 0;
};

int doRun(const RunArgs& args)
{
    const bench::Json cfg = bench::Json::parse(readFile(args.workload));
    const lobref::GeneratorParams p = loadParams(cfg);
    const std::string corpusText = readFile(args.corpus);
    const std::vector<lobref::Command> commands = lobref::parseCorpus(corpusText);
    const std::size_t n = commands.size();
    if (n == 0) throw std::runtime_error("empty corpus");
    const std::string corpusSha = bench::sha256Hex(corpusText);
    const int rounds = args.rounds;
    const int warmup = 1;

    if (args.mode == "batch") {
        std::vector<double> times;
        times.reserve(rounds);
        std::uint64_t expectedHash = 0;
        bool first = true;
        std::size_t fillCount = 0;
        bench::Json perRound = bench::Json(bench::Json::Array{});

        const bench::AllocSnapshot runStart = bench::allocSnapshot();

        // Warmup round (not measured).
        {
            Book book;
            std::vector<Book::FillEvent> sink;
            sink.reserve(n);
            book.setFillSink(&sink);
            for (const auto& c : commands) applyEngine(book, c);
        }

        for (int r = 0; r < rounds; ++r) {
            const bench::AllocSnapshot s0 = bench::allocSnapshot();
            double seconds = 0.0;
            std::uint64_t hash = 0;
            std::size_t fcount = 0;
            {
                Book book;
                std::vector<Book::FillEvent> sink;
                sink.reserve(n);
                book.setFillSink(&sink);

                const auto t0 = Clock::now();
                for (const auto& c : commands) applyEngine(book, c);
                const auto t1 = Clock::now();
                const bench::AllocSnapshot s1 = bench::allocSnapshot();

                seconds = std::chrono::duration<double>(t1 - t0).count();
                hash = hashFills(sink);
                fcount = sink.size();

                bench::Json entry;
                entry["round"] = r;
                entry["alloc"] = allocDeltasJson(s0, s1);
                perRound.push_back(entry);
            }

            times.push_back(seconds);
            fillCount = fcount;
            if (first) {
                expectedHash = hash;
                first = false;
            } else if (hash != expectedHash) {
                std::cerr << "DETERMINISM GUARD FAILED: fill hash " << toHex64(hash)
                          << " != expected " << toHex64(expectedHash) << " (round " << r << ")\n";
                return 1;
            }
        }

        const RoundStats st = roundStats(times);
        const double reqsPerSec = st.mean > 0.0 ? n / st.mean : 0.0;

        const bench::AllocSnapshot runEnd = bench::allocSnapshot();

        bench::Json alloc;
        alloc["total"] = allocDeltasJson(runStart, runEnd);
        alloc["per_round"] = perRound;
        alloc["ru_maxrss_bytes"] = static_cast<long long>(ruMaxRssBytes());

        bench::Json roundsArr(bench::Json::Array{});
        for (double t : times) roundsArr.push_back(t);

        bench::Json r;
        r["schema_version"] = 1;
        r["mode"] = "batch";
        r["workload"] = cfg;
        r["corpus_sha256"] = corpusSha;
        r["corpus_num_commands"] = static_cast<long long>(n);
        r["seed"] = static_cast<long long>(p.seed);
        r["warmup_rounds"] = warmup;
        r["rounds_measured"] = rounds;
        r["round_times_sec"] = roundsArr;
        r["round_time_sec_mean"] = st.mean;
        r["round_time_sec_median"] = st.median;
        r["round_time_sec_min"] = st.min;
        r["round_time_sec_max"] = st.max;
        r["reqs_per_sec_all_ops"] = reqsPerSec;
        r["fill_hash"] = toHex64(expectedHash);
        r["fill_event_count"] = static_cast<long long>(fillCount);
        r["allocation"] = alloc;
        r["env"] = buildEnvJson();

        writeFile(args.out, r.dump());
        return 0;
    }

    if (args.mode == "latency") {
        std::vector<double> all, add, mod, cxl, mkt;
        all.reserve(n * rounds);
        add.reserve(n * rounds);
        mod.reserve(n * rounds);
        cxl.reserve(n * rounds);
        mkt.reserve(n * rounds);

        std::uint64_t expectedHash = 0;
        bool first = true;
        std::size_t fillCount = 0;
        bench::Json perRound = bench::Json(bench::Json::Array{});

        const bench::AllocSnapshot runStart = bench::allocSnapshot();

        // Warmup round (not measured).
        {
            Book book;
            std::vector<Book::FillEvent> sink;
            sink.reserve(n);
            book.setFillSink(&sink);
            for (const auto& c : commands) applyEngine(book, c);
        }

        for (int r = 0; r < rounds; ++r) {
            const bench::AllocSnapshot s0 = bench::allocSnapshot();
            std::uint64_t hash = 0;
            std::size_t fcount = 0;
            {
                Book book;
                std::vector<Book::FillEvent> sink;
                sink.reserve(n);
                book.setFillSink(&sink);

                for (const auto& c : commands) {
                    const auto t0 = Clock::now();
                    applyEngine(book, c);
                    const auto t1 = Clock::now();
                    const double ns = static_cast<double>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
                    all.push_back(ns);
                    switch (c.op) {
                    case lobref::Command::Op::Add: add.push_back(ns); break;
                    case lobref::Command::Op::Mkt: mkt.push_back(ns); break;
                    case lobref::Command::Op::Cxl: cxl.push_back(ns); break;
                    case lobref::Command::Op::Mod: mod.push_back(ns); break;
                    }
                }
                const bench::AllocSnapshot s1 = bench::allocSnapshot();
                hash = hashFills(sink);
                fcount = sink.size();

                bench::Json entry;
                entry["round"] = r;
                entry["alloc"] = allocDeltasJson(s0, s1);
                perRound.push_back(entry);
            }

            if (first) {
                expectedHash = hash;
                first = false;
            } else if (hash != expectedHash) {
                std::cerr << "DETERMINISM GUARD FAILED: fill hash " << toHex64(hash)
                          << " != expected " << toHex64(expectedHash) << " (round " << r << ")\n";
                return 1;
            }
            fillCount = fcount;
        }

        const bench::AllocSnapshot runEnd = bench::allocSnapshot();
        const double ovhPair = clockOverheadNsPerPair();

        bench::Json latency;
        latency["all"] = percentileBucketJson(all);
        latency["Add"] = percentileBucketJson(add);
        latency["Mod"] = percentileBucketJson(mod);
        latency["Cxl"] = percentileBucketJson(cxl);
        latency["Mkt"] = percentileBucketJson(mkt);

        bench::Json alloc;
        alloc["total"] = allocDeltasJson(runStart, runEnd);
        alloc["per_round"] = perRound;
        alloc["ru_maxrss_bytes"] = static_cast<long long>(ruMaxRssBytes());

        bench::Json r;
        r["schema_version"] = 1;
        r["mode"] = "latency";
        r["workload"] = cfg;
        r["corpus_sha256"] = corpusSha;
        r["corpus_num_commands"] = static_cast<long long>(n);
        r["seed"] = static_cast<long long>(p.seed);
        r["warmup_rounds"] = warmup;
        r["rounds_measured"] = rounds;
        r["clock_overhead_ns_per_pair"] = ovhPair;
        r["clock_overhead_ns_per_now"] = ovhPair / 2.0;
        r["latency"] = latency;
        r["fill_hash"] = toHex64(expectedHash);
        r["fill_event_count"] = static_cast<long long>(fillCount);
        r["allocation"] = alloc;
        r["env"] = buildEnvJson();

        writeFile(args.out, r.dump());
        return 0;
    }

    throw std::runtime_error("unknown mode: " + args.mode);
}

int doSelfcheck()
{
    // 1. Percentile monotonicity on a synthetic dataset.
    std::vector<double> data;
    for (int i = 1; i <= 1000; ++i) data.push_back(i * 1.0);
    const double p50 = percentile(data, 50.0);
    const double p95 = percentile(data, 95.0);
    const double p99 = percentile(data, 99.0);
    const bool monotonic = (p50 <= p95) && (p95 <= p99);

    // 2. Clock-overhead estimate.
    const double ovhPair = clockOverheadNsPerPair();

    // 3. Allocation counters for an empty loop (should be ~0).
    const bench::AllocSnapshot before = bench::allocSnapshot();
    volatile std::uint64_t x = 0;
    for (int i = 0; i < 100000; ++i) x += static_cast<std::uint64_t>(i);
    (void)x;
    const bench::AllocSnapshot after = bench::allocSnapshot();

    std::cout << "selfcheck percentile p50=" << p50 << " p95=" << p95 << " p99=" << p99
              << " monotonic=" << (monotonic ? "ok" : "FAIL") << "\n";
    std::cout << "selfcheck clock_overhead_ns_per_pair=" << ovhPair
              << " ns_per_now=" << (ovhPair / 2.0) << "\n";
    std::cout << "selfcheck empty_loop alloc_count_delta="
              << (after.allocCount - before.allocCount)
              << " alloc_bytes_delta=" << (after.allocBytes - before.allocBytes) << "\n";
    return monotonic ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage(std::cerr);
        return 2;
    }
    const std::string sub = argv[1];
    try {
        if (sub == "selfcheck") {
            return doSelfcheck();
        }

        RunArgs args;
        std::string genOut;

        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
                return argv[++i];
            };
            if (a == "--workload") args.workload = next();
            else if (a == "--out") genOut = next();
            else if (a == "--corpus") args.corpus = next();
            else if (a == "--rounds") args.rounds = std::stoi(next());
            else if (a == "--mode") args.mode = next();
            else if (a == "--help" || a == "-h") { usage(std::cout); return 0; }
            else throw std::runtime_error("unknown argument: " + a);
        }

        if (sub == "gen") {
            if (args.workload.empty() || genOut.empty()) {
                usage(std::cerr);
                return 2;
            }
            return doGen(args.workload, genOut);
        }
        if (sub == "run") {
            args.out = genOut;
            if (args.workload.empty() || args.corpus.empty() || args.out.empty() ||
                args.mode.empty()) {
                usage(std::cerr);
                return 2;
            }
            return doRun(args);
        }
        usage(std::cerr);
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
