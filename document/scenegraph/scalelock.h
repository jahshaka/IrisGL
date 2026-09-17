/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_SCALELOCK_H
#define IRIS_SCALELOCK_H

#include "core/math/vec.h"

// scalelock — THE ARITHMETIC OF A SINGLE-CHANNEL SCALE EDIT (SCALE-LOCK-1).
//
// One definition, used by all three callers: node.transform's one-channel scale
// write, the transform panel's three Scale fields, and the scale gizmo's three
// axis handles. Pure and header-only on purpose — the panel and the gizmo both
// measure their ratio from the scale their GESTURE started at, not from the
// node's current one, so the rule has to be a function of a vector rather than
// a method that reads a node.
//
// THE RULE. Without the lock, a channel edit writes that channel. With it, the
// other two are multiplied by the same RATIO the edited channel moved by
// (Unreal's per-actor "preserve ratio"; Blender's chain link) — which is a
// ratio and not a difference, because that is what "keeps its proportions"
// means: a 2x on X is a 2x on everything, whatever the other two happen to be.
//
// THE THREE DEGENERATE CASES, decided and documented rather than discovered:
//
//   * THE OLD VALUE IS ZERO. There is no ratio: every multiple of 0 is 0, so
//     no factor can be recovered from "it was 0 and now it is 3". The edited
//     channel takes the new value and THE OTHER TWO KEEP THEIRS. The
//     alternative (treat 0 -> v as v and set all three to v) would silently
//     throw away two numbers the user never touched, and a flattened axis is a
//     thing people deliberately have.
//
//   * A NEGATIVE CHANNEL (a mirrored object). The ratio carries the sign, so
//     dragging X from 1 to -1 mirrors Y and Z with it: the proportions are
//     preserved, and so is the mirroring. Nothing is clamped or abs()'d.
//
//   * THE VALUE DID NOT CHANGE. The ratio is exactly 1 and the result is the
//     input vector, bit for bit — so a caller that compares before writing
//     records no undo step for a typed value that was already there.
//
// Non-finite input (a NaN coming out of a division elsewhere, an infinite
// ratio) is refused: the vector comes back unchanged except for the channel the
// caller named. `axis` is 0/1/2 = x/y/z; anything else is a no-op.

namespace iris
{
namespace scalelock
{

inline bool isFinite(float f)
{
    // Bit test, not `f == f` — self-equality is folded to true by the shader
    // compiler on this stack and reads badly here for the same reason it reads
    // badly there (CLAUDE.md, HDR-1). This is plain C++ and std::isfinite would
    // do, but the intent is the same and it needs no <cmath> in a header every
    // document TU includes.
    return f > -3.4028235e38f && f < 3.4028235e38f;
}

/// The scale vector that setting `axis` to `value` produces.
/// `uniform` false = the plain per-channel write; true = preserve the ratio.
inline Vec3 apply(const Vec3 &current, int axis, float value, bool uniform)
{
    if (axis < 0 || axis > 2) return current;
    if (!isFinite(value)) return current;

    Vec3 out = current;
    out[axis] = value;
    if (!uniform) return out;

    const float old = current[axis];
    if (old == 0.0f) return out;            // no ratio exists — see the note above
    const float ratio = value / old;
    if (!isFinite(ratio)) return out;

    for (int i = 0; i < 3; ++i)
        if (i != axis) out[i] = current[i] * ratio;
    return out;
}

}   // namespace scalelock
}   // namespace iris

#endif   // IRIS_SCALELOCK_H
