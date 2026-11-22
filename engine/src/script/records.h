#pragma once
// ONE FIELD TABLE PER RECORD (SCRIPT-IN-ENGINE-1 2b b1; the lead's ruling 5) - an engine record (GiParams, LightDesc,
// PublishedFrame ...) carried as a script value without hand-writing its fields twice. Private to the engine.
//
// A RECORD's TABLE names every field ONCE, in declaration order:
//     field("cascadeCount", &GiParams::cascadeCount).range(0, 4)
// The member pointer gives the C++ type, so the field's Kind, its signature Param (with the record's default as the
// member's default), and both conversions are DERIVED. Generic over: bool, integers (their type's range, within 2^53),
// float (a number past a float's range FAILS by name at the conversion) and double, std::string, an enum (a name table, JAH_ENUM_NAMES), Vec3 / Quat / Colour (3 / 4 / 4
// numbers: x y z, x y z w, r g b a), a nested record, std::vector<T> (its maximum declared on the field: .upTo(n), in
// the array's VALUES - a flat vector's numbers; Registry::add refuses an unbounded array), T[N], std::optional<T> and std::shared_ptr<const T> (null = absent). A
// vector or an array of Vec3 / Quat / Colour is FLAT: 3 / 4 / 4 numbers an element (the batch convention).
//
// THE GUARD: JAH_RECORD_COUNT(T, N) - beside the table's declaration, in the subject's header - expands a structured
// binding of EXACTLY N names (records_bind.inc, generated up to 128), so a field added to T BREAKS THE BUILD until its
// table names it; Table::inOrder (the CPU case) then checks each entry's byte offset against the binding's field, in
// declaration order (a swapped, missing or misnamed entry fails by name). A record with a base class, a union, a
// private or a bit-field member cannot be bound (Types.h has none: the stage-2 preflight).
//
// The tables live in fields_<subject>.cpp (each including `jahshaka/engine/Types.h` path-qualified, one line);
// a subject's header declares its records with JAH_RECORD_COUNT and its enums with JAH_ENUM_NAMES. The commands that
// take or answer a record use Table::param / toValue / fromValue.
#include "jahshaka/engine/Script.h"
#include "jahshaka/engine/Types.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace jahshaka { namespace engine { namespace script {

namespace detail {
#include "script/records_bind.inc"
}  // namespace detail

template <class R> class Table;
/// A RECORD: specialised by JAH_RECORD_COUNT (its field count, its table's declaration); the table is defined in its
/// subject's fields_<subject>.cpp. A record type used without it does not compile.
template <class R> struct Record;
/// AN ENUM's NAMES: specialised by JAH_ENUM_NAMES, every enumerator named.
template <class E> struct EnumNames;

