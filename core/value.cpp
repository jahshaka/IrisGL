// iris::Value (core/value.h says the type; its bodies MOVED here from the script host's registry.cpp, NOQT-CORE-1 stage 3).
#include "core/value.h"

namespace iris {

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

}  // namespace iris
