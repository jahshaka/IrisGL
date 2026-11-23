#pragma once
// THE MCP SESSION (MCP-ON-HOST-1 stage 2; SPECS/MCP_ON_HOST_1_SPEC.md L1-L13): the protocol (revision 2026-07-28 only) and
// its five tools over the engine's script host, TRANSPORT-FREE - a line in, lines out through `send`, and a `poll` the
// session asks for the lines that arrived while a request runs (a cancel of the running request, more requests to
// queue, or stdin's end). The adapter KNOWS NO COMMAND: arguments by name go to Registry::callNamed, a command's schema
// is generated from its signature (mcp/schema.h); the one command named here is the picture tool's (stage 3).
//
// THE HOSTS (L11): the session asks `makeHost` for a DOCUMENTATION-ONLY host first (server/discover, tools/list and
// api_docs need no engine); the FIRST tools/call of call, read, run_script or picture asks it for the ONE engine host
// (the engine boots there), and the docs host is destroyed. No handle exists before that host. The engine host runs a
// FRESH realm per run_script, answers its completion value within kCompletionBounds, holds its script heap to
// kHeapLimit and serves an OUTSIDE client (Host::Options::outsideClient: what is engine-wide, engine.arm, is refused).
//
// Requests are served strictly in order, one at a time. A cancel of the running request stops it at its next keep-going
// poll (Host::keepGoing) and NOTHING more is written for its id; a cancel of a queued one drops it unanswered; stdin's
// end is a cancel, then the session ends.

#include "jahshaka/engine/Script.h"
#include "core/json.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jahshaka { namespace engine { namespace mcp {

/// The one protocol revision this server speaks (forward-only).
constexpr const char *kRevision = "2026-07-28";

/// THE LONGEST LINE THE SERVER READS, checked BEFORE a byte past it is kept (over: the rest of the line is discarded
/// and the reply is -32700 with no id; the session lives on). DERIVED from the codec's bytes cap, so the two cannot
/// drift: the largest legitimate request carries ONE `{"$bytes": ...}` argument at its cap - its base64 text - and
/// everything else around it (a run_script source, the other arguments, the envelope) is given twice that again.
constexpr std::size_t kBytesBase64 = (iris::json::kBytesCap + 2) / 3 * 4;   // 1,398,104
constexpr std::size_t kLineCap = 3 * kBytesBase64;                          // 4,194,312 (~4 MiB)

/// THE MCP HOST's COMPLETION BOUNDS (run_script's answer; Host::Options::completionBounds), each with its reason:
///  - bytes 256 KiB of converted value: the client cuts a tool result over ~25,000 tokens (~100 KB of text), so this
///    never refuses a result an AI could read, and refuses by name one it could not;
///  - elements 16,384: a 128 x 128 table - an array past it is far past the client's cut;
///  - depth 32: the reply's envelope adds its own levels (kEnvelopeLevels) under the codec's 64.
constexpr script::ValueBounds kCompletionBounds{16384, 32, std::size_t(256) << 10};
/// THE MCP HOST's HEAP LIMIT (Host::Options::memoryLimit - STATED, never the derived 2 x callBounds.bytes + 16 MiB =
/// 1168 MiB a local host takes): 32 MiB, about 2.5x the largest script working set measured (the full 65,536-transform
/// setTransforms batch built as JS numbers, 12.6 MiB, beside the interpreter's 0.11 MiB: 2b's D1b measure). It also
/// bounds the one cost no cancel reaches inside the vendored JS engine - a Proxy's own-key checks, n log n through
/// the engine's patch 0001 (irisgl/thirdparty, its patch stack): ~430,000 keys fit 32 MiB, listed in ~0.1 s.
constexpr std::size_t kHeapLimit = std::size_t(32) << 20;
/// What one call's console capture keeps (the `console` array of call / read / run_script): the rest goes to stderr
/// only, the array ending in a line that says how many were left out.
constexpr std::size_t kConsoleCap = std::size_t(128) << 10;
/// THE REPLY FITS THE LINE CAP (checked here): a string's worst JSON expansion is 6x (a control character as \uXXXX),
/// for the completion value and the console both, plus the envelope's own text; and the value's nesting plus the
/// envelope's levels stays under the codec's 64.
constexpr std::size_t kEnvelopeBytes = std::size_t(64) << 10;
constexpr int kEnvelopeLevels = 4;   // {"result": {"structuredContent": {"result": <value>}}}
static_assert(kCompletionBounds.bytes * 6 + kConsoleCap * 6 + kEnvelopeBytes < kLineCap,
              "a run_script reply at its bounds must fit one line under the cap");
static_assert(kCompletionBounds.depth + kEnvelopeLevels < iris::json::kMaxDepth,
              "a completion value at its depth must nest under the codec's 64 inside its reply");

/// One line from the client: its text, or `overlong` (its first kLineCap bytes were read and the rest discarded).
struct Incoming {
    std::string line;
    bool overlong = false;
};

class Session {
public:
    struct Doors {
        /// A host with every command registered, made with `options` (the session states them: documentationOnly for
        /// the docs host; a fresh realm and the completion bounds for the engine host - whose making boots the engine).
        std::function<std::unique_ptr<script::Host>(const script::Host::Options &options, bool engine)> makeHost;
        /// One protocol line out (no newline: the transport frames it).
        std::function<void(const std::string &line)> send;
        /// A line for stderr (a cancel, a console line, a failed boot).
        std::function<void(const std::string &line)> log;
        /// The lines that arrived since the last ask, without waiting; false = stdin ended.
        std::function<bool(std::vector<Incoming> &arrived)> poll;
        /// serverInfo's version.
        std::string version;
    };
    explicit Session(Doors doors);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    /// One line from the client, served now - with every line that queues behind it while it runs.
    void receive(Incoming in);
    /// stdin ended: no further request is served; the engine host's scoped handles are released (L13).
    void end();
    /// True once stdin ended (end(), or a poll that answered false).
    bool ended() const { return mEnded; }
    /// The engine host, once a tools/call made it (null before).
    script::Host *engineHost() const { return mEngine.get(); }

private:
    void serve(const Incoming &in);
    void request(const script::Value &id, const std::string &method, const script::Value &params);
    script::Value toolCall(const std::string &tool, const script::Value &args, bool &isError);
    script::Value apiDocs(const script::Value &args, bool &isError);
    script::Host *host(bool engine);
    bool keepGoing(double done, double total);
    void cancelled(const script::Value &requestId);
    void reply(const script::Value &id, script::Value result);
    void error(const script::Value *id, int code, const std::string &message, script::Value data = script::Value());
    void write(script::Value message);

    Doors mDoors;
    std::unique_ptr<script::Host> mDocs, mEngine;
    std::deque<Incoming> mQueue;
    bool mServing = false, mEnded = false;
    // the running request: its id, its cancel, its progress token and the last progress sent
    const script::Value *mRunning = nullptr;
    bool mCancelled = false;
    script::Value mProgressToken;
    double mProgressSent = 0;
    // the running call's console capture
    std::vector<std::string> mConsole;
    std::size_t mConsoleBytes = 0, mConsoleLeftOut = 0;
};

}}}  // namespace jahshaka::engine::mcp
