#pragma once
// THE SCRIPT VALUE IN JSON (MCP-ON-HOST-1 stage 1; SPECS/MCP_ON_HOST_1_SPEC.md) - script::Value <-> JSON text, the
// standard library only: its one include of ours is Script.h for Value and Kind (NOQT-CORE-1 stage 3 MOVES this file and
// json.cpp whole into the Qt-free core library, beside Value: a move, never a second codec).
//
// THE LAWS (both ways):
//   * UTF-8, strictly: an overlong form, a surrogate code point, a code point over U+10FFFF, a stray or missing
//     continuation byte are refused; a `\u` pair is joined, a lone surrogate refused. A Value holding invalid UTF-8 FAILS
//     to write, naming its path.
//   * A control character and `\n` are always written escaped: a written text never holds a newline (one message a line).
//   * An integer literal reads as an Int (outside int64: refused); a literal with a fraction or an exponent as a Number
//     (past a double's range, either way: refused - an UNDERFLOW too: "1e-400", whose nearest double is a zero, is
//     refused, never read as 0; a subnormal such as "4.9e-324" reads exactly). A Number is written with a fraction or an exponent ("1.0", "1e+300",
//     "-0.0"), the shortest text that reads back to the same bits - so its KIND survives the round trip.
//   * NaN and infinity are never written: the write FAILS naming the value's path (read by the number's bits).
//   * An object's members keep their order; a member named twice is refused.
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

#include "jahshaka/engine/Script.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace jahshaka { namespace engine { namespace script {

/// The most a Bytes value carries in JSON (decoded), and the deepest nesting of arrays and objects, either way.
constexpr std::size_t kJsonBytesCap = std::size_t(1) << 20;
constexpr int kJsonMaxDepth = 64;

/// `value` as one line of compact JSON into `out`; false with `why` (naming the value's path) when it cannot be carried.
bool toJson(const Value &value, std::string &out, std::string &why);
/// The one JSON value of `text` into `out`; false with `why` ("line L, column C: ...") when the text is refused.
bool parseJson(std::string_view text, Value &out, std::string &why);

}}}  // namespace jahshaka::engine::script
