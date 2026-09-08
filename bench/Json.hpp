#ifndef BENCH_JSON_HPP
#define BENCH_JSON_HPP

// Minimal self-contained JSON value, parser and pretty printer used by the
// Stage C benchmark harness (workload configs in, stamped result files out).
// Object keys are kept sorted (std::map) so emitted JSON is deterministic.

#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace bench {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    Json() : t_(Type::Null) {}
    Json(std::nullptr_t) : t_(Type::Null) {}
    Json(bool b) : t_(Type::Bool), b_(b) {}
    Json(int v) : t_(Type::Int), i_(v) {}
    Json(long v) : t_(Type::Int), i_(v) {}
    Json(long long v) : t_(Type::Int), i_(v) {}
    Json(unsigned v) : t_(Type::Int), i_(static_cast<long long>(v)) {}
    Json(unsigned long long v) : t_(Type::Int), i_(static_cast<long long>(v)) {}
    Json(double d) : t_(Type::Double), d_(d) {}
    Json(const char* s) : t_(Type::String), s_(s) {}
    Json(const std::string& s) : t_(Type::String), s_(s) {}
    Json(std::string&& s) : t_(Type::String), s_(std::move(s)) {}
    Json(const Array& a) : t_(Type::Array), a_(a) {}
    Json(Array&& a) : t_(Type::Array), a_(std::move(a)) {}
    Json(const Object& o) : t_(Type::Object), o_(o) {}
    Json(Object&& o) : t_(Type::Object), o_(std::move(o)) {}

    Type type() const { return t_; }
    bool isNull() const { return t_ == Type::Null; }
    bool isBool() const { return t_ == Type::Bool; }
    bool isInt() const { return t_ == Type::Int; }
    bool isDouble() const { return t_ == Type::Double; }
    bool isNumber() const { return t_ == Type::Int || t_ == Type::Double; }
    bool isString() const { return t_ == Type::String; }
    bool isArray() const { return t_ == Type::Array; }
    bool isObject() const { return t_ == Type::Object; }

    long long asInt() const
    {
        if (t_ == Type::Int) return i_;
        if (t_ == Type::Double) return static_cast<long long>(d_);
        throw std::runtime_error("JSON value is not a number");
    }
    double asDouble() const
    {
        if (t_ == Type::Double) return d_;
        if (t_ == Type::Int) return static_cast<double>(i_);
        throw std::runtime_error("JSON value is not a number");
    }
    bool asBool() const
    {
        if (t_ == Type::Bool) return b_;
        throw std::runtime_error("JSON value is not a bool");
    }
    const std::string& asString() const
    {
        if (t_ == Type::String) return s_;
        throw std::runtime_error("JSON value is not a string");
    }

    const Object& object() const
    {
        if (t_ != Type::Object) throw std::runtime_error("JSON value is not an object");
        return o_;
    }
    const Array& array() const
    {
        if (t_ != Type::Array) throw std::runtime_error("JSON value is not an array");
        return a_;
    }

    bool has(const std::string& key) const
    {
        return t_ == Type::Object && o_.count(key) != 0;
    }
    const Json& at(const std::string& key) const
    {
        const Object& o = object();
        auto it = o.find(key);
        if (it == o.end()) throw std::runtime_error("JSON object missing key: " + key);
        return it->second;
    }
    const Json& at(std::size_t idx) const
    {
        const Array& a = array();
        if (idx >= a.size()) throw std::runtime_error("JSON array index out of range");
        return a[idx];
    }

    Json& operator[](const std::string& key)
    {
        if (t_ != Type::Object) { t_ = Type::Object; o_.clear(); }
        return o_[key];
    }
    void push_back(const Json& v)
    {
        if (t_ != Type::Array) { t_ = Type::Array; a_.clear(); }
        a_.push_back(v);
    }

    static Json parse(const std::string& text);

    std::string dump(int indent = 2) const;

private:
    Type t_ = Type::Null;
    bool b_ = false;
    long long i_ = 0;
    double d_ = 0.0;
    std::string s_;
    Array a_;
    Object o_;
};

namespace detail {

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    Json parseDocument()
    {
        skipWs();
        Json v = parseValue();
        skipWs();
        if (pos_ != s_.size()) fail("trailing content");
        return v;
    }

private:
    const std::string& s_;
    std::size_t pos_ = 0;

