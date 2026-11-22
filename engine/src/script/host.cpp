// THE HOST (SCRIPT-IN-ENGINE-1 stage 1; jahshaka/engine/Script.h): its registry, its console, its executor and its own
// two commands. It knows the language only through Language.h (source.script_neutral).
#include "script/Language.h"

#include <cstdio>

namespace jahshaka { namespace engine { namespace script {

Host::Host()
{
    mRegistry.add({"host.check", {{arg("ok", Kind::Bool), arg("what", Kind::String)}, Kind::Bool},
                   "One law of a test script: prints `ok:   <what>` or `FAIL: <what>` and counts a failure (the host's "
                   "verdict). Answers `ok`."},
                  [this](Call &c) {
                      const bool ok = c[0].asBool();
                      print((ok ? "ok:   " : "FAIL: ") + c[1].asString());
                      if (!ok) ++mChecksFailed;
                      return Value(ok);
                  });
    mRegistry.add({"host.lastError", {{}, Kind::String},
                   "The last refusal's message: a command that was called correctly but could not act answers falsy "
                   "and says why here (\"\" before any)."},
                  [this](Call &) { return Value(mRegistry.lastError()); });
}

Host::~Host() = default;

void Host::setConsole(ConsoleSink sink) { mConsole = std::move(sink); }

void Host::setExecutor(Executor executor) { mExecutor = std::move(executor); }

Result Host::run(const std::string &source, const std::string &name)
{
    if (!mJs) mJs = detail::makeJavaScript(*this);
    return mJs->run(source, name);
}

Result Host::call(const std::string &name, const Value::Array &args)
{
    if (!mExecutor) return mRegistry.call(name, args);
    Result r;
    mExecutor([&] { r = mRegistry.call(name, args); });
    return r;
}

void Host::print(const std::string &line)
{
    if (mConsole) {
        mConsole(line);
        return;
    }
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

}}}  // namespace jahshaka::engine::script
