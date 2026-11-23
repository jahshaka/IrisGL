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
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/value.h"   // the value (iris::Value): NOQT-CORE-1 stage 3

namespace jahshaka { namespace engine { namespace script {

/// THE VALUE IS THE CORE's (NOQT-CORE-1 stage 3: one value type below the editor, core/value.h says it): the host and
/// every command name it as before.
using Value = iris::Value;
using Kind = iris::Kind;
using iris::kindName;

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
    /// of an array (Registry, THE HANDLES). A handle whose default is NULL (an optional one: a record's id member,
    /// records.h Field::handle) is NONE when null or a literal 0 (both conform to null, nothing checked); so is each
    /// ELEMENT of a handle array that declares a default (Field::handle's array: SkyDesc's six faces).
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
/// released, a value set, a frame drawn); `Files` reads or writes a file; `Test` is a test door (refused at the call
/// unless the host's test doors are open - --test-doors: Registry::openTestDoors). A command takes the HIGHEST any of its arguments reaches - Test > Files > Changes > Reads: a path
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
    const Registry &registry() const { return mRegistry; }

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
    /// THE REGISTRY's END (SCRIPT-IN-ENGINE-1 2b, THE OWNERSHIP LAW): its end hooks (atEnd), the last added first, then
    /// every held object released, the last held first - an object a command MADE is owned by its handle (its deleter
    /// destroys it), one its host's owner HANDED IN is held unowned and survives.
    ~Registry();
    Registry(const Registry &) = delete;
    Registry &operator=(const Registry &) = delete;
    /// A hook run at the registry's end, before any held object is released (the engine's: the ids its commands made
    /// in a scene they do not own, removed).
    void atEnd(std::function<void()> hook);
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
    /// True when validate() would name a fault - the host's check before every run and every call (no copy).
    bool faulted() const { return !mFaults.empty(); }

