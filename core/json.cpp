// iris::json (MOVED from the script host's json.cpp: NOQT-CORE-1 stage 3): core/json.h says the laws. The standard library
// only (std::to_chars / std::from_chars for the numbers): NOQT-CORE-1 moves this file whole.
#include "core/json.h"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace iris { namespace json {

namespace {

const char kBytesKey[] = "$bytes";
const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char kHex[] = "0123456789abcdef";

/// NaN or an infinity, READ BY ITS BITS (CODING_RULES § 2: never `x == x`): the exponent all ones.
bool nonFiniteBits(double d)
{
    std::uint64_t u = 0;
    std::memcpy(&u, &d, sizeof u);
    return (u & 0x7ff0000000000000ull) == 0x7ff0000000000000ull;
}

/// The length (1-4) of the strict UTF-8 sequence at s[i], 0 when there is none: an overlong form, a surrogate, a code
/// point over U+10FFFF, a stray or a missing continuation byte.
std::size_t utf8Length(std::string_view s, std::size_t i)
{
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return 1;
    std::size_t n = 0;
    unsigned char lo = 0x80, hi = 0xBF;   // the second byte's range (the first byte decides it)
    if (c < 0xC2) return 0;               // a continuation byte, or C0/C1 (an overlong two-byte form)
    if (c < 0xE0) n = 2;
    else if (c < 0xF0) {
        n = 3;
        if (c == 0xE0) lo = 0xA0;         // overlong
        if (c == 0xED) hi = 0x9F;         // U+D800..DFFF: a surrogate
    } else if (c < 0xF5) {
        n = 4;
        if (c == 0xF0) lo = 0x90;         // overlong
        if (c == 0xF4) hi = 0x8F;         // over U+10FFFF
    } else {
        return 0;
    }
    if (i + n > s.size()) return 0;
    const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
    if (c1 < lo || c1 > hi) return 0;
    for (std::size_t k = 2; k < n; ++k)
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 0;
    return n;
}

void appendUtf8(std::string &out, std::uint32_t cp)
{
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | cp >> 6);
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | cp >> 12);
        out += char(0x80 | (cp >> 6 & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | cp >> 18);
        out += char(0x80 | (cp >> 12 & 0x3F));
        out += char(0x80 | (cp >> 6 & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

std::string capText(std::size_t n)
{
    return std::to_string(n) + " bytes: over the 1 MiB (" + std::to_string(kBytesCap) + ") a value carries as bytes";
}

/// WHERE A VALUE SITS, as a chain on the stack: made into text only when a write fails.
struct JsonPath {
    const JsonPath *up = nullptr;
    const std::string *name = nullptr;   // an object's member
    std::size_t index = 0;               // else an array's element
};
std::string pathText(const JsonPath *p)
{
    if (!p) return "value";
    return pathText(p->up) + (p->name ? "." + *p->name : "[" + std::to_string(p->index) + "]");
}

class JsonWriter {
public:
    JsonWriter(std::string &out, std::string &why) : mOut(out), mWhy(why) {}

    bool value(const Value &v, const JsonPath *at, int depth)
    {
        switch (v.kind()) {
        case Kind::Null:   mOut += "null"; return true;
        case Kind::Bool:   mOut += v.asBool() ? "true" : "false"; return true;
        case Kind::Int:    return integer(v.asInt());
        case Kind::Number: return number(v.asNumber(), at);
        case Kind::String: return string(v.asString(), at, "");
        case Kind::Bytes:  return bytes(v.asBytes(), at);
        case Kind::Array: {
            if (depth >= kMaxDepth) return fail(at, "is nested deeper than 64 arrays and objects");
            mOut += '[';
            const Value::Array &a = v.asArray();
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (i) mOut += ',';
                const JsonPath element{at, nullptr, i};
                if (!value(a[i], &element, depth + 1)) return false;
            }
            mOut += ']';
            return true;
        }
        case Kind::Object: {
            if (depth >= kMaxDepth) return fail(at, "is nested deeper than 64 arrays and objects");
            mOut += '{';
            const Value::Object &o = v.asObject();
            std::unordered_set<std::string_view> seen;   // views of the value's own names (they hold still meanwhile)
            for (std::size_t i = 0; i < o.size(); ++i) {
                const std::string &name = o[i].first;
                if (name == kBytesKey)
                    return fail(at, "has a member named \"$bytes\": it would read back as bytes");
                if (!seen.insert(name).second) return fail(at, "names the member '" + name + "' twice");
                if (i) mOut += ',';
                if (!string(name, at, "a member name ")) return false;
                mOut += ':';
                const JsonPath member{at, &name, 0};
                if (!value(o[i].second, &member, depth + 1)) return false;
            }
            mOut += '}';
            return true;
        }
        }
        return fail(at, "is of no kind");
    }

private:
    bool fail(const JsonPath *at, const std::string &reason)
    {
        mWhy = pathText(at) + " " + reason;
        return false;
    }

    bool integer(std::int64_t i)
    {
        char b[24];
        const std::to_chars_result r = std::to_chars(b, b + sizeof b, i);
        mOut.append(b, r.ptr);
        return true;
    }

    /// The shortest text that reads back to the same bits, with a fraction or an exponent so it reads back a Number.
    bool number(double d, const JsonPath *at)
    {
        if (nonFiniteBits(d)) return fail(at, "is not a finite number (JSON carries no NaN or infinity)");
        char b[32];
        const std::to_chars_result r = std::to_chars(b, b + sizeof b, d);
        const std::string_view t(b, std::size_t(r.ptr - b));
        mOut += t;
        if (t.find_first_of(".e") == std::string_view::npos) mOut += ".0";
        return true;
    }

    /// `s` quoted: every control character escaped (no newline is ever written), its UTF-8 checked.
    bool string(const std::string &s, const JsonPath *at, const char *what)
    {
        mOut += '"';
        for (std::size_t i = 0; i < s.size();) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c >= 0x80) {
                const std::size_t n = utf8Length(s, i);
                if (!n) return fail(at, std::string(what) + "holds invalid UTF-8 (at byte " + std::to_string(i) + ")");
                mOut.append(s, i, n);
                i += n;
                continue;
            }
            switch (c) {
            case '"':  mOut += "\\\""; break;
            case '\\': mOut += "\\\\"; break;
            case '\n': mOut += "\\n"; break;
            case '\r': mOut += "\\r"; break;
            case '\t': mOut += "\\t"; break;
            case '\b': mOut += "\\b"; break;
            case '\f': mOut += "\\f"; break;
            default:
                if (c < 0x20) {
                    mOut += "\\u00";
                    mOut += kHex[c >> 4];
                    mOut += kHex[c & 15];
                } else {
                    mOut += char(c);
                }
            }
            ++i;
        }
        mOut += '"';
        return true;
    }

    bool bytes(const Value::Bytes &b, const JsonPath *at)
    {
        if (b.size() > kBytesCap) return fail(at, "is " + capText(b.size()));
        mOut += "{\"$bytes\":\"";
        std::size_t i = 0;
        for (; i + 3 <= b.size(); i += 3) {
            const unsigned v = unsigned(b[i]) << 16 | unsigned(b[i + 1]) << 8 | b[i + 2];
            mOut += kBase64[v >> 18];
            mOut += kBase64[v >> 12 & 63];
            mOut += kBase64[v >> 6 & 63];
            mOut += kBase64[v & 63];
        }
        if (i < b.size()) {
            const bool two = i + 1 < b.size();
            const unsigned v = unsigned(b[i]) << 16 | (two ? unsigned(b[i + 1]) << 8 : 0u);
            mOut += kBase64[v >> 18];
            mOut += kBase64[v >> 12 & 63];
            mOut += two ? kBase64[v >> 6 & 63] : '=';
            mOut += '=';
        }
        mOut += "\"}";
        return true;
    }

    std::string &mOut;
    std::string &mWhy;
};

