/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_QUAT_H
#define IRIS_QUAT_H

// -----------------------------------------------------------------------------
// iris::Quat — the document model's rotation type, a drop-in for QQuaternion.
//
// EVERY numeric here is a deliberate reproduction of Qt's, not an independent
// implementation of the same maths. Rotations are the one part of the document
// that is BOTH persisted and compared: `rotQuat` round trips through every saved
// scene, the light convention is a -90 degree pitch applied to it, and
// scene.reopen_fidelity asserts the reopened values byte for byte. An algebraically
// equivalent formula with a different order of operations would drift in the last
// bits and rotate old scenes.
//
// So: the multiply is Qt's three-multiplication trick (NOT the textbook 16-multiply
// form — they differ in the last bits); fromEulerAngles halves the angles before
// converting to radians (which is where its rounding comes from); toEulerAngles
// keeps the same asin/copysign branch structure including the gimbal-lock fallback;
// normalized() divides by length() with no unit short circuit (a Qt 6 change from
// Qt 5's double accumulation). tests/math/test_math_parity checks all of it against
// the real QQuaternion, bit for bit, over randomized input.
// -----------------------------------------------------------------------------

#include "core/math/mat3.h"
#include "core/math/vec.h"

namespace iris
{

class Quat
{
public:
    constexpr Quat() noexcept : wp(1.0f), xp(0.0f), yp(0.0f), zp(0.0f) {}
    constexpr Quat(float aScalar, float xpos, float ypos, float zpos) noexcept
        : wp(aScalar), xp(xpos), yp(ypos), zp(zpos) {}
    constexpr Quat(float aScalar, const Vec3 &aVector) noexcept
        : wp(aScalar), xp(aVector.x()), yp(aVector.y()), zp(aVector.z()) {}
    constexpr explicit Quat(const Vec4 &aVector) noexcept
        : wp(aVector.w()), xp(aVector.x()), yp(aVector.y()), zp(aVector.z()) {}

    constexpr bool isNull() const noexcept
    {
        return wp == 0.0f && xp == 0.0f && yp == 0.0f && zp == 0.0f;
    }
    constexpr bool isIdentity() const noexcept
    {
        return wp == 1.0f && xp == 0.0f && yp == 0.0f && zp == 0.0f;
    }

    constexpr float x() const noexcept { return xp; }
    constexpr float y() const noexcept { return yp; }
    constexpr float z() const noexcept { return zp; }
    constexpr float scalar() const noexcept { return wp; }

    constexpr void setX(float aX) noexcept { xp = aX; }
    constexpr void setY(float aY) noexcept { yp = aY; }
    constexpr void setZ(float aZ) noexcept { zp = aZ; }
    constexpr void setScalar(float aScalar) noexcept { wp = aScalar; }

    constexpr Vec3 vector() const noexcept { return Vec3(xp, yp, zp); }
    constexpr void setVector(const Vec3 &aVector) noexcept
    {
        xp = aVector.x();
        yp = aVector.y();
        zp = aVector.z();
    }
    constexpr void setVector(float aX, float aY, float aZ) noexcept { xp = aX; yp = aY; zp = aZ; }
    constexpr Vec4 toVector4D() const noexcept { return Vec4(xp, yp, zp, wp); }

    float length() const noexcept { return hypot4(xp, yp, zp, wp); }
    float lengthSquared() const noexcept { return xp * xp + yp * yp + zp * zp + wp * wp; }

    // Qt 6 divides by length() unconditionally (there is no "already unit"
    // short circuit, and no double-precision accumulation — that was Qt 5).
    Quat normalized() const noexcept
    {
        const float len = length();
        if (fuzzyIsNull(len))
            return Quat(0.0f, 0.0f, 0.0f, 0.0f);
        return Quat(wp / len, xp / len, yp / len, zp / len);
    }

    void normalize() noexcept
    {
        const float len = length();
        if (fuzzyIsNull(len))
            return;
        wp /= len;
        xp /= len;
        yp /= len;
        zp /= len;
    }

    constexpr Quat inverted() const noexcept
    {
        double len = double(wp) * double(wp) + double(xp) * double(xp)
                   + double(yp) * double(yp) + double(zp) * double(zp);
        if (!fuzzyIsNullConstexpr(len))
            return Quat(float(double(wp) / len), float(double(-xp) / len),
                        float(double(-yp) / len), float(double(-zp) / len));
        return Quat(0.0f, 0.0f, 0.0f, 0.0f);
    }

