#include "Command.hpp"

#include <sstream>
#include <stdexcept>

namespace lobref {

namespace {

std::int64_t parseI64(const std::string& tok)
{
    if (tok.empty()) {
        throw std::runtime_error("empty integer token");
    }
    std::size_t idx = 0;
    std::int64_t v;
    try {
        v = static_cast<std::int64_t>(std::stoll(tok, &idx, 10));
    } catch (const std::exception&) {
        throw std::runtime_error("malformed integer token: " + tok);
    }
    if (idx != tok.size()) {
        throw std::runtime_error("malformed integer token: " + tok);
    }
    return v;
}

bool parseSide(const std::string& tok)
{
    const std::int64_t v = parseI64(tok);
    if (v != 0 && v != 1) {
        throw std::runtime_error("side must be 0 or 1, got: " + tok);
    }
    return v == 1;
}

std::vector<std::string> splitWhitespace(const std::string& line)
{
    std::vector<std::string> toks;
    std::istringstream ss(line);
    std::string t;
    while (ss >> t) {
        toks.push_back(t);
    }
    return toks;
}

} // namespace

std::string serializeCommand(const Command& c)
{
    std::string s;
    switch (c.op) {
    case Command::Op::Add:
        s = "Add " + std::to_string(c.id) + " " + (c.side ? "1" : "0") + " " +
            std::to_string(c.qty) + " " + std::to_string(c.price);
        break;
    case Command::Op::Mkt:
        s = "Mkt " + std::to_string(c.id) + " " + (c.side ? "1" : "0") + " " +
            std::to_string(c.qty);
        break;
    case Command::Op::Cxl:
        s = "Cxl " + std::to_string(c.id);
        break;
    case Command::Op::Mod:
        s = "Mod " + std::to_string(c.id) + " " + std::to_string(c.qty) + " " +
            std::to_string(c.price);
        break;
    }
    return s;
}

std::string serializeCorpus(const std::vector<Command>& commands)
{
    std::string s;
    for (const auto& c : commands) {
        s += serializeCommand(c);
        s += '\n';
    }
    return s;
}

Command parseCommand(const std::string& line)
{
    const std::vector<std::string> t = splitWhitespace(line);
    if (t.empty()) {
        throw std::runtime_error("empty command line");
    }
    Command c;
    if (t[0] == "Add") {
        if (t.size() != 5) {
            throw std::runtime_error("Add expects 4 args: <id> <side> <qty> <price>");
        }
        c.op = Command::Op::Add;
        c.id = parseI64(t[1]);
        c.side = parseSide(t[2]);
        c.qty = parseI64(t[3]);
        c.price = parseI64(t[4]);
    } else if (t[0] == "Mkt") {
        if (t.size() != 4) {
            throw std::runtime_error("Mkt expects 3 args: <id> <side> <qty>");
        }
        c.op = Command::Op::Mkt;
        c.id = parseI64(t[1]);
        c.side = parseSide(t[2]);
        c.qty = parseI64(t[3]);
    } else if (t[0] == "Cxl") {
        if (t.size() != 2) {
            throw std::runtime_error("Cxl expects 1 arg: <id>");
        }
        c.op = Command::Op::Cxl;
        c.id = parseI64(t[1]);
    } else if (t[0] == "Mod") {
        if (t.size() != 4) {
            throw std::runtime_error("Mod expects 3 args: <id> <qty> <price>");
        }
        c.op = Command::Op::Mod;
        c.id = parseI64(t[1]);
        c.qty = parseI64(t[2]);
        c.price = parseI64(t[3]);
    } else {
        throw std::runtime_error("unknown command: " + t[0]);
    }
    return c;
}

std::vector<Command> parseCorpus(const std::string& text)
{
    std::istringstream in(text);
    return parseCorpus(in);
}

std::vector<Command> parseCorpus(std::istream& in)
{
    std::vector<Command> commands;
    std::string line;
    std::size_t lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        // Strip a trailing '\r' for tolerance of CRLF input.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::vector<std::string> t = splitWhitespace(line);
        if (t.empty()) {
            continue; // blank line
        }
        try {
            commands.push_back(parseCommand(line));
        } catch (const std::exception& e) {
            throw std::runtime_error("line " + std::to_string(lineno) + ": " + e.what());
        }
    }
    return commands;
}

} // namespace lobref
