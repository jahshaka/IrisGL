/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_VEC_H
#define IRIS_VEC_H

// -----------------------------------------------------------------------------
// iris::Vec2 / Vec3 / Vec4 — the document model's vector types.
//
// These exist so that the document headers (scenenode.h and everything under it)
// stop dragging QtGui in. QQuaternion and QMatrix4x4 each carry an
// `operator QVariant()`, which pulls qvariant.h — and with it most of QtCore's
// metatype machinery — into every translation unit that so much as forward-
// declares a scene node. The measured cost was 123,573 preprocessed lines per TU,
// 99.7% of them Qt.
//
// This buys COMPILE TIME and LAYERING. It buys no runtime speed: QVector3D was
// already three plain floats with no SIMD, and so is this.
//
// The API deliberately mirrors QVector2D/QVector3D/QVector4D name for name so the
// migration is a rename, and — more importantly — so the NUMERICS are the same.
// Every scene ever saved has its rotations written by Qt's arithmetic; a rounding
// difference here is a scene that reopens rotated. Where Qt's implementation makes
// a choice that a "clean" implementation would not (qHypot instead of sqrt of the
// sum of squares; the fuzzy short-circuits in normalized()), that choice is
// reproduced here on purpose and asserted bit-for-bit by tests/math/test_math_parity.
//
// Conversions to and from the Qt types live in core/math/qtinterop.h and are
// included ONLY by Qt-facing code (UI, serialization, the scripting bridge).
// -----------------------------------------------------------------------------

#include <algorithm>
#include <cmath>

namespace iris
{

// qFuzzyIsNull(float) / qFuzzyCompare(float), reproduced exactly.
inline bool fuzzyIsNull(float f) noexcept
{
    return std::abs(f) <= 0.00001f;
}

inline bool fuzzyIsNull(double d) noexcept
{
    return std::abs(d) <= 0.000000000001;
}

inline bool fuzzyCompare(float p1, float p2) noexcept
{
    return (std::abs(p1 - p2) * 100000.f <= std::min(std::abs(p1), std::abs(p2)));
}

inline bool fuzzyCompare(double p1, double p2) noexcept
{
    return (std::abs(p1 - p2) * 1000000000000. <= std::min(std::abs(p1), std::abs(p2)));
}

// qDegreesToRadians / qRadiansToDegrees, float overloads, same constants.
constexpr inline float degreesToRadians(float degrees) noexcept
{
    return degrees * float(3.14159265358979323846 / 180);
}

constexpr inline float radiansToDegrees(float radians) noexcept
{
    return radians * float(180 / 3.14159265358979323846);
}

// Qt's QHypotHelper, reduced to the float-only case we need. std::hypot has 2- and
// 3-argument overloads (which is what qHypot forwards to for Vec2/Vec3), but no
// 4-argument one, so Vec4::length() goes through the incremental scaling helper —
// exactly as QVector4D::length() does.
inline float hypot4(float x, float y, float z, float w) noexcept
{
    struct Helper {
        float scale, total;
        Helper(float first) : scale(std::abs(first)), total(1.0f) {}
        Helper(float s, float t) : scale(s), total(t) {}
        Helper add(float next) const
        {
            if (std::isinf(scale) || (std::isnan(scale) && !std::isinf(next)))
                return Helper(scale, 1.0f);
            if (std::isnan(next))
                return Helper(next, 1.0f);
            const float val = std::abs(next);
            if (!(scale > 0) || std::isinf(next))
                return Helper(val, 1.0f);
            if (!(val > 0))
                return Helper(scale, total);
            if (val > scale) {
                const float ratio = scale / next;
                return Helper(val, total * ratio * ratio + 1.0f);
            }
            const float ratio = next / scale;
            return Helper(scale, total + ratio * ratio);
        }
        float result() const
        {
            return std::isfinite(scale) ? (scale > 0 ? scale * std::sqrt(total) : 0.0f) : scale;
        }
    };
    return Helper(x).add(y).add(z).add(w).result();
}

class Vec3;
class Vec4;

// ---------------------------------------------------------------- Vec2 -------
class Vec2
{
public:
    constexpr Vec2() noexcept : v{0.0f, 0.0f} {}
    constexpr Vec2(float xpos, float ypos) noexcept : v{xpos, ypos} {}
    explicit Vec2(const Vec3 &vector) noexcept;
    explicit Vec2(const Vec4 &vector) noexcept;