    [[noreturn]] void fail(const std::string& msg) const
    {
        throw std::runtime_error("JSON parse error at byte " + std::to_string(pos_) + ": " + msg);
    }
    void skipWs()
    {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }
    char peek() const
    {
        if (pos_ >= s_.size()) fail("unexpected end of input");
        return s_[pos_];
    }
    char take() { return s_[pos_++]; }
    void expect(char c)
    {
        if (pos_ >= s_.size() || s_[pos_] != c) {
            fail(std::string("expected '") + c + "'");
        }
        ++pos_;
    }

    Json parseValue()
    {
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return Json(parseString());
        if (c == 't') { expectLiteral("true"); return Json(true); }
        if (c == 'f') { expectLiteral("false"); return Json(false); }
        if (c == 'n') { expectLiteral("null"); return Json(nullptr); }
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        fail("unexpected character");
    }
    void expectLiteral(const char* lit)
    {
        for (const char* p = lit; *p; ++p) {
            if (pos_ >= s_.size() || s_[pos_] != *p) fail(std::string("expected ") + lit);
            ++pos_;
        }
    }
    std::string parseString()
    {
        expect('"');
        std::string out;
        while (true) {
            if (pos_ >= s_.size()) fail("unterminated string");
            char c = take();
            if (c == '"') break;
            if (c == '\\') {
                if (pos_ >= s_.size()) fail("unterminated escape");
                char e = take();
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (pos_ + 4 > s_.size()) fail("bad unicode escape");
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = s_[pos_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp += h - '0';
                        else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
                        else fail("bad unicode escape");
                    }
                    // Encode UTF-8 (surrogates collapsed to the BMP code point).
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: fail("bad escape");
                }
            } else {
                out += c;
            }
        }
        return out;
    }
    Json parseNumber()
    {
        std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        bool isInt = true;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            isInt = false;
            ++pos_;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            isInt = false;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        }
        std::string tok = s_.substr(start, pos_ - start);
        if (isInt) {
            try {
                return Json(std::stoll(tok));
            } catch (...) {
                return Json(std::stod(tok));
            }
        }
        return Json(std::stod(tok));
    }
    Json parseArray()
    {
        expect('[');
        Json::Array arr;
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return Json(std::move(arr)); }
        while (true) {
            skipWs();
            arr.push_back(parseValue());
            skipWs();
            char c = take();
            if (c == ']') break;
            if (c != ',') fail("expected ',' or ']'");
        }
        return Json(std::move(arr));
    }
    Json parseObject()
    {
        expect('{');
        Json::Object obj;
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return Json(std::move(obj)); }
        while (true) {
            skipWs();
            if (peek() != '"') fail("expected string key");
            std::string key = parseString();
            skipWs();
            expect(':');
            skipWs();
            obj[key] = parseValue();
            skipWs();
            char c = take();
            if (c == '}') break;
            if (c != ',') fail("expected ',' or '}'");
        }
        return Json(std::move(obj));
    }
};

inline std::string escapeString(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

inline std::string formatDouble(double d)
{
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, d);
    return std::string(buf, r.ptr);
}

} // namespace detail

inline Json Json::parse(const std::string& text)
{
    return detail::JsonParser(text).parseDocument();
}

inline std::string Json::dump(int indent) const
{
    std::string out;
    struct Impl {
        static void write(std::string& out, const Json& j, int indent, int depth)
        {
            std::string pad(depth * indent, ' ');
            switch (j.t_) {
            case Type::Null: out += "null"; break;
            case Type::Bool: out += j.b_ ? "true" : "false"; break;
            case Type::Int: out += std::to_string(j.i_); break;
            case Type::Double: out += detail::formatDouble(j.d_); break;
            case Type::String: out += detail::escapeString(j.s_); break;
            case Type::Array: {
                if (j.a_.empty()) { out += "[]"; break; }
                out += "[";
                for (std::size_t i = 0; i < j.a_.size(); ++i) {
                    if (i) out += ",";
                    out += "\n";
                    out += std::string((depth + 1) * indent, ' ');
                    write(out, j.a_[i], indent, depth + 1);
                }
                out += "\n";
                out += pad;
                out += "]";
                break;
            }
            case Type::Object: {
                if (j.o_.empty()) { out += "{}"; break; }
                out += "{";
                bool first = true;
                for (const auto& kv : j.o_) {
                    if (!first) out += ",";
                    first = false;
                    out += "\n";
                    out += std::string((depth + 1) * indent, ' ');
                    out += detail::escapeString(kv.first);
                    out += ": ";
                    write(out, kv.second, indent, depth + 1);
                }
                out += "\n";
                out += pad;
                out += "}";
                break;
            }
            }
        }
    };
    Impl::write(out, *this, indent, 0);
    out += "\n";
    return out;
}

} // namespace bench

#endif // BENCH_JSON_HPP
