// THE VALUE AND THE REGISTRY (SCRIPT-IN-ENGINE-1 stage 1): jahshaka/engine/Script.h says the laws. The standard library
// only — no script language is known here (source.script_neutral).
#include "jahshaka/engine/Script.h"

#include <cmath>
#include <exception>

namespace jahshaka { namespace engine { namespace script {

const char *kindName(Kind k)
{
    switch (k) {
    case Kind::Null:   return "null";
    case Kind::Bool:   return "bool";
    case Kind::Int:    return "int";
    case Kind::Number: return "number";
    case Kind::String: return "string";
    case Kind::Array:  return "array";
    case Kind::Object: return "object";
    case Kind::Bytes:  return "bytes";
    }
    return "?";
}

Value Value::bytes(Bytes b)
{
    Value v;
    v.mV = std::move(b);
    return v;
}

Kind Value::kind() const
{
    static const Kind kinds[] = {Kind::Null, Kind::Bool, Kind::Int, Kind::Number,
                                 Kind::String, Kind::Array, Kind::Object, Kind::Bytes};
    return kinds[mV.index()];
}

double Value::asNumber() const
{
    return kind() == Kind::Int ? double(std::get<std::int64_t>(mV)) : std::get<double>(mV);
}

const Value *Value::find(const std::string &name) const
{
    if (kind() != Kind::Object) return nullptr;
    for (const Member &m : asObject())
        if (m.first == name) return &m.second;
    return nullptr;
}

const Value &Value::operator[](const std::string &name) const
{
    static const Value null;
    const Value *v = find(name);
    return v ? *v : null;
}

bool Value::operator==(const Value &o) const
{
    const Kind a = kind(), b = o.kind();
    const bool an = a == Kind::Int || a == Kind::Number, bn = b == Kind::Int || b == Kind::Number;
    if (an && bn) {
        if (a == Kind::Int && b == Kind::Int) return asInt() == o.asInt();
        return asNumber() == o.asNumber();
    }
    return mV == o.mV;
}

Param arg(std::string name, Kind kind)
{
    Param p;
    p.name = std::move(name);
    p.kind = kind;
    return p;
}

Param arg(std::string name, Kind kind, Value fallback)
{
    Param p = arg(std::move(name), kind);
    p.fallback = std::move(fallback);
    return p;
}

Param target(std::string name, Kind kind)
{
    Param p = arg(std::move(name), kind);
    p.target = true;
    return p;
}

Param target(Param p)
{
    p.target = true;
    return p;
}

Param numbers(std::string name, unsigned count)
{
    Param p = arg(std::move(name), Kind::Array);
    p.count = count;
    return p;
}

Param numbers(std::string name, unsigned count, Value fallback)
{
    Param p = numbers(std::move(name), count);
    p.fallback = std::move(fallback);
    return p;
}

Param object(std::string name, std::vector<Param> fields)
{
    Param p = arg(std::move(name), Kind::Object);
    p.fields = std::move(fields);
    return p;
}

Param object(std::string name, std::vector<Param> fields, Value fallback)
{
    Param p = object(std::move(name), std::move(fields));
    p.fallback = std::move(fallback);
    return p;
}

Param objects(std::string name, std::vector<Param> fields)
{
    Param p = arg(std::move(name), Kind::Array);
    p.element = Kind::Object;
    p.fields = std::move(fields);
    return p;
}

namespace {

std::string paramText(const Param &p)
{
    std::string t = p.name + (p.target ? ": target " : ": ");
    if (p.kind == Kind::Array) {
        t += p.element == Kind::Object ? "object" : kindName(p.element);
        t += p.count ? "[" + std::to_string(p.count) + "]" : "[]";
    } else {
        t += kindName(p.kind);
    }
    if (!p.fields.empty()) {
        t += " {";
        for (size_t i = 0; i < p.fields.size(); ++i) t += (i ? ", " : "") + paramText(p.fields[i]);
        t += "}";
    }
    if (p.fallback) t += " = default";
    return t;
}

/// THE ARGUMENT CHECK: `in` against `p`, the checked value into `out`; false with `why` (naming `where`) on a mistake.
bool conform(const Param &p, const Value &in, const std::string &where, Value &out, std::string &why);

bool conformKind(Kind kind, const Param &p, const Value &in, const std::string &where, Value &out, std::string &why)
{
    const Kind k = in.kind();
    switch (kind) {
    case Kind::Int:
        if (k == Kind::Int) { out = in; return true; }
        if (k == Kind::Number) {
            const double d = in.asNumber();
            // 2^63 is exact in a double: [-2^63, 2^63) is every int64
            if (std::trunc(d) == d && d >= -9223372036854775808.0 && d < 9223372036854775808.0) {
                out = Value(static_cast<std::int64_t>(d));
                return true;
            }
            why = where + " must be an integer (" + std::to_string(d) + " is not; it is never truncated)";
            return false;
        }
        break;
    case Kind::Number:
        if (k == Kind::Int || k == Kind::Number) { out = Value(in.asNumber()); return true; }
        break;
    case Kind::Array: {
        if (k != Kind::Array) break;
        const Value::Array &a = in.asArray();
        if (p.count && a.size() != p.count) {
            why = where + " must hold " + std::to_string(p.count) + " elements (it holds " + std::to_string(a.size()) + ")";
            return false;
        }
        Param e = p;   // each element: the element kind, the same members
        e.kind = p.element;
        e.fallback.reset();
        Value::Array checked;
        checked.reserve(a.size());
        for (size_t i = 0; i < a.size(); ++i) {
            Value v;
            if (!conformKind(e.kind, e, a[i], where + "[" + std::to_string(i) + "]", v, why)) return false;
            checked.push_back(std::move(v));
        }
        out = Value(std::move(checked));
        return true;
    }
    case Kind::Object: {
        if (k != Kind::Object) break;
        const Value::Object &o = in.asObject();
        for (const Value::Member &m : o) {
            bool known = false;
            for (const Param &f : p.fields) known = known || f.name == m.first;
            if (!known) {
                why = where + " has no member '" + m.first + "'";
                return false;
            }
        }
        Value::Object checked;
        for (const Param &f : p.fields) {
            Value v;
            if (!conform(f, in[f.name], where + "." + f.name, v, why)) return false;
            checked.emplace_back(f.name, std::move(v));
        }
        out = Value(std::move(checked));
        return true;
    }
    default:
        if (k == kind) { out = in; return true; }
        break;
    }
    why = where + " must be " + (kind == Kind::Array ? std::string("an array") : std::string(kindName(kind))) +
          " (it is " + kindName(k) + ")";
    return false;
}

bool conform(const Param &p, const Value &in, const std::string &where, Value &out, std::string &why)
{
    if (in.isNull()) {
        if (!p.fallback) {
            why = where + " is required";
            return false;
        }
        return conformKind(p.kind, p, *p.fallback, where, out, why);   // a default is checked too: an object's fills
    }
    return conformKind(p.kind, p, in, where, out, why);
}

Value falsy(Kind k)
{
    switch (k) {
    case Kind::Bool:   return Value(false);
    case Kind::Int:    return Value(0);
    case Kind::Number: return Value(0.0);
    default:           return Value();
    }
}

void namesMissing(const std::string &command, const std::vector<Param> &params, std::vector<std::string> &out)
{
    for (const Param &p : params) {
        if (p.name.empty()) out.push_back(command + ": a parameter or member has no name");
        namesMissing(command, p.fields, out);
    }
}

}  // namespace

std::string signatureText(const CommandInfo &c)
{
    std::string t = c.name + "(";
    for (size_t i = 0; i < c.signature.params.size(); ++i) t += (i ? ", " : "") + paramText(c.signature.params[i]);
    return t + ") -> " + kindName(c.signature.returns);
}

Value Call::fail(std::string message)
{
    mStatus = Status::Failed;
    mMessage = mInfo.name + ": " + message;
    return Value();
}

Value Call::refuse(std::string message)
{
    mStatus = Status::Refused;
    mMessage = mInfo.name + ": " + message;
    return falsy(mInfo.signature.returns);
}

bool Registry::add(CommandInfo info, CommandFn fn)
{
    std::vector<std::string> faults;
    if (info.name.empty()) faults.push_back("a command has no name");
    else if (find(info.name))
        faults.push_back(info.name + ": registered twice (the second refused: one host's commands serve one engine)");
    if (info.doc.empty()) faults.push_back(info.name + ": no doc");
    if (!fn) faults.push_back(info.name + ": no function");
    namesMissing(info.name, info.signature.params, faults);
    int named = 0;
    for (const Param &p : info.signature.params) {
        if (p.target && named) faults.push_back(info.name + ": the target '" + p.name + "' follows a named parameter");
        if (!p.target) ++named;
    }
    if (named > 1)
        faults.push_back(info.name + ": " + std::to_string(named) + " positional parameters are not targets (one at "
                         "most: the rest are members of one options object)");
    if (!faults.empty()) {
        mFaults.insert(mFaults.end(), faults.begin(), faults.end());
        return false;
    }
    mEntries.push_back(Entry{std::move(info), std::move(fn)});
    return true;
}

const CommandInfo *Registry::find(const std::string &name) const
{
    for (const Entry &e : mEntries)
        if (e.info.name == name) return &e.info;
    return nullptr;
}

std::vector<const CommandInfo *> Registry::commands() const
{
    std::vector<const CommandInfo *> out;
    for (const Entry &e : mEntries) out.push_back(&e.info);
    return out;
}

Result Registry::call(const std::string &name, const Value::Array &args)
{
    Result r;
    const Entry *entry = nullptr;
    for (const Entry &e : mEntries)
        if (e.info.name == name) entry = &e;
    if (!entry) {
        r.status = Status::Failed;
        r.message = name + " is not a command";
        return r;
    }
    const std::vector<Param> &params = entry->info.signature.params;
    if (args.size() > params.size()) {
        r.status = Status::Failed;
        r.message = name + ": " + std::to_string(args.size()) + " arguments given, it takes " +
                    std::to_string(params.size()) + " (" + signatureText(entry->info) + ")";
        return r;
    }
    Value::Array checked;
    checked.reserve(params.size());
    for (size_t i = 0; i < params.size(); ++i) {
        Value v;
        std::string why;
        if (!conform(params[i], i < args.size() ? args[i] : Value(), params[i].name, v, why)) {
            r.status = Status::Failed;
            r.message = name + ": " + why;
            return r;
        }
        checked.push_back(std::move(v));
    }
    Call call(entry->info, std::move(checked));
    try {
        r.value = entry->fn(call);
        r.status = call.status();
        r.message = call.message();
    } catch (const std::exception &e) {
        r.status = Status::Failed;
        r.message = name + ": threw: " + e.what();
    } catch (...) {
        r.status = Status::Failed;
        r.message = name + ": threw an exception that is not a std::exception";
    }
    if (r.status == Status::Failed) r.value = Value();
    if (r.status == Status::Refused) {
        r.value = falsy(entry->info.signature.returns);
        mLastError = r.message;
    }
    return r;
}

}}}  // namespace jahshaka::engine::script
