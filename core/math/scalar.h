// iris scalar helpers (core/math/scalar.h; ONE-MATHS-1 stage 3 adds its own beside these).
#pragma once

namespace iris {

/// qBound's EXACT result, so a converted call site computes the same bits: qMax(lo, qMin(hi, v)) in Qt's operand order
/// (qMin(a, b) = a < b ? a : b; qMax(a, b) = a < b ? b : a). A NaN `v` gives `lo`, and -0.0 gives `lo` when lo is +0.0 -
/// std::clamp gives NaN and -0.0 there, so it is NOT a replacement.
template <typename T>
constexpr T bound(const T &lo, const T &v, const T &hi)
{
    const T &m = (hi < v) ? hi : v;
    return (lo < m) ? m : lo;
}

}   // namespace iris