#define JAH_RECORD_COUNT(T, N)                                                                                          \
    template <> struct Record<T> {                                                                                      \
        static constexpr std::size_t count = N;                                                                         \
        static const Table<T> &table();                                                                                 \
    };                                                                                                                  \
    static_assert(std::tuple_size<decltype(detail::bindFields(std::declval<const T &>(),                                \
                                                              std::integral_constant<std::size_t, N>()))>::value == N, \
                  #T " has a field count other than " #N ": its table names every field")
#define JAH_ENUM_NAMES(E, ...)                                                                                          \
    template <> struct EnumNames<E> {                                                                                   \
        static const std::vector<std::pair<E, const char *>> &all()                                                     \
        {                                                                                                               \
            static const std::vector<std::pair<E, const char *>> names = __VA_ARGS__;                                  \
            return names;                                                                                               \
        }                                                                                                               \
    }

namespace detail {

template <class T> struct IsVector : std::false_type {};
template <class T, class A> struct IsVector<std::vector<T, A>> : std::true_type { using Element = T; };
template <class T> struct IsOptional : std::false_type {};
template <class T> struct IsOptional<std::optional<T>> : std::true_type { using Element = T; };
template <class T> struct IsShared : std::false_type {};
template <class T> struct IsShared<std::shared_ptr<const T>> : std::true_type { using Element = T; };
/// An array's or a vector's element type.
template <class T> struct ElementOf { using type = typename IsVector<T>::Element; };
template <class T, std::size_t N> struct ElementOf<T[N]> { using type = T; };

/// Vec3 / Quat / Colour: their numbers (0 = none of them).
template <class T> constexpr unsigned flatWidth()
{
    return std::is_same<T, Vec3>::value ? 3u : std::is_same<T, Quat>::value || std::is_same<T, Colour>::value ? 4u : 0u;
}
inline void flatOut(const Vec3 &v, Value::Array &a) { a.insert(a.end(), {Value(double(v.x)), Value(double(v.y)), Value(double(v.z))}); }
inline void flatOut(const Quat &q, Value::Array &a)
{
    a.insert(a.end(), {Value(double(q.x)), Value(double(q.y)), Value(double(q.z)), Value(double(q.w))});
}
inline void flatOut(const Colour &c, Value::Array &a)
{
    a.insert(a.end(), {Value(double(c.r)), Value(double(c.g)), Value(double(c.b)), Value(double(c.a))});
}
inline float numberAt(const Value::Array &a, std::size_t i) { return float(a[i].asNumber()); }
/// Every number of `a` inside a float's range, else false with `why` naming the first (a double past it would narrow to
/// an infinity, silently).
inline bool floats(const Value::Array &a, const std::string &where, std::string &why)
{
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::fabs(a[i].asNumber()) > double(FLT_MAX)) {
            why = where + "[" + std::to_string(i) + "] " + std::to_string(a[i].asNumber()) + " is beyond a float";
            return false;
        }
    return true;
}
inline void flatIn(const Value::Array &a, std::size_t i, Vec3 &v) { v = Vec3(numberAt(a, i), numberAt(a, i + 1), numberAt(a, i + 2)); }
inline void flatIn(const Value::Array &a, std::size_t i, Quat &q)
{
    q = Quat(numberAt(a, i), numberAt(a, i + 1), numberAt(a, i + 2), numberAt(a, i + 3));
}
inline void flatIn(const Value::Array &a, std::size_t i, Colour &c)
{
    c = Colour(numberAt(a, i), numberAt(a, i + 1), numberAt(a, i + 2), numberAt(a, i + 3));
}

constexpr double kExact = 9007199254740992.0;   // 2^53: a script's number is an exact integer up to it

}  // namespace detail

/// THE PARAMETER a C++ type derives (no default: Field gives the record's).
template <class T> Param paramOf(std::string name)
{
    if constexpr (std::is_same<T, bool>::value) {
        return arg(std::move(name), Kind::Bool);
    } else if constexpr (std::is_enum<T>::value) {
        std::vector<std::string> names;
        for (const auto &n : EnumNames<T>::all()) names.push_back(n.second);
        return arg(std::move(name), Kind::String).oneOf(std::move(names));
    } else if constexpr (std::is_integral<T>::value) {
        const double lo = std::is_signed<T>::value ? std::max(double(std::numeric_limits<T>::min()), -detail::kExact) : 0.0;
        const double hi = std::min(double(std::numeric_limits<T>::max()), detail::kExact);
        return arg(std::move(name), Kind::Int).range(lo, hi);
    } else if constexpr (std::is_floating_point<T>::value) {
        return arg(std::move(name), Kind::Number);
    } else if constexpr (std::is_same<T, std::string>::value) {
        return arg(std::move(name), Kind::String);
    } else if constexpr (detail::flatWidth<T>() != 0) {
        return numbers(std::move(name), detail::flatWidth<T>());
    } else if constexpr (detail::IsOptional<T>::value || detail::IsShared<T>::value) {
        Param p = paramOf<typename std::conditional<detail::IsOptional<T>::value, detail::IsOptional<T>,
                                                    detail::IsShared<T>>::type::Element>(std::move(name));
        p.fallback = Value();   // null: absent
        return p;
    } else if constexpr (detail::IsVector<T>::value || std::is_array<T>::value) {
        using E = typename detail::ElementOf<T>::type;
        const unsigned n = std::is_array<T>::value ? unsigned(std::extent<T>::value) : 0u;
        Param e = paramOf<E>(name);
        static_assert(!detail::IsVector<E>::value && !std::is_array<E>::value && !detail::IsOptional<E>::value &&
                          !detail::IsShared<E>::value,
                      "an array of arrays or of optionals is not carried");
        Param p = e;
        p.kind = Kind::Array;
        p.fallback.reset();
        if constexpr (detail::flatWidth<E>() != 0) {   // FLAT: an element's numbers in a row
            p.element = Kind::Number;
            p.count = n * detail::flatWidth<E>();
        } else {
            p.element = e.kind;
            p.count = n;
        }
        p.maxCount = 0;   // (a vector's maximum is the table's: Field::upTo)
        return p;
    } else {
        return Record<T>::table().param(std::move(name));
    }
}

