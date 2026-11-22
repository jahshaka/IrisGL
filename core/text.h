// iris::text - THE TEXT HELPERS BELOW THE EDITOR (NOQT-CORE-1 stage 1; V2 plan 0.12). std::string is UTF-8 everywhere below
// the editor; these are the operations the document, the import, the mirror and the data layer measured using (the ground
// read's set, spikes/noqt-core/GROUND.md § 2a), locale-free: case folding is ASCII only (every measured site folds a suffix,
// a key or a name compare), numbers go through std::to_chars / std::from_chars (never the C locale's strtod or printf).
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace iris {
namespace text {

/// True when `s` is well-formed UTF-8: no overlong form, no surrogate (U+D800-DFFF), nothing above U+10FFFF, no
/// truncated sequence.
bool validUtf8(std::string_view s);

/// ASCII case folding; every other byte (UTF-8 continuation bytes included) is kept as it is.
std::string asciiLower(std::string_view s);
std::string asciiUpper(std::string_view s);
bool iequalsAscii(std::string_view a, std::string_view b);

inline bool startsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}
inline bool endsWith(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// `s` without its leading and trailing ASCII whitespace (space, \t, \n, \v, \f, \r).
std::string_view trim(std::string_view s);

/// The pieces of `s` between each `sep` (empty pieces kept: "a,,b" is three; "" is one empty piece).
std::vector<std::string> split(std::string_view s, char sep);
std::string join(const std::vector<std::string> &parts, std::string_view sep);

/// The shortest text that parses back to exactly `v` (std::to_chars: locale-free); "inf", "-inf", "nan" for those.
std::string shortest(double v);
/// `v` with exactly `digits` digits after the point (std::to_chars fixed: locale-free).
std::string fixed(double v, int digits);

/// The WHOLE of `s` as a base-10 integer, or nothing: no sign but a leading '-', no space, no trailing text, no overflow.
std::optional<std::int64_t> parseInt(std::string_view s);
/// The WHOLE of `s` as a double (std::from_chars general form, locale-free), or nothing: no leading '+' or space, no
/// trailing text, nothing out of a double's range.
std::optional<double> parseDouble(std::string_view s);

namespace detail {
inline void append(std::string &out, std::string_view s) { out.append(s.data(), s.size()); }
inline void append(std::string &out, const char *s) { out.append(s); }
inline void append(std::string &out, const std::string &s) { out.append(s); }
inline void append(std::string &out, char c) { out.push_back(c); }
inline void append(std::string &out, bool b) { out.append(b ? "true" : "false"); }
inline void append(std::string &out, double v) { out.append(shortest(v)); }
inline void append(std::string &out, float v) { out.append(shortest(static_cast<double>(v))); }
template <typename T, typename = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>>>
inline void append(std::string &out, T v)
{
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, static_cast<std::size_t>(r.ptr - buf));
}
}   // namespace detail

/// Concatenation instead of a "%1" engine: strings and characters as they are, integers in base 10, floating values in
/// their shortest round-trip form, bools as true/false.
template <typename... Args>
std::string cat(const Args &...args)
{
    std::string out;
    (detail::append(out, args), ...);
    return out;
}

}   // namespace text
}   // namespace iris
