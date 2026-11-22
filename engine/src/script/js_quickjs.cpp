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
#include "script/Language.h"

#include "quickjs.h"

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jahshaka { namespace engine { namespace script {

namespace {

constexpr double kExactInt = 9007199254740992.0;   // 2^53: every integer up to it is exact in a double
constexpr int kMaxDepth = 64;

std::string toUtf8(JSContext *ctx, JSValueConst v)
{
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    if (!s) return std::string();
    std::string out(s, n);
    JS_FreeCString(ctx, s);
    return out;
}

/// The pending exception's text and stack (the script's line), the exception cleared.
std::string takeException(JSContext *ctx)
{
    JSValue e = JS_GetException(ctx);
    std::string text = toUtf8(ctx, e);
    if (JS_IsObject(e)) {
        JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
        if (JS_IsString(stack)) text += "\n" + toUtf8(ctx, stack);
        JS_FreeValue(ctx, stack);
    }
    JS_FreeValue(ctx, e);
    return text;
}

bool fromJs(JSContext *ctx, JSValueConst v, Value &out, std::string &why, int depth);

bool fromJsObject(JSContext *ctx, JSValueConst v, Value &out, std::string &why, int depth)
{
    JSPropertyEnum *props = nullptr;
    uint32_t n = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &n, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        why = takeException(ctx);
        return false;
    }
    Value::Object o;
    bool ok = true;
    for (uint32_t i = 0; ok && i < n; ++i) {
        JSValue m = JS_GetProperty(ctx, v, props[i].atom);
        if (JS_IsException(m)) {
            why = takeException(ctx);
            ok = false;
        } else if (!JS_IsUndefined(m)) {
            const char *key = JS_AtomToCString(ctx, props[i].atom);
            Value mv;
            ok = fromJs(ctx, m, mv, why, depth + 1);
            if (ok) o.emplace_back(key ? key : "", std::move(mv));
            JS_FreeCString(ctx, key);
        }
        JS_FreeValue(ctx, m);
    }
    JS_FreePropertyEnum(ctx, props, n);
    if (ok) out = Value(std::move(o));
    return ok;
}

bool fromJs(JSContext *ctx, JSValueConst v, Value &out, std::string &why, int depth)
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
        int64_t n = 0;
        JS_GetLength(ctx, v, &n);
        Value::Array a;
        a.reserve(size_t(n));
        for (int64_t i = 0; i < n; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, v, uint32_t(i));
            Value ev;
            const bool ok = fromJs(ctx, e, ev, why, depth + 1);
            JS_FreeValue(ctx, e);
            if (!ok) return false;
            a.push_back(std::move(ev));
        }
        out = Value(std::move(a));
        return true;
    }
    if (JS_IsObject(v)) return fromJsObject(ctx, v, out, why, depth);
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
        JSValue key = JS_AtomToValue(ctx, atom);
        JSValue proto = JS_GetPrototype(ctx, obj);
        if (JS_IsSymbol(key) || (JS_IsObject(proto) && JS_HasProperty(ctx, proto, atom) > 0)) {
            JS_FreeValue(ctx, key);
            JSValue v = JS_IsObject(proto) ? JS_GetProperty(ctx, proto, atom) : JS_UNDEFINED;
            JS_FreeValue(ctx, proto);
            return v;
        }
        JS_FreeValue(ctx, proto);
        const Namespace *n = static_cast<const Namespace *>(JS_GetOpaque(obj, self->mNamespaceClass));
        const std::string name = toUtf8(ctx, key);
        JS_FreeValue(ctx, key);
        return JS_ThrowPlainError(ctx, "%s.%s is not a command", n ? n->path.c_str() : "?", name.c_str());
    }

    static JSValue command(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int, JSValueConst *data)
    {
        JavaScript *self = static_cast<JavaScript *>(JS_GetContextOpaque(ctx));
        const std::size_t index = std::size_t(JS_VALUE_GET_INT(data[0]));
        while (argc > 0 && JS_IsUndefined(argv[argc - 1])) --argc;   // a trailing undefined is an absent argument
        Value::Array args;
        args.reserve(size_t(argc));
        for (int i = 0; i < argc; ++i) {
            Value a;
            std::string why;
            if (!fromJs(ctx, argv[i], a, why, 0))
                return JS_ThrowTypeError(ctx, "%s: argument %d: %s", self->mHost.registry().commands()[index]->name.c_str(),
                                         i + 1, why.c_str());
            args.push_back(std::move(a));
        }
        const Result r = self->mHost.callAt(index, std::move(args));
        if (r.status == Status::Failed) return JS_ThrowPlainError(ctx, "%s", r.message.c_str());
        return toJs(ctx, r.value);
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
        std::string why = toUtf8(ctx, reason);
        if (JS_IsObject(reason)) {
            JSValue stack = JS_GetPropertyStr(ctx, reason, "stack");
            if (JS_IsString(stack)) why += "\n" + toUtf8(ctx, stack);
            JS_FreeValue(ctx, stack);
        }
        held.emplace_back(JS_DupValue(ctx, promise), why);
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
        std::string line;
        for (int i = 0; i < argc; ++i) {
            if (i) line += ' ';
            line += toUtf8(ctx, argv[i]);
        }
        self->mHost.print(line);
        return JS_UNDEFINED;
    }

    Host &mHost;
    JSRuntime *mRt = nullptr;
    JSContext *mCtx = nullptr;
    struct Namespace { std::string path; };
    JSClassID mNamespaceClass = 0;
    std::vector<std::pair<std::unique_ptr<Namespace>, JSValue>> mSpaces;   // each namespace's object, by path
    std::size_t mInstalled = 0;
    std::uint64_t mRandom = 1;                                              // Math.random's state (run's seed)                                            // the commands made so far
    std::vector<std::pair<JSValue, std::string>> mRejections;  // rejected, no handler yet: the promise, its reason
};

}  // namespace

std::unique_ptr<Host::Language> detail::makeJavaScript(Host &host) { return std::make_unique<JavaScript>(host); }

}}}  // namespace jahshaka::engine::script
