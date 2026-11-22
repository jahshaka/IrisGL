#pragma once
// THE SIGNATURE AS A SCHEMA (MCP-ON-HOST-1 stage 1; SPECS/MCP_ON_HOST_1_SPEC.md L5) - the JSON Schema (2020-12) of ONE
// command's arguments by name, made from its signature's FIELDS alone: the adapter knows no command, so every command,
// today's and tomorrow's, is described by the same few lines. The MCP library (JahshakaEngineMcp): the standard library,
// Script.h and ScriptJson.h, nothing else of the engine.
//
// THE FORM: {"type":"object","description":<doc>,"properties":{<param>: <its schema>, ...},"required":[<params with no
// default>],"additionalProperties":false} ("required" only when one is). A parameter's schema:
//   * its kind's type: null / boolean / integer / number / string / array / object; BYTES the {"$bytes": "<base64>"}
//     object (propertyNames {"const":"$bytes"}: the form without "$bytes" as a key, so the schema is itself a Value the
//     codec carries), its base64 at most the 1 MiB cap's length;
//   * choices -> "enum"; min / max -> "minimum" / "maximum" (an integer's as integers);
//   * a handle -> {"type":"integer","minimum":1,"description":"a <kind> handle"}; a target -> " (target)" after its
//     description ("the target" when it has none); a unit -> the description; non-finite allowed -> said there;
//   * an array -> "items" (its element's kind, and the parameter's domain: the registry checks it on each element; an
//     object element its own object schema), count > 0 -> "minItems" = "maxItems" = count;
//   * an object -> "properties", "required" and "additionalProperties":false the same way, nested;
//   * a default -> "default", the checked default as it is (the codec writes it).
// A FIELD THE GENERATOR CANNOT CARRY - one set where its kind has no form for it (choices on an integer, a count on a
// scalar, an array of arrays), or a name a client refuses (not 1-64 of [A-Za-z0-9_.-]) - is a NAMED FAULT, never dropped.
// A field ADDED to Param or Signature is held by mcp.schema's census: kParamFieldsMapped / kSignatureFieldsMapped equal
// the structs' own field counts, so a new field is given its form here before that row is green again.

#include "jahshaka/engine/Script.h"

#include <string>

namespace jahshaka { namespace engine { namespace mcp {

/// The Param and Signature fields this generator reads (Signature's `returns` and `returnsHandle` describe the answer,
/// not the arguments: read, and given no form in an argument schema).
constexpr int kParamFieldsMapped = 13;
constexpr int kSignatureFieldsMapped = 3;

/// The JSON Schema of `command`'s arguments into `schema`; false with `why` naming EVERY fault (command: where: what).
bool argumentSchema(const script::CommandInfo &command, script::Value &schema, std::string &why);

}}}  // namespace jahshaka::engine::mcp
