// tools/replay.cpp — Stage E deterministic replay tool (ENGINE ONLY).
//
// Consumes a canonical corpus file (same Add/Mkt/Cxl/Mod format as
// reference/Command.hpp), drives it through the ENGINE Book only (no reference
// model at runtime, and none linked — unlike tools/diff_fuzz.cpp), and emits a
// deterministic, canonical OUTPUT RECORD. Same input => byte-identical output,
// independent of build/run: no timestamps, no pointers, and no hashing that
// depends on unordered_map iteration order. The final-state summary is derived
// from Book::snapshot(), which walks the AVL trees in sorted (in-order) order
// and the per-level FIFO queues head->tail — fully deterministic.
//
// Record format (plain text, stable field order):
//   header:  replay v1 <num_requests> <fill_count> <add> <mod> <cxl> <mkt>
//   fills:   fill <seq> <aggressorId> <restingId> <price> <qty> <side>
//            (one line per fill, in seq order; side is "buy"/"sell" = aggressor side)
//   state:   level <side> <price> <totalVolume> <nOrders> id:qty,id:qty,...
//            (buy levels ascending, then sell levels ascending; FIFO head->tail)
//   empty:   state empty
//
// Note on "rejects": the engine exposes validation failures (duplicate live id,
// non-positive qty/price, cancel/modify of unknown id, market into empty side)
// only as the *absence* of any effect. There is no engine-level reject counter,
// and we deliberately do NOT wrap the engine to synthesize one: rejects are
// already deterministically reflected in the fill stream and the final state, so
// a separate count adds no information and would only risk diverging from the
// reference model's semantics. Hence the header carries op counts only (A/M/C/Mkt
// = Add/Mod/Cxl/Mkt, matching the A/M/C/Mkt column order in the record header).
//
// Modes:
//   replay --replay <corpus> [--golden <file>]
//     Replay the corpus and print the canonical record to stdout.
//     With --golden, compare against the golden file instead of printing:
//     exit 0 on byte-identical match, exit 1 on mismatch (prints a diff report).
//   replay --emit-golden <corpus> [--out <file>]
//     Replay the corpus and write the canonical record to <file> (default stdout).
//     This is the regeneration command used to produce the committed .golden files.
//   replay --check-golden <golden-file>
//     Shorthand: derive the corpus from <golden-file> (strip ".golden"), compare.
//   replay --help
//
// Exit codes: 0 = success / golden match, 1 = golden mismatch, 2 = usage or
// parse/IO error.

#include "../Limit_Order_Book/Book.hpp"
#include "../reference/Command.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void usage(std::ostream& os)
{
    os << "usage:\n"
       << "  replay --replay <corpus> [--golden <file>]\n"
       << "  replay --emit-golden <corpus> [--out <file>]\n"
       << "  replay --check-golden <golden-file>\n"
       << "  replay --help\n";
}

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open file: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const std::filesystem::path& path, const std::string& content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write file: " + path.string());
    }
    out << content;
    out.flush();
    if (!out) {
        throw std::runtime_error("write failed: " + path.string());
    }
}

// Resolve a corpus argument to an existing file. `--replay` may be handed the
// corpus path directly, or a golden-derived path (e.g. "testdata/golden/mix-heavy"
// after stripping ".golden" in the CI shell loop). Try, in order: the argument
// as-is, the argument with ".txt" appended, then the basename under
// testdata/corpora and testdata/corpora/adversarial.
std::string resolveCorpus(const std::string& arg)
{
    if (std::filesystem::is_regular_file(arg)) {
        return arg;
    }
    if (std::filesystem::is_regular_file(arg + ".txt")) {
        return arg + ".txt";
    }
    const std::filesystem::path base = std::filesystem::path(arg).filename();
    for (const char* dir : {"testdata/corpora", "testdata/corpora/adversarial"}) {
        const std::filesystem::path p = std::filesystem::path(dir) / (base.string() + ".txt");
        if (std::filesystem::is_regular_file(p)) {
            return p.string();
        }
        const std::filesystem::path q = std::filesystem::path(dir) / base;
        if (std::filesystem::is_regular_file(q)) {
            return q.string();
        }
    }
    return arg; // let readFile() report a clear error
}

void applyOp(Book& book, const lobref::Command& c)
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

