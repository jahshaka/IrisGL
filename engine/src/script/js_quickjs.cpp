// THE JAVASCRIPT BINDING (SCRIPT-IN-ENGINE-1 stage 1) — THE ONE FILE THAT NAMES THE JS ENGINE (quickjs-ng, vendored at
// irisgl/thirdparty/quickjs-ng; source.script_neutral holds every other file to it). One runtime and one realm a host,
// used on the host's thread only.
//
// EVERY COMMAND IS A FUNCTION of its namespace's object: `engine.scene.create` is the function `create` of `engine.scene`,
// made ONCE (at the first run after its registration) and bound to the command's REGISTRY INDEX - a call looks nothing up
// by name. A NAMESPACE is an object of the binding's own class: its commands and sub-namespaces are its ordinary own
// properties (the engine's fast path), and a name it does not hold reaches the class's C hook, which throws naming it
// ("engine.scene.creat is not a command") - no JS Proxy trap on any call (SCRIPT-IN-ENGINE-1 S9: the call's cost). A call
// converts its arguments to script::Value, calls Host::call (the registry's strict check, then the command), and answers:
// Ok -> the value; Refused -> the declared return's falsy value (host.lastError() says why); Failed -> a thrown Error
// carrying the message, which the script may catch (its stack names the script's line).
//
// THE CONVERSIONS. JS -> Value: undefined and null -> null (a trailing undefined argument is an absent one); a boolean;
// a number -> Int when it is integral, not -0 and of magnitude <= 2^53, else Number; a BigInt -> Int (outside int64 an
// error); a string -> UTF-8; an Array -> array; an ArrayBuffer or a Uint8Array -> bytes; any other object -> an object of
// its own enumerable string keys in their order (a member holding undefined is absent). A function, a symbol or a nesting
// deeper than 64 is an error. Value -> JS: an Int of magnitude <= 2^53 -> a number, a larger one -> a BigInt; bytes -> a
// Uint8Array; the rest by kind.
//
// THE CONVERSION WALKS THE DECLARATION (SCRIPT-IN-ENGINE-1 2b; the 2a final read's L1): a script's `a.length = 4e9` costs
// it nothing and would cost the host 4e9 values. So an array's length is judged against the parameter's declared count
// or maximum (Registry::add refuses an array that declares neither) BEFORE any element is read or any room reserved -
// past it, the call FAILS by name with the registry's own words; and an array or object where the declaration takes
// none (an argument too many, an undeclared member, another kind) is carried EMPTY: the registry's verdict there (a
// surplus argument, an undeclared member, a wrong kind) never reads its contents. NO C++ EXCEPTION CROSSES quickjs's C:
// every callback below (a command, a namespace's miss, the rejection tracker, console.log) catches its own - a call's
// is a thrown error naming the command, the host lives on.
#include "script/Language.h"

#include "quickjs.h"

#include <cmath>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace jahshaka { namespace engine { namespace script {

