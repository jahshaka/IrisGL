/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_MAT4_H
#define IRIS_MAT4_H

// -----------------------------------------------------------------------------
// iris::Mat4 — the document model's 4x4, a drop-in for QMatrix4x4.
//
// Storage is column-major (m[column][row]), like Qt's and like OpenGL's, so
// constData() can still be handed straight to anything that wants 16 floats.
//
// THE FLAG BITS ARE PART OF THE NUMERICS, not an optimization we may drop.
// QMatrix4x4 tracks what kind of transform it holds and takes a shorter
// arithmetic path when it can — a translate() on an identity matrix ASSIGNS
// where the general path would multiply-add, and inverted() of a
// translation+rotation transposes the basis instead of running Cramer's rule.
// Those paths do not merely run faster, they round differently. A Mat4 without
// them would disagree with every matrix Qt has ever built in this program, and
// the disagreement would surface as gizmo and picking drift. So the flags are
// reproduced, transition for transition, and tests/math/test_math_parity walks
// randomized call SEQUENCES (not just single calls) through both types and
// compares all 16 floats bitwise.
//
// Deliberately NOT provided: optimize(), toTransform(), mapRect(), the QPoint /
// QRect overloads and the QGenericMatrix template constructors. They have zero
// call sites in the tree; a compile error is a better outcome than a
// reimplementation nobody exercises. Mat3 conversions live in mat3.h,
// Qt conversions in qtinterop.h.
// -----------------------------------------------------------------------------

#include "core/math/mat3.h"
#include "core/math/quat.h"
#include "core/math/vec.h"

namespace iris
{

class Mat4
{
public:
    // When matrices are multiplied, the flag bits are or-ed together.
    // The ordering of the values matters: ident < t < s < r2d < r < p.
    enum Flag {
        Identity    = 0x0000,
        Translation = 0x0001,
        Scale       = 0x0002,
        Rotation2D  = 0x0004,
        Rotation    = 0x0008,
        Perspective = 0x0010,
        General     = 0x001f
    };

    struct Uninitialized_t {};
    static constexpr Uninitialized_t Uninitialized{};

    Mat4() noexcept { setToIdentity(); }

    // Uninitialized elements, General flags — Qt's QMatrix4x4(Qt::Uninitialized).
    explicit Mat4(Uninitialized_t) noexcept : flagBits(General) {}

    // Row-major argument order, column-major storage — as Qt spells it.
    Mat4(float m11, float m12, float m13, float m14,
         float m21, float m22, float m23, float m24,
         float m31, float m32, float m33, float m34,
         float m41, float m42, float m43, float m44) noexcept
    {
        m[0][0] = m11; m[0][1] = m21; m[0][2] = m31; m[0][3] = m41;
        m[1][0] = m12; m[1][1] = m22; m[1][2] = m32; m[1][3] = m42;
        m[2][0] = m13; m[2][1] = m23; m[2][2] = m33; m[2][3] = m43;
        m[3][0] = m14; m[3][1] = m24; m[3][2] = m34; m[3][3] = m44;
        flagBits = General;
    }

    // 16 floats in ROW-MAJOR order (this is what QMatrix4x4(const float *) means).
    explicit Mat4(const float *values) noexcept
    {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                m[col][row] = values[row * 4 + col];
        flagBits = General;
    }