    /// A HELD KIND IS BOUND TO ONE C++ TYPE (SCRIPT-IN-ENGINE-1 2b): a kind whose type is known at registration is
    /// bound THERE (registerEngineCommands binds scene, view and picture), so no hold - whoever makes it, in whatever
    /// order - can claim it for another type; a kind with none (a test's own) binds at its first hold. A kind bound
    /// before to another type is a registry FAULT (the host refuses every run). nameKind marks a NAMED kind (the
    /// engine's own ids: node, mesh, material): it holds no object. `identity` (SCRIPT-IN-ENGINE-1 E0): the object's
    /// IDENTITY read when it is HELD (a scene's serial, a view's id - live then, whoever holds it): identityOf answers it
    /// for the handle, so a holder compares what it held with what the engine holds now - never the address alone.
    using Identity = std::uint64_t (*)(const void *object);
    template <class T> void bindKind(const std::string &kind, Identity identity = nullptr)
    {
        bindAs(kind, &typeid(T), identity);
    }
    void nameKind(const std::string &kind) { bindAs(kind, nullptr, nullptr); }
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
    /// Releases every scoped handle (a picture), before the registry's own end releases every held object: a caller
    /// that drives the host call by call frees them at ITS end (MCP-ON-HOST-1's session). Host::run never calls it.
    void releaseScoped();
    /// The T of a live held handle - read through its kind's bound type: null when the handle is not live, or its kind
    /// holds another C++ type than T.
    template <class T> T *object(std::int64_t handle) const { return static_cast<T *>(objectAs(handle, typeid(T))); }
    /// The identity a live held handle's object had when it was held (bindKind's `identity`); 0 for none.
    std::uint64_t identityOf(std::int64_t handle) const;
    /// True when `h` is a live handle of this registry of kind `kind`; else false with `why` (naming `where`).
    bool checkHandle(std::int64_t h, const std::string &kind, const std::string &where, std::string &why) const;
    /// THE EXISTENCE CHECK of a NAMED kind (its family's: the engine asked whether the id is its scene's now), run at
    /// every call on every argument declared a handle of `kind` (on every element of an array of them), after the
    /// arguments are checked and before the
    /// command: false with `why` FAILS the call by name. No check = the kind's ids are taken as they come.
    using HandleCheck = std::function<bool(const Call &call, std::uint32_t id, std::string &why)>;
    void checkKind(const std::string &kind, HandleCheck check);
    /// THE TEST DOORS (Access::Test): a command tagged `test` is REFUSED AT THE CALL - FAILS by name, before its
    /// arguments are read - while the doors are closed (the default); the engine's registration opens them for a host
    /// started with --test-doors (registerEngineCommands' testDoors). Listed either way (the documentation shows them).
    void openTestDoors(bool open) { mTestDoors = open; }
    bool testDoorsOpen() const { return mTestDoors; }

private:
    struct Entry { CommandInfo info; CommandFn fn; };
    std::vector<Entry> mEntries;
    std::unordered_map<std::string, std::size_t> mByName;   // a name's index (call / find by name: MCP's door)
    std::vector<std::string> mFaults;
    std::string mLastError;
    struct Held { unsigned kind = 0; std::shared_ptr<void> object; bool scoped = false; std::uint64_t identity = 0; };
    unsigned kindIndex(const std::string &kind);   // interned, 1..255 (0: the 256th kind, refused)
    std::int64_t holdAs(const std::string &kind, const std::type_info &type, std::shared_ptr<void> object, bool scoped,
                        std::string *why);
    void *objectAs(std::int64_t handle, const std::type_info &type) const;
    void bindAs(const std::string &kind, const std::type_info *type, Identity identity);
    bool judgeHandles(const Call &call, const Param &p, const Value &v, std::string &why) const;
    std::uint64_t mSerial = 0;
    std::vector<std::string> mKinds;   // index - 1
    std::vector<bool> mKindHeld;       // index - 1: a held kind (a table slot) or a named one (the engine's id)
    std::vector<const std::type_info *> mKindType;   // index - 1: the C++ type a held kind is bound to (null: none yet)
    std::vector<bool> mKindNamed;      // index - 1: a NAMED kind (nameKind): it holds no object
    std::vector<HandleCheck> mChecks;  // index - 1
    std::vector<Identity> mKindIdentity;   // index - 1: a held kind's identity read (bindKind; null: none)
    std::vector<Held> mHeld;           // slot - 1
    std::vector<std::function<void()>> mAtEnd;
    bool mTestDoors = false;
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
        /// THE SCRIPT's HEAP LIMIT (2b's final read, D1: the class fix): the most the language's own heap may hold -
        /// the script's values AND what the language allocates for a conversion (an own-key list) - past it an
        /// allocation is refused, the run or the call FAILS by name ("out of memory") and the host lives on. 0 =
        /// DERIVED, heapLimit(): 2 x callBounds.bytes + kHeapFloor - the script's own copy of the largest argument a
        /// call carries (a picture's rgba at the cap: the host's converted copy is outside this heap) BESIDE the
        /// largest answer it may hold (a picture's bytes read back, at most pictureBytesCap <= callBounds.bytes); the
        /// floor the interpreter and a script's working set (measured 2026-11-22: a fresh host with its 31 commands
        /// 0.11 MiB; the full 65,536-transform batch built as JS numbers 12.6 MiB more). jahengine's: 1168 MiB. A
        /// host for an outside client (MCP) derives it from its own smaller callBounds, or states its own. NO HOST IS
        /// UNLIMITED: 0 never means "no limit".
        /// IT BOUNDS MEMORY, and through it the CPU the JS engine spends in its own C where no cancel reaches: a
        /// Proxy's ownKeys list (a trap makes ~13,400 distinct keys a MiB of this limit before it refuses) is checked
        /// for duplicates and target keys in n log n WORST CASE by the vendored engine's patch 0001 (irisgl/thirdparty,
        /// its patch stack; it was O(n^2), ~15 h at the derived 1168 MiB): ~0.1 s at an outside client's 32 MiB,
        /// ~4.5 s at 1168 MiB (INFERRED from 131,072 keys in 0.027 s, Debug). A string or member-name conversion
        /// refused by this limit gives its string back (patch 0002; the pin leaked it for the runtime's life).
        static constexpr std::size_t kHeapFloor = std::size_t(16) << 20;
        std::size_t memoryLimit = 0;
        std::size_t heapLimit() const
        {
            const std::size_t twice = callBounds.bytes > (SIZE_MAX - kHeapFloor) / 2 ? SIZE_MAX - kHeapFloor
                                                                                     : 2 * callBounds.bytes;
            return memoryLimit ? memoryLimit : twice + kHeapFloor;
        }
        /// THE SCRIPT's RANDOM STREAM (no wall clock, no entropy: CODING_RULES § 5): Math.random is xorshift64* from
        /// this seed, reset at every run - the same sequence every run; a caller that wants another stream states it.
        std::uint64_t randomSeed = 0x9E3779B97F4A7C15ull;
        /// A DOCUMENTATION-ONLY host (jahengine --list; the MCP server before its first tools/call): its registry
        /// lists every command, and every run and every call FAILS "this host lists commands for documentation only".
        bool documentationOnly = false;
        /// A FRESH REALM PER RUN (the MCP server's run_script: state across calls is only what a handle names): the
        /// language made for a run is dropped at its end, so no global of one run survives into the next. Off: one
        /// realm for the host's life (jahengine runs one script).
        bool freshRealmPerRun = false;
        /// THE COMPLETION VALUE's BOUNDS (MCP-ON-HOST-1: run_script answers the script's completion value): stated, a
        /// run's Result::value is the script's last expression (a promise's settled value), converted within them - one
        /// past them, or one that cannot be carried, FAILS the run by name. Absent (jahengine): the value is dropped.
        std::optional<ValueBounds> completionBounds;
        /// A HOST FOR AN OUTSIDE CLIENT (the MCP server's): what is ENGINE-WIDE - beyond the scenes and views this host
        /// made - is refused by name: engine.arm (an arm changes what the owner's own scenes draw) and every engine-wide
        /// setting (the engine's commands ask one helper, Commands::engineWide).
        bool outsideClient = false;
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
    /// ..."); a command's Failed is a thrown error the script may catch. Ok's value: the script's COMPLETION VALUE when
    /// Options::completionBounds is stated (else null).
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
    /// and a console line. A host whose registry holds a fault serves NO call (as it runs no script): each FAILS with
    /// run's words, naming every fault; a documentation-only host serves none either (Options::documentationOnly).
    Result call(const std::string &name, const Value::Array &args);
    Result callNamed(const std::string &name, Value::Object args);
    Result callAt(std::size_t index, Value::Array args);
    void print(const std::string &line);

