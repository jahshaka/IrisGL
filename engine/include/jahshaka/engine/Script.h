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
#include <typeinfo>
#include <unordered_map>
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
    /// The same, to change or move from (the registry consumes the arguments it checks: S9).
    Array              &asArray() { return std::get<Array>(mV); }
    Object             &asObject() { return std::get<Object>(mV); }
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
    /// Kind::Array: the most elements it may hold, printed "node handle[] <= 65536"; more FAILS by name. An array
    /// declares a count or a maximum (Registry::add refuses one with neither): a binding judges a script's array length
    /// against it BEFORE it converts a single element.
    unsigned maxCount = 0;
    /// A TARGET: what the command acts on (a positional parameter; the shape rule above).
    bool target = false;
    /// A HANDLE (Kind::Int; or Kind::Array of them, handles()): the kind of object it must name ("scene", "node",
    /// "mesh"); empty = a plain number. The registry checks the handle's owner and kind at the call - each element's
    /// of an array (Registry, THE HANDLES).
    std::string handle;
    /// THE DECLARED DOMAIN (the lead's S2): checked by the host at the call and printed by signatureText - so the strict
    /// host enforces it and the generated documentation shows it, and a command's body never re-checks it.
    std::vector<std::string> choices;   ///< Kind::String: the only values allowed (empty: any)
    std::optional<double> min, max;     ///< Kind::Int / Number, an array's elements too: the closed range
    bool minExclusive = false;          ///< min itself is OUTSIDE (above(): "> 0")
    std::string unit;                   ///< a number's unit ("px", "m", "frames"): documentation
    /// Kind::Number: NaN and infinity are FAILED (read by their bits) unless this says the parameter takes them.
    bool nonFinite = false;
    /// Kind::Number (an array's elements too): SINGLE PRECISION - printed `float`, and a number past a float's range
    /// FAILS by name (it would narrow to an infinity, silently).
    bool single = false;
    /// Kind::Array: a FLAT array of items of `stride` numbers each (a Vec3 is 3): printed `float[3n] <= N numbers`
    /// (count and maxCount count NUMBERS), and a length that is no multiple of the stride FAILS by name. 0 = not flat.
    unsigned stride = 0;
    Param oneOf(std::vector<std::string> values) const;
    Param range(double lo, double hi) const;
    Param atLeast(double lo) const;
    Param above(double lo) const;
    Param upTo(unsigned elements) const;   ///< an array's maxCount
    Param in(std::string unitName) const;
    Param allowNonFinite() const;
    Param asFloat() const;                 ///< single precision
    Param flat(unsigned numbersEach) const;   ///< a flat array, `numbersEach` numbers an item
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
/// A required array of handles of `kind` ("nodes: node handle[]"): EVERY element is checked as a handle() is - its
/// owner and kind, and a named kind's existence asked of the engine - and a bad one FAILS naming it ("nodes[3]").
Param handles(std::string name, std::string kind);

/// THE BOUNDS OF ONE CONVERSION of a script's value into a Value (SCRIPT-IN-ENGINE-1 2b): a language binding has ONE
/// conversion, and every caller of it states these - a command's arguments (Host::Options::callBounds), a script's
/// completion value for an outside client (MCP-ON-HOST-1). Past any of them the conversion FAILS by name, and nothing
/// is read or reserved from a length the script stated before it is judged (a JS `a.length = 4e9` costs the script
/// nothing). A declared parameter's own count or maximum bounds its array as well (the smaller governs). NO DEFAULTS
/// TO LEAN ON: each is 0 until its caller states it (0 carries no element, no nesting, no byte).
struct ValueBounds {
    std::size_t elements = 0;   ///< the most elements one array may hold
    int depth = 0;              ///< the deepest nesting (the arguments themselves are depth 0)
    std::size_t bytes = 0;      ///< the most the converted value may hold: strings, bytes, elements, members
};

/// WHAT CALLING A COMMAND MAY DO (SCRIPT-IN-ENGINE-1 2a; MCP-ON-HOST-1's read-only surface): `Reads` changes nothing
/// the engine holds (a read, a reduction, an assertion); `Changes` changes the engine's state (an object made or
/// released, a value set, a frame drawn); `Files` reads or writes a file; `Test` is a test door (registered only behind
/// --test-doors). A command takes the HIGHEST any of its arguments reaches - Test > Files > Changes > Reads: a path
/// anywhere is Files. `Unset` is no tag: Registry::add refuses it.
enum class Access { Unset, Reads, Changes, Files, Test };
const char *accessName(Access);

