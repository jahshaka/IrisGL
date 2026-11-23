#pragma once
// ONE FIELD TABLE PER RECORD (SCRIPT-IN-ENGINE-1 2b b1; the lead's ruling 5) - an engine record (GiParams, LightDesc,
// PublishedFrame ...) carried as a script value without hand-writing its fields twice. Private to the engine.
//
// A RECORD's TABLE names every field ONCE, in declaration order:
//     field("cascadeCount", &GiParams::cascadeCount).range(0, 4)
// The member pointer gives the C++ type, so the field's Kind, its signature Param (with the record's default as the
// member's default), and both conversions are DERIVED. Generic over: bool, integers (their type's range, within 2^53),
// float (single precision: printed `float`, a number past its range FAILS by name - the registry's check) and double,
// std::string, an enum (a name table, JAH_ENUM_NAMES), Vec3 / Quat / Colour (3 / 4 / 4 floats: x y z, x y z w,
// r g b a), a nested record, std::vector<T> (its maximum declared on the field, .upTo(n), in the array's VALUES - a flat
// vector's numbers; Registry::add refuses an unbounded array), T[N], std::optional<T> and std::shared_ptr<const T>
// (null = absent). A vector or an array of Vec3 / Quat / Colour is FLAT: its items' numbers in a row (the batch
// convention: Param::flat, printed `float[3n] <= N numbers`; a length no multiple of the stride FAILS by name - the
// registry's check).
//
// THE GUARD: JAH_RECORD_COUNT(T, N) - beside the table's declaration, in the subject's header - expands a structured
// binding of EXACTLY N names (records_bind.inc, generated up to 128), so a field added to T BREAKS THE BUILD until its
// table names it; Table::inOrder (the CPU case) then checks each entry's byte offset against the binding's field, in
// declaration order (a swapped, missing or misnamed entry fails by name). A record with a base class, a union, a
// private or a bit-field member cannot be bound (the engine's records had none at the stage-2 preflight).
//
// KEYED BY FAMILY (SCRIPT-IN-ENGINE-1 stage 2: two families share a subject header - LightDesc and DecalDesc are both
// Lights.h's): a family's records are declared in records_<family>.h - JAH_RECORD_COUNT beside each table's declaration,
// JAH_ENUM_NAMES for its enums, the subject headers it needs included path-qualified ("jahshaka/engine/Lights.h") - and
// their tables live in fields_<family>.cpp. The commands that take or answer a record use Table::param / toValue /
// fromValue; an engine id a record carries (a TextureId, a NodeId) is a typed handle: Field::handle.
#include "jahshaka/engine/Script.h"
#include "jahshaka/engine/Math.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
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

/// AN ARRAY CARRIED AS BYTES (SCRIPT-IN-ENGINE-1 E0; RULED 3: the big arrays as `bytes`, under the call's byte bound): a
/// vector or T[N] of a trivially copyable element that is no array itself (never bool) - its elements' bytes as the
/// engine holds them (this machine's byte order; a record element in its declared layout). Field::bytes() carries a
/// member so; an array OF such arrays (MeshData::lodIndices, GiVoxelVolume's levels) is always an array of bytes, and
/// an array of VECTORS of such arrays (GiVoxelVolume::directional[3]) an array of PACKED bytes (PackedOf, below).
template <class T> struct BytesOf : std::false_type {};
template <class E, class A>
struct BytesOf<std::vector<E, A>>
    : std::integral_constant<bool, std::is_trivially_copyable<E>::value && !std::is_array<E>::value &&
                                       !IsVector<E>::value && !std::is_same<E, bool>::value> {
    using Element = E;
};
template <class E, std::size_t N>
struct BytesOf<E[N]> : std::integral_constant<bool, std::is_trivially_copyable<E>::value && !std::is_array<E>::value &&
                                                       !std::is_same<E, bool>::value> {
    using Element = E;
};
template <class T> Value bytesOut(const T &v)
{
    using E = typename BytesOf<T>::Element;
    Value::Bytes b(std::size(v) * sizeof(E));
    if (!b.empty()) std::memcpy(b.data(), &v[0], b.size());
    return Value::bytes(std::move(b));
}
/// `v` (Kind::Bytes) into `out`: false with `why` naming `where` when its length is no whole number of elements (or,
/// for a T[N], not N of them).
template <class T> bool bytesIn(const Value &v, T &out, const std::string &where, std::string &why)
{
    using E = typename BytesOf<T>::Element;
    const Value::Bytes &b = v.asBytes();
    if (b.size() % sizeof(E)) {
        why = where + " holds " + std::to_string(b.size()) + " bytes, not a whole number of its " +
              std::to_string(sizeof(E)) + "-byte elements";
        return false;
    }
    const std::size_t n = b.size() / sizeof(E);
    if constexpr (IsVector<T>::value) {
        out.assign(n, E{});
    } else if (n != std::extent<T>::value) {
        why = where + " holds " + std::to_string(n) + " elements, the record's " + std::to_string(std::extent<T>::value);
        return false;
    }
    if (n) std::memcpy(&out[0], b.data(), b.size());
    return true;
}