    constexpr bool isNull() const noexcept { return v[0] == 0.0f && v[1] == 0.0f; }

    constexpr float x() const noexcept { return v[0]; }
    constexpr float y() const noexcept { return v[1]; }

    constexpr void setX(float aX) noexcept { v[0] = aX; }
    constexpr void setY(float aY) noexcept { v[1] = aY; }

    constexpr float &operator[](int i) { return v[i]; }
    constexpr float operator[](int i) const { return v[i]; }

    float length() const noexcept { return std::hypot(v[0], v[1]); }
    constexpr float lengthSquared() const noexcept { return v[0] * v[0] + v[1] * v[1]; }

    Vec2 normalized() const noexcept
    {
        const float len = length();
        return fuzzyIsNull(len - 1.0f) ? *this
             : fuzzyIsNull(len)        ? Vec2()
                                       : Vec2(v[0] / len, v[1] / len);
    }
    void normalize() noexcept
    {
        const float len = length();
        if (fuzzyIsNull(len - 1.0f) || fuzzyIsNull(len))
            return;
        v[0] /= len;
        v[1] /= len;
    }

    float distanceToPoint(Vec2 point) const noexcept { return (*this - point).length(); }

    constexpr Vec2 &operator+=(Vec2 vector) noexcept { v[0] += vector.v[0]; v[1] += vector.v[1]; return *this; }
    constexpr Vec2 &operator-=(Vec2 vector) noexcept { v[0] -= vector.v[0]; v[1] -= vector.v[1]; return *this; }
    constexpr Vec2 &operator*=(float factor) noexcept { v[0] *= factor; v[1] *= factor; return *this; }
    constexpr Vec2 &operator*=(Vec2 vector) noexcept { v[0] *= vector.v[0]; v[1] *= vector.v[1]; return *this; }
    constexpr Vec2 &operator/=(float divisor) { v[0] /= divisor; v[1] /= divisor; return *this; }
    constexpr Vec2 &operator/=(Vec2 vector) { v[0] /= vector.v[0]; v[1] /= vector.v[1]; return *this; }

    static constexpr float dotProduct(Vec2 a, Vec2 b) noexcept { return a.v[0] * b.v[0] + a.v[1] * b.v[1]; }

    Vec3 toVector3D() const noexcept;
    Vec4 toVector4D() const noexcept;

    friend constexpr bool operator==(Vec2 a, Vec2 b) noexcept { return a.v[0] == b.v[0] && a.v[1] == b.v[1]; }
    friend constexpr bool operator!=(Vec2 a, Vec2 b) noexcept { return !(a == b); }
    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept { return Vec2(a.v[0] + b.v[0], a.v[1] + b.v[1]); }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept { return Vec2(a.v[0] - b.v[0], a.v[1] - b.v[1]); }
    friend constexpr Vec2 operator*(float f, Vec2 a) noexcept { return Vec2(a.v[0] * f, a.v[1] * f); }
    friend constexpr Vec2 operator*(Vec2 a, float f) noexcept { return Vec2(a.v[0] * f, a.v[1] * f); }
    friend constexpr Vec2 operator*(Vec2 a, Vec2 b) noexcept { return Vec2(a.v[0] * b.v[0], a.v[1] * b.v[1]); }
    friend constexpr Vec2 operator-(Vec2 a) noexcept { return Vec2(-a.v[0], -a.v[1]); }
    friend constexpr Vec2 operator/(Vec2 a, float d) { return Vec2(a.v[0] / d, a.v[1] / d); }
    friend constexpr Vec2 operator/(Vec2 a, Vec2 b) { return Vec2(a.v[0] / b.v[0], a.v[1] / b.v[1]); }

private:
    float v[2];
    friend class Vec3;
    friend class Vec4;
};

inline bool fuzzyCompare(Vec2 a, Vec2 b) noexcept
{
    return fuzzyCompare(a.x(), b.x()) && fuzzyCompare(a.y(), b.y());
}

// ---------------------------------------------------------------- Vec3 -------
class Vec3
{
public:
    constexpr Vec3() noexcept : v{0.0f, 0.0f, 0.0f} {}
    constexpr Vec3(float xpos, float ypos, float zpos) noexcept : v{xpos, ypos, zpos} {}
    constexpr explicit Vec3(Vec2 vector) noexcept : v{vector.v[0], vector.v[1], 0.0f} {}
    constexpr Vec3(Vec2 vector, float zpos) noexcept : v{vector.v[0], vector.v[1], zpos} {}
    explicit Vec3(const Vec4 &vector) noexcept;