/// A command's signature: its access, its parameters and its answer.
struct Signature {
    Access access = Access::Unset;
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
/// "engine.frames(n: int >= 0 frames) -> null [changes]" — the signature as text (the help's line), its access last.
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
    /// THE HANDLES (Registry): a new held object's handle (the call FAILS, null, by name: a kind bound to another C++
    /// type, a full table); a new handle naming the engine's own 32-bit id `id` (the call FAILS, null, when `id` does
    /// not fit); the T a checked held handle names - null when its kind holds another C++ type; the id a checked id
    /// handle carries.
    template <class T> Value hold(const std::string &kind, std::shared_ptr<T> object, bool scoped = false);
    Value name(const std::string &kind, unsigned long long id);
    template <class T> T *object(const Value &h) const;
    static unsigned idOf(const Value &h) { return unsigned(std::uint64_t(h.asInt()) & 0xffffffffull); }
    Registry &registry() { return mRegistry; }

private:
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
/// value and no table (existence is its family's own record). A parameter declared handle(name, kind) - or each
/// element of one declared handles(name, kind) - is checked at the call, before the command runs: a number that is no handle of this registry (another host's or engine's), a
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
    /// THE CALL BY NAME (MCP-ON-HOST-1: an adapter that knows no command): each member of `args` names a parameter (one
    /// that names none FAILS, "<name>: has no parameter 'x' (<signature>)"; one named twice FAILS); the members go to
    /// their parameters' POSITIONS in the signature's order, an absent one null (its default, or "is required"), and the
    /// call is callAt's - every strict law holds, in the same words as call.
    Result callNamed(const std::string &name, Value::Object args);
    /// The same call by the command's INDEX (its place in commands(), fixed at registration): a language binding
    /// resolves each command ONCE and calls it by index - no lookup by name a call - and hands its arguments over (the
    /// check moves them into the call, it copies nothing it can move).
    Result callAt(std::size_t index, Value::Array args);
    /// The last refusal's message ("" before any).
    const std::string &lastError() const { return mLastError; }
    /// One line per add() refused: a command that lacks a name, a doc, a function, its access tag or a name on any
    /// parameter or member, declares an array with neither a count nor a maximum, breaks the shape rule (a target after a non-target, or more than one non-target parameter), or repeats
    /// a name. Empty when every command offered was documented, shaped and new.
    std::vector<std::string> validate() const { return mFaults; }

    /// A HELD KIND IS BOUND TO ONE C++ TYPE (SCRIPT-IN-ENGINE-1 2b): a kind whose type is known at registration is
    /// bound THERE (registerEngineCommands binds scene, view and picture), so no hold - whoever makes it, in whatever
    /// order - can claim it for another type; a kind with none (a test's own) binds at its first hold. A kind bound
    /// before to another type is a registry FAULT (the host refuses every run). nameKind marks a NAMED kind (the
    /// engine's own ids: node, mesh, material): it holds no object.
    template <class T> void bindKind(const std::string &kind) { bindAs(kind, &typeid(T)); }
    void nameKind(const std::string &kind) { bindAs(kind, nullptr); }
    /// A held object's new handle; `scoped`: released by releaseScoped - NOT at a run's end: a held object lives across
    /// calls and runs until it is released (MCP-ON-HOST-1: a session drives the host call by call). 0, with `why`,
    /// for a kind bound to another type or a named kind, and when the table is full.
    template <class T>
    std::int64_t hold(const std::string &kind, std::shared_ptr<T> object, bool scoped = false, std::string *why = nullptr)
    {
        return holdAs(kind, typeid(T), std::shared_ptr<void>(std::move(object)), scoped, why);
    }
    /// A handle of the NAMED kind `kind` carrying `id` (< 2^32; the caller has checked it). A HANDLE IS MINTED ONLY BY
    /// THE DOOR THAT OWNS ITS KIND: 0 for a HELD kind (an id there would name a slot of the table - a forged handle).
    std::int64_t name(const std::string &kind, std::uint32_t id, std::string *why = nullptr);
    /// Releases a held handle: false when it names no live held object of this registry.
    bool release(std::int64_t handle);
    /// Releases every scoped handle: the caller's at ITS end (jahengine, at its script's end). Host::run never calls it.
    void releaseScoped();
    /// The T of a live held handle - read through its kind's bound type: null when the handle is not live, or its kind
    /// holds another C++ type than T.
    template <class T> T *object(std::int64_t handle) const { return static_cast<T *>(objectAs(handle, typeid(T))); }
    /// True when `h` is a live handle of this registry of kind `kind`; else false with `why` (naming `where`).
    bool checkHandle(std::int64_t h, const std::string &kind, const std::string &where, std::string &why) const;
    /// THE EXISTENCE CHECK of a NAMED kind (its family's: the engine asked whether the id is its scene's now), run at
    /// every call on every argument declared a handle of `kind` (on every element of an array of them), after the
    /// arguments are checked and before the
    /// command: false with `why` FAILS the call by name. No check = the kind's ids are taken as they come.
    using HandleCheck = std::function<bool(const Call &call, std::uint32_t id, std::string &why)>;
    void checkKind(const std::string &kind, HandleCheck check);

private:
    struct Entry { CommandInfo info; CommandFn fn; };
    std::vector<Entry> mEntries;
    std::unordered_map<std::string, std::size_t> mByName;   // a name's index (call / find by name: MCP's door)
    std::vector<std::string> mFaults;
    std::string mLastError;
    struct Held { unsigned kind = 0; std::shared_ptr<void> object; bool scoped = false; };
    unsigned kindIndex(const std::string &kind);   // interned, 1..255 (0: the 256th kind, refused)
    std::int64_t holdAs(const std::string &kind, const std::type_info &type, std::shared_ptr<void> object, bool scoped,
                        std::string *why);
    void *objectAs(std::int64_t handle, const std::type_info &type) const;
    void bindAs(const std::string &kind, const std::type_info *type);
    bool judgeHandles(const Call &call, const Param &p, const Value &v, std::string &why) const;
    std::uint64_t mSerial = 0;
    std::vector<std::string> mKinds;   // index - 1
    std::vector<bool> mKindHeld;       // index - 1: a held kind (a table slot) or a named one (the engine's id)
    std::vector<const std::type_info *> mKindType;   // index - 1: the C++ type a held kind is bound to (null: none yet)
    std::vector<bool> mKindNamed;      // index - 1: a NAMED kind (nameKind): it holds no object
    std::vector<HandleCheck> mChecks;  // index - 1
    std::vector<Held> mHeld;           // slot - 1
};