/// A VECTOR OF BYTE-CARRIED VECTORS AS ONE BYTES VALUE - only as the ELEMENT of an array (GiVoxelVolume::directional[3]:
/// three axes of mip levels): PACKED, a uint32 count n, then n uint32 element counts, then the n vectors' bytes in order.
template <class T> struct PackedOf : std::false_type {};
template <class V, class A> struct PackedOf<std::vector<V, A>> : std::integral_constant<bool, BytesOf<V>::value && IsVector<V>::value> {};
/// The element of an array carried as bytes: an array of elements (BytesOf) or a vector of them (PackedOf).
template <class E> constexpr bool elementAsBytes() { return BytesOf<E>::value || PackedOf<E>::value; }
template <class T> Value packedOut(const T &v)
{
    using E = typename T::value_type::value_type;
    std::size_t data = 0;
    for (const auto &x : v) data += x.size() * sizeof(E);
    Value::Bytes b(4 * (1 + v.size()) + data);
    const auto put = [&b](std::size_t at, std::uint32_t u) { std::memcpy(b.data() + at, &u, 4); };
    put(0, std::uint32_t(v.size()));
    std::size_t at = 4 * (1 + v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        put(4 * (1 + i), std::uint32_t(v[i].size()));
        if (!v[i].empty()) std::memcpy(b.data() + at, v[i].data(), v[i].size() * sizeof(E));
        at += v[i].size() * sizeof(E);
    }
    return Value::bytes(std::move(b));
}
template <class T> bool packedIn(const Value &v, T &out, const std::string &where, std::string &why)
{
    using E = typename T::value_type::value_type;
    const Value::Bytes &b = v.asBytes();
    const auto get = [&b](std::size_t at) { std::uint32_t u = 0; std::memcpy(&u, b.data() + at, 4); return u; };
    const auto bad = [&](const std::string &what) {
        why = where + " is no packed array of arrays (" + what + ")";
        return false;
    };
    if (b.size() < 4) return bad("no count");
    const std::size_t n = get(0);
    if (n > (b.size() - 4) / 4) return bad("its count is past its bytes");
    std::size_t at = 4 * (1 + n), need = at;
    for (std::size_t i = 0; i < n; ++i) need += std::size_t(get(4 * (1 + i))) * sizeof(E);
    if (need != b.size()) return bad(std::to_string(b.size()) + " bytes where its counts say " + std::to_string(need));
    out.assign(n, typename T::value_type());
    for (std::size_t i = 0; i < n; ++i) {
        out[i].assign(get(4 * (1 + i)), E{});
        if (!out[i].empty()) std::memcpy(out[i].data(), b.data() + at, out[i].size() * sizeof(E));
        at += out[i].size() * sizeof(E);
    }
    return true;
}
template <class E> Value elementOut(const E &e)
{
    if constexpr (BytesOf<E>::value) return bytesOut(e);
    else return packedOut(e);
}
template <class E> bool elementIn(const Value &v, E &out, const std::string &where, std::string &why)
{
    if constexpr (BytesOf<E>::value) return bytesIn(v, out, where, why);
    else return packedIn(v, out, where, why);
}

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
    } else if constexpr (std::is_same<T, float>::value) {
        return arg(std::move(name), Kind::Number).asFloat();
    } else if constexpr (std::is_floating_point<T>::value) {
        return arg(std::move(name), Kind::Number);
    } else if constexpr (std::is_same<T, std::string>::value) {
        return arg(std::move(name), Kind::String);
    } else if constexpr (detail::flatWidth<T>() != 0) {
        return numbers(std::move(name), detail::flatWidth<T>()).asFloat();
    } else if constexpr (detail::IsOptional<T>::value || detail::IsShared<T>::value) {
        Param p = paramOf<typename std::conditional<detail::IsOptional<T>::value, detail::IsOptional<T>,
                                                    detail::IsShared<T>>::type::Element>(std::move(name));
        p.fallback = Value();   // null: absent
        return p;
    } else if constexpr (detail::IsVector<T>::value || std::is_array<T>::value) {
        using E = typename detail::ElementOf<T>::type;
        const unsigned n = std::is_array<T>::value ? unsigned(std::extent<T>::value) : 0u;
        if constexpr (detail::IsVector<E>::value || std::is_array<E>::value) {   // AN ARRAY OF ARRAYS: of bytes
            static_assert(detail::elementAsBytes<E>(), "an array of arrays is carried as an array of bytes: its inner "
                                                       "element trivially copyable (and no array, no bool)");
            Param p = arg(std::move(name), Kind::Array);
            p.element = Kind::Bytes;
            p.count = n;
            return p;
        } else {
            Param e = paramOf<E>(name);
            static_assert(!detail::IsOptional<E>::value && !detail::IsShared<E>::value, "an array of optionals is not carried");
            Param p = e;
            p.kind = Kind::Array;
            p.fallback.reset();
            if constexpr (detail::flatWidth<E>() != 0) {   // FLAT: an element's numbers in a row
                p.element = Kind::Number;
                p.count = n * detail::flatWidth<E>();
                p.stride = detail::flatWidth<E>();
            } else {
                p.element = e.kind;
                p.count = n;
            }
            p.maxCount = 0;   // (a vector's maximum is the table's: Field::upTo)
            return p;
        }
    } else {
        return Record<T>::table().param(std::move(name));
    }
}

