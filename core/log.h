// iris::log - THE ONE LOG BELOW THE EDITOR (NOQT-CORE-1 stage 2; V2 plan 0.12), in place of the Qt logger (core/logger.h).
// Three levels, one host sink, no file. A record is one line of UTF-8 text; a line is never a RESULT (a tool that needs a
// fact reads a returned value, never a log line).
//
// THE LOCK. The log is written from every thread (the import worker, the bake pool, the UI thread) under ONE non-recursive
// mutex, and the installed sink is called UNDER it, one record at a time: the log serialises its records, so a sink needs
// no lock of its own to see each record whole. ONE LOCK ORDER: the log's mutex, then the sink's own.
//
// NO SINK: each record is written to stderr as ONE write of the whole line - "[info] " / "[warn] " / "[error] ", the text,
// a newline - and flushed. Nothing else: no file, no init, no flush call.
#pragma once

#include "core/export.h"

#include <functional>
#include <string_view>

namespace iris::log {

enum class Severity { Info, Warning, Error };

/// A host's sink: one record, its severity and its text (valid only for the call).
using Sink = std::function<void(Severity, std::string_view)>;

/// Installs `sink` in place of the one before it (an empty Sink: back to stderr). The sink is called UNDER the log's
/// mutex, one record at a time. A SINK MUST NEVER LOG: the mutex is not recursive, so a sink that calls info / warn /
/// error deadlocks. A slow sink blocks every logging thread (accepted: the one sink is the host's queueing write).
IRIS_CORE_API void setSink(Sink sink);

IRIS_CORE_API void info(std::string_view text);    ///< status, timing, a wait
IRIS_CORE_API void warn(std::string_view text);    ///< a failure
IRIS_CORE_API void error(std::string_view text);   ///< a failed or rolled-back operation on the user's data

}   // namespace iris::log