    constexpr Quat conjugated() const noexcept { return Quat(wp, -xp, -yp, -zp); }

    Vec3 rotatedVector(const Vec3 &vector) const noexcept
    {
        return (*this * Quat(0, vector) * conjugated()).vector();
    }

    constexpr Quat &operator+=(const Quat &q) noexcept { wp += q.wp; xp += q.xp; yp += q.yp; zp += q.zp; return *this; }
    constexpr Quat &operator-=(const Quat &q) noexcept { wp -= q.wp; xp -= q.xp; yp -= q.yp; zp -= q.zp; return *this; }
    constexpr Quat &operator*=(float factor) noexcept { wp *= factor; xp *= factor; yp *= factor; zp *= factor; return *this; }
    constexpr Quat &operator/=(float divisor) { wp /= divisor; xp /= divisor; yp /= divisor; zp /= divisor; return *this; }
    constexpr Quat &operator*=(const Quat &q) noexcept { *this = *this * q; return *this; }

    static constexpr float dotProduct(const Quat &q1, const Quat &q2) noexcept
    {
        return q1.wp * q2.wp + q1.xp * q2.xp + q1.yp * q2.yp + q1.zp * q2.zp;
    }

    // ---- axis/angle ---------------------------------------------------------
    static Quat fromAxisAndAngle(const Vec3 &axis, float angle)
    {
        // Normalize the result in case the axis is close to zero, as Qt does.
        float a = degreesToRadians(angle / 2.0f);
        float s = std::sin(a);
        float c = std::cos(a);
        Vec3 ax = axis.normalized();
        return Quat(c, ax.x() * s, ax.y() * s, ax.z() * s).normalized();
    }

    static Quat fromAxisAndAngle(float x, float y, float z, float angle)
    {
        float length = std::hypot(x, y, z);
        if (!fuzzyIsNull(length - 1.0f) && !fuzzyIsNull(length)) {
            x /= length;
            y /= length;
            z /= length;
        }
        float a = degreesToRadians(angle / 2.0f);
        float s = std::sin(a);
        float c = std::cos(a);
        return Quat(c, x * s, y * s, z * s).normalized();
    }

    void getAxisAndAngle(float *x, float *y, float *z, float *angle) const
    {
        // q = cos(A/2) + sin(A/2) * (x*i + y*j + z*k). Qt 6 takes the angle from
        // atan2(|v|, w) rather than acos(w) — better conditioned near identity —
        // and computes |v| scaled by the largest component so it cannot overflow.
        const float ax = std::abs(xp), ay = std::abs(yp), az = std::abs(zp);
        const float scale = std::max(ax, std::max(ay, az));
        if (scale == 0.0f) {
            *x = *y = *z = *angle = 0.0f;
            return;
        }

        const float sx = xp / scale, sy = yp / scale, sz = zp / scale;
        const float length = scale * std::sqrt(sz * sz + (sx * sx + sy * sy));
        if (fuzzyIsNull(length)) {
            *x = *y = *z = *angle = 0.0f;
            return;
        }

        if (fuzzyCompare(length, 1.0f)) {
            *x = xp;
            *y = yp;
            *z = zp;
        } else {
            *x = xp / length;
            *y = yp / length;
            *z = zp / length;
        }

        *angle = radiansToDegrees(2.0f * std::atan2(length, wp));
    }

    void getAxisAndAngle(Vec3 *axis, float *angle) const
    {
        float aX, aY, aZ;
        getAxisAndAngle(&aX, &aY, &aZ, angle);
        *axis = Vec3(aX, aY, aZ);
    }

    // ---- euler angles -------------------------------------------------------
    // pitch = rotation about X, yaw = about Y, roll = about Z; applied in the
    // order Qt applies them. This is the convention every saved scene uses.
    static Quat fromEulerAngles(float pitch, float yaw, float roll)
    {
        pitch *= 0.5f;
        yaw *= 0.5f;
        roll *= 0.5f;

        const float c1 = std::cos(degreesToRadians(yaw));
        const float s1 = std::sin(degreesToRadians(yaw));
        const float c2 = std::cos(degreesToRadians(roll));
        const float s2 = std::sin(degreesToRadians(roll));
        const float c3 = std::cos(degreesToRadians(pitch));
        const float s3 = std::sin(degreesToRadians(pitch));
        const float c1c2 = c1 * c2;
        const float s1s2 = s1 * s2;

        const float w = c1c2 * c3 + s1s2 * s3;
        const float x = c1c2 * s3 + s1s2 * c3;
        const float y = s1 * c2 * c3 - c1 * s2 * s3;
        const float z = c1 * s2 * c3 - s1 * c2 * s3;

        return Quat(w, x, y, z);
    }

