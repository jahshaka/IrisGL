// THE HOST (SCRIPT-IN-ENGINE-1 stage 1; jahshaka/engine/Script.h): its registry, its console, its languages and its own
// two commands. It knows the language only through Language.h (source.script_neutral).
#include "script/Language.h"

#include <cstdio>

namespace jahshaka { namespace engine { namespace script {

Host::Host() : Host(Options()) {}

Host::Host(Options options) : mOptions(options)
{
    // THE LANGUAGE TABLE (LanguageTable.inc): one addLanguage a line
#define JAH_SCRIPT_LANGUAGE(extension, maker) addLanguage(extension, &detail::maker);
#include "script/LanguageTable.inc"
#undef JAH_SCRIPT_LANGUAGE
    mRegistry.add({"host.check", {Access::Reads,
                                  {object("law", {arg("ok", Kind::Bool), arg("what", Kind::String)})}, Kind::Bool},
                   "One law of a test script, {ok, what}: prints `ok:   <what>` or `FAIL: <what>` and counts a failure "
                   "(the host's verdict). Answers `ok`."},
                  [this](Call &c) { return Value(law(c[0]["ok"].asBool(), c[0]["what"].asString())); });
    mRegistry.add({"host.lastError", {Access::Reads, {}, Kind::String},
                   "The last refusal's message: a command that was called correctly but could not act answers falsy "
                   "and says why here (\"\" before any)."},
                  [this](Call &) { return Value(mRegistry.lastError()); });
}

Host::~Host() = default;

void Host::addLanguage(std::string extension, LanguageMaker make)
{
    for (LanguageEntry &e : mLanguages)
        if (e.extension == extension) {
            e.make = std::move(make);
            e.made.reset();
            return;
        }
    mLanguages.push_back(LanguageEntry{std::move(extension), std::move(make), nullptr});
}

void Host::setConsole(ConsoleSink sink) { mConsole = std::move(sink); }

/// THE HOST STARTS ONLY WHOLE (the final read's W4): a command its registry refused is a named start failure - of a
/// run and of every call alike (MCP-ON-HOST-1: a call that went round the run's check served a host the run refuses).
static Result notWhole(const Registry &registry)
{
    const std::vector<std::string> faults = registry.validate();
    Result r;
    r.status = Status::Failed;
    r.message = "the host did not start: its registry refused what was offered (" + std::to_string(faults.size()) + " fault(s)):";
    for (const std::string &f : faults) r.message += "\n  " + f;
    return r;
}

/// THE DOORS' ONE CHECK: true (with the failure in `r`) when this host serves no run and no call - a documentation-only
/// host, or one not whole.
static bool shut(const Host::Options &options, const Registry &registry, Result &r)
{
    if (options.documentationOnly) {
        r.status = Status::Failed;
        r.message = "this host lists commands for documentation only";
        return true;
    }
    if (!registry.faulted()) return false;
    r = notWhole(registry);
    return true;
}

Result Host::run(const std::string &source, const std::string &name)
{
    if (Result r; shut(mOptions, mRegistry, r)) return r;
    // THE LANGUAGE OF THE NAME's EXTENSION (the table, LanguageTable.inc, and any addLanguage)
    const size_t dot = name.rfind('.');
    const std::string ext = dot == std::string::npos || name.find('/', dot) != std::string::npos ? "" : name.substr(dot);
    for (LanguageEntry &e : mLanguages) {
        if (e.extension != ext) continue;
        if (!e.made) e.made = e.make(*this);
        return e.made->run(source, name);
    }
    Result r;
    r.status = Status::Failed;
    r.message = "no script language reads '" + name + "' (this host reads:";
    for (const LanguageEntry &e : mLanguages) r.message += " *" + e.extension;
    r.message += ")";
    return r;
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
    if (Result r; shut(mOptions, mRegistry, r)) return r;
    return mRegistry.call(name, args);
}

Result Host::callNamed(const std::string &name, Value::Object args)
{
    if (Result r; shut(mOptions, mRegistry, r)) return r;
    return mRegistry.callNamed(name, std::move(args));
}

Result Host::callAt(std::size_t index, Value::Array args)
{
    if (Result r; shut(mOptions, mRegistry, r)) return r;
    return mRegistry.callAt(index, std::move(args));
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
