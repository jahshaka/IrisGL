/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_QT_INTEROP_H
#define IRIS_QT_INTEROP_H

// -----------------------------------------------------------------------------
// iris <-> QtGui math conversions.
//
// THE POINT OF THIS FILE IS THAT MOST FILES DO NOT INCLUDE IT. The document model
// stopped speaking QVector3D/QQuaternion/QMatrix4x4 so that the translation units
// which include a scene node stop paying for QtGui (and, through QQuaternion's and
// QMatrix4x4's `operator QVariant()`, for most of QtCore's metatype machinery
// too). Pulling this header into a document header would put all of that back.
//
// Include it where a Qt type genuinely arrives or departs: the UI (spin boxes,
// gizmo widgets, painters), QVariant / QJSValue bridges, QDataStream, and the
// serializers. The conversions are member-for-member copies — free at -O1 and
// above, and exact by construction, since both sides are the same floats in the
// same order.
//
// ONE ASYMMETRY WORTH KNOWING: fromQt(QMatrix4x4) cannot recover Qt's flag bits
// (they are private), so the result is flagged General — same as any Mat4 built
// from raw floats, and same as what Qt itself does for QMatrix4x4(const float*).
// A matrix that has been round-tripped through Qt therefore takes Mat4's general
// arithmetic paths. That is a correctness-preserving difference, not a numeric
// one, but it is a reason not to round-trip a matrix for no purpose.
// -----------------------------------------------------------------------------

#include <QMatrix3x3>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

#include "core/math/mat3.h"
#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"

namespace iris
{

// ---- iris -> Qt --------------------------------------------------------------
inline QVector2D toQt(const Vec2 &v) { return QVector2D(v.x(), v.y()); }
inline QVector3D toQt(const Vec3 &v) { return QVector3D(v.x(), v.y(), v.z()); }
inline QVector4D toQt(const Vec4 &v) { return QVector4D(v.x(), v.y(), v.z(), v.w()); }
inline QQuaternion toQt(const Quat &q) { return QQuaternion(q.scalar(), q.x(), q.y(), q.z()); }

inline QMatrix3x3 toQt(const Mat3 &m)
{
    QMatrix3x3 r;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            r(row, col) = m(row, col);
    return r;
}

inline QMatrix4x4 toQt(const Mat4 &m)
{
    // The 16-argument constructor takes values in ROW-MAJOR order.
    return QMatrix4x4(m(0, 0), m(0, 1), m(0, 2), m(0, 3),
                      m(1, 0), m(1, 1), m(1, 2), m(1, 3),
                      m(2, 0), m(2, 1), m(2, 2), m(2, 3),
                      m(3, 0), m(3, 1), m(3, 2), m(3, 3));
}

// ---- Qt -> iris --------------------------------------------------------------
inline Vec2 fromQt(const QVector2D &v) { return Vec2(v.x(), v.y()); }
inline Vec3 fromQt(const QVector3D &v) { return Vec3(v.x(), v.y(), v.z()); }
inline Vec4 fromQt(const QVector4D &v) { return Vec4(v.x(), v.y(), v.z(), v.w()); }
inline Quat fromQt(const QQuaternion &q) { return Quat(q.scalar(), q.x(), q.y(), q.z()); }

inline Mat3 fromQt(const QMatrix3x3 &m)
{
    Mat3 r(Mat3::Uninitialized);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            r(row, col) = m(row, col);
    return r;
}

inline Mat4 fromQt(const QMatrix4x4 &m)
{
    return Mat4(m(0, 0), m(0, 1), m(0, 2), m(0, 3),
                m(1, 0), m(1, 1), m(1, 2), m(1, 3),
                m(2, 0), m(2, 1), m(2, 2), m(2, 3),
                m(3, 0), m(3, 1), m(3, 2), m(3, 3));
}

} // namespace iris

#endif // IRIS_QT_INTEROP_H
