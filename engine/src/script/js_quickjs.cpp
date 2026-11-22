// THE JAVASCRIPT BINDING (SCRIPT-IN-ENGINE-1 stage 1) — THE ONE FILE THAT NAMES THE JS ENGINE (quickjs-ng, vendored at
// irisgl/thirdparty/quickjs-ng; source.script_neutral holds every other file to it). One runtime and one realm a host,
// used on the host's thread only.
//
// EVERY COMMAND IS A FUNCTION of its namespace's object: `engine.scene.create` is the function `create` of `engine.scene`,
// installed before each run from the registry (commands registered since the last run included); a name nothing
// registered throws naming itself (the namespace is a Proxy). A call
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
        mCtx = JS_NewContext(mRt);
        JS_SetContextOpaque(mCtx, this);
        JSValue global = JS_GetGlobalObject(mCtx);
        JSValue console = JS_NewObject(mCtx);
        JS_SetPropertyStr(mCtx, console, "log", JS_NewCFunction(mCtx, &JavaScript::log, "log", 1));
        JS_SetPropertyStr(mCtx, global, "console", console);
        JS_FreeValue(mCtx, global);
        static const char kStrict[] =
            "(function (target, path) {\n"
            "  return new Proxy(target, { get(o, k) {\n"
            "    if (typeof k === 'symbol' || k in o) return o[k];\n"
            "    throw new Error(path + '.' + k + ' is not a command');\n"
            "  } });\n"
            "})";
        mStrict = JS_Eval(mCtx, kStrict, sizeof(kStrict) - 1, "<host>", JS_EVAL_TYPE_GLOBAL);
    }
    ~JavaScript() override
    {
        for (auto &s : mSpaces) JS_FreeValue(mCtx, s.second);
        JS_FreeValue(mCtx, mStrict);
        JS_FreeContext(mCtx);
        JS_FreeRuntime(mRt);
    }

    Result run(const std::string &source, const std::string &name) override
    {
        install();
        Result r;
        JSValue v = JS_Eval(mCtx, source.c_str(), source.size(), name.c_str(), JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) {
            r.status = Status::Failed;
            r.message = takeException(mCtx);
        }
        JS_FreeValue(mCtx, v);
        return r;
    }

private:
    /// Every registered command as a function of its namespace's object (a re-install replaces the same functions). A
    /// namespace is seen by the script through a Proxy that FAILS on a name nothing registered (the strict host: an
    /// unregistered command throws naming itself, never JS's bare "not a function").
    void install()
    {
        for (const CommandInfo *c : mHost.registry().commands()) {
            const size_t dot = c->name.rfind('.');
            if (dot == std::string::npos) continue;   // a command is always under a root
            JSValue name = JS_NewStringLen(mCtx, c->name.data(), c->name.size());
            JSValue fn = JS_NewCFunctionData2(mCtx, &JavaScript::command, c->name.c_str() + dot + 1,
                                              int(c->signature.params.size()), 0, 1, &name);
            JS_FreeValue(mCtx, name);   // (the function holds its own reference)
            JS_SetPropertyStr(mCtx, space(c->name.substr(0, dot)), c->name.c_str() + dot + 1, fn);
        }
    }

    /// The target object of the namespace `path` ("engine.scene"), made (with its Proxy in its parent) the first time.
    JSValueConst space(const std::string &path)
    {
        for (const auto &s : mSpaces)
            if (s.first == path) return s.second;
        const size_t dot = path.rfind('.');
        JSValue parent = dot == std::string::npos ? JS_GetGlobalObject(mCtx) : JS_DupValue(mCtx, space(path.substr(0, dot)));
        JSValue target = JS_NewObject(mCtx);
        JSValue pathValue = JS_NewStringLen(mCtx, path.data(), path.size());
        JSValue args[2] = {target, pathValue};
        JSValue proxy = JS_Call(mCtx, mStrict, JS_UNDEFINED, 2, args);
        JS_FreeValue(mCtx, pathValue);
        JS_SetPropertyStr(mCtx, parent, path.c_str() + (dot == std::string::npos ? 0 : dot + 1), proxy);
        JS_FreeValue(mCtx, parent);
        mSpaces.emplace_back(path, target);
        return target;
    }

    static JSValue command(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int, JSValueConst *data)
    {
        JavaScript *self = static_cast<JavaScript *>(JS_GetContextOpaque(ctx));
        const std::string name = toUtf8(ctx, data[0]);
        while (argc > 0 && JS_IsUndefined(argv[argc - 1])) --argc;   // a trailing undefined is an absent argument
        Value::Array args;
        for (int i = 0; i < argc; ++i) {
            Value a;
            std::string why;
            if (!fromJs(ctx, argv[i], a, why, 0))
                return JS_ThrowTypeError(ctx, "%s: argument %d: %s", name.c_str(), i + 1, why.c_str());
            args.push_back(std::move(a));
        }
        const Result r = self->mHost.call(name, args);
        if (r.status == Status::Failed) return JS_ThrowPlainError(ctx, "%s", r.message.c_str());
        return toJs(ctx, r.value);
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
    JSValue mStrict = JS_UNDEFINED;                             // the namespaces' Proxy maker
    std::vector<std::pair<std::string, JSValue>> mSpaces;      // each namespace's target object, by path
};

}  // namespace

std::unique_ptr<Host::Language> detail::makeJavaScript(Host &host) { return std::make_unique<JavaScript>(host); }

}}}  // namespace jahshaka::engine::script
