/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_VALUESHAPES_H
#define IRIS_VALUESHAPES_H

// THE DOCUMENT's VALUE SHAPES (NOQT-DOCUMENT-1 stage 4): how a colour and a vector are carried as an iris::Value -
// in a property row's getValue / setValue, in the JSON members, and (through Studio's src/app/qtstd.h) in every file
// the editor writes. ONE shape each, written here and nowhere else.
//
// A COLOUR is an Object {"a", "b", "g", "r"} of four Numbers, each the F accessor's float widened to double, inserted
// in that order (two equal colours are equal Values: Value == compares members in order) - for ANY colour, an invalid
// one included: SrgbColor() writes r 0, g 0, b 0, a 1; an out-of-range 8-bit construction writes 0, 0, 0, 0 (its
// members, srgbcolor.h). Read back with each channel clamped to [0, 1] (iris::bound, Qt's qBound operand order: a NaN
// reads 0) through SrgbColor::fromRgbF; `a` absent = opaque; any other channel absent = 0; a member that is not a
// number reads as absent. An EMPTY object, a Null or a non-object reads as the caller's `absent` (so a reader can tell
// "not written" from black). This file is NOT srgbcolor.h on purpose: srgbcolor.h is WATCHED by source.bake_key_guard.
//
// A VECTOR (iris::Vec3) is an Array of three Numbers, each component widened to double.

#include "core/math/scalar.h"
#include "core/math/vec.h"
#include "core/value.h"
#include "document/srgbcolor.h"

namespace iris
{

inline Value colourToValue(const SrgbColor &c)
{
    return Value(Value::Object{{"a", Value(double(c.alphaF()))},
                               {"b", Value(double(c.blueF()))},
                               {"g", Value(double(c.greenF()))},
                               {"r", Value(double(c.redF()))}});
}

inline SrgbColor colourFromValue(const Value &v, const SrgbColor &absent = SrgbColor())
{
    if (v.kind() != Kind::Object || v.asObject().empty()) return absent;
    const auto channel = [&v](const char *key, double fallback) {
        const Value *m = v.find(key);
        const bool number = m && (m->kind() == Kind::Number || m->kind() == Kind::Int);
        return iris::bound(0.0, number ? m->asNumber() : fallback, 1.0);
    };
    return SrgbColor::fromRgbF(float(channel("r", 0.0)), float(channel("g", 0.0)),
                               float(channel("b", 0.0)), float(channel("a", 1.0)));
}

/// A colour PROPERTY's Value (a row, a node's colour name - NOT the file codec, which writes colourToValue for any
/// colour): THE invalid colour is a NULL ("none": an unset particle fade is one, and the mirror reads its validity), any
/// valid colour its Object shape.
inline Value colourRowValue(const SrgbColor &c)
{
    return c.isValid() ? colourToValue(c) : Value();
}

/// The colour a colour property takes from `v` into `out`: a Null is THE invalid colour; an Object that is empty or
/// carries any of r / g / b / a is its colour (colourFromValue: a channel absent reads 0, `a` 1); anything else - a
/// non-empty Object with none of the four (a texture reference, say), a number, a string - is false, out unchanged
/// (STRICT: the lead's C1).
inline bool colourOfRowValue(const Value &v, SrgbColor &out)
{
    if (v.isNull()) { out = SrgbColor(); return true; }
    if (v.kind() != Kind::Object) return false;
    if (!v.asObject().empty() && !v.find("r") && !v.find("g") && !v.find("b") && !v.find("a")) return false;
    out = colourFromValue(v);
    return true;
}

inline Value vec3ToValue(const Vec3 &v)
{
    return Value(Value::Array{Value(double(v.x())), Value(double(v.y())), Value(double(v.z()))});
}

/// The vector an Array of three numbers holds into `out`; false (out unchanged) for any other Value.
inline bool vec3FromValue(const Value &v, Vec3 &out)
{
    if (v.kind() != Kind::Array || v.asArray().size() != 3) return false;
    for (const Value &c : v.asArray())
        if (c.kind() != Kind::Number && c.kind() != Kind::Int) return false;
    const Value::Array &a = v.asArray();
    out = Vec3(float(a[0].asNumber()), float(a[1].asNumber()), float(a[2].asNumber()));
    return true;
}

/// A Float-kind value's number into `out` (a Number, or an Int: one JSON number type - the spec's RULED E (1));
/// false, out unchanged, for any other kind.
inline bool numberOf(const Value &v, double &out)
{
    if (v.kind() != Kind::Number && v.kind() != Kind::Int) return false;
    out = v.asNumber();
    return true;
}

}  // namespace iris

#endif  // IRIS_VALUESHAPES_H
