#pragma once
// THE STDIO TRANSPORT (MCP-ON-HOST-1 stage 2; SPECS/MCP_ON_HOST_1_SPEC.md L1, L8, L13): newline-delimited JSON-RPC on
// stdin / stdout, NOTHING but protocol lines on the protocol fd, logging on stderr. POSIX only (jahengine's platform).

#include "mcp/session.h"

#include <cstddef>
#include <string>
#include <vector>

namespace jahshaka { namespace engine { namespace mcp {

/// THE PROTOCOL's FD (L1): a duplicate of fd 1 kept for the protocol, then fd 1 pointed at stderr - called BEFORE any
/// engine starts, so nothing else in the process (Ogre's log echo to std::cout, a printf) can write a line the client
/// reads. Answers the protocol fd, or -1 with `why`.
int claimProtocolFd(std::string &why);

/// THE LINE READER: whole lines from `fd`, at most kLineCap bytes of one KEPT - a line past the cap is read to its end
/// and discarded AS IT IS READ (never buffered: a 10 GB line costs kLineCap) and delivered `overlong`.
class LineReader {
public:
    explicit LineReader(int fd) : mFd(fd) {}
    /// Reads what the fd holds - `wait`: blocking for the first byte; else a zero-timeout poll - and appends the whole
    /// lines; false once the fd reached its end (a last line with no newline is delivered first).
    bool read(std::vector<Incoming> &out, bool wait);
    /// The most bytes of one line it ever held (its own counter: never above kLineCap).
    std::size_t maxBuffered() const { return mMax; }

private:
    int mFd;
    std::string mLine;
    bool mDiscarding = false, mEnded = false;
    std::size_t mMax = 0;
};

/// THE SERVER: a Session over `inFd` (the client's lines) and `protoFd` (one line each reply), its log lines and every
/// console line on stderr, until `inFd` ends; then Session::end() (L13). `doors` gives makeHost and version: send, log
/// and poll are this transport's.
void serveStdio(int inFd, int protoFd, Session::Doors doors);

}}}  // namespace jahshaka::engine::mcp