int base64Digit(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/// Strict base64 (padded, its unused bits zero) into `out`, at most kBytesCap decoded; false with `why`.
bool base64Decode(const std::string &s, Value::Bytes &out, std::string &why)
{
    const std::size_t n = s.size();
    if (n % 4) {
        why = "the $bytes text is not base64 (its length " + std::to_string(n) + " is no multiple of 4)";
        return false;
    }
    const std::size_t pad = n ? std::size_t(s[n - 1] == '=') + std::size_t(s[n - 2] == '=') : 0;
    const std::size_t size = n / 4 * 3 - pad;
    if (size > kBytesCap) {
        why = "the $bytes value is " + capText(size);
        return false;
    }
    out.clear();
    out.reserve(size);
    for (std::size_t i = 0; i < n; i += 4) {
        const bool last = i + 4 == n;
        unsigned v = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            int d = base64Digit(s[i + k]);
            if (d < 0 && last && s[i + k] == '=' && k >= 4 - pad) d = 0;
            if (d < 0) {
                why = "the $bytes text is not base64 (character " + std::to_string(i + k) + ")";
                return false;
            }
            v = v << 6 | unsigned(d);
        }
        if (last && ((pad == 1 && (v & 0xFF)) || (pad == 2 && (v & 0xFFFF)))) {
            why = "the $bytes text is not canonical base64 (its last character carries bits past the bytes)";
            return false;
        }
        out.push_back(std::uint8_t(v >> 16));
        if (!(last && pad == 2)) out.push_back(std::uint8_t(v >> 8 & 0xFF));
        if (!(last && pad >= 1)) out.push_back(std::uint8_t(v & 0xFF));
    }
    return true;
}

