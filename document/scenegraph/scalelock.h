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

#include <cmath>

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
//   * ZERO, ON EITHER SIDE. There is no ratio: every multiple of 0 is 0, so no
//     factor can be recovered from "it was 0 and now it is 3" — and none can be
//     APPLIED to reach 0 without taking the other two with it, permanently. So
//     the rule is symmetric: if the old value is zero OR the new one is, the
//     edited channel takes the new value and THE OTHER TWO KEEP THEIRS.
//
//     The `value == 0` half is not an ornament (found by the lead's review):
//     with a ratio of 0 a single typed zero on a locked (1, 2, 0.5) collapses
//     it to (0, 0, 0), and every later edit is then a 0 -> v edit with no ratio
//     — so the two numbers the user never touched are gone for good. Setting
//     one axis flat is a thing people do on purpose; it must not be a
//     one-keystroke way to lose the other two.
//
//   * A SIGN CHANGE FLIPS ALL THREE. The ratio carries its sign and is applied
//     to every channel, so dragging X from 1 to -1 mirrors Y and Z with it.
//     Stated exactly: the result's handedness flips whenever the EDITED channel
//     changes sign, whatever the other two were. That is a mirror when they
//     agreed in sign with it and a rotation-like flip when they did not —
//     (-2, 3, 4) edited to x = 1 gives (1, -1.5, -2) — and it is the only rule
//     that preserves the ratios, which is what the lock promises. Nothing is
//     clamped or abs()'d.
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


/// The scale vector that setting `axis` to `value` produces.
/// `uniform` false = the plain per-channel write; true = preserve the ratio.
inline Vec3 apply(const Vec3 &current, int axis, float value, bool uniform)
{
    if (axis < 0 || axis > 2) return current;
    // std::isfinite, not `value == value`: a self-equality is a NaN test only
    // where the compiler leaves it alone (CLAUDE.md, HDR-1 — this stack's
    // shader compiler folds it to true), and this rejects the infinities too.
    // No fast-math anywhere in this tree's CMake, so it is the real predicate.
    if (!std::isfinite(value)) return current;

    Vec3 out = current;
    out[axis] = value;
    if (!uniform) return out;

    const float old = current[axis];
    // No ratio exists in EITHER direction — see the note above.
    if (old == 0.0f || value == 0.0f) return out;
    const float ratio = value / old;
    if (!std::isfinite(ratio)) return out;

    for (int i = 0; i < 3; ++i)
        if (i != axis) out[i] = current[i] * ratio;
    return out;
}

}   // namespace scalelock
}   // namespace iris

#endif   // IRIS_SCALELOCK_H