namespace {

constexpr double kExactInt = 9007199254740992.0;   // 2^53: every integer up to it is exact in a double
constexpr int kMaxDepth = 64;

/// A JSValue this frame owns, freed however the frame is left: a C++ exception between its making and its free would
/// leak it (and quickjs asserts at its runtime's end that every object was freed).
class Owned {
public:
    Owned(JSContext *ctx, JSValue v) : mCtx(ctx), mV(v) {}
    ~Owned() { JS_FreeValue(mCtx, mV); }
    Owned(const Owned &) = delete;
    Owned &operator=(const Owned &) = delete;
    JSValueConst get() const { return mV; }

private:
    JSContext *mCtx;
    JSValue mV;
};

/// A C string quickjs made, freed the same way.
struct OwnedCString {
    JSContext *ctx = nullptr;
    const char *s = nullptr;
    ~OwnedCString() { if (s) JS_FreeCString(ctx, s); }
};

std::string toUtf8(JSContext *ctx, JSValueConst v)
{
    size_t n = 0;
    const OwnedCString s{ctx, JS_ToCStringLen(ctx, &n, v)};
    return s.s ? std::string(s.s, n) : std::string();
}

/// The pending exception's text and stack (the script's line), the exception cleared.
std::string takeException(JSContext *ctx)
{
    const Owned e(ctx, JS_GetException(ctx));
    std::string text = toUtf8(ctx, e.get());
    if (JS_IsObject(e.get())) {
        const Owned stack(ctx, JS_GetPropertyStr(ctx, e.get(), "stack"));
        if (JS_IsString(stack.get())) text += "\n" + toUtf8(ctx, stack.get());
    }
    return text;
}

/// WHAT THE DECLARATION TAKES AT ONE PLACE of an argument: `p` the parameter or member declared there and `kind` what
/// it takes there - p->kind, or for an array's element the element kind (p then the array's own parameter: an element
/// object's members are its fields, as the registry reads them) - and the place itself ("transforms.nodes", "o.v[2]"),
/// a chain on the stack made text only on a failure. p null: the declaration takes nothing here.
struct Place {
    const Param *p = nullptr;
    Kind kind = Kind::Null;
    const Place *up = nullptr;
    const std::string *name = nullptr;   // a parameter or a member
    long index = -1;                     // an array's element
};
std::string placeText(const Place &w)
{
    std::string t = w.up ? placeText(*w.up) : std::string();
    if (w.name) t += (t.empty() ? "" : ".") + *w.name;
    else t += "[" + std::to_string(w.index) + "]";
    return t;
}

bool fromJs(JSContext *ctx, JSValueConst v, const Place &at, Value &out, std::string &why, int depth);

bool fromJsObject(JSContext *ctx, JSValueConst v, const Place &at, Value &out, std::string &why, int depth)
{
    JSPropertyEnum *props = nullptr;
    uint32_t n = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &n, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        why = takeException(ctx);
        return false;
    }
    const struct Free {
        JSContext *c = nullptr;
        JSPropertyEnum *p = nullptr;
        uint32_t n = 0;
        ~Free() { JS_FreePropertyEnum(c, p, n); }
    } free{ctx, props, n};
    Value::Object o;
    for (uint32_t i = 0; i < n; ++i) {
        const Owned m(ctx, JS_GetProperty(ctx, v, props[i].atom));
        if (JS_IsException(m.get())) {
            why = takeException(ctx);
            return false;
        }
        if (JS_IsUndefined(m.get())) continue;
        const OwnedCString name{ctx, JS_AtomToCString(ctx, props[i].atom)};
        const std::string key = name.s ? name.s : "";
        Place member;   // its declaration: the member of that name, or none (the registry FAILS it, unread)
        member.up = &at;
        for (const Param &f : at.p->fields)
            if (f.name == key) {
                member.p = &f;
                member.kind = f.kind;
                member.name = &f.name;
            }
        if (!member.p) member.name = &key;
        Value mv;
        if (!fromJs(ctx, m.get(), member, mv, why, depth + 1)) return false;
        o.emplace_back(key, std::move(mv));
    }
    out = Value(std::move(o));
    return true;
}

