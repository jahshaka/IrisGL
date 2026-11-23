#pragma once
// iris::json - THE ONE JSON CODEC BELOW THE EDITOR (NOQT-CORE-1 stage 3; V2 plan 0.12): iris::Value <-> JSON text, the
// standard library only. MOVED here whole from the script host (MCP-ON-HOST-1 stage 1, jahshaka/engine/ScriptJson.h,
// deleted): one codec, never a second. Its one include of ours is core/value.h.
//
// THE LAWS (both ways):
//   * UTF-8, strictly: an overlong form, a surrogate code point, a code point over U+10FFFF, a stray or missing
//     continuation byte are refused; a `\u` pair is joined, a lone surrogate refused. A Value holding invalid UTF-8 FAILS
//     to write, naming its path.
//   * A control character and `\n` are always written escaped: a string never holds a raw newline (`write`: one message
//     a line).
//   * An integer literal reads as an Int (outside int64: refused); a literal with a fraction or an exponent as a Number
//     (past a double's range, either way: refused - an UNDERFLOW too: "1e-400", whose nearest double is a zero, is
//     refused, never read as 0; a subnormal such as "4.9e-324" reads exactly).
//   * ONE NUMBER TEXT (NOQT-CORE-1 OPEN CALL 2, the lead's ruling: OPTION B, Qt's - measured on QJsonDocument): a Number
//     that is integral and of magnitude <= 2^53 is written as an integer ("1", "-3", "9007199254740992"; -0.0 as "0", its
//     sign dropped as Qt drops it); any other Number as the shortest text that reads back to the same bits, the shorter of
//     its fixed and exponent forms, a tie to fixed, the exponent of at least two digits ("0.1", "1e+16", "1e-04", "0.001",
//     "12345678901234567000") - std::to_chars' own rule. So an integral Number READS BACK AS AN INT: a Number's kind does
//     not survive the round trip (a Number parameter admits an Int: the registry's law, script.host); its value does.
//   * NaN and infinity are never written: the write FAILS naming the value's path (read by the number's bits).
//   * An object's members keep their order (`write`); a member named twice is refused.
//   * `writePretty` is QJsonDocument::Indented's text, byte for byte (core.json_qt_parity): members sorted by name (UTF-16
//     code-unit order, Qt's), four spaces a level, `"name": value`, an empty array or object `[`/`{`, a newline, the
//     closing bracket at the container's own indent, and one newline after the document.
//   * Bytes are `{"$bytes": "<base64>"}` both ways, at most 1 MiB decoded (over: refused by name); any other object
//     holding a `$bytes` member is refused (and a Value object with a member `$bytes` FAILS to write: it would read back
//     as bytes).
//   * At most 64 levels of arrays and objects (no recursion bomb): a deeper one is refused by name.
//   * Reading: one value, whitespace around it, nothing after it; a byte-order mark and an empty input are refused. A
//     refusal names its line and column (1-based; the column in bytes).
// A path in a failure is the registry's form under the root `value`: `value.transform.position[1]`.
// A NUL IS TEXT HERE: "\u0000" is valid JSON and reads into a String holding the byte 0 (std::string carries it). THE
// FIRST COMMAND THAT TAKES A FILE PATH must refuse a NUL in it by name (a C path stops at the first NUL: "a.png\0../x"
// would name another file than the one checked).

#include "core/export.h"
#include "core/value.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace iris { namespace json {

/// The most a Bytes value carries in JSON (decoded), and the deepest nesting of arrays and objects, either way.
constexpr std::size_t kBytesCap = std::size_t(1) << 20;
constexpr int kMaxDepth = 64;

/// `value` as one line of compact JSON into `out`; false with `why` (naming the value's path) when it cannot be carried.
IRIS_CORE_API bool write(const Value &value, std::string &out, std::string &why);
/// The same value as QJsonDocument::Indented writes it (the laws above): members sorted, four spaces a level, a final newline.
IRIS_CORE_API bool writePretty(const Value &value, std::string &out, std::string &why);
/// The one JSON value of `text` into `out`; false with `why` ("line L, column C: ...") when the text is refused.
IRIS_CORE_API bool parse(std::string_view text, Value &out, std::string &why);

}}  // namespace iris::json