class JsonReader {
public:
    JsonReader(std::string_view s, std::string &why) : mS(s), mWhy(why) {}

    bool document(Value &out)
    {
        if (mS.empty()) return fail("an empty input holds no value");
        if (mS.size() >= 3 && mS.compare(0, 3, "\xEF\xBB\xBF") == 0)
            return fail("a byte-order mark is refused (JSON text is UTF-8 with none)");
        space();
        if (!value(out, 0)) return false;
        space();
        return mI == mS.size() || fail("text follows the value");
    }

private:
    bool failAt(std::size_t at, const std::string &reason)
    {
        std::size_t line = 1, lineStart = 0;
        for (std::size_t i = 0; i < at && i < mS.size(); ++i)
            if (mS[i] == '\n') {
                ++line;
                lineStart = i + 1;
            }
        mWhy = "line " + std::to_string(line) + ", column " + std::to_string(at - lineStart + 1) + ": " + reason;
        return false;
    }
    bool fail(const std::string &reason) { return failAt(mI, reason); }

    std::string here() const
    {
        const unsigned char c = static_cast<unsigned char>(mS[mI]);
        if (c > 0x20 && c < 0x7F) return std::string("'") + char(c) + "'";
        return std::string("byte 0x") + kHex[c >> 4] + kHex[c & 15];
    }

    void space()
    {
        while (mI < mS.size() && (mS[mI] == ' ' || mS[mI] == '\t' || mS[mI] == '\n' || mS[mI] == '\r')) ++mI;
    }
    bool digit() const { return mI < mS.size() && mS[mI] >= '0' && mS[mI] <= '9'; }

    bool value(Value &out, int depth)
    {
        if (mI >= mS.size()) return fail("the input ends where a value belongs");
        switch (mS[mI]) {
        case '{': return object(out, depth);
        case '[': return array(out, depth);
        case '"': {
            std::string s;
            if (!string(s)) return false;
            out = Value(std::move(s));
            return true;
        }
        case 't': return word("true", Value(true), out);
        case 'f': return word("false", Value(false), out);
        case 'n': return word("null", Value(), out);
        default:
            if (mS[mI] == '-' || digit()) return number(out);
            return fail(here() + " begins no value");
        }
    }

    bool word(std::string_view w, Value v, Value &out)
    {
        if (mS.compare(mI, w.size(), w) != 0) return fail(here() + " begins no value");
        mI += w.size();
        out = std::move(v);
        return true;
    }

    bool array(Value &out, int depth)
    {
        if (depth >= kMaxDepth) return fail("nested deeper than 64 arrays and objects");
        ++mI;
        space();
        Value::Array a;
        if (mI < mS.size() && mS[mI] == ']') {
            ++mI;
            out = Value(std::move(a));
            return true;
        }
        for (;;) {
            Value v;
            if (!value(v, depth + 1)) return false;
            a.push_back(std::move(v));
            space();
            if (mI < mS.size() && mS[mI] == ',') {
                ++mI;
                space();
                continue;
            }
            if (mI < mS.size() && mS[mI] == ']') break;
            return fail(mI < mS.size() ? "',' or ']' expected, not " + here() : "the input ends inside an array");
        }
        ++mI;
        out = Value(std::move(a));
        return true;
    }

    bool object(Value &out, int depth)
    {
        if (depth >= kMaxDepth) return fail("nested deeper than 64 arrays and objects");
        const std::size_t start = mI;
        ++mI;
        space();
        Value::Object o;
        std::unordered_set<std::string> seen;
        if (mI < mS.size() && mS[mI] == '}') {
            ++mI;
            out = Value(std::move(o));
            return true;
        }
        for (;;) {
            if (mI >= mS.size() || mS[mI] != '"')
                return fail(mI < mS.size() ? "a member's name (a string) expected, not " + here()
                                           : "the input ends inside an object");
            const std::size_t at = mI;
            std::string name;
            if (!string(name)) return false;
            if (!seen.insert(name).second) return failAt(at, "the member '" + name + "' is named twice");
            space();
            if (mI >= mS.size() || mS[mI] != ':')
                return fail(mI < mS.size() ? "':' expected, not " + here() : "the input ends inside an object");
            ++mI;
            space();
            Value v;
            if (!value(v, depth + 1)) return false;
            o.emplace_back(std::move(name), std::move(v));
            space();
            if (mI < mS.size() && mS[mI] == ',') {
                ++mI;
                space();
                continue;
            }
            if (mI < mS.size() && mS[mI] == '}') break;
            return fail(mI < mS.size() ? "',' or '}' expected, not " + here() : "the input ends inside an object");
        }
        ++mI;
        if (seen.count(kBytesKey)) {   // BYTES: exactly {"$bytes": "<base64>"}
            if (o.size() != 1 || o[0].second.kind() != Kind::String)
                return failAt(start, "an object holding \"$bytes\" must be exactly {\"$bytes\": \"<base64>\"}");
            Value::Bytes b;
            std::string why;
            if (!base64Decode(o[0].second.asString(), b, why)) return failAt(start, why);
            out = Value::bytes(std::move(b));
            return true;
        }
        out = Value(std::move(o));
        return true;
    }