    constexpr bool isNull() const noexcept { return v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f; }

    constexpr float x() const noexcept { return v[0]; }
    constexpr float y() const noexcept { return v[1]; }
    constexpr float z() const noexcept { return v[2]; }

    constexpr void setX(float aX) noexcept { v[0] = aX; }
    constexpr void setY(float aY) noexcept { v[1] = aY; }
    constexpr void setZ(float aZ) noexcept { v[2] = aZ; }

    constexpr float &operator[](int i) { return v[i]; }
    constexpr float operator[](int i) const { return v[i]; }

    float length() const noexcept { return std::hypot(v[0], v[1], v[2]); }
    constexpr float lengthSquared() const noexcept { return v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; }

    Vec3 normalized() const noexcept
    {
        const float len = length();
        return fuzzyIsNull(len - 1.0f) ? *this
             : fuzzyIsNull(len)        ? Vec3()
                                       : Vec3(v[0] / len, v[1] / len, v[2] / len);
    }
    void normalize() noexcept
    {
        const float len = length();
        if (fuzzyIsNull(len - 1.0f) || fuzzyIsNull(len))
            return;
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }

    constexpr Vec3 &operator+=(Vec3 vector) noexcept { v[0] += vector.v[0]; v[1] += vector.v[1]; v[2] += vector.v[2]; return *this; }
    constexpr Vec3 &operator-=(Vec3 vector) noexcept { v[0] -= vector.v[0]; v[1] -= vector.v[1]; v[2] -= vector.v[2]; return *this; }
    constexpr Vec3 &operator*=(float factor) noexcept { v[0] *= factor; v[1] *= factor; v[2] *= factor; return *this; }
    constexpr Vec3 &operator*=(Vec3 vector) noexcept { v[0] *= vector.v[0]; v[1] *= vector.v[1]; v[2] *= vector.v[2]; return *this; }
    constexpr Vec3 &operator/=(float divisor) { v[0] /= divisor; v[1] /= divisor; v[2] /= divisor; return *this; }
    constexpr Vec3 &operator/=(Vec3 vector) { v[0] /= vector.v[0]; v[1] /= vector.v[1]; v[2] /= vector.v[2]; return *this; }

    static constexpr float dotProduct(Vec3 a, Vec3 b) noexcept
    {
        return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2];
    }
    static constexpr Vec3 crossProduct(Vec3 a, Vec3 b) noexcept
    {
        return Vec3(a.v[1] * b.v[2] - a.v[2] * b.v[1],
                    a.v[2] * b.v[0] - a.v[0] * b.v[2],
                    a.v[0] * b.v[1] - a.v[1] * b.v[0]);
    }
    static Vec3 normal(Vec3 a, Vec3 b) noexcept { return crossProduct(a, b).normalized(); }
    static Vec3 normal(Vec3 a, Vec3 b, Vec3 c) noexcept { return crossProduct(b - a, c - a).normalized(); }

    float distanceToPoint(Vec3 point) const noexcept { return (*this - point).length(); }
    float distanceToPlane(Vec3 plane, Vec3 normal) const noexcept { return dotProduct(*this - plane, normal); }
    float distanceToPlane(Vec3 plane1, Vec3 plane2, Vec3 plane3) const noexcept
    {
        Vec3 n = normal(plane2 - plane1, plane3 - plane1);
        return dotProduct(*this - plane1, n);
    }
    float distanceToLine(Vec3 point, Vec3 direction) const noexcept
    {
        if (direction.isNull())
            return (*this - point).length();
        Vec3 p = point + dotProduct(*this - point, direction) * direction;
        return (*this - p).length();
    }

    Vec2 toVector2D() const noexcept { return Vec2(v[0], v[1]); }
    Vec4 toVector4D() const noexcept;

