#pragma once
// THE ENGINE'S SCRIPT HOST (SCRIPT-IN-ENGINE-1 stage 1; SPECS/SCRIPT_IN_ENGINE_1_SPEC.md) — a value, a command registry,
// and the host that runs a script against it. The standard library only: no Qt, no Ogre, and NO SCRIPT LANGUAGE — the
// value and the registry know nothing of JavaScript (source.script_neutral holds it); a language binding is one private
// file of the engine that converts its own values to these and calls Registry::call. A second language is one more
// such file.
//
// A COMMAND is a name under a root ("engine.scene.create"), a SIGNATURE (its parameters, each with a kind, and its
// return's kind) and a DOC (the help text: the generated documentation later), and a function `Value fn(Call &)`.
// Registry::call checks the arguments against the signature BEFORE the function runs; the function sees them checked,
// each default filled, each object's members in the declared order.
//
// THIS REGISTRY IS STRICT (the lead's ruling, 2026-11-22: its first users are tests, and a test must fail on its own
// mistake). A call FAILS — Result::Status::Failed, a thrown error in a script — when:
//   * the name is not a registered command;
//   * there are more arguments than parameters, or an object carries a member its parameter does not declare;
//   * an argument is missing (or null) and its parameter declares no default;
//   * an argument is not of its parameter's kind — and a Number is an Int only when it is integral (3.0 is 3; 3.7 FAILS,
//     it is never truncated);
//   * the command itself calls Call::fail.
// THE SHAPE OF A SIGNATURE (the lead's rule, 2026-11-22; Registry::validate enforces it): THE TARGETS ARE POSITIONAL —
// what the command acts on (the scene; then the node, view or material in it; a picture it reduces), declared with
// target() and first — AND EVERYTHING ELSE IS NAMED, in at most ONE further parameter (one scalar, or one options object
// of named members). The strict call catches a misspelt name; it cannot catch two swapped positional values.
//
// FAIL IS THE CALLER'S MISTAKE; REFUSE IS THE ENGINE'S STATE. A command that is known and correctly called but cannot act
// now (no node has that id, the engine did not take the value) returns Call::refuse(message): the call answers the falsy
// value of its declared return (false, 0, null — the script carries on) and the message is kept as lastError().

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace jahshaka { namespace engine { namespace script {

/// A value's kind. Int and Number are two kinds of one script number: a language with one number type converts an
/// integral number of magnitude <= 2^53 to Int and any other to Number. An Int parameter takes a Number only when it
/// is integral and of magnitude <= 2^53 (above it a script's number is no longer exact: a larger integer is an Int
/// value - a BigInt in JS).
enum class Kind { Null, Bool, Int, Number, String, Array, Object, Bytes };
const char *kindName(Kind);

/// One value: null, bool, 64-bit integer, double, UTF-8 string, array, ordered object (members in insertion order,
/// names unique) or bytes.
class Value {
public:
    using Array  = std::vector<Value>;
    using Member = std::pair<std::string, Value>;
    using Object = std::vector<Member>;
    using Bytes  = std::vector<std::uint8_t>;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : mV(b) {}
    template <class T, std::enable_if_t<std::is_integral<T>::value && !std::is_same<T, bool>::value, int> = 0>
    Value(T i) : mV(static_cast<std::int64_t>(i)) {}
    Value(double d) : mV(d) {}
    Value(const char *s) : mV(std::string(s)) {}
    Value(std::string s) : mV(std::move(s)) {}
    Value(Array a) : mV(std::move(a)) {}
    Value(Object o) : mV(std::move(o)) {}
    static Value bytes(Bytes b);

    Kind kind() const;
    bool isNull() const { return kind() == Kind::Null; }
    /// The value as its kind. Each assumes the kind (Registry::call has checked it): asNumber takes an Int too.
    bool                asBool() const { return std::get<bool>(mV); }
    std::int64_t        asInt() const { return std::get<std::int64_t>(mV); }
    double              asNumber() const;
    const std::string  &asString() const { return std::get<std::string>(mV); }
    const Array        &asArray() const { return std::get<Array>(mV); }
    const Object       &asObject() const { return std::get<Object>(mV); }
    const Bytes        &asBytes() const { return std::get<Bytes>(mV); }
    /// An object's member by name: null when this is not an object or has no such member.
    const Value *find(const std::string &name) const;
    /// The same, a null Value when absent.
    const Value &operator[](const std::string &name) const;

    /// Equal kinds and contents, except that an Int and a Number are equal when their numbers are (a script cannot
    /// tell them apart).
    bool operator==(const Value &o) const;
    bool operator!=(const Value &o) const { return !(*this == o); }

private:
    std::variant<std::monostate, bool, std::int64_t, double, std::string, Array, Object, Bytes> mV;
};

/// One parameter of a command (or one member of an object parameter).
struct Param {
    std::string name;
    Kind kind = Kind::Null;
    /// The default an absent or null argument takes. EMPTY = the argument is required.
    std::optional<Value> fallback;
    /// Kind::Object: its members, exactly (a member not declared here FAILS). Kind::Array of objects: each element's.
    std::vector<Param> fields;
    /// Kind::Array: its elements' kind (Kind::Object: each is checked against `fields`) and their count (0 = any).
    Kind element = Kind::Number;
    unsigned count = 0;
    /// A TARGET: what the command acts on (a positional parameter; the shape rule above).
    bool target = false;
    /// A HANDLE (Kind::Int): the kind of object it must name ("scene", "node", "mesh"); empty = a plain number. The
    /// registry checks the handle's owner and kind at the call (Registry, THE HANDLES).
    std::string handle;
    /// THE DECLARED DOMAIN (the lead's S2): checked by the host at the call and printed by signatureText - so the strict
    /// host enforces it and the generated documentation shows it, and a command's body never re-checks it.
    std::vector<std::string> choices;   ///< Kind::String: the only values allowed (empty: any)
    std::optional<double> min, max;     ///< Kind::Int / Number, an array's elements too: the closed range
    std::string unit;                   ///< a number's unit ("px", "m", "frames"): documentation
    /// Kind::Number: NaN and infinity are FAILED (read by their bits) unless this says the parameter takes them.
    bool nonFinite = false;
    Param oneOf(std::vector<std::string> values) const;
    Param range(double lo, double hi) const;
    Param atLeast(double lo) const;
    Param in(std::string unitName) const;
    Param allowNonFinite() const;
};
/// A required parameter; one with its default.
Param arg(std::string name, Kind kind);
/// A TARGET (required): a handle (Kind::Int) or the value the command reduces (a picture object: target(object(...))).
Param target(std::string name, Kind kind);
Param target(Param p);
Param arg(std::string name, Kind kind, Value fallback);
/// An array of exactly `count` numbers (a vector, a colour, a quaternion), required or with its default.
Param numbers(std::string name, unsigned count);
Param numbers(std::string name, unsigned count, Value fallback);
/// An object of exactly these members (each member required unless it declares its default), or an array of them.
Param object(std::string name, std::vector<Param> fields);
Param object(std::string name, std::vector<Param> fields, Value fallback);
Param objects(std::string name, std::vector<Param> fields);
/// A required handle of `kind` (target(handle(...)) when it is what the command acts on).
Param handle(std::string name, std::string kind);

/// A command's signature: its parameters and its answer. (A struct of named fields, extended by a field: stage 2 adds
/// the command's ACCESS tag here - MCP-ON-HOST-1's "every command declares its access" - and validate() requires it.)
struct Signature {
    std::vector<Param> params;
    Kind returns = Kind::Null;
    /// The kind of handle the command answers ("-> scene handle"); empty = none.
    std::string returnsHandle;
};

struct CommandInfo {
    std::string name;        ///< "engine.scene.create": its root and path, dotted
    Signature   signature;
    std::string doc;         ///< one or two sentences: the help, and the generated documentation
};
/// "engine.frames(n: int = 1) -> null" — the signature as text (the help's line).
std::string signatureText(const CommandInfo &);

enum class Status { Ok, Failed, Refused };

class Registry;

/// One call in progress: its checked arguments and its outcome.
class Call {
public:
    Call(const CommandInfo &info, Value::Array args, Registry &registry)
        : mInfo(info), mArgs(std::move(args)), mRegistry(registry) {}
    const CommandInfo &info() const { return mInfo; }
    /// The i-th argument, checked, its default filled.
    const Value &operator[](std::size_t i) const { return mArgs[i]; }
    /// THE CALLER'S MISTAKE: the call fails with `message` (a thrown error in a script). Returns null to return.
    Value fail(std::string message);
    /// THE ENGINE'S STATE: the call answers its declared return's falsy value; `message` becomes lastError().
    Value refuse(std::string message);
    Status status() const { return mStatus; }
    const std::string &message() const { return mMessage; }
    /// THE HANDLES (Registry): a new held object's handle; a new handle naming the engine's own 32-bit id `id` (the
    /// call FAILS, null, when `id` does not fit); the object a checked held handle names; the id a checked id handle
    /// carries.
    Value hold(const std::string &kind, std::shared_ptr<void> object, bool scoped = false);
    Value name(const std::string &kind, unsigned long long id);
    template <class T> T *object(const Value &h) const { return static_cast<T *>(heldObject(h)); }
    static unsigned idOf(const Value &h) { return unsigned(std::uint64_t(h.asInt()) & 0xffffffffull); }
    Registry &registry() { return mRegistry; }

private:
    void *heldObject(const Value &h) const;
    const CommandInfo &mInfo;
    Value::Array mArgs;
    Registry &mRegistry;
    Status mStatus = Status::Ok;
    std::string mMessage;
};
using CommandFn = std::function<Value(Call &)>;

struct Result {
    Status      status = Status::Ok;
    Value       value;      ///< Ok: the answer; Refused: the declared return's falsy value; Failed: null
    std::string message;    ///< Failed / Refused: why, beginning with the command's name
};

/// The commands, by name, in registration order. THE REGISTRY ENFORCES ITSELF (the final read's W4): a command that
/// breaks validate's rules, or a second command of one name, is never added - add() answers false AND keeps the fault,
/// and a Host whose registry holds a fault refuses every run by name (a refusal can never be ignored); a C++ exception
/// a command throws is caught at the call and is a Failed call naming it (it never unwinds through a language's C).
///
/// THE HANDLES (the final read's W1; the lead's S1): a handle is ONE script number of 52 bits - its OWNER (the
/// registry's process-wide serial, 12 bits) << 40 | its KIND (interned by the registry, 8 bits) << 32 | its VALUE (32
/// bits). A HELD kind (scene, view, picture: objects the host holds) has a slot of the registry's table as its value -
/// released, the handle is STALE; a NAMED kind (node, mesh, material: the engine's own ids) has the engine's id as its
/// value and no table (existence is its family's own record). A parameter declared handle(name, kind) is checked at
/// the call, before the command runs: a number that is no handle of this registry (another host's or engine's), a
/// handle of another kind and a stale one each FAIL by name. No allocation and no table lookup for a named kind.
class Registry {
public:
    Registry();
    /// False (nothing added, the fault kept: validate()) when the name is already registered or the command breaks
    /// validate's rules.
    bool add(CommandInfo info, CommandFn fn);
    const CommandInfo *find(const std::string &name) const;
    std::vector<const CommandInfo *> commands() const;
    /// The call by name: the arguments checked against the signature (the laws above), then the command run (an
    /// exception it throws: Failed, "<name>: threw: <what>").
    Result call(const std::string &name, const Value::Array &args);
    /// The same call by the command's INDEX (its place in commands(), fixed at registration): a language binding
    /// resolves each command ONCE and calls it by index - no lookup by name a call.
    Result callAt(std::size_t index, const Value::Array &args);
    /// The last refusal's message ("" before any).
    const std::string &lastError() const { return mLastError; }
    /// One line per add() refused: a command that lacks a name, a doc, a function or a name on any parameter or
    /// member, breaks the shape rule (a target after a non-target, or more than one non-target parameter), or repeats
    /// a name. Empty when every command offered was documented, shaped and new.
    std::vector<std::string> validate() const { return mFaults; }

    /// A held object's new handle; `scoped`: released at the end of the run that made it (releaseScoped).
    std::int64_t hold(const std::string &kind, std::shared_ptr<void> object, bool scoped = false);
    /// A handle of the NAMED kind `kind` carrying `id` (< 2^32; the caller has checked it).
    std::int64_t name(const std::string &kind, std::uint32_t id);
    /// Releases a held handle: false when it names no live held object of this registry.
    bool release(std::int64_t handle);
    /// Releases every scoped handle (Host::run calls it at a run's end).
    void releaseScoped();
    /// The object of a live held handle; null otherwise.
    void *object(std::int64_t handle) const;
    /// True when `h` is a live handle of this registry of kind `kind`; else false with `why` (naming `where`).
    bool checkHandle(std::int64_t h, const std::string &kind, const std::string &where, std::string &why) const;
    /// THE EXISTENCE CHECK of a NAMED kind (its family's: the engine asked whether the id is its scene's now), run at
    /// every call on every argument declared a handle of `kind`, after the arguments are checked and before the
    /// command: false with `why` FAILS the call by name. No check = the kind's ids are taken as they come.
    using HandleCheck = std::function<bool(const Call &call, std::uint32_t id, std::string &why)>;
    void checkKind(const std::string &kind, HandleCheck check);

private:
    struct Entry { CommandInfo info; CommandFn fn; };
    std::vector<Entry> mEntries;
    std::vector<std::string> mFaults;
    std::string mLastError;
    struct Held { unsigned kind = 0; std::shared_ptr<void> object; bool scoped = false; };
    unsigned kindIndex(const std::string &kind);   // interned, 1..255 (0: the 256th kind, refused)
    bool judgeHandles(const Call &call, const Param &p, const Value &v, std::string &why) const;
    std::uint64_t mSerial = 0;
    std::vector<std::string> mKinds;   // index - 1
    std::vector<bool> mKindHeld;       // index - 1: a held kind (a table slot) or a named one (the engine's id)
    std::vector<HandleCheck> mChecks;  // index - 1
    std::vector<Held> mHeld;           // slot - 1
};

/// THE HOST — one per process (the process makes it; every Engine registers its commands into its registry, so a
/// process whose engine failed to start still has a host that can say why). It runs a script on the CALLING thread and
/// every command inline; a host whose scripts run on another thread than the engine's (Studio, later) installs an
/// EXECUTOR that carries each command call to the engine's thread. Its own commands: `host.check({ok, what})` (one
/// law: Host::law) and `host.lastError()` (the registry's last refusal).
class Host {
public:
    using ConsoleSink = std::function<void(const std::string &line)>;
    using Executor = std::function<void(const std::function<void()> &call)>;
    /// THE HOST's OPTIONS, fixed at its construction (no environment variable, no script command changes them): each
    /// caller - jahengine, the MCP adapter - passes its own.
    struct Options {
        /// The bytes the live pictures may hold (engine.picture.*: a picture past it FAILS by name) - 512 MiB: fifteen
        /// 4K RGBA8 pictures, or three of PICTURE-REQUEST-1's 4K float radiance.
        std::size_t pictureBytesCap = std::size_t(512) << 20;
    };
    Host();   // the default options
    explicit Host(Options options);
    const Options &options() const { return mOptions; }
    ~Host();
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;

    Registry &registry() { return mRegistry; }
    /// Where the script's console lines and host.check's lines go (default: stdout, flushed a line).
    void setConsole(ConsoleSink sink);
    /// Where a command call runs (default: inline, on the script's thread).
    void setExecutor(Executor executor);
    /// A SCRIPT LANGUAGE (the lead's S5): one small interface, each language's binding ONE file of the engine that
    /// converts its values to Value, installs the registry's commands as its functions and calls Host::call. The
    /// built-in languages are THE LANGUAGE TABLE (src/script/LanguageTable.inc: an extension and a maker a line);
    /// addLanguage adds another (a test's, an embedder's).
    class Language {
    public:
        virtual ~Language() = default;
        /// Runs `source` to its end (`name` its file name in errors): Result as Host::run says.
        virtual Result run(const std::string &source, const std::string &name) = 0;
    };
    using LanguageMaker = std::function<std::unique_ptr<Language>(Host &host)>;
    /// The language that reads scripts named `*<extension>` (".js"): made the first time a script needs it.
    void addLanguage(std::string extension, LanguageMaker make);

    /// Runs the script `source` in THE LANGUAGE OF ITS NAME's EXTENSION (`name` its file name in errors; no language
    /// for it: Failed, naming the extensions this host reads), TO ITS END - in JavaScript: the script, then every job it
    /// queued (the code after an `await`, a promise's reactions) until none is left. Failed carries an uncaught error's
    /// text and its stack (the script's line), or a rejected promise no handler took ("an unhandled promise rejection:
    /// ..."); a command's Failed is a thrown error the script may catch.
    Result run(const std::string &source, const std::string &name);
    /// The host.check calls that ran, and those that failed, over every run.
    int checksRun() const { return mChecksRun; }
    int checksFailed() const { return mChecksFailed; }
    /// THE VERDICT of a test script whose last run answered `run` (jahengine's): the faults, each printed as a
    /// `FAIL: <why>` line through the console — the run stopped (its message), every failed check, and A SCRIPT THAT
    /// CHECKED NOTHING (no host.check ran: a row that measured nothing is never a silent green). Answers their count;
    /// 0 = PASSED.
    int verdict(const Result &run);
    /// ONE LAW JUDGED: printed `ok:   <what>` or `FAIL: <what>` and counted as a check that ran (and failed) — what
    /// host.check does, and what a command that judges a law the script declared calls (engine.view.declareRoutes'
    /// judgement: a FAIL here fails the verdict whatever the script does with the command's error). Answers `ok`.
    bool law(bool ok, const std::string &what);
    /// A judgement the verdict runs before it counts (a declaration no frame asserted is a failed law(), by name).
    void atVerdict(std::function<void()> judge);

    /// The language binding's doors: a command call by name or by its registry index (through the executor) and a
    /// console line.
    Result call(const std::string &name, const Value::Array &args);
    Result callAt(std::size_t index, const Value::Array &args);
    void print(const std::string &line);

private:
    Options mOptions;
    Registry mRegistry;
    ConsoleSink mConsole;
    Executor mExecutor;
    int mChecksRun = 0;
    int mChecksFailed = 0;
    std::vector<std::function<void()>> mJudges;
    struct LanguageEntry { std::string extension; LanguageMaker make; std::unique_ptr<Language> made; };
    std::vector<LanguageEntry> mLanguages;
};

}  // namespace script
class Engine;
namespace script {
/// THE ENGINE REGISTERS INTO THE HOST (src/script/commands_engine.cpp): its `engine.*` commands on `engine`, which the
/// calling thread created (its commands are called inline), and its judgements into the host's verdict (a route
/// declaration no frame asserted). A null `engine` (it did not start) still registers them: each FAILS with "the engine
/// did not start: <bootError>". `testDoors`: the engine's TEST arms (ArmInfo::test) open (jahengine --test-doors);
/// without it engine.arm FAILS on one.
void registerEngineCommands(Host &host, Engine *engine, std::string bootError, bool testDoors);

}}}  // namespace jahshaka::engine::script