    // The upper-left 3x3 of a basis, rest identity.
    explicit Mat4(const Mat3 &basis) noexcept
    {
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                if (col < 3 && row < 3)
                    m[col][row] = basis(row, col);
                else
                    m[col][row] = (col == row) ? 1.0f : 0.0f;
            }
        }
        flagBits = General;
    }

    // ---- element access -----------------------------------------------------
    const float &operator()(int row, int column) const { return m[column][row]; }
    float &operator()(int row, int column)
    {
        flagBits = General;
        return m[column][row];
    }

    Vec4 column(int index) const
    {
        return Vec4(m[index][0], m[index][1], m[index][2], m[index][3]);
    }
    void setColumn(int index, const Vec4 &value)
    {
        m[index][0] = value.x();
        m[index][1] = value.y();
        m[index][2] = value.z();
        m[index][3] = value.w();
        flagBits = General;
    }
    Vec4 row(int index) const
    {
        return Vec4(m[0][index], m[1][index], m[2][index], m[3][index]);
    }
    void setRow(int index, const Vec4 &value)
    {
        m[0][index] = value.x();
        m[1][index] = value.y();
        m[2][index] = value.z();
        m[3][index] = value.w();
        flagBits = General;
    }

    float *data() noexcept
    {
        // The caller may write through this pointer.
        flagBits = General;
        return *m;
    }
    const float *data() const noexcept { return *m; }
    const float *constData() const noexcept { return *m; }

    int flags() const noexcept { return flagBits; }

    bool isAffine() const noexcept
    {
        return m[0][3] == 0.0f && m[1][3] == 0.0f && m[2][3] == 0.0f && m[3][3] == 1.0f;
    }

    bool isIdentity() const noexcept
    {
        if (flagBits == Identity)
            return true;
        if (m[0][0] != 1.0f || m[0][1] != 0.0f || m[0][2] != 0.0f) return false;
        if (m[0][3] != 0.0f || m[1][0] != 0.0f || m[1][1] != 1.0f) return false;
        if (m[1][2] != 0.0f || m[1][3] != 0.0f || m[2][0] != 0.0f) return false;
        if (m[2][1] != 0.0f || m[2][2] != 1.0f || m[2][3] != 0.0f) return false;
        if (m[3][0] != 0.0f || m[3][1] != 0.0f || m[3][2] != 0.0f) return false;
        return (m[3][3] == 1.0f);
    }

    void setToIdentity() noexcept
    {
        m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = 0.0f;
        m[3][0] = 0.0f; m[3][1] = 0.0f; m[3][2] = 0.0f; m[3][3] = 1.0f;
        flagBits = Identity;
    }

    void fill(float value) noexcept
    {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                m[col][row] = value;
        flagBits = General;
    }

    void copyDataTo(float *values) const noexcept
    {
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                values[row * 4 + col] = m[col][row];
    }

    // ---- transform builders -------------------------------------------------
    void translate(const Vec3 &vector) { translate(vector.x(), vector.y(), vector.z()); }

    void translate(float vx, float vy, float vz)
    {
        if (flagBits == Identity) {
            m[3][0] = vx;
            m[3][1] = vy;
            m[3][2] = vz;
        } else if (flagBits == Translation) {
            m[3][0] += vx;
            m[3][1] += vy;
            m[3][2] += vz;
        } else if (flagBits == Scale) {
            m[3][0] = m[0][0] * vx;
            m[3][1] = m[1][1] * vy;
            m[3][2] = m[2][2] * vz;
        } else if (flagBits == (Scale | Translation)) {
            m[3][0] += m[0][0] * vx;
            m[3][1] += m[1][1] * vy;
            m[3][2] += m[2][2] * vz;
        } else if (flagBits < Rotation) {
            // Anything with Rotation2D but no arbitrary rotation: Z is still
            // uncoupled and the last row is untouched.
            m[3][0] += m[0][0] * vx + m[1][0] * vy;
            m[3][1] += m[0][1] * vx + m[1][1] * vy;
            m[3][2] += m[2][2] * vz;
        } else {
            m[3][0] += m[0][0] * vx + m[1][0] * vy + m[2][0] * vz;
            m[3][1] += m[0][1] * vx + m[1][1] * vy + m[2][1] * vz;
            m[3][2] += m[0][2] * vx + m[1][2] * vy + m[2][2] * vz;
            m[3][3] += m[0][3] * vx + m[1][3] * vy + m[2][3] * vz;
        }
        flagBits |= Translation;
    }

    void translate(float x, float y) { translate(x, y, 0.0f); }

    void scale(const Vec3 &vector) { scale(vector.x(), vector.y(), vector.z()); }

    void scale(float vx, float vy, float vz)
    {
        if (flagBits < Scale) {
            m[0][0] = vx;
            m[1][1] = vy;
            m[2][2] = vz;
        } else if (flagBits < Rotation2D) {
            m[0][0] *= vx;
            m[1][1] *= vy;
            m[2][2] *= vz;
        } else if (flagBits < Rotation) {
            m[0][0] *= vx;
            m[0][1] *= vx;
            m[1][0] *= vy;
            m[1][1] *= vy;
            m[2][2] *= vz;
        } else {
            m[0][0] *= vx; m[0][1] *= vx; m[0][2] *= vx; m[0][3] *= vx;
            m[1][0] *= vy; m[1][1] *= vy; m[1][2] *= vy; m[1][3] *= vy;
            m[2][0] *= vz; m[2][1] *= vz; m[2][2] *= vz; m[2][3] *= vz;
        }
        flagBits |= Scale;
    }

    void scale(float x, float y)
    {
        if (flagBits < Scale) {
            m[0][0] = x;
            m[1][1] = y;
        } else if (flagBits < Rotation2D) {
            m[0][0] *= x;
            m[1][1] *= y;
        } else if (flagBits < Rotation) {
            m[0][0] *= x;
            m[0][1] *= x;
            m[1][0] *= y;
            m[1][1] *= y;
        } else {
            m[0][0] *= x; m[0][1] *= x; m[0][2] *= x; m[0][3] *= x;
            m[1][0] *= y; m[1][1] *= y; m[1][2] *= y; m[1][3] *= y;
        }
        flagBits |= Scale;
    }

    void scale(float factor)
    {
        if (flagBits < Scale) {
            m[0][0] = factor;
            m[1][1] = factor;
            m[2][2] = factor;
        } else if (flagBits < Rotation2D) {
            m[0][0] *= factor;
            m[1][1] *= factor;
            m[2][2] *= factor;
        } else if (flagBits < Rotation) {
            m[0][0] *= factor;
            m[0][1] *= factor;
            m[1][0] *= factor;
            m[1][1] *= factor;
            m[2][2] *= factor;
        } else {
            m[0][0] *= factor; m[0][1] *= factor; m[0][2] *= factor; m[0][3] *= factor;
            m[1][0] *= factor; m[1][1] *= factor; m[1][2] *= factor; m[1][3] *= factor;
            m[2][0] *= factor; m[2][1] *= factor; m[2][2] *= factor; m[2][3] *= factor;
        }
        flagBits |= Scale;
    }

    void rotate(float angle, const Vec3 &vector) { rotate(angle, vector.x(), vector.y(), vector.z()); }

    void rotate(float angle, float x, float y, float z = 0.0f)
    {
        if (angle == 0.0f)
            return;
        float c, s;
        if (angle == 90.0f || angle == -270.0f) {
            s = 1.0f;
            c = 0.0f;
        } else if (angle == -90.0f || angle == 270.0f) {
            s = -1.0f;
            c = 0.0f;
        } else if (angle == 180.0f || angle == -180.0f) {
            s = 0.0f;
            c = -1.0f;
        } else {
            float a = degreesToRadians(angle);
            c = std::cos(a);
            s = std::sin(a);
        }
        if (x == 0.0f) {
            if (y == 0.0f) {
                if (z != 0.0f) {
                    // Rotate around the Z axis.
                    if (z < 0)
                        s = -s;
                    float tmp;
                    m[0][0] = (tmp = m[0][0]) * c + m[1][0] * s;
                    m[1][0] = m[1][0] * c - tmp * s;
                    m[0][1] = (tmp = m[0][1]) * c + m[1][1] * s;
                    m[1][1] = m[1][1] * c - tmp * s;
                    m[0][2] = (tmp = m[0][2]) * c + m[1][2] * s;
                    m[1][2] = m[1][2] * c - tmp * s;
                    m[0][3] = (tmp = m[0][3]) * c + m[1][3] * s;
                    m[1][3] = m[1][3] * c - tmp * s;

                    flagBits |= Rotation2D;
                    return;
                }
            } else if (z == 0.0f) {
                // Rotate around the Y axis.
                if (y < 0)
                    s = -s;
                float tmp;
                m[2][0] = (tmp = m[2][0]) * c + m[0][0] * s;
                m[0][0] = m[0][0] * c - tmp * s;
                m[2][1] = (tmp = m[2][1]) * c + m[0][1] * s;
                m[0][1] = m[0][1] * c - tmp * s;
                m[2][2] = (tmp = m[2][2]) * c + m[0][2] * s;
                m[0][2] = m[0][2] * c - tmp * s;
                m[2][3] = (tmp = m[2][3]) * c + m[0][3] * s;
                m[0][3] = m[0][3] * c - tmp * s;

                flagBits |= Rotation;
                return;
            }
        } else if (y == 0.0f && z == 0.0f) {
            // Rotate around the X axis.
            if (x < 0)
                s = -s;
            float tmp;
            m[1][0] = (tmp = m[1][0]) * c + m[2][0] * s;
            m[2][0] = m[2][0] * c - tmp * s;
            m[1][1] = (tmp = m[1][1]) * c + m[2][1] * s;
            m[2][1] = m[2][1] * c - tmp * s;
            m[1][2] = (tmp = m[1][2]) * c + m[2][2] * s;
            m[2][2] = m[2][2] * c - tmp * s;
            m[1][3] = (tmp = m[1][3]) * c + m[2][3] * s;
            m[2][3] = m[2][3] * c - tmp * s;

            flagBits |= Rotation;
            return;
        }

        double len = double(x) * double(x) + double(y) * double(y) + double(z) * double(z);
        if (!fuzzyIsNull(len - 1.0) && !fuzzyIsNull(len)) {
            len = std::sqrt(len);
            x = float(double(x) / len);
            y = float(double(y) / len);
            z = float(double(z) / len);
        }
        float ic = 1.0f - c;
        Mat4 rot(Uninitialized);
        rot.m[0][0] = x * x * ic + c;
        rot.m[1][0] = x * y * ic - z * s;
        rot.m[2][0] = x * z * ic + y * s;
        rot.m[3][0] = 0.0f;
        rot.m[0][1] = y * x * ic + z * s;
        rot.m[1][1] = y * y * ic + c;
        rot.m[2][1] = y * z * ic - x * s;
        rot.m[3][1] = 0.0f;
        rot.m[0][2] = x * z * ic - y * s;
        rot.m[1][2] = y * z * ic + x * s;
        rot.m[2][2] = z * z * ic + c;
        rot.m[3][2] = 0.0f;
        rot.m[0][3] = 0.0f;
        rot.m[1][3] = 0.0f;
        rot.m[2][3] = 0.0f;
        rot.m[3][3] = 1.0f;
        rot.flagBits = Rotation;
        *this *= rot;
    }

    void rotate(const Quat &quaternion)
    {
        Mat4 mm(Uninitialized);

        const float f2x = quaternion.x() + quaternion.x();
        const float f2y = quaternion.y() + quaternion.y();
        const float f2z = quaternion.z() + quaternion.z();
        const float f2xw = f2x * quaternion.scalar();
        const float f2yw = f2y * quaternion.scalar();
        const float f2zw = f2z * quaternion.scalar();
        const float f2xx = f2x * quaternion.x();
        const float f2xy = f2x * quaternion.y();
        const float f2xz = f2x * quaternion.z();
        const float f2yy = f2y * quaternion.y();
        const float f2yz = f2y * quaternion.z();
        const float f2zz = f2z * quaternion.z();

        mm.m[0][0] = 1.0f - (f2yy + f2zz);
        mm.m[1][0] =         f2xy - f2zw;
        mm.m[2][0] =         f2xz + f2yw;
        mm.m[3][0] = 0.0f;
        mm.m[0][1] =         f2xy + f2zw;
        mm.m[1][1] = 1.0f - (f2xx + f2zz);
        mm.m[2][1] =         f2yz - f2xw;
        mm.m[3][1] = 0.0f;
        mm.m[0][2] =         f2xz - f2yw;
        mm.m[1][2] =         f2yz + f2xw;
        mm.m[2][2] = 1.0f - (f2xx + f2yy);
        mm.m[3][2] = 0.0f;
        mm.m[0][3] = 0.0f;
        mm.m[1][3] = 0.0f;
        mm.m[2][3] = 0.0f;
        mm.m[3][3] = 1.0f;
        mm.flagBits = Rotation;
        *this *= mm;
    }

    void ortho(float left, float right, float bottom, float top, float nearPlane, float farPlane)
    {
        // Bail out if the projection volume is zero-sized.
        if (left == right || bottom == top || nearPlane == farPlane)
            return;

        float width = right - left;
        float invheight = top - bottom;
        float clip = farPlane - nearPlane;
        Mat4 mm(Uninitialized);
        mm.m[0][0] = 2.0f / width;
        mm.m[1][0] = 0.0f;
        mm.m[2][0] = 0.0f;
        mm.m[3][0] = -(left + right) / width;
        mm.m[0][1] = 0.0f;
        mm.m[1][1] = 2.0f / invheight;
        mm.m[2][1] = 0.0f;
        mm.m[3][1] = -(top + bottom) / invheight;
        mm.m[0][2] = 0.0f;
        mm.m[1][2] = 0.0f;
        mm.m[2][2] = -2.0f / clip;
        mm.m[3][2] = -(nearPlane + farPlane) / clip;
        mm.m[0][3] = 0.0f;
        mm.m[1][3] = 0.0f;
        mm.m[2][3] = 0.0f;
        mm.m[3][3] = 1.0f;
        mm.flagBits = Translation | Scale;

        *this *= mm;
    }

    void frustum(float left, float right, float bottom, float top, float nearPlane, float farPlane)
    {
        if (left == right || bottom == top || nearPlane == farPlane)
            return;

        Mat4 mm(Uninitialized);
        float width = right - left;
        float invheight = top - bottom;
        float clip = farPlane - nearPlane;
        mm.m[0][0] = 2.0f * nearPlane / width;
        mm.m[1][0] = 0.0f;
        mm.m[2][0] = (left + right) / width;
        mm.m[3][0] = 0.0f;
        mm.m[0][1] = 0.0f;
        mm.m[1][1] = 2.0f * nearPlane / invheight;
        mm.m[2][1] = (top + bottom) / invheight;
        mm.m[3][1] = 0.0f;
        mm.m[0][2] = 0.0f;
        mm.m[1][2] = 0.0f;
        mm.m[2][2] = -(nearPlane + farPlane) / clip;
        mm.m[3][2] = -2.0f * nearPlane * farPlane / clip;
        mm.m[0][3] = 0.0f;
        mm.m[1][3] = 0.0f;
        mm.m[2][3] = -1.0f;
        mm.m[3][3] = 0.0f;
        mm.flagBits = Perspective;

        *this *= mm;
    }

    void perspective(float verticalAngle, float aspectRatio, float nearPlane, float farPlane)
    {
        if (nearPlane == farPlane || aspectRatio == 0.0f)
            return;

        Mat4 mm(Uninitialized);
        float radians = degreesToRadians(verticalAngle / 2.0f);
        float sine = std::sin(radians);
        if (sine == 0.0f)
            return;
        float cotan = std::cos(radians) / sine;
        float clip = farPlane - nearPlane;
        mm.m[0][0] = cotan / aspectRatio;
        mm.m[1][0] = 0.0f;
        mm.m[2][0] = 0.0f;
        mm.m[3][0] = 0.0f;
        mm.m[0][1] = 0.0f;
        mm.m[1][1] = cotan;
        mm.m[2][1] = 0.0f;
        mm.m[3][1] = 0.0f;
        mm.m[0][2] = 0.0f;
        mm.m[1][2] = 0.0f;
        mm.m[2][2] = -(nearPlane + farPlane) / clip;
        mm.m[3][2] = -(2.0f * nearPlane * farPlane) / clip;
        mm.m[0][3] = 0.0f;
        mm.m[1][3] = 0.0f;
        mm.m[2][3] = -1.0f;
        mm.m[3][3] = 0.0f;
        mm.flagBits = Perspective;

        *this *= mm;
    }

    void lookAt(const Vec3 &eye, const Vec3 &center, const Vec3 &up)
    {
        Vec3 forward = center - eye;
        if (fuzzyIsNull(forward.x()) && fuzzyIsNull(forward.y()) && fuzzyIsNull(forward.z()))
            return;

        forward.normalize();
        Vec3 side = Vec3::crossProduct(forward, up).normalized();
        Vec3 upVector = Vec3::crossProduct(side, forward);

        Mat4 mm(Uninitialized);
        mm.m[0][0] = side.x();
        mm.m[1][0] = side.y();
        mm.m[2][0] = side.z();
        mm.m[3][0] = 0.0f;
        mm.m[0][1] = upVector.x();
        mm.m[1][1] = upVector.y();
        mm.m[2][1] = upVector.z();
        mm.m[3][1] = 0.0f;
        mm.m[0][2] = -forward.x();
        mm.m[1][2] = -forward.y();
        mm.m[2][2] = -forward.z();
        mm.m[3][2] = 0.0f;
        mm.m[0][3] = 0.0f;
        mm.m[1][3] = 0.0f;
        mm.m[2][3] = 0.0f;
        mm.m[3][3] = 1.0f;
        mm.flagBits = Rotation;

        *this *= mm;
        translate(-eye);
    }

    void viewport(float left, float bottom, float width, float height,
                  float nearPlane = 0.0f, float farPlane = 1.0f)
    {
        const float w2 = width / 2.0f;
        const float h2 = height / 2.0f;

        Mat4 mm(Uninitialized);
        mm.m[0][0] = w2;
        mm.m[1][0] = 0.0f;
        mm.m[2][0] = 0.0f;
        mm.m[3][0] = left + w2;
        mm.m[0][1] = 0.0f;
        mm.m[1][1] = h2;
        mm.m[2][1] = 0.0f;
        mm.m[3][1] = bottom + h2;
        mm.m[0][2] = 0.0f;
        mm.m[1][2] = 0.0f;
        mm.m[2][2] = (farPlane - nearPlane) / 2.0f;
        mm.m[3][2] = (nearPlane + farPlane) / 2.0f;
        mm.m[0][3] = 0.0f;
        mm.m[1][3] = 0.0f;
        mm.m[2][3] = 0.0f;
        mm.m[3][3] = 1.0f;
        mm.flagBits = General;

        *this *= mm;
    }

    void flipCoordinates()
    {
        // Multiplying the y and z coordinates by -1 keeps the flags meaningful.
        if (flagBits < Rotation2D) {
            m[1][1] = -m[1][1];
            m[2][2] = -m[2][2];
        } else {
            m[1][0] = -m[1][0]; m[1][1] = -m[1][1];
            m[1][2] = -m[1][2]; m[1][3] = -m[1][3];
            m[2][0] = -m[2][0]; m[2][1] = -m[2][1];
            m[2][2] = -m[2][2]; m[2][3] = -m[2][3];
        }
        flagBits |= Scale;
    }

    // ---- derived matrices ---------------------------------------------------
    double determinant() const
    {
        if ((flagBits & ~(Translation | Rotation2D | Rotation)) == Identity)
            return 1.0;
        return det4(m);
    }

    Mat4 transposed() const
    {
        Mat4 result(Uninitialized);
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                result.m[col][row] = m[row][col];
        // A transposed translation is a perspective transformation.
        result.flagBits = (flagBits & Translation) ? General : flagBits;
        return result;
    }

    Mat4 inverted(bool *invertible = nullptr) const
    {
        // The easy cases first — these are not just faster, they round
        // differently from the general path, and Qt takes them.
        if (flagBits == Identity) {
            if (invertible) *invertible = true;
            return Mat4();
        } else if (flagBits == Translation) {
            Mat4 inv;
            inv.m[3][0] = -m[3][0];
            inv.m[3][1] = -m[3][1];
            inv.m[3][2] = -m[3][2];
            inv.flagBits = Translation;
            if (invertible) *invertible = true;
            return inv;
        } else if (flagBits < Rotation2D) {
            // Translation | Scale
            if (m[0][0] == 0.0f || m[1][1] == 0.0f || m[2][2] == 0.0f) {
                if (invertible) *invertible = false;
                return Mat4();
            }
            Mat4 inv;
            inv.m[0][0] = 1.0f / m[0][0];
            inv.m[1][1] = 1.0f / m[1][1];
            inv.m[2][2] = 1.0f / m[2][2];
            inv.m[3][0] = -m[3][0] * inv.m[0][0];
            inv.m[3][1] = -m[3][1] * inv.m[1][1];
            inv.m[3][2] = -m[3][2] * inv.m[2][2];
            inv.flagBits = flagBits;
            if (invertible) *invertible = true;
            return inv;
        } else if ((flagBits & ~(Translation | Rotation2D | Rotation)) == Identity) {
            if (invertible) *invertible = true;
            return orthonormalInverse();
        } else if (flagBits < Perspective) {
            // Affine: invert the upper 3x3 in double, then carry the translation
            // through it. The last row is ASSIGNED (0,0,0,1) — which is why Qt's
            // affine inverse has a positive zero there where a general Cramer
            // solve would leave a negative one.
            double det = det3(m, 0, 1, 2, 0, 1, 2);
            if (det == 0.0) {
                if (invertible) *invertible = false;
                return Mat4();
            }
            det = 1.0 / det;

            Mat4 inv(Uninitialized);
            inv.m[0][0] =  float((double(m[1][1]) * m[2][2] - double(m[2][1]) * m[1][2]) * det);
            inv.m[0][1] = -float((double(m[0][1]) * m[2][2] - double(m[2][1]) * m[0][2]) * det);
            inv.m[0][2] =  float((double(m[0][1]) * m[1][2] - double(m[1][1]) * m[0][2]) * det);
            inv.m[0][3] = 0.0f;
            inv.m[1][0] = -float((double(m[1][0]) * m[2][2] - double(m[2][0]) * m[1][2]) * det);
            inv.m[1][1] =  float((double(m[0][0]) * m[2][2] - double(m[2][0]) * m[0][2]) * det);
            inv.m[1][2] = -float((double(m[0][0]) * m[1][2] - double(m[1][0]) * m[0][2]) * det);
            inv.m[1][3] = 0.0f;
            inv.m[2][0] =  float((double(m[1][0]) * m[2][1] - double(m[2][0]) * m[1][1]) * det);
            inv.m[2][1] = -float((double(m[0][0]) * m[2][1] - double(m[2][0]) * m[0][1]) * det);
            inv.m[2][2] =  float((double(m[0][0]) * m[1][1] - double(m[1][0]) * m[0][1]) * det);
            inv.m[2][3] = 0.0f;
            // Float arithmetic here, on the already-rounded inverse basis.
            inv.m[3][0] = -(inv.m[0][0] * m[3][0] + inv.m[1][0] * m[3][1] + inv.m[2][0] * m[3][2]);
            inv.m[3][1] = -(inv.m[0][1] * m[3][0] + inv.m[1][1] * m[3][1] + inv.m[2][1] * m[3][2]);
            inv.m[3][2] = -(inv.m[0][2] * m[3][0] + inv.m[1][2] * m[3][1] + inv.m[2][2] * m[3][2]);
            inv.m[3][3] = 1.0f;
            inv.flagBits = flagBits;

            if (invertible) *invertible = true;
            return inv;
        }

        Mat4 inv(Uninitialized);

        double det = det4(m);
        if (det == 0.0) {
            if (invertible) *invertible = false;
            return Mat4();
        }
        det = 1.0 / det;

        inv.m[0][0] =  float(det3(m, 1, 2, 3, 1, 2, 3) * det);
        inv.m[0][1] = -float(det3(m, 0, 2, 3, 1, 2, 3) * det);
        inv.m[0][2] =  float(det3(m, 0, 1, 3, 1, 2, 3) * det);
        inv.m[0][3] = -float(det3(m, 0, 1, 2, 1, 2, 3) * det);
        inv.m[1][0] = -float(det3(m, 1, 2, 3, 0, 2, 3) * det);
        inv.m[1][1] =  float(det3(m, 0, 2, 3, 0, 2, 3) * det);
        inv.m[1][2] = -float(det3(m, 0, 1, 3, 0, 2, 3) * det);
        inv.m[1][3] =  float(det3(m, 0, 1, 2, 0, 2, 3) * det);
        inv.m[2][0] =  float(det3(m, 1, 2, 3, 0, 1, 3) * det);
        inv.m[2][1] = -float(det3(m, 0, 2, 3, 0, 1, 3) * det);
        inv.m[2][2] =  float(det3(m, 0, 1, 3, 0, 1, 3) * det);
        inv.m[2][3] = -float(det3(m, 0, 1, 2, 0, 1, 3) * det);
        inv.m[3][0] = -float(det3(m, 1, 2, 3, 0, 1, 2) * det);
        inv.m[3][1] =  float(det3(m, 0, 2, 3, 0, 1, 2) * det);
        inv.m[3][2] = -float(det3(m, 0, 1, 3, 0, 1, 2) * det);
        inv.m[3][3] =  float(det3(m, 0, 1, 2, 0, 1, 2) * det);

        if (invertible) *invertible = true;
        return inv;
    }

    Mat3 normalMatrix() const
    {
        Mat3 inv;

        // The inverse of the upper 3x3, transposed. Identity if not invertible.
        if (flagBits < Scale) {
            return inv;
        } else if (flagBits < Rotation2D) {
            if (fuzzyIsNull(m[0][0]) || fuzzyIsNull(m[1][1]) || fuzzyIsNull(m[2][2]))
                return inv;
            inv.data()[0] = 1.0f / m[0][0];
            inv.data()[4] = 1.0f / m[1][1];
            inv.data()[8] = 1.0f / m[2][2];
            return inv;
        }

        double det = det3(m, 0, 1, 2, 0, 1, 2);
        if (det == 0.0)
            return inv;
        det = 1.0 / det;

        float *invm = inv.data();

        // Invert and transpose in a single step.
        invm[0 + 0 * 3] =  float((double(m[1][1]) * double(m[2][2]) - double(m[2][1]) * double(m[1][2])) * det);
        invm[1 + 0 * 3] = -float((double(m[1][0]) * double(m[2][2]) - double(m[1][2]) * double(m[2][0])) * det);
        invm[2 + 0 * 3] =  float((double(m[1][0]) * double(m[2][1]) - double(m[1][1]) * double(m[2][0])) * det);
        invm[0 + 1 * 3] = -float((double(m[0][1]) * double(m[2][2]) - double(m[2][1]) * double(m[0][2])) * det);
        invm[1 + 1 * 3] =  float((double(m[0][0]) * double(m[2][2]) - double(m[0][2]) * double(m[2][0])) * det);
        invm[2 + 1 * 3] = -float((double(m[0][0]) * double(m[2][1]) - double(m[0][1]) * double(m[2][0])) * det);
        invm[0 + 2 * 3] =  float((double(m[0][1]) * double(m[1][2]) - double(m[0][2]) * double(m[1][1])) * det);
        invm[1 + 2 * 3] = -float((double(m[0][0]) * double(m[1][2]) - double(m[0][2]) * double(m[1][0])) * det);
        invm[2 + 2 * 3] =  float((double(m[0][0]) * double(m[1][1]) - double(m[1][0]) * double(m[0][1])) * det);

        return inv;
    }

    // ---- mapping ------------------------------------------------------------
    Vec3 map(const Vec3 &point) const
    {
        float x, y, z, w;
        if (flagBits == Identity) {
            return point;
        } else if (flagBits < Rotation2D) {
            // Translation | Scale
            return Vec3(point.x() * m[0][0] + m[3][0],
                        point.y() * m[1][1] + m[3][1],
                        point.z() * m[2][2] + m[3][2]);
        } else if (flagBits < Rotation) {
            // Translation | Scale | Rotation2D
            return Vec3(point.x() * m[0][0] + point.y() * m[1][0] + m[3][0],
                        point.x() * m[0][1] + point.y() * m[1][1] + m[3][1],
                        point.z() * m[2][2] + m[3][2]);
        } else {
            x = point.x() * m[0][0] + point.y() * m[1][0] + point.z() * m[2][0] + m[3][0];
            y = point.x() * m[0][1] + point.y() * m[1][1] + point.z() * m[2][1] + m[3][1];
            z = point.x() * m[0][2] + point.y() * m[1][2] + point.z() * m[2][2] + m[3][2];
            w = point.x() * m[0][3] + point.y() * m[1][3] + point.z() * m[2][3] + m[3][3];
            if (w == 1.0f)
                return Vec3(x, y, z);
            return Vec3(x / w, y / w, z / w);
        }
    }

    Vec3 mapVector(const Vec3 &vector) const
    {
        if (flagBits < Scale) {
            // Translation
            return vector;
        } else if (flagBits < Rotation2D) {
            return Vec3(vector.x() * m[0][0], vector.y() * m[1][1], vector.z() * m[2][2]);
        } else {
            return Vec3(vector.x() * m[0][0] + vector.y() * m[1][0] + vector.z() * m[2][0],
                        vector.x() * m[0][1] + vector.y() * m[1][1] + vector.z() * m[2][1],
                        vector.x() * m[0][2] + vector.y() * m[1][2] + vector.z() * m[2][2]);
        }
    }

    Vec4 map(const Vec4 &point) const { return *this * point; }

    // ---- operators ----------------------------------------------------------
    Mat4 &operator+=(const Mat4 &other)
    {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                m[col][row] += other.m[col][row];
        flagBits = General;
        return *this;
    }

    Mat4 &operator-=(const Mat4 &other)
    {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                m[col][row] -= other.m[col][row];
        flagBits = General;
        return *this;
    }

    Mat4 &operator*=(float factor)
    {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                m[col][row] *= factor;
        flagBits = General;
        return *this;
    }

    Mat4 &operator/=(float divisor)
    {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                m[col][row] /= divisor;
        flagBits = General;
        return *this;
    }

    Mat4 &operator*=(const Mat4 &o)
    {
        const Mat4 other = o; // prevent aliasing when &o == this
        flagBits |= other.flagBits;

        if (flagBits < Rotation2D) {
            m[3][0] += m[0][0] * other.m[3][0];
            m[3][1] += m[1][1] * other.m[3][1];
            m[3][2] += m[2][2] * other.m[3][2];

            m[0][0] *= other.m[0][0];
            m[1][1] *= other.m[1][1];
            m[2][2] *= other.m[2][2];
            return *this;
        }

        for (int row = 0; row < 4; ++row) {
            float m0 = m[0][row] * other.m[0][0] + m[1][row] * other.m[0][1]
                     + m[2][row] * other.m[0][2] + m[3][row] * other.m[0][3];
            float m1 = m[0][row] * other.m[1][0] + m[1][row] * other.m[1][1]
                     + m[2][row] * other.m[1][2] + m[3][row] * other.m[1][3];
            float m2 = m[0][row] * other.m[2][0] + m[1][row] * other.m[2][1]
                     + m[2][row] * other.m[2][2] + m[3][row] * other.m[2][3];
            m[3][row] = m[0][row] * other.m[3][0] + m[1][row] * other.m[3][1]
                      + m[2][row] * other.m[3][2] + m[3][row] * other.m[3][3];
            m[0][row] = m0;
            m[1][row] = m1;
            m[2][row] = m2;
        }
        return *this;
    }

    friend bool operator==(const Mat4 &a, const Mat4 &b)
    {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                if (a.m[col][row] != b.m[col][row])
                    return false;
        return true;
    }
    friend bool operator!=(const Mat4 &a, const Mat4 &b) { return !(a == b); }

    friend Mat4 operator+(const Mat4 &m1, const Mat4 &m2)
    {
        Mat4 r(Uninitialized);
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                r.m[col][row] = m1.m[col][row] + m2.m[col][row];
        return r;
    }

    friend Mat4 operator-(const Mat4 &m1, const Mat4 &m2)
    {
        Mat4 r(Uninitialized);
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                r.m[col][row] = m1.m[col][row] - m2.m[col][row];
        return r;
    }

    friend Mat4 operator*(const Mat4 &m1, const Mat4 &m2)
    {
        int flagBits = m1.flagBits | m2.flagBits;
        if (flagBits < Rotation2D) {
            Mat4 r(Uninitialized);
            r.m[0][0] = m1.m[0][0] * m2.m[0][0];
            r.m[0][1] = 0.0f;
            r.m[0][2] = 0.0f;
            r.m[0][3] = 0.0f;

            r.m[1][0] = 0.0f;
            r.m[1][1] = m1.m[1][1] * m2.m[1][1];
            r.m[1][2] = 0.0f;
            r.m[1][3] = 0.0f;

            r.m[2][0] = 0.0f;
            r.m[2][1] = 0.0f;
            r.m[2][2] = m1.m[2][2] * m2.m[2][2];
            r.m[2][3] = 0.0f;

            r.m[3][0] = m1.m[3][0] + m1.m[0][0] * m2.m[3][0];
            r.m[3][1] = m1.m[3][1] + m1.m[1][1] * m2.m[3][1];
            r.m[3][2] = m1.m[3][2] + m1.m[2][2] * m2.m[3][2];
            r.m[3][3] = 1.0f;
            r.flagBits = flagBits;
            return r;
        }

        Mat4 r(Uninitialized);
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                r.m[col][row] = m1.m[0][row] * m2.m[col][0]
                              + m1.m[1][row] * m2.m[col][1]
                              + m1.m[2][row] * m2.m[col][2]
                              + m1.m[3][row] * m2.m[col][3];
            }
        }
        r.flagBits = flagBits;
        return r;
    }

    friend Mat4 operator-(const Mat4 &matrix)
    {
        Mat4 r(Uninitialized);
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                r.m[col][row] = -matrix.m[col][row];
        return r;
    }

    friend Mat4 operator*(float factor, const Mat4 &matrix)
    {
        Mat4 r(Uninitialized);
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                r.m[col][row] = matrix.m[col][row] * factor;
        return r;
    }

    friend Mat4 operator*(const Mat4 &matrix, float factor)
    {
        return factor * matrix;
    }

    friend Mat4 operator/(const Mat4 &matrix, float divisor)
    {
        Mat4 r(Uninitialized);
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                r.m[col][row] = matrix.m[col][row] / divisor;
        return r;
    }

    friend Vec4 operator*(const Mat4 &matrix, const Vec4 &vector)
    {
        const float x = vector.x() * matrix.m[0][0] + vector.y() * matrix.m[1][0]
                      + vector.z() * matrix.m[2][0] + vector.w() * matrix.m[3][0];
        const float y = vector.x() * matrix.m[0][1] + vector.y() * matrix.m[1][1]
                      + vector.z() * matrix.m[2][1] + vector.w() * matrix.m[3][1];
        const float z = vector.x() * matrix.m[0][2] + vector.y() * matrix.m[1][2]
                      + vector.z() * matrix.m[2][2] + vector.w() * matrix.m[3][2];
        const float w = vector.x() * matrix.m[0][3] + vector.y() * matrix.m[1][3]
                      + vector.z() * matrix.m[2][3] + vector.w() * matrix.m[3][3];
        return Vec4(x, y, z, w);
    }

    friend Vec4 operator*(const Vec4 &vector, const Mat4 &matrix)
    {
        const float x = vector.x() * matrix.m[0][0] + vector.y() * matrix.m[0][1]
                      + vector.z() * matrix.m[0][2] + vector.w() * matrix.m[0][3];
        const float y = vector.x() * matrix.m[1][0] + vector.y() * matrix.m[1][1]
                      + vector.z() * matrix.m[1][2] + vector.w() * matrix.m[1][3];
        const float z = vector.x() * matrix.m[2][0] + vector.y() * matrix.m[2][1]
                      + vector.z() * matrix.m[2][2] + vector.w() * matrix.m[2][3];
        const float w = vector.x() * matrix.m[3][0] + vector.y() * matrix.m[3][1]
                      + vector.z() * matrix.m[3][2] + vector.w() * matrix.m[3][3];
        return Vec4(x, y, z, w);
    }

    // Qt deprecated these in 6.1 but the tree still leans on them; they are the
    // same arithmetic, kept so that the migration is a rename.
    friend Vec3 operator*(const Mat4 &matrix, const Vec3 &vector) { return matrix.map(vector); }

    friend Vec3 operator*(const Vec3 &vector, const Mat4 &matrix)
    {
        const float x = vector.x() * matrix.m[0][0] + vector.y() * matrix.m[0][1]
                      + vector.z() * matrix.m[0][2] + matrix.m[0][3];
        const float y = vector.x() * matrix.m[1][0] + vector.y() * matrix.m[1][1]
                      + vector.z() * matrix.m[1][2] + matrix.m[1][3];
        const float z = vector.x() * matrix.m[2][0] + vector.y() * matrix.m[2][1]
                      + vector.z() * matrix.m[2][2] + matrix.m[2][3];
        const float w = vector.x() * matrix.m[3][0] + vector.y() * matrix.m[3][1]
                      + vector.z() * matrix.m[3][2] + matrix.m[3][3];
        if (w == 1.0f)
            return Vec3(x, y, z);
        return Vec3(x / w, y / w, z / w);
    }

