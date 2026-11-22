// THE VALUE AND THE REGISTRY (SCRIPT-IN-ENGINE-1 stage 1): jahshaka/engine/Script.h says the laws. The standard library
// only — no script language is known here (source.script_neutral).
#include "jahshaka/engine/Script.h"

#include <atomic>
#include <cmath>
#include <cstring>
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

const char *accessName(Access a)
{
    switch (a) {
    case Access::Unset:   return "unset";
    case Access::Reads:   return "reads";
    case Access::Changes: return "changes";
    case Access::Files:   return "files";
    case Access::Test:    return "test";
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

Param Param::oneOf(std::vector<std::string> values) const
{
    Param p = *this;
    p.choices = std::move(values);
    return p;
}
Param Param::range(double lo, double hi) const
{
    Param p = *this;
    p.min = lo;
    p.max = hi;
    return p;
}
Param Param::atLeast(double lo) const
{
    Param p = *this;
    p.min = lo;
    return p;
}
Param Param::above(double lo) const
{
    Param p = *this;
    p.min = lo;
    p.minExclusive = true;
    return p;
}
Param Param::upTo(unsigned elements) const
{
    Param p = *this;
    p.maxCount = elements;
    return p;
}
Param Param::in(std::string unitName) const
{
    Param p = *this;
    p.unit = std::move(unitName);
    return p;
}
Param Param::allowNonFinite() const
{
    Param p = *this;
    p.nonFinite = true;
    return p;
}

Param handle(std::string name, std::string kind)
{
    Param p = arg(std::move(name), Kind::Int);
    p.handle = std::move(kind);
    return p;
}

Param handles(std::string name, std::string kind)
{
    Param p = arg(std::move(name), Kind::Array);
    p.element = Kind::Int;
    p.handle = std::move(kind);
    return p;
}

namespace {
// THE HANDLE'S BITS (Script.h, THE HANDLES): owner 12 | kind 8 | value 32 - 52 bits, exact in a script's number
constexpr int kOwnerShift = 40, kKindShift = 32;
constexpr std::uint64_t kValueMask = 0xffffffffull, kKindMask = 0xffull, kOwnerMask = 0xfffull;
}

Registry::Registry()
{
    static std::atomic<std::uint64_t> next{0};
    mSerial = next.fetch_add(1, std::memory_order_relaxed) % kOwnerMask + 1;   // 1..4095
}

unsigned Registry::kindIndex(const std::string &kind)
{
    for (size_t i = 0; i < mKinds.size(); ++i)
        if (mKinds[i] == kind) return unsigned(i + 1);
    if (mKinds.size() >= kKindMask) return 0;
    mKinds.push_back(kind);
    mKindHeld.push_back(false);
    mKindType.push_back(nullptr);
    mKindNamed.push_back(false);
    mChecks.emplace_back();
    return unsigned(mKinds.size());
}

std::int64_t Registry::holdAs(const std::string &kind, const std::type_info &type, std::shared_ptr<void> object,
                              bool scoped, std::string *why)
{
    const unsigned k = kindIndex(kind);
    const auto refuse = [why](std::string text) {
        if (why) *why = std::move(text);
        return std::int64_t(0);
    };
    if (!k) return refuse("no handle kind is left for '" + kind + "' (255 at most)");
    if (!object) return refuse("the " + kind + " is no object (a null pointer cannot be held)");
    if (mKindNamed[k - 1])
        return refuse("the " + kind + " kind names the engine's own ids: it holds no object");
    if (mKindType[k - 1] && *mKindType[k - 1] != type)
        return refuse("the " + kind + " kind is bound to another C++ type (one kind, one type: a " + kind +
                      " handle names only what its first hold named)");
    if (mHeld.size() >= kValueMask) return refuse("the " + kind + " could not be given a handle (the host's table is full)");
    mKindType[k - 1] = &type;
    mKindHeld[k - 1] = true;
    mHeld.push_back(Held{k, std::move(object), scoped});
    return std::int64_t(mSerial << kOwnerShift | std::uint64_t(k) << kKindShift | std::uint64_t(mHeld.size()));
}

std::int64_t Registry::name(const std::string &kind, std::uint32_t id, std::string *why)
{
    const unsigned k = kindIndex(kind);
    if (!k || mKindHeld[k - 1]) {   // (a held kind's value is a slot of the table: never minted from an id)
        if (why)
            *why = !k ? "no handle kind is left for '" + kind + "' (255 at most)"
                      : "the " + kind + " kind is HELD: its handles are made by holding a " + kind + ", never minted "
                        "from an id";
        return 0;
    }
    return std::int64_t(mSerial << kOwnerShift | std::uint64_t(k) << kKindShift | std::uint64_t(id));
}

bool Registry::release(std::int64_t h)
{
    const std::uint64_t u = std::uint64_t(h);
    const unsigned k = unsigned(u >> kKindShift & kKindMask);
    if (h <= 0 || (u >> kOwnerShift) != mSerial || !k || k > mKinds.size() || !mKindHeld[k - 1]) return false;
    const std::uint64_t slot = u & kValueMask;
    if (!slot || slot > mHeld.size() || mHeld[slot - 1].kind != k || !mHeld[slot - 1].object) return false;
    mHeld[slot - 1].object.reset();
    return true;
}

void Registry::releaseScoped()
{
    for (Held &h : mHeld)
        if (h.scoped) h.object.reset();
}

void *Registry::objectAs(std::int64_t h, const std::type_info &type) const
{
    const std::uint64_t u = std::uint64_t(h), slot = u & kValueMask;
    if (h <= 0 || (u >> kOwnerShift) != mSerial || !slot || slot > mHeld.size()) return nullptr;
    const Held &held = mHeld[slot - 1];
    if (held.kind != unsigned(u >> kKindShift & kKindMask)) return nullptr;
    const std::type_info *bound = mKindType[held.kind - 1];
    return bound && *bound == type ? held.object.get() : nullptr;   // (read only as the type its kind is bound to)
}

bool Registry::checkHandle(std::int64_t h, const std::string &kind, const std::string &where, std::string &why) const
{
    const std::uint64_t u = std::uint64_t(h);
    const unsigned k = unsigned(u >> kKindShift & kKindMask);
    if (h <= 0 || (u >> kOwnerShift) != mSerial || !k || k > mKinds.size()) {
        why = where + ": " + std::to_string(h) + " is no " + kind + " handle of this host (a plain number, or another "
              "host's or engine's handle)";
        return false;
    }
    if (mKinds[k - 1] != kind) {
        why = where + ": " + std::to_string(h) + " is a " + mKinds[k - 1] + " handle, not a " + kind + " handle";
        return false;
    }
    if (mKindHeld[k - 1]) {
        const std::uint64_t slot = u & kValueMask;
        if (!slot || slot > mHeld.size() || mHeld[slot - 1].kind != k) {
            why = where + ": " + std::to_string(h) + " is no " + kind + " handle of this host";
            return false;
        }
        if (!mHeld[slot - 1].object) {
            why = where + ": the " + kind + " handle " + std::to_string(h) + " is stale (its " + kind + " was released)";
            return false;
        }
    }
    return true;
}

void Registry::bindAs(const std::string &kind, const std::type_info *type)
{
    const unsigned k = kindIndex(kind);
    if (!k) {
        mFaults.push_back(kind + ": no handle kind is left to bind (255 at most)");
        return;
    }
    const bool held = mKindHeld[k - 1], conflict = type ? (mKindNamed[k - 1] || (mKindType[k - 1] && *mKindType[k - 1] != *type))
                                                        : held;
    if (conflict) {
        mFaults.push_back(kind + ": the kind is bound to another C++ type (or holds objects) before its registration "
                          "binds it - one kind, one type");
        return;
    }
    if (type) {
        mKindType[k - 1] = type;
        mKindHeld[k - 1] = true;
    } else {
        mKindNamed[k - 1] = true;
    }
}

void Registry::checkKind(const std::string &kind, HandleCheck check)
{
    const unsigned k = kindIndex(kind);
    if (k) mChecks[k - 1] = std::move(check);
}

bool Registry::judgeHandles(const Call &call, const Param &p, const Value &v, std::string &why) const
{
    // one handle (its kind checked at conform) asked of its kind's existence check, if the kind has one
    const auto exists = [&](const Value &h) {
        const unsigned k = unsigned(std::uint64_t(h.asInt()) >> kKindShift & kKindMask);
        return !(k && k <= mChecks.size() && mChecks[k - 1] &&
                 !mChecks[k - 1](call, std::uint32_t(std::uint64_t(h.asInt()) & kValueMask), why));
    };
    if (!p.handle.empty() && v.kind() == Kind::Int) {
        if (exists(v)) return true;
        why = p.name + ": " + why;
        return false;
    }
    if (!p.handle.empty() && v.kind() == Kind::Array) {   // handles(): EVERY element asked, a dead one named by its index
        const Value::Array &a = v.asArray();
        for (size_t i = 0; i < a.size(); ++i)
            if (!exists(a[i])) {
                why = p.name + "[" + std::to_string(i) + "]: " + why;
                return false;
            }
        return true;
    }
    if (p.kind == Kind::Object && v.kind() == Kind::Object) {
        for (const Param &f : p.fields)
            if (!judgeHandles(call, f, v[f.name], why)) return false;
    } else if (p.kind == Kind::Array && p.element == Kind::Object && v.kind() == Kind::Array) {
        for (const Value &x : v.asArray())
            for (const Param &f : p.fields)
                if (!judgeHandles(call, f, x[f.name], why)) return false;
    }
    return true;
}

Value Call::name(const std::string &kind, unsigned long long id)
{
    if (!id || id > kValueMask) return fail("the " + kind + " id " + std::to_string(id) + " does not fit a handle's 32 bits");
    std::string why;
    const std::int64_t h = mRegistry.name(kind, std::uint32_t(id), &why);
    return h ? Value(h) : fail(why);
}


namespace {

/// A double's number, as text without trailing zeros ("16384", "0.5").
std::string numberText(double d)
{
    std::string t = std::to_string(d);
    if (t.find('.') != std::string::npos) {
        while (t.back() == '0') t.pop_back();
        if (t.back() == '.') t.pop_back();
    }
    return t;
}

/// NaN or an infinity, READ BY ITS BITS (CODING_RULES § 2: never `x == x`, which an optimiser may fold): the exponent
/// all ones.
bool nonFiniteBits(double d)
{
    std::uint64_t u = 0;
    std::memcpy(&u, &d, sizeof u);
    return (u & 0x7ff0000000000000ull) == 0x7ff0000000000000ull;
}

/// The declared domain as text: ` "a"|"b"`, ` 1..16384`, ` >= 0`, then the unit.
std::string domainText(const Param &p)
{
    std::string t;
    for (size_t i = 0; i < p.choices.size(); ++i) t += (i ? "|\"" : " \"") + p.choices[i] + "\"";
    if (p.min && p.max && !p.minExclusive) {
        t += " " + numberText(*p.min) + ".." + numberText(*p.max);
    } else {
        if (p.min) t += (p.minExclusive ? " > " : " >= ") + numberText(*p.min);
        if (p.max) t += " <= " + numberText(*p.max);
    }
    if (!p.unit.empty()) t += " " + p.unit;
    if (p.nonFinite) t += " (non-finite allowed)";
    return t;
}

/// WHERE AN ARGUMENT SITS ("transform.position[0]"), as a chain on the stack: no string is built unless a check
/// fails (S9: the argument paths were a quarter of a call's cost, built on the success path).
struct Where {
    const Where *up = nullptr;
    const std::string *name = nullptr;   // a parameter or a member
    long index = -1;                     // an array's element
};
std::string whereText(const Where &w)
{
    std::string t = w.up ? whereText(*w.up) : std::string();
    if (w.name) t += (t.empty() ? "" : ".") + *w.name;
    else t += "[" + std::to_string(w.index) + "]";
    return t;
}

/// The value `v` (an Int or a Number, checked of its kind) inside `p`'s declared range and finite, else `why`.
bool inDomain(const Param &p, double v, const Where &where, std::string &why)
{
    if (!p.nonFinite && nonFiniteBits(v)) {
        why = whereText(where) + " is not a finite number (" + numberText(v) + ")";
        return false;
    }
    if ((p.min && (v < *p.min || (p.minExclusive && v == *p.min))) || (p.max && v > *p.max)) {
        why = whereText(where) + " " + numberText(v) + " is outside" + domainText(p);
        return false;
    }
    return true;
}

std::string paramText(const Param &p)
{
    std::string t = p.name + (p.target ? ": target " : ": ");
    if (p.kind == Kind::Array) {
        t += !p.handle.empty() ? p.handle + " handle" : p.element == Kind::Object ? "object" : kindName(p.element);
        t += p.count ? "[" + std::to_string(p.count) + "]" : "[]";
        if (p.maxCount) t += " <= " + std::to_string(p.maxCount);
    } else if (!p.handle.empty()) {
        t += p.handle + " handle";
    } else {
        t += kindName(p.kind);
    }
    t += domainText(p);
    if (!p.fields.empty()) {
        t += " {";
        for (size_t i = 0; i < p.fields.size(); ++i) t += (i ? ", " : "") + paramText(p.fields[i]);
        t += "}";
    }
    if (p.fallback) t += " = default";
    return t;
}

/// THE ARGUMENT CHECK: `in` against `p`, the checked value into `out`; false with `why` (naming `where`) on a mistake.
/// `in` is CONSUMED (moved into `out` wherever it already conforms: S9 - the registry copies nothing it can move).
bool conform(const Registry &reg, const Param &p, Value &in, const Where &where, Value &out, std::string &why);

/// `in` as `kind` - p.kind, or an array's element kind with the array's own declarations (its range, its members).
bool conformKind(const Registry &reg, Kind kind, const Param &p, Value &in, const Where &where, Value &out,
                 std::string &why)
{
    const Kind k = in.kind();
    switch (kind) {
    case Kind::Int:
        if (k == Kind::Int) {
            out = std::move(in);
        } else if (k == Kind::Number) {
            const double d = in.asNumber();
            // an integral number is an integer only while a script's number is exact: |d| <= 2^53 (above it, a
            // neighbouring integer reads the same: an Int value - a BigInt - says it exactly)
            if (nonFiniteBits(d) || std::trunc(d) != d) {
                why = whereText(where) + " must be an integer (" + numberText(d) + " is not; it is never truncated)";
                return false;
            }
            if (std::fabs(d) > 9007199254740992.0) {
                why = whereText(where) + " " + numberText(d) + " is beyond 2^53, where a number is no longer an exact integer";
                return false;
            }
            out = Value(static_cast<std::int64_t>(d));
        } else {
            break;
        }
        if ((p.min || p.max) && !inDomain(p, double(out.asInt()), where, why)) return false;
        if (!p.handle.empty() && !reg.checkHandle(out.asInt(), p.handle, std::string(), why)) {
            why = whereText(where) + why;   // (checkHandle's text begins ": ")
            return false;
        }
        return true;
    case Kind::Number:
        if (k == Kind::Int || k == Kind::Number) {
            out = Value(in.asNumber());
            return inDomain(p, out.asNumber(), where, why);
        }
        break;
    case Kind::String:
        if (k != Kind::String) break;
        if (!p.choices.empty()) {
            bool known = false;
            for (const std::string &c : p.choices) known = known || c == in.asString();
            if (!known) {
                why = whereText(where) + " '" + in.asString() + "' is not one of" + domainText(p);
                return false;
            }
        }
        out = std::move(in);
        return true;
    case Kind::Array: {
        if (k != Kind::Array || kind != p.kind) break;   // (no array of arrays is declared)
        Value::Array &a = in.asArray();
        if (p.count && a.size() != p.count) {
            why = whereText(where) + " must hold " + std::to_string(p.count) + " elements (it holds " +
                  std::to_string(a.size()) + ")";
            return false;
        }
        if (p.maxCount && a.size() > p.maxCount) {
            why = whereText(where) + " holds " + std::to_string(a.size()) + " elements, at most " +
                  std::to_string(p.maxCount);
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {   // each element checked IN PLACE: the array is moved, never rebuilt
            Value v;
            const Where element{&where, nullptr, long(i)};
            if (!conformKind(reg, p.element, p, a[i], element, v, why)) return false;   // the element kind, p's declarations
            a[i] = std::move(v);
        }
        out = std::move(in);
        return true;
    }
    case Kind::Object: {
        if (k != Kind::Object) break;
        Value::Object &o = in.asObject();
        for (const Value::Member &m : o) {
            bool known = false;
            for (const Param &f : p.fields) known = known || f.name == m.first;
            if (!known) {
                why = whereText(where) + " has no member '" + m.first + "'";
                return false;
            }
        }
        Value::Object checked;
        checked.reserve(p.fields.size());
        for (const Param &f : p.fields) {
            Value v, absent;
            Value *given = &absent;
            for (Value::Member &m : o)
                if (m.first == f.name) given = &m.second;
            const Where member{&where, &f.name, -1};
            if (!conform(reg, f, *given, member, v, why)) return false;   // (the member moved into the checked object)
            checked.emplace_back(f.name, std::move(v));
        }
        out = Value(std::move(checked));
        return true;
    }
    default:
        if (k == kind) { out = std::move(in); return true; }
        break;
    }
    why = whereText(where) + " must be " + (kind == Kind::Array ? std::string("an array") : std::string(kindName(kind))) +
          " (it is " + kindName(k) + ")";
    return false;
}

bool conform(const Registry &reg, const Param &p, Value &in, const Where &where, Value &out, std::string &why)
{
    if (in.isNull()) {
        if (!p.fallback) {
            why = whereText(where) + " is required";
            return false;
        }
        out = *p.fallback;   // a default was checked ONCE, at registration (settleDefaults: an object's filled there)
        return true;
    }
    return conformKind(reg, p.kind, p, in, where, out, why);
}

/// THE DEFAULTS CHECKED ONCE (S9): each parameter's and member's default conformed at registration - an object default
/// filled with its members' - so a call copies a checked default and checks nothing again. False, `why`, when a
/// default does not conform to its own parameter (a registration fault).
bool settleDefaults(const Registry &reg, Param &p, std::string &why)
{
    for (Param &f : p.fields)
        if (!settleDefaults(reg, f, why)) return false;
    if (!p.fallback || p.fallback->isNull()) return true;   // (a null default: the parameter's own null)
    Value in = *p.fallback, out;
    const Where where{nullptr, &p.name, -1};
    if (!conformKind(reg, p.kind, p, in, where, out, why)) return false;
    p.fallback = std::move(out);
    return true;
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

/// AN ARRAY DECLARES ITS BOUND (SCRIPT-IN-ENGINE-1 2b): its count or its maximum - so a language binding judges a
/// script's array length against it before converting a single element (a JS `a.length = 4e9` costs the script
/// nothing), and no command is handed an unbounded amount of work.
void arraysUnbounded(const std::string &command, const std::vector<Param> &params, std::vector<std::string> &out)
{
    for (const Param &p : params) {
        if (p.kind == Kind::Array && !p.count && !p.maxCount)
            out.push_back(command + ": the array '" + p.name + "' declares neither a count nor a maximum");
        arraysUnbounded(command, p.fields, out);
    }
}

}  // namespace

std::string signatureText(const CommandInfo &c)
{
    std::string t = c.name + "(";
    for (size_t i = 0; i < c.signature.params.size(); ++i) t += (i ? ", " : "") + paramText(c.signature.params[i]);
    return t + ") -> " + (c.signature.returnsHandle.empty() ? std::string(kindName(c.signature.returns))
                                                           : c.signature.returnsHandle + " handle") +
           " [" + accessName(c.signature.access) + "]";
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
    if (info.signature.access == Access::Unset) faults.push_back(info.name + ": no access tag");
    if (!fn) faults.push_back(info.name + ": no function");
    namesMissing(info.name, info.signature.params, faults);
    arraysUnbounded(info.name, info.signature.params, faults);
    int named = 0;
    for (const Param &p : info.signature.params) {
        if (p.target && named) faults.push_back(info.name + ": the target '" + p.name + "' follows a named parameter");
        if (!p.target) ++named;
    }
    if (named > 1)
        faults.push_back(info.name + ": " + std::to_string(named) + " positional parameters are not targets (one at "
                         "most: the rest are members of one options object)");
    for (Param &p : info.signature.params) {
        std::string why;
        if (!settleDefaults(*this, p, why)) faults.push_back(info.name + ": a default does not conform: " + why);
    }
    if (!faults.empty()) {
        mFaults.insert(mFaults.end(), faults.begin(), faults.end());
        return false;
    }
    mByName.emplace(info.name, mEntries.size());
    mEntries.push_back(Entry{std::move(info), std::move(fn)});
    return true;
}

const CommandInfo *Registry::find(const std::string &name) const
{
    const auto it = mByName.find(name);
    return it == mByName.end() ? nullptr : &mEntries[it->second].info;
}

std::vector<const CommandInfo *> Registry::commands() const
{
    std::vector<const CommandInfo *> out;
    for (const Entry &e : mEntries) out.push_back(&e.info);
    return out;
}

namespace {
Result failed(std::string message)
{
    Result r;
    r.status = Status::Failed;
    r.message = std::move(message);
    return r;
}
}  // namespace

Result Registry::call(const std::string &name, const Value::Array &args)
{
    const auto it = mByName.find(name);
    if (it != mByName.end()) return callAt(it->second, args);
    return failed(name + " is not a command");
}

Result Registry::callNamed(const std::string &name, Value::Object args)
{
    const auto it = mByName.find(name);
    if (it == mByName.end()) return failed(name + " is not a command");
    const CommandInfo &info = mEntries[it->second].info;
    const std::vector<Param> &params = info.signature.params;
    Value::Array positional(params.size());   // null: absent (conform gives its default, or "is required")
    std::vector<bool> given(params.size(), false);
    for (Value::Member &m : args) {
        std::size_t i = 0;
        while (i < params.size() && params[i].name != m.first) ++i;   // the SIGNATURE's order decides the position
        if (i == params.size())
            return failed(name + ": has no parameter '" + m.first + "' (" + signatureText(info) + ")");
        if (given[i]) return failed(name + ": the parameter '" + m.first + "' is named twice");
        given[i] = true;
        positional[i] = std::move(m.second);
    }
    return callAt(it->second, std::move(positional));
}

Result Registry::callAt(std::size_t index, Value::Array args)
{
    Result r;
    if (index >= mEntries.size()) {
        r.status = Status::Failed;
        r.message = "command " + std::to_string(index) + " is not a command";
        return r;
    }
    const Entry *entry = &mEntries[index];
    const std::string &name = entry->info.name;
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
        const Where where{nullptr, &params[i].name, -1};
        Value absent;
        if (!conform(*this, params[i], i < args.size() ? args[i] : absent, where, v, why)) {
            r.status = Status::Failed;
            r.message = name + ": " + why;
            return r;
        }
        checked.push_back(std::move(v));
    }
    Call call(entry->info, std::move(checked), *this);
    // THE EXISTENCE CHECKS of the named kinds (checkKind: an engine read) and then the command, both inside the try - a
    // C++ exception from either is a Failed call by name, never an unwind through a language's C
    try {
        for (size_t i = 0; i < params.size(); ++i) {
            std::string why;
            if (!judgeHandles(call, params[i], call[i], why)) {
                r.status = Status::Failed;
                r.message = name + ": " + why;
                return r;
            }
        }
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