bool fromJs(JSContext *ctx, JSValueConst v, const Place &at, Value &out, std::string &why, int depth)
{
    if (depth > kMaxDepth) {
        why = "a value nested deeper than 64 (a cycle?) cannot be carried";
        return false;
    }
    if (JS_IsUndefined(v) || JS_IsNull(v)) { out = Value(); return true; }
    if (JS_IsBool(v)) { out = Value(bool(JS_ToBool(ctx, v))); return true; }
    if (JS_IsNumber(v)) {
        double d = 0.0;
        JS_ToFloat64(ctx, &d, v);
        if (std::trunc(d) == d && std::fabs(d) <= kExactInt && !(d == 0.0 && std::signbit(d)))
            out = Value(static_cast<std::int64_t>(d));
        else
            out = Value(d);
        return true;
    }
    if (JS_IsBigInt(v)) {
        int64_t i = 0;
        JS_ToBigInt64(ctx, &i, v);
        if (toUtf8(ctx, v) != std::to_string(i)) {
            why = "the BigInt " + toUtf8(ctx, v) + " is outside a 64-bit integer";
            return false;
        }
        out = Value(static_cast<std::int64_t>(i));
        return true;
    }
    if (JS_IsString(v)) { out = Value(toUtf8(ctx, v)); return true; }
    if (JS_IsSymbol(v) || JS_IsFunction(ctx, v)) {
        why = std::string("a ") + (JS_IsSymbol(v) ? "symbol" : "function") + " cannot be carried";
        return false;
    }
    if (JS_IsArrayBuffer(v)) {
        size_t n = 0;
        const uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);
        out = Value::bytes(Value::Bytes(p, p + (p ? n : 0)));
        return true;
    }
    if (JS_GetTypedArrayType(v) == JS_TYPED_ARRAY_UINT8) {
        size_t n = 0;
        const uint8_t *p = JS_GetUint8Array(ctx, &n, v);
        out = Value::bytes(Value::Bytes(p, p + (p ? n : 0)));
        return true;
    }
    if (JS_IsArray(v)) {
        if (!at.p || at.kind != Kind::Array) {   // no array is declared here: carried empty, never read
            out = Value(Value::Array{});
            return true;
        }
        // THE LENGTH JUDGED FIRST, in the registry's own words: nothing is read or reserved past the declaration
        const Param &p = *at.p;
        int64_t n = 0;
        if (JS_GetLength(ctx, v, &n) < 0) {
            why = takeException(ctx);
            return false;
        }
        if (p.count && n > int64_t(p.count)) {
            why = placeText(at) + " must hold " + std::to_string(p.count) + " elements (it holds " + std::to_string(n) + ")";
            return false;
        }
        if (!p.count && n > int64_t(p.maxCount)) {   // (a declared array has a count or a maximum: Registry::add)
            why = placeText(at) + " holds " + std::to_string(n) + " elements, at most " + std::to_string(p.maxCount);
            return false;
        }
        Value::Array a;
        a.reserve(size_t(n));
        for (int64_t i = 0; i < n; ++i) {
            const Owned e(ctx, JS_GetPropertyUint32(ctx, v, uint32_t(i)));
            if (JS_IsException(e.get())) {
                why = takeException(ctx);
                return false;
            }
            Place element;
            element.p = &p;
            element.kind = p.element;
            element.up = &at;
            element.index = long(i);
            Value ev;
            if (!fromJs(ctx, e.get(), element, ev, why, depth + 1)) return false;
            a.push_back(std::move(ev));
        }
        out = Value(std::move(a));
        return true;
    }
    if (JS_IsObject(v)) {
        if (!at.p || at.kind != Kind::Object) {   // no object is declared here: carried empty, never read
            out = Value(Value::Object{});
            return true;
        }
        return fromJsObject(ctx, v, at, out, why, depth);
    }
    why = "a value of this type cannot be carried";
    return false;
}

JSValue toJs(JSContext *ctx, const Value &v)
{
    switch (v.kind()) {
    case Kind::Null:   return JS_NULL;
    case Kind::Bool:   return JS_NewBool(ctx, v.asBool());
    case Kind::Int: {
        const std::int64_t i = v.asInt();
        if (double(i) <= kExactInt && double(i) >= -kExactInt) return JS_NewInt64(ctx, i);
        return JS_NewBigInt64(ctx, i);
    }
    case Kind::Number: return JS_NewFloat64(ctx, v.asNumber());
    case Kind::String: return JS_NewStringLen(ctx, v.asString().data(), v.asString().size());
    case Kind::Bytes:  return JS_NewUint8ArrayCopy(ctx, v.asBytes().data(), v.asBytes().size());
    case Kind::Array: {
        JSValue a = JS_NewArray(ctx);
        uint32_t i = 0;
        for (const Value &e : v.asArray()) JS_SetPropertyUint32(ctx, a, i++, toJs(ctx, e));
        return a;
    }
    case Kind::Object: {
        JSValue o = JS_NewObject(ctx);
        for (const Value::Member &m : v.asObject()) JS_SetPropertyStr(ctx, o, m.first.c_str(), toJs(ctx, m.second));
        return o;
    }
    }
    return JS_UNDEFINED;
}