/// A C++ value as a script value.
template <class T> Value toValue(const T &v)
{
    if constexpr (std::is_same<T, bool>::value) {
        return Value(v);
    } else if constexpr (std::is_enum<T>::value) {
        for (const auto &n : EnumNames<T>::all())
            if (n.first == v) return Value(n.second);
        return Value(std::int64_t(v));   // an unnamed value (its table is incomplete: the round-trip case finds it)
    } else if constexpr (std::is_integral<T>::value) {
        return Value(std::int64_t(v));
    } else if constexpr (std::is_floating_point<T>::value) {
        return Value(double(v));
    } else if constexpr (std::is_same<T, std::string>::value) {
        return Value(v);
    } else if constexpr (detail::flatWidth<T>() != 0) {
        Value::Array a;
        detail::flatOut(v, a);
        return Value(std::move(a));
    } else if constexpr (detail::IsOptional<T>::value || detail::IsShared<T>::value) {
        return v ? toValue(*v) : Value();
    } else if constexpr (detail::IsVector<T>::value || std::is_array<T>::value) {
        Value::Array a;
        for (const auto &e : v) {
            if constexpr (detail::flatWidth<typename std::decay<decltype(e)>::type>() != 0) detail::flatOut(e, a);
            else a.push_back(toValue(e));
        }
        return Value(std::move(a));
    } else {
        return Record<T>::table().toValue(v);
    }
}

/// A script value - conformed to paramOf<T>'s parameter by the registry - into `out`; false with `why` (naming
/// `where`) for what the parameter cannot say: a flat vector whose length is no multiple of its element's numbers.
template <class T> bool fromValue(const Value &v, T &out, const std::string &where, std::string &why)
{
    if constexpr (std::is_same<T, bool>::value) {
        out = v.asBool();
    } else if constexpr (std::is_enum<T>::value) {
        for (const auto &n : EnumNames<T>::all())
            if (v.asString() == n.second) {
                out = n.first;
                return true;
            }
        why = where + " '" + v.asString() + "' names no value";
        return false;
    } else if constexpr (std::is_integral<T>::value) {
        out = T(v.asInt());
    } else if constexpr (std::is_same<T, float>::value) {
        if (std::fabs(v.asNumber()) > double(FLT_MAX)) {
            why = where + " " + std::to_string(v.asNumber()) + " is beyond a float";
            return false;
        }
        out = float(v.asNumber());
    } else if constexpr (std::is_floating_point<T>::value) {
        out = T(v.asNumber());
    } else if constexpr (std::is_same<T, std::string>::value) {
        out = v.asString();
    } else if constexpr (detail::flatWidth<T>() != 0) {
        if (!detail::floats(v.asArray(), where, why)) return false;
        detail::flatIn(v.asArray(), 0, out);
    } else if constexpr (detail::IsOptional<T>::value) {
        if (v.isNull()) out.reset();
        else return fromValue(v, out.emplace(), where, why);
    } else if constexpr (detail::IsShared<T>::value) {
        if (v.isNull()) {
            out.reset();
        } else {
            auto made = std::make_shared<typename detail::IsShared<T>::Element>();
            if (!fromValue(v, *made, where, why)) return false;
            out = std::move(made);
        }
    } else if constexpr (detail::IsVector<T>::value || std::is_array<T>::value) {
        using E = typename std::decay<decltype(*std::begin(out))>::type;
        const Value::Array &a = v.asArray();
        constexpr unsigned w = detail::flatWidth<E>();
        const std::size_t n = w ? a.size() / w : a.size();
        if (w && a.size() % w) {
            why = where + " holds " + std::to_string(a.size()) + " numbers, not " + std::to_string(w) + " an element";
            return false;
        }
        if (w && !detail::floats(a, where, why)) return false;
        if constexpr (detail::IsVector<T>::value) out.assign(n, E{});
        for (std::size_t i = 0; i < n; ++i) {
            if constexpr (w != 0) detail::flatIn(a, i * w, out[i]);
            else if (!fromValue(a[i], out[i], where + "[" + std::to_string(i) + "]", why)) return false;
        }
    } else {
        return Record<T>::table().fromValue(v, out, where, why);
    }
    return true;
}

