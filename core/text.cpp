// iris::text (core/text.h says what each does).
#include "core/text.h"

namespace iris {
namespace text {

bool validUtf8(std::string_view s)
{
    const auto *p = reinterpret_cast<const unsigned char *>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) { ++i; continue; }
        std::size_t len;
        std::uint32_t cp;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;                                  // a continuation byte first, or 0xF8-0xFF
        if (n - i < len) return false;                      // truncated
        for (std::size_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        static const std::uint32_t kMin[5] = { 0, 0, 0x80, 0x800, 0x10000 };
        if (cp < kMin[len]) return false;                   // overlong
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;     // a surrogate
        i += len;
    }
    return true;
}

std::string asciiLower(std::string_view s)
{
    std::string out(s);
    for (char &c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string asciiUpper(std::string_view s)
{
    std::string out(s);
    for (char &c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

bool iequalsAscii(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

std::string_view trim(std::string_view s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> split(std::string_view s, char sep)
{
    std::vector<std::string> out;
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = s.find(sep, from);
        if (at == std::string_view::npos) {
            out.emplace_back(s.substr(from));
            return out;
        }
        out.emplace_back(s.substr(from, at - from));
        from = at + 1;
    }
}

std::string join(const std::vector<std::string> &parts, std::string_view sep)
{
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out.append(sep.data(), sep.size());
        out.append(parts[i]);
    }
    return out;
}

std::string shortest(double v)
{
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

std::string fixed(double v, int digits)
{
    if (digits < 0) digits = 0;
    std::string buf(static_cast<std::size_t>(330 + digits), '\0');   // 1e308 has 309 integer digits
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, digits);
    buf.resize(r.ec == std::errc() ? static_cast<std::size_t>(r.ptr - buf.data()) : 0);
    return buf;
}

std::optional<std::int64_t> parseInt(std::string_view s)
{
    std::int64_t v = 0;
    const char *end = s.data() + s.size();
    const auto r = std::from_chars(s.data(), end, v, 10);
    if (s.empty() || r.ec != std::errc() || r.ptr != end) return std::nullopt;
    return v;
}

std::optional<double> parseDouble(std::string_view s)
{
    double v = 0;
    const char *end = s.data() + s.size();
    const auto r = std::from_chars(s.data(), end, v, std::chars_format::general);
    if (s.empty() || r.ec != std::errc() || r.ptr != end) return std::nullopt;
    return v;
}

}   // namespace text
}   // namespace iris
