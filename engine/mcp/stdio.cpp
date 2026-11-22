// THE STDIO TRANSPORT (MCP-ON-HOST-1 stage 2): mcp/include/mcp/stdio.h says the laws.
#include "mcp/stdio.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <unistd.h>

namespace jahshaka { namespace engine { namespace mcp {

int claimProtocolFd(std::string &why)
{
    std::fflush(nullptr);   // what stdio holds for fd 1 is written before fd 1 changes
    const int proto = ::dup(1);
    if (proto < 0 || ::dup2(2, 1) < 0) {
        why = std::string("the protocol fd could not be claimed: ") + std::strerror(errno);
        return -1;
    }
    return proto;
}

bool LineReader::read(std::vector<Incoming> &out, bool wait)
{
    if (mEnded) return false;
    pollfd p{mFd, POLLIN, 0};
    int ready;
    do ready = ::poll(&p, 1, wait ? -1 : 0);
    while (ready < 0 && errno == EINTR);
    if (ready <= 0) return true;   // nothing yet (or a poll error: the next read says)
    char buf[65536];
    ssize_t n;
    do n = ::read(mFd, buf, sizeof buf);
    while (n < 0 && errno == EINTR);
    if (n <= 0) {   // the end (or a read error: the client is gone)
        mEnded = true;
        if (mDiscarding) out.push_back({std::string(), true});
        else if (!mLine.empty()) out.push_back({std::move(mLine), false});
        mLine.clear();
        return false;
    }
    for (const char *at = buf, *end = buf + n; at < end;) {
        const char *nl = static_cast<const char *>(std::memchr(at, '\n', std::size_t(end - at)));
        const char *stop = nl ? nl : end;
        if (!mDiscarding) {
            const std::size_t take = std::size_t(stop - at);
            if (mLine.size() + take > kLineCap) {   // past the cap: the line is dropped, the rest read and discarded
                mDiscarding = true;
                std::string().swap(mLine);
            } else {
                mLine.append(at, take);
                mMax = std::max(mMax, mLine.size());
            }
        }
        if (nl) {
            if (mDiscarding) out.push_back({std::string(), true});
            else out.push_back({std::move(mLine), false});
            mLine.clear();
            mDiscarding = false;
        }
        at = nl ? nl + 1 : end;
    }
    return true;
}

void serveStdio(int inFd, int protoFd, Session::Doors doors)
{
    LineReader reader(inFd);
    doors.send = [protoFd](const std::string &line) {
        std::string framed = line + "\n";
        for (std::size_t at = 0; at < framed.size();) {
            const ssize_t w = ::write(protoFd, framed.data() + at, framed.size() - at);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return;   // the client is gone: its end comes as stdin's
            at += std::size_t(w);
        }
    };
    doors.log = [](const std::string &line) { std::fprintf(stderr, "%s\n", line.c_str()); };
    doors.poll = [&reader](std::vector<Incoming> &arrived) { return reader.read(arrived, false); };
    Session session(std::move(doors));
    for (bool more = true; more && !session.ended();) {
        std::vector<Incoming> lines;
        more = reader.read(lines, true);
        for (Incoming &in : lines) {
            if (session.ended()) break;
            session.receive(std::move(in));
        }
    }
    session.end();
}

}}}  // namespace jahshaka::engine::mcp
