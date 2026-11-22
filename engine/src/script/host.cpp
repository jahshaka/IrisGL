// THE HOST (SCRIPT-IN-ENGINE-1 stage 1; jahshaka/engine/Script.h): its registry, its console, its executor and its own
// two commands. It knows the language only through Language.h (source.script_neutral).
#include "script/Language.h"

#include <cstdio>

namespace jahshaka { namespace engine { namespace script {

Host::Host()
{
    mRegistry.add({"host.check", {{object("law", {arg("ok", Kind::Bool), arg("what", Kind::String)})}, Kind::Bool},
                   "One law of a test script, {ok, what}: prints `ok:   <what>` or `FAIL: <what>` and counts a failure "
                   "(the host's verdict). Answers `ok`."},
                  [this](Call &c) { return Value(law(c[0]["ok"].asBool(), c[0]["what"].asString())); });
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
    // THE HOST STARTS ONLY WHOLE (the final read's W4): a command its registry refused is a named start failure
    if (const std::vector<std::string> faults = mRegistry.validate(); !faults.empty()) {
        Result r;
        r.status = Status::Failed;
        r.message = "the host did not start: its registry refused what was offered (" + std::to_string(faults.size()) + " fault(s)):";
        for (const std::string &f : faults) r.message += "\n  " + f;
        return r;
    }
    if (!mJs) mJs = detail::makeJavaScript(*this);
    return mJs->run(source, name);
}

bool Host::law(bool ok, const std::string &what)
{
    print((ok ? "ok:   " : "FAIL: ") + what);
    ++mChecksRun;
    if (!ok) ++mChecksFailed;
    return ok;
}

void Host::atVerdict(std::function<void()> judge) { mJudges.push_back(std::move(judge)); }

int Host::verdict(const Result &run)
{
    for (const auto &judge : mJudges) judge();
    int faults = mChecksFailed;
    if (run.status == Status::Failed) {
        print("FAIL: the script stopped: " + run.message);
        ++faults;
    }
    if (mChecksRun == 0) {
        print("FAIL: the script checked nothing (no host.check ran: a row that measured nothing is never a green)");
        ++faults;
    }
    return faults;
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