/// A C++ value as a script value. `call`: the call that answers it - a record's HANDLE member (Field::handle) is minted
/// through it (null: no handle member may carry an id; a field's default is read so, an id 0 answers null).
template <class T> Value toValue(const T &v, Call *call = nullptr)
{
    if constexpr (std::is_same<T, bool>::value) {
        return Value(v);
    } else if constexpr (std::is_enum<T>::value) {
        for (const auto &n : EnumNames<T>::all())
            if (n.first == v) return Value(n.second);
        // an unnamed value FAILS by name (never an integer under a declared string): the call that answers it is
        // Failed, "<command>: threw: ..." (Registry catches a command's exception)
        throw std::logic_error("the enum value " + std::to_string(std::int64_t(v)) + " has no name in its table "
                               "(JAH_ENUM_NAMES)");
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
        return v ? toValue(*v, call) : Value();
    } else if constexpr (detail::IsVector<T>::value || std::is_array<T>::value) {
        Value::Array a;
        for (const auto &e : v) {
            using E = typename std::decay<decltype(e)>::type;
            if constexpr (detail::flatWidth<E>() != 0) detail::flatOut(e, a);
            else if constexpr (detail::IsVector<E>::value || std::is_array<E>::value) a.push_back(detail::elementOut(e));
            else a.push_back(toValue(e, call));
        }
        return Value(std::move(a));
    } else {
        return Record<T>::table().toValue(v, call);
    }
}