class JavaScript final : public Host::Language {
public:
    explicit JavaScript(Host &host) : mHost(host)
    {
        mRt = JS_NewRuntime();
        JS_SetHostPromiseRejectionTracker(mRt, &JavaScript::rejection, this);
        // THE REALM HAS NO WALL CLOCK (the re-read's C1; CODING_RULES § 5: a test counts FRAMES): JS_NewContext's own
        // intrinsics (quickjs.c, JS_NewContext) MINUS Date and performance; Math.random REPLACED below by a fixed-seed
        // stream (quickjs seeds its own from the time of day)
        mCtx = JS_NewContextRaw(mRt);
        JS_AddIntrinsicBaseObjects(mCtx);
        JS_AddIntrinsicEval(mCtx);
        JS_AddIntrinsicRegExp(mCtx);
        JS_AddIntrinsicJSON(mCtx);
        JS_AddIntrinsicProxy(mCtx);
        JS_AddIntrinsicMapSet(mCtx);
        JS_AddIntrinsicTypedArrays(mCtx);
        JS_AddIntrinsicPromise(mCtx);
        JS_AddIntrinsicWeakRef(mCtx);
        JS_AddIntrinsicAToB(mCtx);
        JS_SetContextOpaque(mCtx, this);
        JSValue global = JS_GetGlobalObject(mCtx);
        JSValue math = JS_GetPropertyStr(mCtx, global, "Math");
        JS_SetPropertyStr(mCtx, math, "random", JS_NewCFunction(mCtx, &JavaScript::random, "random", 0));
        JS_FreeValue(mCtx, math);
        JSValue console = JS_NewObject(mCtx);
        JS_SetPropertyStr(mCtx, console, "log", JS_NewCFunction(mCtx, &JavaScript::log, "log", 1));
        JS_SetPropertyStr(mCtx, global, "console", console);
        JS_FreeValue(mCtx, global);
        // THE NAMESPACE CLASS: one id a process, the class a runtime
        static JSClassID namespaceClass = 0;
        JS_NewClassID(mRt, &namespaceClass);
        mNamespaceClass = namespaceClass;
        static JSClassExoticMethods exotic = {};
        exotic.get_property = &JavaScript::missing;
        JSClassDef def = {};
        def.class_name = "Commands";
        def.exotic = &exotic;
        JS_NewClass(mRt, mNamespaceClass, &def);
    }
    ~JavaScript() override
    {
        clearRejections();
        for (auto &s : mSpaces) JS_FreeValue(mCtx, s.second);
        JS_FreeContext(mCtx);
        JS_FreeRuntime(mRt);
    }

    /// THE RUN TO ITS END (the final read's F1): the script, then EVERY JOB IT QUEUED (the code after an `await`, a
    /// promise's reactions) until none is left — a check after an `await` runs, and a row cannot pass by never reaching
    /// it. The first fault decides: an uncaught error (its text and stack), else a rejected promise no handler took by
    /// the end (the tracker below). The queue is drained even after a fault, so no job of this run leaks into the next.
    Result run(const std::string &source, const std::string &name) override
    {
        install();
        clearRejections();
        mRejectionLost = false;
        mRandom = mHost.options().randomSeed ? mHost.options().randomSeed : 1;   // every run draws the same stream
        Result r;
        JSValue v = JS_Eval(mCtx, source.c_str(), source.size(), name.c_str(), JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) {
            r.status = Status::Failed;
            r.message = takeException(mCtx);
        }
        JS_FreeValue(mCtx, v);
        for (;;) {
            JSContext *jobCtx = nullptr;
            const int ran = JS_ExecutePendingJob(mRt, &jobCtx);
            if (ran == 0) break;
            if (ran < 0 && r.status != Status::Failed) {
                r.status = Status::Failed;
                r.message = takeException(jobCtx);
            }
        }
        if (r.status != Status::Failed && !mRejections.empty()) {
            r.status = Status::Failed;
            r.message = "an unhandled promise rejection: " + mRejections.front().second;
        }
        if (r.status != Status::Failed && mRejectionLost) {
            r.status = Status::Failed;
            r.message = "an unhandled promise rejection could not be recorded (its tracker threw)";
        }
        clearRejections();
        return r;
    }

private:
    /// Every command registered since the last install as a function of its namespace's object, bound to its
    /// registry index (made once: the registry only grows).
    void install()
    {
        const std::vector<const CommandInfo *> commands = mHost.registry().commands();
        for (; mInstalled < commands.size(); ++mInstalled) {
            const CommandInfo *c = commands[mInstalled];
            const size_t dot = c->name.rfind('.');
            if (dot == std::string::npos) continue;   // a command is always under a root
            JSValue index = JS_NewInt32(mCtx, int32_t(mInstalled));
            JSValue fn = JS_NewCFunctionData2(mCtx, &JavaScript::command, c->name.c_str() + dot + 1,
                                              int(c->signature.params.size()), 0, 1, &index);
            JS_DefinePropertyValueStr(mCtx, space(c->name.substr(0, dot)), c->name.c_str() + dot + 1, fn,
                                      JS_PROP_ENUMERABLE);
        }
    }

