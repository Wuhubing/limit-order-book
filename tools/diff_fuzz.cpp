// tools/diff_fuzz.cpp — standalone differential fuzz / replay driver.
//
// Generate mode:
//   diff_fuzz --seed N --requests N --ops a/c/m/x:w1/w2/w3/w4 [--out corpus.txt]
//   Generates a deterministic workload, optionally writes the corpus, then
//   replays it on the engine and reference model and verifies identity.
//
// Replay mode:
//   diff_fuzz --replay corpus.txt
//   Parses a saved corpus and replays it.
//
// Exit codes: 0 = identical, 1 = divergence (first divergence + state diff
// printed, corpus saved/minimised), 2 = parse error.

#include "../Limit_Order_Book/Book.hpp"
#include "../reference/Command.hpp"
#include "../reference/DifferentialHarness.hpp"
#include "../reference/ReferenceBook.hpp"
#include "../reference/WorkloadGenerator.hpp"

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
       << "  diff_fuzz --seed N --requests N --ops a/c/m/x:w1/w2/w3/w4 [--out corpus.txt]\n"
       << "  diff_fuzz --replay corpus.txt\n";
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
    out << content;
}

std::string dumpEngine(const Book& book)
{
    std::string s;
    for (const auto& ls : book.snapshot()) {
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
    return s;
}

std::string dumpRef(const lobref::ReferenceBook& rb)
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
    return s;
}

// Replay a parsed command stream; returns the process exit code.
int replay(const std::vector<lobref::Command>& commands, std::uint32_t seed)
{
    Book book;
    std::vector<Book::FillEvent> sink;
    book.setFillSink(&sink);
    lobref::ReferenceBook ref;

    for (std::size_t i = 0; i < commands.size(); ++i) {
        lobdiff::applyToEngine(book, commands[i]);
        lobdiff::applyToReference(ref, commands[i]);

        std::string diff;
        if (!lobdiff::fillsEqual(sink, ref.fills(), &diff)) {
            std::cerr << "DIVERGENCE (seed=" << seed << ", request=" << i << "): " << diff << "\n";
            std::cerr << "engine state: " << dumpEngine(book) << "\n";
            std::cerr << "ref state:    " << dumpRef(ref) << "\n";
            return 1;
        }
        if (!lobdiff::stateEqual(book, ref, &diff)) {
            std::cerr << "DIVERGENCE (seed=" << seed << ", request=" << i << "): " << diff << "\n";
            std::cerr << "engine state: " << dumpEngine(book) << "\n";
            std::cerr << "ref state:    " << dumpRef(ref) << "\n";
            return 1;
        }
        const std::vector<std::string> problems = lobdiff::checkInvariants(book);
        if (!problems.empty()) {
            std::cerr << "INVARIANT VIOLATION (seed=" << seed << ", request=" << i
                      << "): " << problems.front() << "\n";
            return 1;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::uint32_t seed = 0;
    std::size_t requests = 2000;
    std::string opsSpec = "a/c/m/x:50/20/20/10";
    std::string outPath;
    std::string replayPath;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + arg);
            }
            return argv[++i];
        };
        if (arg == "--seed") {
            seed = static_cast<std::uint32_t>(std::stoul(next()));
        } else if (arg == "--requests") {
            requests = std::stoull(next());
        } else if (arg == "--ops") {
            opsSpec = next();
        } else if (arg == "--out") {
            outPath = next();
        } else if (arg == "--replay") {
            replayPath = next();
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
        if (!replayPath.empty()) {
            const std::vector<lobref::Command> commands = lobref::parseCorpus(readFile(replayPath));
            std::cout << "replayed " << commands.size() << " commands from " << replayPath << "\n";
            return replay(commands, seed);
        }

        // Parse ops weights "a/c/m/x:w1/w2/w3/w4".
        lobref::GeneratorParams p;
        p.seed = seed;
        p.num_requests = requests;
        {
            const std::size_t colon = opsSpec.find(':');
            if (colon == std::string::npos) {
                throw std::runtime_error("--ops must be a/c/m/x:w1/w2/w3/w4");
            }
            const std::string keys = opsSpec.substr(0, colon);
            const std::string vals = opsSpec.substr(colon + 1);
            std::istringstream kss(keys);
            std::vector<char> keyChars;
            std::string ktok;
            while (std::getline(kss, ktok, '/')) {
                if (ktok.size() == 1) {
                    keyChars.push_back(ktok[0]);
                }
            }
            std::istringstream vss(vals);
            std::vector<int> w;
            std::string tok;
            while (std::getline(vss, tok, '/')) {
                w.push_back(std::stoi(tok));
            }
            if (keyChars.size() != 4 || w.size() != 4) {
                throw std::runtime_error("--ops must be a/c/m/x:w1/w2/w3/w4");
            }
            for (std::size_t k = 0; k < 4; ++k) {
                const int weight = w[k];
                switch (keyChars[k]) {
                case 'a': p.weightAdd = weight; break;
                case 'c': p.weightCxl = weight; break;
                case 'm': p.weightMod = weight; break;
                case 'x': p.weightMkt = weight; break;
                default: throw std::runtime_error("unknown op key in --ops");
                }
            }
        }

        const std::vector<lobref::Command> commands = lobref::generateWorkload(p);
        if (!outPath.empty()) {
            writeFile(outPath, lobref::serializeCorpus(commands));
            std::cout << "wrote " << commands.size() << " commands to " << outPath << "\n";
        }
        return replay(commands, seed);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
}