/// A script value - conformed to paramOf<T>'s parameter by the registry (its kinds, ranges, float range and flat
/// strides) - into `out`; false with `why` (naming `where`) for an enum name its table lacks.
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
    } else if constexpr (std::is_floating_point<T>::value) {
        out = T(v.asNumber());
    } else if constexpr (std::is_same<T, std::string>::value) {
        out = v.asString();
    } else if constexpr (detail::flatWidth<T>() != 0) {
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
        const std::size_t n = w ? a.size() / w : a.size();   // (a multiple of w: the registry checked the stride)
        if constexpr (detail::IsVector<T>::value) out.assign(n, E{});
        for (std::size_t i = 0; i < n; ++i) {
            if constexpr (w != 0) {
                detail::flatIn(a, i * w, out[i]);
            } else if constexpr (detail::IsVector<E>::value || std::is_array<E>::value) {
                if (!detail::elementIn(a[i], out[i], where + "[" + std::to_string(i) + "]", why)) return false;
            } else if (!fromValue(a[i], out[i], where + "[" + std::to_string(i) + "]", why)) {
                return false;
            }
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
    std::function<Value(const R &, Call *call)> get;   ///< `call`: the answering call (toValue)
    std::function<bool(const Value &, R &, const std::string &where, std::string &why)> set;
    /// The member's BYTES form, when it has one (a vector or T[N] of trivially copyable elements: detail::BytesOf).
    std::function<void(Field &)> bytesForm;

    Field range(double lo, double hi) const { return with(param.range(lo, hi)); }
    Field atLeast(double lo) const { return with(param.atLeast(lo)); }
    Field above(double lo) const { return with(param.above(lo)); }
    Field upTo(unsigned elements) const { return with(param.upTo(elements)); }
    Field in(std::string unit) const { return with(param.in(std::move(unit))); }
    Field allowNonFinite() const { return with(param.allowNonFinite()); }
    /// THE MEMBER AS BYTES (SCRIPT-IN-ENGINE-1 E0, RULED 3: the big arrays - a voxel volume, a card's radiance): a
    /// vector or T[N] of trivially copyable elements carried as Kind::Bytes, its elements' bytes as the engine holds
    /// them, bounded by the call's byte bound (Host::Options::callBounds); a length that is no whole number of elements
    /// (or not N of them) FAILS by its member. A member with no bytes form THROWS at the table's making (a fault of the
    /// table, never a silent array of numbers).
    Field bytes() const
    {
        if (!bytesForm)
            throw std::logic_error("the field '" + param.name + "' has no bytes form (a vector or T[N] of trivially "
                                   "copyable elements)");
        Field f = *this;
        bytesForm(f);
        return f;
    }
    /// A HANDLE MEMBER (SCRIPT-IN-ENGINE-1 E0): the member is the engine's id of `kind` (an integral id, or a vector of
    /// them) and the script carries it as that kind's TYPED HANDLE - checked at the call by the registry's walk of
    /// object members (the kind's own existence read, asked of the command's first target, its scene), its id taken
    /// by Call::idOf. NONE is id 0: null, or a literal 0 (the registry conforms both to null) - a scalar member's, and
    /// EACH ELEMENT's of an array member (a vector, or T[N] at its count: its default the record's ids, each 0 a null).
    /// ANSWERED (toValue through its call), an id is minted as a handle of `kind` - 0 answers null. Declared after the
    /// other domain methods (it drops the id's numeric range: a handle is no id).
    Field handle(std::string kind) const
    {
        Field f = *this;
        f.param.handle = kind;
        f.param.min.reset();
        f.param.max.reset();
        const auto get = f.get;
        const auto set = f.set;
        f.get = [get, kind](const R &r, Call *call) {
            const auto one = [&kind, call](const Value &id) -> Value {
                if (id.asInt() == 0) return Value();
                if (!call) throw std::logic_error("the " + kind + " handle member is answered only through its call");
                return call->name(kind, static_cast<unsigned long long>(id.asInt()));
            };
            const Value ids = get(r, call);
            if (ids.kind() != Kind::Array) return one(ids);
            Value::Array a;
            for (const Value &id : ids.asArray()) a.push_back(one(id));
            return Value(std::move(a));
        };
        f.set = [set](const Value &v, R &r, const std::string &where, std::string &why) {
            const auto idOf = [](const Value &h) { return h.isNull() ? Value(0) : Value(std::int64_t(Call::idOf(h))); };
            if (v.kind() != Kind::Array) return set(idOf(v), r, where, why);
            Value::Array a;
            for (const Value &h : v.asArray()) a.push_back(idOf(h));
            return set(Value(std::move(a)), r, where, why);
        };
        f.param.fallback = f.get(R{}, nullptr);   // the record's default ids (0) as none: null, or a null per element
        return f;
    }

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
    f.get = [member](const R &r, Call *call) { return toValue(r.*member, call); };
    f.set = [member](const Value &v, R &r, const std::string &where, std::string &why) {
        return fromValue(v, r.*member, where, why);
    };
    if constexpr (detail::BytesOf<M>::value) {
        f.bytesForm = [member](Field<R> &g) {
            Param p = arg(g.param.name, Kind::Bytes);
            p.fallback = detail::bytesOut(R{}.*member);
            g.param = std::move(p);
            g.get = [member](const R &r, Call *) { return detail::bytesOut(r.*member); };
            g.set = [member](const Value &v, R &r, const std::string &where, std::string &why) {
                return detail::bytesIn(v, r.*member, where, why);
            };
            g.bytesForm = nullptr;
        };
    }
    return f;
}

/// THE FREEZE's PROOF (SCRIPT-IN-ENGINE-1 E0): carried<T>() is true when every member of T, walked down through its
/// nested records, is a shape paramOf / toValue / fromValue carry - bool, an enum, integers, floats, a string, Vec3 /
/// Quat / Colour, optional / shared_ptr<const> of a carried shape, a vector or T[N] of one, an array of arrays of
/// trivially copyable elements (bytes), a record (an aggregate: its fields counted and walked). It needs no table: a
/// compile-only check (script.commands' script_record_shapes.cpp) asserts it for every record a family will table, so
/// no lane meets a shape records.h cannot carry after records.h freezes. KEEP IT BESIDE paramOf: a shape added to one
/// is added to the other.
template <class T> constexpr bool carried();
namespace detail {
/// Converts to every type but std::nullptr_t (declared only: unevaluated) - the field count's probe.
struct AnyField {
    template <class T, class = std::enable_if_t<!std::is_same<T, std::nullptr_t>::value>> operator T() const;
};
template <class T, std::size_t... I>
constexpr auto initsWith(std::index_sequence<I...>, int) -> decltype(T{{(void(I), AnyField{})}...}, true)
{
    return true;
}
template <class T, std::size_t... I> constexpr bool initsWith(std::index_sequence<I...>, long) { return false; }
/// An aggregate's field count: the most members it takes, each by its own braces (no brace elision: an array member
/// counts once). A count it gets wrong cannot pass: bindTypes binds exactly that many names.
template <class T, std::size_t N = 128> constexpr std::size_t fieldCount()
{
    if constexpr (N == 0) return 0;
    else if constexpr (initsWith<T>(std::make_index_sequence<N>(), 0)) return N;
    else return fieldCount<T, N - 1>();
}
template <class Tuple, std::size_t... I> constexpr bool carriedEach(std::index_sequence<I...>)
{
    return (true && ... && carried<std::remove_pointer_t<std::tuple_element_t<I, Tuple>>>());
}
}  // namespace detail
template <class T> constexpr bool carried()
{
    if constexpr (std::is_same<T, bool>::value || std::is_enum<T>::value || std::is_arithmetic<T>::value ||
                  std::is_same<T, std::string>::value || detail::flatWidth<T>() != 0) {
        return true;
    } else if constexpr (detail::IsOptional<T>::value) {
        return carried<typename detail::IsOptional<T>::Element>();
    } else if constexpr (detail::IsShared<T>::value) {
        return carried<typename detail::IsShared<T>::Element>();
    } else if constexpr (detail::IsVector<T>::value || std::is_array<T>::value) {
        using E = typename detail::ElementOf<T>::type;
        if constexpr (detail::IsVector<E>::value || std::is_array<E>::value) return detail::elementAsBytes<E>();
        else if constexpr (detail::IsOptional<E>::value || detail::IsShared<E>::value) return false;
        else return carried<E>();
    } else if constexpr (std::is_class<T>::value && std::is_aggregate<T>::value) {
        constexpr std::size_t n = detail::fieldCount<T>();
        if constexpr (n == 0) {
            return false;
        } else {
            using Fields = decltype(detail::bindTypes(std::declval<const T &>(), std::integral_constant<std::size_t, n>()));
            return detail::carriedEach<Fields>(std::make_index_sequence<n>());
        }
    } else {
        return false;
    }
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
    /// The record as a script value; `call`: the answering call (a handle member is minted through it).
    Value toValue(const R &r, Call *call = nullptr) const
    {
        Value::Object o;
        o.reserve(mFields.size());
        for (const Field<R> &f : mFields) o.emplace_back(f.param.name, f.get(r, call));
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