    static Quat fromEulerAngles(const Vec3 &eulerAngles)
    {
        return fromEulerAngles(eulerAngles.x(), eulerAngles.y(), eulerAngles.z());
    }

    void getEulerAngles(float *pitch, float *yaw, float *roll) const
    {
        // Qt 6's shape: normalize by length() first, then a single asin with a
        // copysign fallback once the pitch is within 1e-5 of straight up/down.
        float x = xp, y = yp, z = zp, w = wp;
        const float len = length();
        if (!fuzzyIsNull(len)) {
            x /= len;
            y /= len;
            z /= len;
            w /= len;
        }

        const float xx = x * x;
        const float sinp = (y * z - w * x) * -2.0f;
        if (std::abs(sinp) < kGimbalLimit) {
            const float yawNum = z * x + w * y;
            const float yawDen = y * y + xx;
            const float rollNum = x * y + z * w;
            const float rollDen = z * z + xx;
            *pitch = std::asin(sinp);
            *yaw = std::atan2(yawNum + yawNum, 1.0f - (yawDen + yawDen));
            *roll = std::atan2(rollNum + rollNum, 1.0f - (rollDen + rollDen));
        } else {
            *pitch = std::copysign(float(kHalfPi), sinp);
            *yaw = 2.0f * std::atan2(y, w);
            *roll = 0.0f;
        }

        *pitch = radiansToDegrees(*pitch);
        *yaw = radiansToDegrees(*yaw);
        *roll = radiansToDegrees(*roll);
    }

    Vec3 toEulerAngles() const
    {
        float pitch, yaw, roll;
        getEulerAngles(&pitch, &yaw, &roll);
        return Vec3(pitch, yaw, roll);
    }

    // ---- rotation matrices --------------------------------------------------
    Mat3 toRotationMatrix() const
    {
        Mat3 rot3x3(Mat3::Uninitialized);

        const float f2x = xp + xp;
        const float f2y = yp + yp;
        const float f2z = zp + zp;
        const float f2xw = f2x * wp;
        const float f2yw = f2y * wp;
        const float f2zw = f2z * wp;
        const float f2xx = f2x * xp;
        const float f2xy = f2x * yp;
        const float f2xz = f2x * zp;
        const float f2yy = f2y * yp;
        const float f2yz = f2y * zp;
        const float f2zz = f2z * zp;

        rot3x3(0, 0) = 1.0f - (f2yy + f2zz);
        rot3x3(0, 1) =         f2xy - f2zw;
        rot3x3(0, 2) =         f2xz + f2yw;
        rot3x3(1, 0) =         f2xy + f2zw;
        rot3x3(1, 1) = 1.0f - (f2xx + f2zz);
        rot3x3(1, 2) =         f2yz - f2xw;
        rot3x3(2, 0) =         f2xz - f2yw;
        rot3x3(2, 1) =         f2yz + f2xw;
        rot3x3(2, 2) = 1.0f - (f2xx + f2yy);

        return rot3x3;
    }

    static Quat fromRotationMatrix(const Mat3 &rot3x3)
    {
        float scalar;
        float axis[3];

        const float trace = rot3x3(0, 0) + rot3x3(1, 1) + rot3x3(2, 2);
        if (trace > 0.00000001f) {
            const float s = 2.0f * std::sqrt(trace + 1.0f);
            scalar = 0.25f * s;
            axis[0] = (rot3x3(2, 1) - rot3x3(1, 2)) / s;
            axis[1] = (rot3x3(0, 2) - rot3x3(2, 0)) / s;
            axis[2] = (rot3x3(1, 0) - rot3x3(0, 1)) / s;
        } else {
            static const int s_next[3] = { 1, 2, 0 };
            int i = 0;
            if (rot3x3(1, 1) > rot3x3(0, 0))
                i = 1;
            if (rot3x3(2, 2) > rot3x3(i, i))
                i = 2;
            int j = s_next[i];
            int k = s_next[j];

            const float s = 2.0f * std::sqrt(rot3x3(i, i) - rot3x3(j, j) - rot3x3(k, k) + 1.0f);
            axis[i] = 0.25f * s;
            scalar = (rot3x3(k, j) - rot3x3(j, k)) / s;
            axis[j] = (rot3x3(j, i) + rot3x3(i, j)) / s;
            axis[k] = (rot3x3(k, i) + rot3x3(i, k)) / s;
        }

        return Quat(scalar, axis[0], axis[1], axis[2]);
    }

