// iris::Value - THE ONE VALUE TYPE BELOW THE EDITOR (NOQT-CORE-1 stage 3; V2 plan 0.12): null, bool, 64-bit integer,
// double, UTF-8 string, array, ordered object or bytes. MOVED here whole from the engine's script host (SCRIPT-IN-ENGINE-1
// stage 1, jahshaka/engine/Script.h, which keeps the names `Value`, `Kind` and `kindName` by using-declarations: one type,
// never a second). The standard library only; its JSON text is core/json.h.
#pragma once

#include "core/export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace iris {

/// A value's kind. Int and Number are two kinds of one script number: a language with one number type converts an
/// integral number of magnitude <= 2^53 to Int and any other to Number. An Int parameter takes a Number only when it
/// is integral and of magnitude <= 2^53 (above it a script's number is no longer exact: a larger integer is an Int
/// value - a BigInt in JS).
enum class Kind { Null, Bool, Int, Number, String, Array, Object, Bytes };
IRIS_CORE_API const char *kindName(Kind);

/// One value: null, bool, 64-bit integer, double, UTF-8 string, array, ordered object (members in insertion order,
/// names unique) or bytes.
class IRIS_CORE_API Value {
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

}  // namespace iris
