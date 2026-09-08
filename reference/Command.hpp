#ifndef REFERENCE_COMMAND_HPP
#define REFERENCE_COMMAND_HPP

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

// Canonical text corpus command stream, shared by the parser, the workload
// generator, the differential tests, the standalone fuzz tool and (later) the
// Stage C benchmarks / Stage E replay. One command per line, whitespace
// separated, plain ASCII, LF terminated:
//   Add <id> <side> <qty> <price>     side: 0 = sell, 1 = buy
//   Mkt <id> <side> <qty>
//   Cxl <id>
//   Mod <id> <qty> <price>
// Any unknown or malformed line is a parse error (corpora are generated, not
// hand-edited). Field ranges are bounded by the generator (see
// WorkloadGenerator.hpp) but the parser accepts any well-formed int64 tokens.

namespace lobref {

struct Command {
    enum class Op { Add, Mkt, Cxl, Mod };

    Op op = Op::Add;
    std::int64_t id = 0;
    bool side = false;      // true = buy, false = sell (Add / Mkt only)
    std::int64_t qty = 0;   // Add / Mkt / Mod
    std::int64_t price = 0; // Add / Mod
};

// Generator parameter block (full definition in WorkloadGenerator.hpp).
struct GeneratorParams;

// Seeded workload generator; implemented in WorkloadGenerator.cpp.
std::vector<Command> generateWorkload(const GeneratorParams& params);

// Serialize a command stream to canonical text (one command per line).
std::string serializeCorpus(const std::vector<Command>& commands);

// Parse a canonical text corpus. Throws std::runtime_error on any malformed or
// unknown line (with a 1-based line number in the message).
std::vector<Command> parseCorpus(const std::string& text);
std::vector<Command> parseCorpus(std::istream& in);

// Serialize/parse a single command (used internally and by tests).
std::string serializeCommand(const Command& c);
Command parseCommand(const std::string& line);

} // namespace lobref

#endif
