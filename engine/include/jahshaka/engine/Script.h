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
/// integral number of magnitude <= 2^53 to Int and any other to Number.
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

struct Signature {
    std::vector<Param> params;
    Kind returns = Kind::Null;
};

struct CommandInfo {
    std::string name;        ///< "engine.scene.create": its root and path, dotted
    Signature   signature;
    std::string doc;         ///< one or two sentences: the help, and the generated documentation
};
/// "engine.frames(n: int = 1) -> null" — the signature as text (the help's line).
std::string signatureText(const CommandInfo &);

enum class Status { Ok, Failed, Refused };

/// One call in progress: its checked arguments and its outcome.
class Call {
public:
    Call(const CommandInfo &info, Value::Array args) : mInfo(info), mArgs(std::move(args)) {}
    const CommandInfo &info() const { return mInfo; }
    /// The i-th argument, checked, its default filled.
    const Value &operator[](std::size_t i) const { return mArgs[i]; }
    /// THE CALLER'S MISTAKE: the call fails with `message` (a thrown error in a script). Returns null to return.
    Value fail(std::string message);
    /// THE ENGINE'S STATE: the call answers its declared return's falsy value; `message` becomes lastError().
    Value refuse(std::string message);
    Status status() const { return mStatus; }
    const std::string &message() const { return mMessage; }

private:
    const CommandInfo &mInfo;
    Value::Array mArgs;
    Status mStatus = Status::Ok;
    std::string mMessage;
};
using CommandFn = std::function<Value(Call &)>;

struct Result {
    Status      status = Status::Ok;
    Value       value;      ///< Ok: the answer; Refused: the declared return's falsy value; Failed: null
    std::string message;    ///< Failed / Refused: why, beginning with the command's name
};

/// The commands, by name, in registration order.
class Registry {
public:
    /// False (and nothing added) when the name is already registered.
    bool add(CommandInfo info, CommandFn fn);
    const CommandInfo *find(const std::string &name) const;
    std::vector<const CommandInfo *> commands() const;
    /// The call by name: the arguments checked against the signature (the laws above), then the command run.
    Result call(const std::string &name, const Value::Array &args);
    /// The last refusal's message ("" before any).
    const std::string &lastError() const { return mLastError; }
    /// One line per command that lacks a name, a doc, or a name on any parameter or member, or breaks the shape rule (a
    /// target after a non-target, or more than one non-target parameter): empty when every command is documented and
    /// shaped.
    std::vector<std::string> validate() const;

private:
    struct Entry { CommandInfo info; CommandFn fn; };
    std::vector<Entry> mEntries;
    std::string mLastError;
};

/// THE HOST — one per process (the process makes it; every Engine registers its commands into its registry, so a
/// process whose engine failed to start still has a host that can say why). It runs a script on the CALLING thread and
/// every command inline; a host whose scripts run on another thread than the engine's (Studio, later) installs an
/// EXECUTOR that carries each command call to the engine's thread. Its own commands: `host.check(ok, what)` (prints
/// `ok:   <what>` or `FAIL: <what>`, counting the failures) and `host.lastError()` (the registry's last refusal).
class Host {
public:
    using ConsoleSink = std::function<void(const std::string &line)>;
    using Executor = std::function<void(const std::function<void()> &call)>;
    Host();
    ~Host();
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;

    Registry &registry() { return mRegistry; }
    /// Where the script's console lines and host.check's lines go (default: stdout, flushed a line).
    void setConsole(ConsoleSink sink);
    /// Where a command call runs (default: inline, on the script's thread).
    void setExecutor(Executor executor);
    /// Runs JavaScript `source`, `name` its file name in errors, TO ITS END: the script, then every job it queued (the
    /// code after an `await`, a promise's reactions) until none is left. Failed carries an uncaught error's text and
    /// its stack (the script's line), or a rejected promise no handler took ("an unhandled promise rejection: ...");
    /// a command's Failed is a thrown error the script may catch.
    Result run(const std::string &source, const std::string &name);
    /// The host.check calls that ran, and those that failed, over every run.
    int checksRun() const { return mChecksRun; }
    int checksFailed() const { return mChecksFailed; }
    /// THE VERDICT of a test script whose last run answered `run` (jahengine's): the faults, each printed as a
    /// `FAIL: <why>` line through the console — the run stopped (its message), every failed check, and A SCRIPT THAT
    /// CHECKED NOTHING (no host.check ran: a row that measured nothing is never a silent green). Answers their count;
    /// 0 = PASSED.
    int verdict(const Result &run);

    /// The language binding's two doors: a command call by name (through the executor) and a console line.
    Result call(const std::string &name, const Value::Array &args);
    void print(const std::string &line);

    class Language;   // a script language's binding (private to the engine)

private:
    Registry mRegistry;
    ConsoleSink mConsole;
    Executor mExecutor;
    int mChecksRun = 0;
    int mChecksFailed = 0;
    std::unique_ptr<Language> mJs;
};

}  // namespace script
class Engine;
namespace script {
/// THE ENGINE REGISTERS INTO THE HOST (src/script/commands_engine.cpp): its `engine.*` commands on `engine`, which the
/// calling thread created (its commands are called inline). A null `engine` (it did not start) still registers them:
/// each FAILS with "the engine did not start: <bootError>". `testDoors`: the engine's TEST arms (ArmInfo::test) open
/// (jahengine --test-doors); without it engine.arm FAILS on one.
void registerEngineCommands(Registry &registry, Engine *engine, std::string bootError, bool testDoors);

}}}  // namespace jahshaka::engine::script
