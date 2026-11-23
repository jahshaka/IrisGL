// iris::log (core/log.h says the laws).
#include "core/log.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

namespace iris::log {

namespace {

// The log's state, ONE a process (IrisCore is one shared library: source.no_qt_below T4). Never destroyed: a thread
// that logs during static destruction must still find its mutex.
struct State
{
    std::mutex mutex;   // not recursive: a sink that logs deadlocks (log.h)
    Sink sink;
};

State &state()
{
    static State *const s = new State();
    return *s;
}

void emit(Severity severity, const char *prefix, std::string_view text)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.sink) {
        s.sink(severity, text);
        return;
    }
    std::string line(prefix);
    line.append(text.data(), text.size());
    line.push_back('\n');
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

}   // namespace

void setSink(Sink sink)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.sink = std::move(sink);
}

void info(std::string_view text)  { emit(Severity::Info, "[info] ", text); }
void warn(std::string_view text)  { emit(Severity::Warning, "[warn] ", text); }
void error(std::string_view text) { emit(Severity::Error, "[error] ", text); }

}   // namespace iris::log