private:
    Mat4 orthonormalInverse() const
    {
        Mat4 result(Uninitialized);

        result.m[0][0] = m[0][0];
        result.m[1][0] = m[0][1];
        result.m[2][0] = m[0][2];

        result.m[0][1] = m[1][0];
        result.m[1][1] = m[1][1];
        result.m[2][1] = m[1][2];

        result.m[0][2] = m[2][0];
        result.m[1][2] = m[2][1];
        result.m[2][2] = m[2][2];

        result.m[0][3] = 0.0f;
        result.m[1][3] = 0.0f;
        result.m[2][3] = 0.0f;

        result.m[3][0] = -(result.m[0][0] * m[3][0] + result.m[1][0] * m[3][1] + result.m[2][0] * m[3][2]);
        result.m[3][1] = -(result.m[0][1] * m[3][0] + result.m[1][1] * m[3][1] + result.m[2][1] * m[3][2]);
        result.m[3][2] = -(result.m[0][2] * m[3][0] + result.m[1][2] * m[3][1] + result.m[2][2] * m[3][2]);
        result.m[3][3] = 1.0f;

        result.flagBits = flagBits;

        return result;
    }

    static double det3(const float mm[4][4], int col0, int col1, int col2,
                       int row0, int row1, int row2)
    {
        return double(mm[col0][row0])
                   * (double(mm[col1][row1]) * double(mm[col2][row2])
                      - double(mm[col1][row2]) * double(mm[col2][row1]))
             - double(mm[col1][row0])
                   * (double(mm[col0][row1]) * double(mm[col2][row2])
                      - double(mm[col0][row2]) * double(mm[col2][row1]))
             + double(mm[col2][row0])
                   * (double(mm[col0][row1]) * double(mm[col1][row2])
                      - double(mm[col0][row2]) * double(mm[col1][row1]));
    }

    static double det4(const float mm[4][4])
    {
        double det;
        det  = double(mm[0][0]) * det3(mm, 1, 2, 3, 1, 2, 3);
        det -= double(mm[1][0]) * det3(mm, 0, 2, 3, 1, 2, 3);
        det += double(mm[2][0]) * det3(mm, 0, 1, 3, 1, 2, 3);
        det -= double(mm[3][0]) * det3(mm, 0, 1, 2, 1, 2, 3);
        return det;
    }

    float m[4][4]; // column-major, m[column][row]
    int flagBits;
};

inline bool fuzzyCompare(const Mat4 &m1, const Mat4 &m2) noexcept
{
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            if (!fuzzyCompare(m1(row, col), m2(row, col)))
                return false;
    return true;
}

} // namespace iris

#endif // IRIS_MAT4_H