    friend constexpr bool operator==(Vec3 a, Vec3 b) noexcept { return a.v[0] == b.v[0] && a.v[1] == b.v[1] && a.v[2] == b.v[2]; }
    friend constexpr bool operator!=(Vec3 a, Vec3 b) noexcept { return !(a == b); }
    friend constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept { return Vec3(a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2]); }
    friend constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept { return Vec3(a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2]); }
    friend constexpr Vec3 operator*(float f, Vec3 a) noexcept { return Vec3(a.v[0] * f, a.v[1] * f, a.v[2] * f); }
    friend constexpr Vec3 operator*(Vec3 a, float f) noexcept { return Vec3(a.v[0] * f, a.v[1] * f, a.v[2] * f); }
    friend constexpr Vec3 operator*(Vec3 a, Vec3 b) noexcept { return Vec3(a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2]); }
    friend constexpr Vec3 operator-(Vec3 a) noexcept { return Vec3(-a.v[0], -a.v[1], -a.v[2]); }
    friend constexpr Vec3 operator/(Vec3 a, float d) { return Vec3(a.v[0] / d, a.v[1] / d, a.v[2] / d); }
    friend constexpr Vec3 operator/(Vec3 a, Vec3 b) { return Vec3(a.v[0] / b.v[0], a.v[1] / b.v[1], a.v[2] / b.v[2]); }

private:
    float v[3];
    friend class Vec2;
    friend class Vec4;
};

inline bool fuzzyCompare(Vec3 a, Vec3 b) noexcept
{
    return fuzzyCompare(a.x(), b.x()) && fuzzyCompare(a.y(), b.y()) && fuzzyCompare(a.z(), b.z());
}

// ---------------------------------------------------------------- Vec4 -------
class Vec4
{
public:
    constexpr Vec4() noexcept : v{0.0f, 0.0f, 0.0f, 0.0f} {}
    constexpr Vec4(float xpos, float ypos, float zpos, float wpos) noexcept : v{xpos, ypos, zpos, wpos} {}
    constexpr explicit Vec4(Vec2 vector) noexcept : v{vector.v[0], vector.v[1], 0.0f, 0.0f} {}
    constexpr Vec4(Vec2 vector, float zpos, float wpos) noexcept : v{vector.v[0], vector.v[1], zpos, wpos} {}
    constexpr explicit Vec4(Vec3 vector) noexcept : v{vector.v[0], vector.v[1], vector.v[2], 0.0f} {}
    constexpr Vec4(Vec3 vector, float wpos) noexcept : v{vector.v[0], vector.v[1], vector.v[2], wpos} {}

    constexpr bool isNull() const noexcept
    {
        return v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 0.0f;
    }

    constexpr float x() const noexcept { return v[0]; }
    constexpr float y() const noexcept { return v[1]; }
    constexpr float z() const noexcept { return v[2]; }
    constexpr float w() const noexcept { return v[3]; }

    constexpr void setX(float aX) noexcept { v[0] = aX; }
    constexpr void setY(float aY) noexcept { v[1] = aY; }
    constexpr void setZ(float aZ) noexcept { v[2] = aZ; }
    constexpr void setW(float aW) noexcept { v[3] = aW; }

    constexpr float &operator[](int i) { return v[i]; }
    constexpr float operator[](int i) const { return v[i]; }