    // ---- axes / direction ---------------------------------------------------
    static Quat fromAxes(const Vec3 &xAxis, const Vec3 &yAxis, const Vec3 &zAxis)
    {
        Mat3 rot3x3(Mat3::Uninitialized);
        rot3x3(0, 0) = xAxis.x();
        rot3x3(1, 0) = xAxis.y();
        rot3x3(2, 0) = xAxis.z();
        rot3x3(0, 1) = yAxis.x();
        rot3x3(1, 1) = yAxis.y();
        rot3x3(2, 1) = yAxis.z();
        rot3x3(0, 2) = zAxis.x();
        rot3x3(1, 2) = zAxis.y();
        rot3x3(2, 2) = zAxis.z();

        return fromRotationMatrix(rot3x3);
    }

    void getAxes(Vec3 *xAxis, Vec3 *yAxis, Vec3 *zAxis) const
    {
        const Mat3 rot3x3(toRotationMatrix());

        if (xAxis)
            *xAxis = Vec3(rot3x3(0, 0), rot3x3(1, 0), rot3x3(2, 0));
        if (yAxis)
            *yAxis = Vec3(rot3x3(0, 1), rot3x3(1, 1), rot3x3(2, 1));
        if (zAxis)
            *zAxis = Vec3(rot3x3(0, 2), rot3x3(1, 2), rot3x3(2, 2));
    }

    static Quat fromDirection(const Vec3 &direction, const Vec3 &up)
    {
        if (fuzzyIsNull(direction.x()) && fuzzyIsNull(direction.y()) && fuzzyIsNull(direction.z()))
            return Quat();

        const Vec3 zAxis(direction.normalized());
        Vec3 xAxis(Vec3::crossProduct(up, zAxis));
        if (fuzzyIsNull(xAxis.lengthSquared())) {
            // collinear or invalid up vector; derive shortest arc to new direction
            return rotationTo(Vec3(0.0f, 0.0f, 1.0f), zAxis);
        }

        xAxis.normalize();
        const Vec3 yAxis(Vec3::crossProduct(zAxis, xAxis));

        return fromAxes(xAxis, yAxis, zAxis);
    }

    static Quat rotationTo(const Vec3 &from, const Vec3 &to)
    {
        const Vec3 v0(from.normalized());
        const Vec3 v1(to.normalized());

        float d = Vec3::dotProduct(v0, v1) + 1.0f;

        // If the destination is close to the inverse of the source, any axis works.
        if (fuzzyIsNull(d)) {
            Vec3 axis(Vec3::crossProduct(Vec3(1.0f, 0.0f, 0.0f), v0));
            if (fuzzyIsNull(axis.lengthSquared()))
                axis = Vec3::crossProduct(Vec3(0.0f, 1.0f, 0.0f), v0);
            axis.normalize();

            // same as fromAxisAndAngle(axis, 180.0f)
            return Quat(0.0f, axis.x(), axis.y(), axis.z());
        }

        d = std::sqrt(2.0f * d);
        const Vec3 axis(Vec3::crossProduct(v0, v1) / d);

        return Quat(d * 0.5f, axis).normalized();
    }

    // ---- interpolation ------------------------------------------------------
    static Quat slerp(const Quat &q1, const Quat &q2, float t)
    {
        if (t <= 0.0f)
            return q1;
        if (t >= 1.0f)
            return q2;

        Quat q2b(q2);
        float dot = dotProduct(q1, q2);
        if (dot < 0.0f) {
            q2b = -q2b;
            dot = -dot;
        }

        float factor1 = 1.0f - t;
        float factor2 = t;
        if ((1.0f - dot) > 0.0000001) {
            float angle = float(std::acos(dot));
            float sinOfAngle = float(std::sin(angle));
            if (sinOfAngle > 0.0000001) {
                factor1 = float(std::sin((1.0f - t) * angle)) / sinOfAngle;
                factor2 = float(std::sin(t * angle)) / sinOfAngle;
            }
        }

        return q1 * factor1 + q2b * factor2;
    }