    /// The object of the namespace `path` ("engine.scene"), made (an own property of its parent) the first time.
    JSValueConst space(const std::string &path)
    {
        for (const auto &s : mSpaces)
            if (s.first->path == path) return s.second;
        const size_t dot = path.rfind('.');
        JSValue parent = dot == std::string::npos ? JS_GetGlobalObject(mCtx) : JS_DupValue(mCtx, space(path.substr(0, dot)));
        JSValue plain = JS_NewObject(mCtx);
        JSValue objectProto = JS_GetPrototype(mCtx, plain);   // Object.prototype: toString, ... read through `missing`
        JSValue object = JS_NewObjectProtoClass(mCtx, objectProto, mNamespaceClass);
        JS_FreeValue(mCtx, objectProto);
        JS_FreeValue(mCtx, plain);
        auto named = std::make_unique<Namespace>();
        named->path = path;
        JS_SetOpaque(object, named.get());
        JS_DefinePropertyValueStr(mCtx, parent, path.c_str() + (dot == std::string::npos ? 0 : dot + 1),
                                  JS_DupValue(mCtx, object), JS_PROP_ENUMERABLE);
        JS_FreeValue(mCtx, parent);
        mSpaces.emplace_back(std::move(named), object);
        return object;
    }

    /// A NAME THE NAMESPACE DOES NOT HOLD (its own properties are found first: this runs only for a miss): a symbol,
    /// or a name the prototype answers (toString, ...), reads through to it; any other name FAILS naming itself.
    static JSValue missing(JSContext *ctx, JSValueConst obj, JSAtom atom, JSValueConst)
    {
        JavaScript *self = static_cast<JavaScript *>(JS_GetContextOpaque(ctx));
        try {
            const Owned key(ctx, JS_AtomToValue(ctx, atom));
            const Owned proto(ctx, JS_GetPrototype(ctx, obj));
            if (JS_IsSymbol(key.get()) || (JS_IsObject(proto.get()) && JS_HasProperty(ctx, proto.get(), atom) > 0))
                return JS_IsObject(proto.get()) ? JS_GetProperty(ctx, proto.get(), atom) : JS_UNDEFINED;
            const Namespace *n = static_cast<const Namespace *>(JS_GetOpaque(obj, self->mNamespaceClass));
            const std::string name = toUtf8(ctx, key.get());
            return JS_ThrowPlainError(ctx, "%s.%s is not a command", n ? n->path.c_str() : "?", name.c_str());
        } catch (const std::exception &e) {   // (no C++ exception crosses quickjs's C)
            return JS_ThrowPlainError(ctx, "a command's name could not be read: %s", e.what());
        } catch (...) {
            return JS_ThrowPlainError(ctx, "a command's name could not be read");
        }
    }

    static JSValue command(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int, JSValueConst *data)
    {
        JavaScript *self = static_cast<JavaScript *>(JS_GetContextOpaque(ctx));
        const std::size_t index = std::size_t(JS_VALUE_GET_INT(data[0]));
        const char *name = "a command";
        try {   // (no C++ exception crosses quickjs's C: one here is the call's FAIL by name, as the registry's own)
            const CommandInfo &info = *self->mHost.registry().commands()[index];   // (read at the call: never kept)
            name = info.name.c_str();
            while (argc > 0 && JS_IsUndefined(argv[argc - 1])) --argc;   // a trailing undefined is an absent argument
            const std::vector<Param> &params = info.signature.params;
            Value::Array args;
            args.reserve(size_t(argc));
            for (int i = 0; i < argc; ++i) {
                Place at;   // the parameter declared at this position (none past the last: the registry FAILS it)
                if (size_t(i) < params.size()) {
                    at.p = &params[size_t(i)];
                    at.kind = at.p->kind;
                    at.name = &at.p->name;
                }
                Value a;
                std::string why;
                if (!fromJs(ctx, argv[i], at, a, why, 0))
                    return JS_ThrowTypeError(ctx, "%s: argument %d: %s", info.name.c_str(), i + 1, why.c_str());
                args.push_back(std::move(a));
            }
            const Result r = self->mHost.callAt(index, std::move(args));
            if (r.status == Status::Failed) return JS_ThrowPlainError(ctx, "%s", r.message.c_str());
            return toJs(ctx, r.value);
        } catch (const std::exception &e) {
            return JS_ThrowPlainError(ctx, "%s: threw: %s", name, e.what());
        } catch (...) {
            return JS_ThrowPlainError(ctx, "%s: threw an exception that is not a std::exception", name);
        }
    }