    /// THE KEEP-GOING HOOK (MCP-ON-HOST-1: a caller's cancel, and its progress): long work asks keepGoing at its
    /// boundaries - the script language's interrupt poll as (0, 0) (no count), engine.frames before each frame as
    /// (frames done, frames asked) and once more at its end as (n, n) - and stops when it answers false: a script FAILS
    /// "cancelled", engine.frames "cancelled at frame i of n" (a frame is never cut; the state is what the drawn frames
    /// made). Unset: always true. The same call carries progress: a hook may report `done` of `total` (total > 0).
    using KeepGoing = std::function<bool(double done, double total)>;
    void setKeepGoing(KeepGoing hook);
    bool keepGoing(double done, double total) { return !mKeepGoing || mKeepGoing(done, total); }

private:
    Options mOptions;
    Registry mRegistry;
    ConsoleSink mConsole;
    KeepGoing mKeepGoing;
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
/// declaration no frame asserted). THE HOST DIES BEFORE `engine`: its end destroys what its commands made (THE
/// OWNERSHIP LAW: the scenes and views, and the nodes, meshes and materials made in a scene its owner handed in); a
/// host ending after its engine calls nothing and is a named fault (a debug build aborts). WHAT IS HANDED IN (held by
/// the owner's Registry::hold, unowned): a SCENE's own objects and settings are READ-ONLY to the script - a command
/// changes or destroys only a node, mesh or material its own host made there (a handle is computable: the made record
/// is the authority), and the scene's settings (ambient, GI) not at all; the script may make its own objects in it. A
/// VIEW is the script's to drive (bind a scene, place, post-fx, declare, read), never to destroy; its owner restores
/// its own state when it takes the view back. (Not built: a scene handed in WRITABLE - the app's, when it comes.)
/// engine.arm STAYS OPEN in such a host (the lab's scripts and the tests measure with arms on purpose): an arm is
/// ENGINE-WIDE - it changes what the owner's own scenes draw - so a host for an OUTSIDE client refuses it (the MCP's).
/// WHAT IS HANDED IN OUTLIVES THE HOST (the owner's duty, as the host's own before its engine): the host holds the
/// handed-in Scene* and View* - its end removes what it made in such a scene, and a route declared on such a view is
/// judged at every frame. GUARDED (SCRIPT-IN-ENGINE-1 E0): the host knows its engine by its SERIAL (Engine::isLive -
/// never the pointer alone) and asks the engine whether it still holds a scene or view before reading one
/// (Engine::scenePresence / viewPresence): a call on one its owner destroyed FAILS by name, a declaration on it is a
/// failed law, and the host's end skips it (what the host made there went with it). The guard stands in lockstep; while a
/// render thread owns the engine the reads are unanswered and the owner's duty is the contract. A null `engine` (it did not start) still registers them: each FAILS with "the engine
/// did not start: <bootError>". `testDoors`: the engine's TEST arms (ArmInfo::test) and every command tagged `test`
/// open (jahengine --test-doors; Registry::openTestDoors); without it engine.arm FAILS on a test arm and a test door
/// FAILS at the call.
void registerEngineCommands(Host &host, Engine *engine, std::string bootError, bool testDoors);

}}}  // namespace jahshaka::engine::script