    static Quat nlerp(const Quat &q1, const Quat &q2, float t)
    {
        if (t <= 0.0f)
            return q1;
        if (t >= 1.0f)
            return q2;

        Quat q2b(q2);
        if (dotProduct(q1, q2) < 0.0f)
            q2b = -q2b;

        return (q1 * (1.0f - t) + q2b * t).normalized();
    }

    // ---- operators ----------------------------------------------------------
    friend constexpr bool operator==(const Quat &q1, const Quat &q2) noexcept
    {
        return q1.wp == q2.wp && q1.xp == q2.xp && q1.yp == q2.yp && q1.zp == q2.zp;
    }
    friend constexpr bool operator!=(const Quat &q1, const Quat &q2) noexcept { return !(q1 == q2); }

    friend constexpr Quat operator*(const Quat &q1, const Quat &q2) noexcept
    {
        // Qt's three-multiplication form. Do not "simplify" it: the textbook
        // 16-multiply product differs in the last bits and moves saved rotations.
        float yy = (q1.wp - q1.yp) * (q2.wp + q2.zp);
        float zz = (q1.wp + q1.yp) * (q2.wp - q2.zp);
        float ww = (q1.zp + q1.xp) * (q2.xp + q2.yp);
        float xx = ww + yy + zz;
        float qq = 0.5f * (xx + (q1.zp - q1.xp) * (q2.xp - q2.yp));

        float w = qq - ww + (q1.zp - q1.yp) * (q2.yp - q2.zp);
        float x = qq - xx + (q1.xp + q1.wp) * (q2.xp + q2.wp);
        float y = qq - yy + (q1.wp - q1.xp) * (q2.yp + q2.zp);
        float z = qq - zz + (q1.zp + q1.yp) * (q2.wp - q2.xp);

        return Quat(w, x, y, z);
    }

    friend constexpr Quat operator+(const Quat &q1, const Quat &q2) noexcept
    {
        return Quat(q1.wp + q2.wp, q1.xp + q2.xp, q1.yp + q2.yp, q1.zp + q2.zp);
    }
    friend constexpr Quat operator-(const Quat &q1, const Quat &q2) noexcept
    {
        return Quat(q1.wp - q2.wp, q1.xp - q2.xp, q1.yp - q2.yp, q1.zp - q2.zp);
    }
    friend constexpr Quat operator*(float factor, const Quat &q) noexcept
    {
        return Quat(q.wp * factor, q.xp * factor, q.yp * factor, q.zp * factor);
    }
    friend constexpr Quat operator*(const Quat &q, float factor) noexcept
    {
        return Quat(q.wp * factor, q.xp * factor, q.yp * factor, q.zp * factor);
    }
    friend constexpr Quat operator-(const Quat &q) noexcept
    {
        return Quat(-q.wp, -q.xp, -q.yp, -q.zp);
    }
    friend constexpr Quat operator/(const Quat &q, float divisor)
    {
        return Quat(q.wp / divisor, q.xp / divisor, q.yp / divisor, q.zp / divisor);
    }
    friend Vec3 operator*(const Quat &q, const Vec3 &vec) { return q.rotatedVector(vec); }

private:
    // constexpr-usable form of fuzzyIsNull(double), for inverted().
    static constexpr bool fuzzyIsNullConstexpr(double d) noexcept
    {
        return (d < 0 ? -d : d) <= 0.000000000001;
    }

    static constexpr double kHalfPi = 1.57079632679489661923;
    // 1.0f - 0.00001f: past this the pitch is straight up or down and the
    // yaw/roll split stops being unique.
    static constexpr float kGimbalLimit = 0.99999f;

    float wp, xp, yp, zp;
};

inline bool fuzzyCompare(const Quat &q1, const Quat &q2) noexcept
{
    return fuzzyCompare(q1.scalar(), q2.scalar()) && fuzzyCompare(q1.x(), q2.x())
        && fuzzyCompare(q1.y(), q2.y()) && fuzzyCompare(q1.z(), q2.z());
}

} // namespace iris

#endif // IRIS_QUAT_H