/// ONE FIELD of a record R: its derived parameter (the record's default as its default), its byte offset (the
/// guard's), and its two conversions. The domain methods narrow its parameter (an array's apply to its elements).
template <class R> struct Field {
    Param param;
    std::size_t offset = 0;
    std::function<Value(const R &)> get;
    std::function<bool(const Value &, R &, const std::string &where, std::string &why)> set;

    Field range(double lo, double hi) const { return with(param.range(lo, hi)); }
    Field atLeast(double lo) const { return with(param.atLeast(lo)); }
    Field above(double lo) const { return with(param.above(lo)); }
    Field upTo(unsigned elements) const { return with(param.upTo(elements)); }
    Field in(std::string unit) const { return with(param.in(std::move(unit))); }
    Field allowNonFinite() const { return with(param.allowNonFinite()); }

private:
    Field with(Param p) const
    {
        Field f = *this;
        f.param = std::move(p);
        return f;
    }
};

/// THE ENTRY: `name` and the member pointer; everything else derived.
template <class R, class M> Field<R> field(std::string name, M R::*member)
{
    static_assert(std::is_default_constructible<R>::value, "a record has a default (its members' defaults)");
    const R probe{};
    Field<R> f;
    f.param = paramOf<M>(std::move(name));
    f.param.fallback = toValue(probe.*member);   // an absent member takes the record's default
    f.offset = std::size_t(reinterpret_cast<const char *>(&(probe.*member)) - reinterpret_cast<const char *>(&probe));
    f.get = [member](const R &r) { return toValue(r.*member); };
    f.set = [member](const Value &v, R &r, const std::string &where, std::string &why) {
        return fromValue(v, r.*member, where, why);
    };
    return f;
}

/// A RECORD's TABLE: its fields in declaration order.
template <class R> class Table {
public:
    explicit Table(std::vector<Field<R>> fields) : mFields(std::move(fields)) {}
    const std::vector<Field<R>> &fields() const { return mFields; }

    /// The record as a parameter (or a member) `name`: an object of every field, each defaulting to the record's,
    /// the whole defaulting to the record's default.
    Param param(std::string name) const
    {
        std::vector<Param> members;
        for (const Field<R> &f : mFields) members.push_back(f.param);
        return object(std::move(name), std::move(members), Value(Value::Object{}));
    }
    Value toValue(const R &r) const
    {
        Value::Object o;
        o.reserve(mFields.size());
        for (const Field<R> &f : mFields) o.emplace_back(f.param.name, f.get(r));
        return Value(std::move(o));
    }
    /// `v` conformed to param(): each member present (the registry filled the defaults) set; one absent keeps `out`'s.
    bool fromValue(const Value &v, R &out, const std::string &where, std::string &why) const
    {
        for (const Field<R> &f : mFields)
            if (const Value *m = v.find(f.param.name))
                if (!f.set(*m, out, where.empty() ? f.param.name : where + "." + f.param.name, why)) return false;
        return true;
    }
    /// THE GUARD's second half (a CPU case): the table holds exactly the record's fields, and entry i is the record's
    /// field i - its byte offset the structured binding's, in declaration order. False with `why` naming the first
    /// that is not.
    bool inOrder(std::string &why) const
    {
        const R probe{};
        const auto bound = detail::bindFields(probe, std::integral_constant<std::size_t, Record<R>::count>());
        if (mFields.size() != bound.size()) {
            why = "the table names " + std::to_string(mFields.size()) + " fields of " + std::to_string(bound.size());
            return false;
        }
        for (std::size_t i = 0; i < bound.size(); ++i) {
            const std::size_t at = std::size_t(static_cast<const char *>(bound[i]) - reinterpret_cast<const char *>(&probe));
            if (mFields[i].offset != at) {
                why = "entry " + std::to_string(i) + " '" + mFields[i].param.name + "' is not the record's field " +
                      std::to_string(i) + " (its offset " + std::to_string(mFields[i].offset) + ", the field's " +
                      std::to_string(at) + ")";
                return false;
            }
        }
        return true;
    }

private:
    std::vector<Field<R>> mFields;
};

}}}  // namespace jahshaka::engine::script