    bool hex4(std::uint32_t &u)
    {
        if (mI + 4 > mS.size()) return fail("four hex digits expected after \\u");
        u = 0;
        for (int k = 0; k < 4; ++k, ++mI) {
            const char c = mS[mI];
            const int d = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
            if (d < 0) return fail("four hex digits expected after \\u");
            u = u << 4 | std::uint32_t(d);
        }
        return true;
    }

    bool string(std::string &out)
    {
        ++mI;   // the opening quote
        for (;;) {
            if (mI >= mS.size()) return fail("the input ends inside a string");
            const unsigned char c = static_cast<unsigned char>(mS[mI]);
            if (c == '"') {
                ++mI;
                return true;
            }
            if (c >= 0x80) {
                const std::size_t n = utf8Length(mS, mI);
                if (!n) return fail("invalid UTF-8 (" + here() + ")");
                out.append(mS.data() + mI, n);
                mI += n;
                continue;
            }
            if (c < 0x20) return fail("a raw control character (" + here() + ") in a string: it must be escaped");
            if (c != '\\') {
                out += char(c);
                ++mI;
                continue;
            }
            const std::size_t at = mI++;
            if (mI >= mS.size()) return fail("the input ends inside a string");
            const char e = mS[mI++];
            switch (e) {
            case '"':  out += '"'; break;
            case '\\': out += '\\'; break;
            case '/':  out += '/'; break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u': {
                std::uint32_t u = 0;
                if (!hex4(u)) return false;
                if (u >= 0xDC00 && u <= 0xDFFF) return failAt(at, "a lone low surrogate escape is refused");
                if (u >= 0xD800 && u <= 0xDBFF) {
                    std::uint32_t lo = 0;
                    if (mS.compare(mI, 2, "\\u") != 0) return failAt(at, "a lone high surrogate escape is refused");
                    mI += 2;
                    if (!hex4(lo)) return false;
                    if (lo < 0xDC00 || lo > 0xDFFF) return failAt(at, "a lone high surrogate escape is refused");
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                }
                appendUtf8(out, u);
                break;
            }
            default:
                return failAt(at, std::string("an unknown escape '\\") + e + "'");
            }
        }
    }

    bool number(Value &out)
    {
        const std::size_t start = mI;
        bool integral = true;
        if (mS[mI] == '-') ++mI;
        if (!digit()) return fail("a digit expected");
        if (mS[mI] == '0') {
            ++mI;
            if (digit()) return failAt(start, "a number has no leading zero");
        } else {
            while (digit()) ++mI;
        }
        if (mI < mS.size() && mS[mI] == '.') {
            integral = false;
            ++mI;
            if (!digit()) return fail("a digit expected after '.'");
            while (digit()) ++mI;
        }
        if (mI < mS.size() && (mS[mI] == 'e' || mS[mI] == 'E')) {
            integral = false;
            ++mI;
            if (mI < mS.size() && (mS[mI] == '+' || mS[mI] == '-')) ++mI;
            if (!digit()) return fail("a digit expected in the exponent");
            while (digit()) ++mI;
        }
        const char *first = mS.data() + start, *last = mS.data() + mI;
        if (integral) {
            std::int64_t i = 0;
            const std::from_chars_result r = std::from_chars(first, last, i);
            if (r.ec != std::errc() || r.ptr != last)
                return failAt(start, "the integer " + std::string(first, last) + " is outside a 64-bit integer");
            out = Value(i);
            return true;
        }
        double d = 0;
        const std::from_chars_result r = std::from_chars(first, last, d);
        if (r.ec != std::errc() || r.ptr != last)
            return failAt(start, "the number " + std::string(first, last) + " is outside a double's range");
        out = Value(d);
        return true;
    }

    std::string_view mS;
    std::size_t mI = 0;
    std::string &mWhy;
};

}  // namespace

bool write(const Value &value, std::string &out, std::string &why)
{
    out.clear();
    JsonWriter w(out, why);
    if (w.value(value, nullptr, 0)) return true;
    out.clear();
    return false;
}

bool parse(std::string_view text, Value &out, std::string &why)
{
    JsonReader p(text, why);
    Value v;
    if (!p.document(v)) return false;
    out = std::move(v);
    return true;
}

}}  // namespace iris::json