    /// THE UNHANDLED REJECTIONS: a promise rejected with no handler is held (with its reason's text and stack) until a
    /// handler is attached (`is_handled`: it leaves) or the run ends (it FAILS the run).
    static void rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason, bool handled, void *opaque)
    {
        JavaScript *self = static_cast<JavaScript *>(opaque);
        auto &held = self->mRejections;
        for (auto it = held.begin(); it != held.end(); ++it)
            if (JS_VALUE_GET_PTR(it->first) == JS_VALUE_GET_PTR(promise)) {
                JS_FreeValue(ctx, it->first);
                held.erase(it);
                break;
            }
        if (handled) return;
        try {   // (no C++ exception crosses quickjs's C: a rejection that cannot be held FAILS the run instead)
            std::string why = toUtf8(ctx, reason);
            if (JS_IsObject(reason)) {
                const Owned stack(ctx, JS_GetPropertyStr(ctx, reason, "stack"));
                if (JS_IsString(stack.get())) why += "\n" + toUtf8(ctx, stack.get());
            }
            held.reserve(held.size() + 1);   // (then the emplace cannot throw after the promise is held)
            held.emplace_back(JS_DupValue(ctx, promise), std::move(why));
        } catch (...) {
            self->mRejectionLost = true;
        }
    }
    void clearRejections()
    {
        for (auto &p : mRejections) JS_FreeValue(mCtx, p.first);
        mRejections.clear();
    }

    /// Math.random: xorshift64* from the host's fixed seed (Host::Options::randomSeed), 53 bits into [0, 1).
    static JSValue random(JSContext *ctx, JSValueConst, int, JSValueConst *)
    {
        JavaScript *self = static_cast<JavaScript *>(JS_GetContextOpaque(ctx));
        std::uint64_t &x = self->mRandom;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        return JS_NewFloat64(ctx, double((x * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0));
    }

    static JSValue log(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        JavaScript *self = static_cast<JavaScript *>(JS_GetContextOpaque(ctx));
        try {   // (no C++ exception crosses quickjs's C: the console's sink is the embedder's code)
            std::string line;
            for (int i = 0; i < argc; ++i) {
                if (i) line += ' ';
                line += toUtf8(ctx, argv[i]);
            }
            self->mHost.print(line);
            return JS_UNDEFINED;
        } catch (const std::exception &e) {
            return JS_ThrowPlainError(ctx, "console.log: threw: %s", e.what());
        } catch (...) {
            return JS_ThrowPlainError(ctx, "console.log: threw an exception that is not a std::exception");
        }
    }

    Host &mHost;
    JSRuntime *mRt = nullptr;
    JSContext *mCtx = nullptr;
    struct Namespace { std::string path; };
    JSClassID mNamespaceClass = 0;
    std::vector<std::pair<std::unique_ptr<Namespace>, JSValue>> mSpaces;   // each namespace's object, by path
    std::size_t mInstalled = 0;                                             // the commands made so far
    std::uint64_t mRandom = 1;                                              // Math.random's state (run's seed)
    std::vector<std::pair<JSValue, std::string>> mRejections;  // rejected, no handler yet: the promise, its reason
    bool mRejectionLost = false;                                // a rejection the tracker could not hold (it threw)
};

}  // namespace

std::unique_ptr<Host::Language> detail::makeJavaScript(Host &host) { return std::make_unique<JavaScript>(host); }

}}}  // namespace jahshaka::engine::script