    float length() const noexcept { return hypot4(v[0], v[1], v[2], v[3]); }
    constexpr float lengthSquared() const noexcept
    {
        return v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3];
    }

    Vec4 normalized() const noexcept
    {
        const float len = length();
        return fuzzyIsNull(len - 1.0f) ? *this
             : fuzzyIsNull(len)        ? Vec4()
                                       : Vec4(v[0] / len, v[1] / len, v[2] / len, v[3] / len);
    }
    void normalize() noexcept
    {
        const float len = length();
        if (fuzzyIsNull(len - 1.0f) || fuzzyIsNull(len))
            return;
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
        v[3] /= len;
    }

    constexpr Vec4 &operator+=(Vec4 vector) noexcept { v[0] += vector.v[0]; v[1] += vector.v[1]; v[2] += vector.v[2]; v[3] += vector.v[3]; return *this; }
    constexpr Vec4 &operator-=(Vec4 vector) noexcept { v[0] -= vector.v[0]; v[1] -= vector.v[1]; v[2] -= vector.v[2]; v[3] -= vector.v[3]; return *this; }
    constexpr Vec4 &operator*=(float factor) noexcept { v[0] *= factor; v[1] *= factor; v[2] *= factor; v[3] *= factor; return *this; }
    constexpr Vec4 &operator*=(Vec4 vector) noexcept { v[0] *= vector.v[0]; v[1] *= vector.v[1]; v[2] *= vector.v[2]; v[3] *= vector.v[3]; return *this; }
    constexpr Vec4 &operator/=(float divisor) { v[0] /= divisor; v[1] /= divisor; v[2] /= divisor; v[3] /= divisor; return *this; }
    constexpr Vec4 &operator/=(Vec4 vector) { v[0] /= vector.v[0]; v[1] /= vector.v[1]; v[2] /= vector.v[2]; v[3] /= vector.v[3]; return *this; }

    static constexpr float dotProduct(Vec4 a, Vec4 b) noexcept
    {
        return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2] + a.v[3] * b.v[3];
    }

    Vec2 toVector2D() const noexcept { return Vec2(v[0], v[1]); }
    Vec2 toVector2DAffine() const noexcept
    {
        if (fuzzyIsNull(v[3]))
            return Vec2();
        return Vec2(v[0] / v[3], v[1] / v[3]);
    }
    Vec3 toVector3D() const noexcept { return Vec3(v[0], v[1], v[2]); }
    Vec3 toVector3DAffine() const noexcept
    {
        if (fuzzyIsNull(v[3]))
            return Vec3();
        return Vec3(v[0] / v[3], v[1] / v[3], v[2] / v[3]);
    }

    friend constexpr bool operator==(Vec4 a, Vec4 b) noexcept
    {
        return a.v[0] == b.v[0] && a.v[1] == b.v[1] && a.v[2] == b.v[2] && a.v[3] == b.v[3];
    }
    friend constexpr bool operator!=(Vec4 a, Vec4 b) noexcept { return !(a == b); }
    friend constexpr Vec4 operator+(Vec4 a, Vec4 b) noexcept { return Vec4(a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]); }
    friend constexpr Vec4 operator-(Vec4 a, Vec4 b) noexcept { return Vec4(a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3]); }
    friend constexpr Vec4 operator*(float f, Vec4 a) noexcept { return Vec4(a.v[0] * f, a.v[1] * f, a.v[2] * f, a.v[3] * f); }
    friend constexpr Vec4 operator*(Vec4 a, float f) noexcept { return Vec4(a.v[0] * f, a.v[1] * f, a.v[2] * f, a.v[3] * f); }
    friend constexpr Vec4 operator*(Vec4 a, Vec4 b) noexcept { return Vec4(a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3]); }
    friend constexpr Vec4 operator-(Vec4 a) noexcept { return Vec4(-a.v[0], -a.v[1], -a.v[2], -a.v[3]); }
    friend constexpr Vec4 operator/(Vec4 a, float d) { return Vec4(a.v[0] / d, a.v[1] / d, a.v[2] / d, a.v[3] / d); }
    friend constexpr Vec4 operator/(Vec4 a, Vec4 b) { return Vec4(a.v[0] / b.v[0], a.v[1] / b.v[1], a.v[2] / b.v[2], a.v[3] / b.v[3]); }

private:
    float v[4];
    friend class Vec2;
    friend class Vec3;
};

inline bool fuzzyCompare(Vec4 a, Vec4 b) noexcept
{
    return fuzzyCompare(a.x(), b.x()) && fuzzyCompare(a.y(), b.y())
        && fuzzyCompare(a.z(), b.z()) && fuzzyCompare(a.w(), b.w());
}

// --- cross-type members that needed the complete types ------------------------
inline Vec2::Vec2(const Vec3 &vector) noexcept : v{vector.v[0], vector.v[1]} {}
inline Vec2::Vec2(const Vec4 &vector) noexcept : v{vector.v[0], vector.v[1]} {}
inline Vec3 Vec2::toVector3D() const noexcept { return Vec3(v[0], v[1], 0.0f); }
inline Vec4 Vec2::toVector4D() const noexcept { return Vec4(v[0], v[1], 0.0f, 0.0f); }
inline Vec3::Vec3(const Vec4 &vector) noexcept : v{vector.v[0], vector.v[1], vector.v[2]} {}
inline Vec4 Vec3::toVector4D() const noexcept { return Vec4(v[0], v[1], v[2], 0.0f); }

} // namespace iris

#endif // IRIS_VEC_H