std::string buildRecord(const std::vector<lobref::Command>& commands,
                        const std::vector<Book::FillEvent>& fills,
                        const std::array<std::size_t, 4>& opCounts, const Book& book)
{
    std::ostringstream out;
    out << "replay v1 " << commands.size() << ' ' << fills.size() << ' '
        << opCounts[0] << ' ' << opCounts[1] << ' ' << opCounts[2] << ' ' << opCounts[3]
        << '\n';

    for (const auto& f : fills) {
        out << "fill " << f.seq << ' ' << f.aggressorId << ' ' << f.restingId << ' '
            << f.price << ' ' << f.qty << ' ' << (f.aggressorBuy ? "buy" : "sell")
            << '\n';
    }

    const std::vector<Book::LevelState> snap = book.snapshot();
    if (snap.empty()) {
        out << "state empty\n";
    } else {
        for (const auto& ls : snap) {
            out << "level " << (ls.side ? "buy" : "sell") << ' ' << ls.price << ' '
                << ls.totalVolume << ' ' << ls.orders.size() << ' ';
            for (std::size_t i = 0; i < ls.orders.size(); ++i) {
                if (i) {
                    out << ',';
                }
                out << ls.orders[i].id << ':' << ls.orders[i].qty;
            }
            out << '\n';
        }
    }
    return out.str();
}

std::vector<std::string> splitLines(const std::string& s)
{
    std::vector<std::string> lines;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

// Report a golden mismatch to `os` (line counts + first differing lines).
void reportDiff(std::ostream& os, const std::string& record, const std::string& golden)
{
    const std::vector<std::string> a = splitLines(record);
    const std::vector<std::string> b = splitLines(golden);
    os << "record has " << a.size() << " lines, golden has " << b.size() << " lines\n";
    const std::size_t n = std::min(a.size(), b.size());
    std::size_t shown = 0;
    for (std::size_t i = 0; i < n && shown < 20; ++i) {
        if (a[i] != b[i]) {
            os << "line " << (i + 1) << " differs:\n"
               << "  golden: " << b[i] << "\n"
               << "  record: " << a[i] << "\n";
            ++shown;
        }
    }
    if (a.size() != b.size() && shown == 0) {
        const std::size_t start = n;
        const std::size_t end = std::max(a.size(), b.size());
        for (std::size_t i = start; i < end && shown < 20; ++i) {
            os << "line " << (i + 1) << " differs:\n"
               << "  golden: " << (i < b.size() ? b[i] : "<missing>") << "\n"
               << "  record: " << (i < a.size() ? a[i] : "<missing>") << "\n";
            ++shown;
        }
    }
}

// Replay the corpus and return the canonical record. Fills + final state are the
// deterministic output; op counts feed the header.
std::string replay(const std::vector<lobref::Command>& commands)
{
    Book book;
    std::vector<Book::FillEvent> fills;
    book.setFillSink(&fills);

    std::array<std::size_t, 4> opCounts = {0, 0, 0, 0}; // Add, Mod, Cxl, Mkt

    for (const auto& c : commands) {
        switch (c.op) {
        case lobref::Command::Op::Add: ++opCounts[0]; break;
        case lobref::Command::Op::Mod: ++opCounts[1]; break;
        case lobref::Command::Op::Cxl: ++opCounts[2]; break;
        case lobref::Command::Op::Mkt: ++opCounts[3]; break;
        }
        applyOp(book, c);
    }

    return buildRecord(commands, fills, opCounts, book);
}

} // namespace

int main(int argc, char** argv)
{
    std::string replayArg;
    std::string goldenArg;
    std::string emitArg;
    std::string checkArg;
    std::string outArg;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + arg);
            }
            return argv[++i];
        };
        if (arg == "--replay") {
            replayArg = next();
        } else if (arg == "--golden") {
            goldenArg = next();
        } else if (arg == "--emit-golden") {
            emitArg = next();
        } else if (arg == "--out") {
            outArg = next();
        } else if (arg == "--check-golden") {
            checkArg = next();
        } else if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            usage(std::cerr);
            return 2;
        }
    }

    try {
        if (!checkArg.empty()) {
            // --check-golden <golden-file>: corpus = golden path minus ".golden".
            std::string corpus = checkArg;
            if (corpus.size() >= 7 &&
                corpus.compare(corpus.size() - 7, 7, ".golden") == 0) {
                corpus = corpus.substr(0, corpus.size() - 7);
            }
            replayArg = corpus;
            goldenArg = checkArg;
        }

        if (!replayArg.empty() && emitArg.empty()) {
            const std::string corpus = resolveCorpus(replayArg);
            const std::vector<lobref::Command> commands =
                lobref::parseCorpus(readFile(corpus));
            const std::string record = replay(commands);

            if (goldenArg.empty()) {
                std::cout << record;
                return 0;
            }
            const std::string golden = readFile(goldenArg);
            if (record == golden) {
                return 0;
            }
            std::cerr << "golden mismatch: " << goldenArg << "\n";
            reportDiff(std::cerr, record, golden);
            return 1;
        }

        if (!emitArg.empty()) {
            const std::string corpus = resolveCorpus(emitArg);
            const std::vector<lobref::Command> commands =
                lobref::parseCorpus(readFile(corpus));
            const std::string record = replay(commands);
            if (!outArg.empty()) {
                writeFile(outArg, record);
                std::cerr << "wrote golden record (" << commands.size()
                          << " requests) to " << outArg << "\n";
            } else {
                std::cout << record;
            }
            return 0;
        }

        usage(std::cerr);
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
}
