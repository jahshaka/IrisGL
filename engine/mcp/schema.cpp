// THE SIGNATURE AS A SCHEMA (MCP-ON-HOST-1 stage 1): mcp/include/mcp/schema.h says the form. From the signature's fields alone.
#include "mcp/schema.h"

#include "core/json.h"

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace jahshaka { namespace engine { namespace mcp {

using script::Kind;
using script::Param;
using script::Value;

namespace {

/// The longest base64 text of a Bytes value the codec carries (4 characters per 3 bytes, padded).
constexpr std::int64_t kBytesTextCap = std::int64_t((iris::json::kBytesCap + 2) / 3 * 4);

const char *typeName(Kind k)
{
    switch (k) {
    case Kind::Null:   return "null";
    case Kind::Bool:   return "boolean";
    case Kind::Int:    return "integer";
    case Kind::Number: return "number";
    case Kind::String: return "string";
    case Kind::Array:  return "array";
    case Kind::Object: return "object";
    case Kind::Bytes:  return "object";
    }
    return "?";
}

/// A name every client takes: 1-64 of [A-Za-z0-9_.-] (Claude Code refuses a tool whose top-level names break it).
bool clientName(const std::string &n)
{
    if (n.empty() || n.size() > 64) return false;
    for (const char c : n)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return false;
    return true;
}

/// A bound as the parameter's kind: an integer's integral bound as an integer.
Value bound(double b, Kind k)
{
    if (k == Kind::Int && std::trunc(b) == b && std::fabs(b) <= 9007199254740992.0) return Value(static_cast<std::int64_t>(b));
    return Value(b);
}

class SchemaGenerator {
public:
    explicit SchemaGenerator(const std::string &command) : mCommand(command) {}
    std::vector<std::string> faults;

    /// "properties", "required" (when one is) and "additionalProperties":false of `fields`, appended to `s`.
    void members(const std::vector<Param> &fields, const std::string &where, Value::Object &s)
    {
        Value::Object properties;
        Value::Array required;
        for (const Param &f : fields) {
            const std::string at = where.empty() ? f.name : where + "." + f.name;
            if (!clientName(f.name)) fault(at, "the name '" + f.name + "' is not 1-64 of [A-Za-z0-9_.-]");
            properties.emplace_back(f.name, param(f, at));
            if (!f.fallback) required.emplace_back(f.name);
        }
        s.emplace_back("properties", Value(std::move(properties)));
        if (!required.empty()) s.emplace_back("required", Value(std::move(required)));
        s.emplace_back("additionalProperties", Value(false));
    }

private:
    void fault(const std::string &where, const std::string &what) { faults.push_back(mCommand + ": " + where + ": " + what); }
    void unmapped(const std::string &where, const char *field, Kind kind)
    {
        fault(where, std::string("its '") + field + "' on a " + script::kindName(kind) + " has no schema form");
    }

    /// The schema of one value of kind `k` carrying `p`'s domain (choices, range, handle, unit, non-finite, members) -
    /// a parameter's own, or each element of an array parameter.
    Value::Object value(const Param &p, Kind k, const std::string &where, std::vector<std::string> &said)
    {
        Value::Object s{{"type", Value(typeName(k))}};
        if (k == Kind::Bytes) {
            s.emplace_back("propertyNames", Value(Value::Object{{"const", Value("$bytes")}}));
            s.emplace_back("minProperties", Value(1));
            s.emplace_back("additionalProperties", Value(Value::Object{{"type", Value("string")},
                                                                       {"contentEncoding", Value("base64")},
                                                                       {"maxLength", Value(kBytesTextCap)}}));
            said.push_back("bytes as {\"$bytes\": \"<base64>\"}, at most 1 MiB");
        }
        if (k == Kind::Object) members(p.fields, where, s);
        if (!p.choices.empty()) {
            Value::Array e;
            for (const std::string &c : p.choices) e.emplace_back(c);
            s.emplace_back("enum", Value(std::move(e)));
        }
        if (!p.handle.empty()) {
            s.emplace_back("minimum", Value(1));
            said.push_back("a " + p.handle + " handle");
        } else {
            if (p.min) s.emplace_back(p.minExclusive ? "exclusiveMinimum" : "minimum", bound(*p.min, k));
            if (p.max) s.emplace_back("maximum", bound(*p.max, k));
        }
        if (p.single && k == Kind::Number) {   // (SCRIPT-IN-ENGINE-1 2b: a client knows a 1e39 is refused before it sends it)
            s.emplace_back("format", Value("float"));
            said.push_back("single precision: past a float's range FAILS");
        }
        if (!p.unit.empty()) said.push_back(p.unit);
        if (p.nonFinite) said.push_back("NaN and infinity allowed (JSON carries neither)");
        return s;
    }

public:
    Value param(const Param &p, const std::string &where)
    {
        const bool array = p.kind == Kind::Array;
        const Kind k = array ? p.element : p.kind;   // the kind the domain applies to (the registry checks each element)
        // A FIELD SET WHERE ITS KIND HAS NO FORM: named, never dropped
        if (!array && p.count) unmapped(where, "count", p.kind);
        if (!array && p.maxCount) unmapped(where, "maxCount", p.kind);
        if (array && p.count && p.maxCount) fault(where, "its 'maxCount' beside a fixed count has no schema form");
        if (p.minExclusive && !p.min) unmapped(where, "minExclusive", k);
        if (!array && p.element != Param().element) unmapped(where, "element", p.kind);
        if (array && (k == Kind::Array || k == Kind::Null)) unmapped(where, "element", k);
        if (!p.fields.empty() && k != Kind::Object) unmapped(where, "fields", k);
        if (!p.choices.empty() && k != Kind::String) unmapped(where, "choices", k);
        if ((p.min || p.max) && ((k != Kind::Int && k != Kind::Number) || !p.handle.empty())) unmapped(where, "min/max", k);
        if (!p.handle.empty() && k != Kind::Int) unmapped(where, "handle", k);
        if (p.nonFinite && k != Kind::Number) unmapped(where, "nonFinite", k);
        if (p.single && k != Kind::Number) unmapped(where, "single", k);
        if (p.stride && !array) unmapped(where, "stride", p.kind);

        std::vector<std::string> said;
        Value::Object s;
        if (array) {
            std::vector<std::string> each;
            Value::Object items = value(p, k, where + "[]", each);
            if (!each.empty()) items.emplace_back("description", Value(join(each)));
            s = {{"type", Value("array")}, {"items", Value(std::move(items))}};
            if (p.count) {
                s.emplace_back("minItems", Value(p.count));
                s.emplace_back("maxItems", Value(p.count));
            } else if (p.maxCount) {
                s.emplace_back("maxItems", Value(p.maxCount));
            }
            if (p.stride > 1)   // FLAT (SCRIPT-IN-ENGINE-1 2b): the items count numbers; a length off the stride FAILS
                said.push_back("flat: " + std::to_string(p.stride) + " numbers an item, the length a multiple of " +
                               std::to_string(p.stride));
        } else {
            s = value(p, k, where, said);
        }
        std::string text = join(said);
        if (p.target) text = text.empty() ? "the target" : text + " (target)";
        if (!text.empty()) s.emplace_back("description", Value(text));
        if (p.fallback) s.emplace_back("default", *p.fallback);
        return Value(std::move(s));
    }

private:
    static std::string join(const std::vector<std::string> &parts)
    {
        std::string t;
        for (const std::string &x : parts) t += (t.empty() ? "" : ", ") + x;
        return t;
    }
    const std::string &mCommand;
};

}  // namespace

bool argumentSchema(const script::CommandInfo &command, Value &schema, std::string &why)
{
    SchemaGenerator g(command.name);
    Value::Object s{{"type", Value("object")}, {"description", Value(command.doc)}};
    g.members(command.signature.params, "", s);
    if (!g.faults.empty()) {
        why.clear();
        for (const std::string &f : g.faults) why += (why.empty() ? "" : "; ") + f;
        return false;
    }
    schema = Value(std::move(s));
    return true;
}

}}}  // namespace jahshaka::engine::mcp