template <class T> Value Call::hold(const std::string &kind, std::shared_ptr<T> object, bool scoped)
{
    std::string why;
    const std::int64_t h = mRegistry.hold(kind, std::move(object), scoped, &why);
    return h ? Value(h) : fail(why);
}
template <class T> T *Call::object(const Value &h) const { return mRegistry.object<T>(h.asInt()); }

/// THE HOST — one per process (the process makes it; every Engine registers its commands into its registry, so a
/// process whose engine failed to start still has a host that can say why). It runs a script on the CALLING thread and
/// every command inline (a host whose scripts run on another thread than the engine's comes with the item that needs
/// it: STUDIO-CLIENT-1). Its own commands: `host.check({ok, what})` (one law: Host::law) and `host.lastError()` (the
/// registry's last refusal).
class Host {
public:
    using ConsoleSink = std::function<void(const std::string &line)>;
    /// THE HOST's OPTIONS, fixed at its construction (no environment variable, no script command changes them): each
    /// caller - jahengine, the MCP adapter - passes its own.
    struct Options {
        /// The bytes the live pictures may hold (engine.picture.*: a picture past it FAILS by name) - 512 MiB: fifteen
        /// 4K RGBA8 pictures, or three of PICTURE-REQUEST-1's 4K float radiance.
        static constexpr std::size_t kPictureBytesCap = std::size_t(512) << 20;
        std::size_t pictureBytesCap = kPictureBytesCap;
        /// THE BOUNDS OF ONE CALL's ARGUMENTS as a language converts them (ValueBounds), each DERIVED from what a call
        /// can legitimately carry - a call past one FAILS by name before its command is asked. A host for an OUTSIDE
        /// client (MCP) states its own, smaller.
        ///  - bytes: ONE PICTURE AT THE CAP (engine.picture.create's rgba, kPictureBytesCap: 512 MiB) PLUS AN EIGHTH OF
        ///    IT (64 MiB) for everything else in the call - the largest other argument today, engine.node.setTransforms'
        ///    full batch (65,536 nodes x 11 values, about 28 MiB as converted values), fits it twice. 576 MiB.
        ///  - elements: 2^20, above every declared maximum (the largest, setTransforms' rotations, 262,144): a command's
        ///    arrays are bounded by their declarations; this bounds only what no declaration reaches.
        ///  - depth: 64, the registry's deepest declaration is 3 (an object of an array of objects).
        ValueBounds callBounds{std::size_t(1) << 20, 64, kPictureBytesCap + kPictureBytesCap / 8};
        /// THE SCRIPT's RANDOM STREAM (no wall clock, no entropy: CODING_RULES § 5): Math.random is xorshift64* from
        /// this seed, reset at every run - the same sequence every run; a caller that wants another stream states it.
        std::uint64_t randomSeed = 0x9E3779B97F4A7C15ull;
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

    /// The callers' doors: a command call by name (positional arguments, or named: callNamed), by its registry index,
    /// and a console line.
    Result call(const std::string &name, const Value::Array &args);
    Result callNamed(const std::string &name, Value::Object args);
    Result callAt(std::size_t index, Value::Array args);
    void print(const std::string &line);

private:
    Options mOptions;
    Registry mRegistry;
    ConsoleSink mConsole;
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
