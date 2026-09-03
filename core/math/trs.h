/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_TRS_H
#define IRIS_TRS_H

#include <QMatrix4x4>
#include <QQuaternion>
#include <QVector3D>
#include <algorithm>
#include <cmath>

namespace iris
{

/// Decomposes an affine 4x4 into translation / rotation / scale and returns the
/// worst |cos| between the basis axes — a SHEAR measure, 0 for a clean TRS.
///
/// The single implementation of this in the tree. It lived in the anonymous
/// namespace of irisgl/mirror/scenemirror.cpp until the clip extractor needed
/// exactly the same decomposition (a clip track is TRS, and the bind pose the
/// exporter writes is TRS), and two copies of a decomposition that has to agree
/// bit-for-bit across the boundary is how a rig comes apart.
///
/// A negative determinant means the basis is mirrored; a quaternion cannot
/// express that, so the flip is folded into the X scale (the convention assimp
/// and glTF importers use) rather than producing a silently wrong rotation.
inline float decomposeTRS(const QMatrix4x4 &m, QVector3D &pos, QQuaternion &rot, QVector3D &scale)
{
    pos = m.column(3).toVector3D();
    QVector3D c0 = m.column(0).toVector3D();
    QVector3D c1 = m.column(1).toVector3D();
    QVector3D c2 = m.column(2).toVector3D();
    float s0 = c0.length(), s1 = c1.length(), s2 = c2.length();
    if (QVector3D::dotProduct(QVector3D::crossProduct(c0, c1), c2) < 0.0f) { s0 = -s0; c0 = -c0; }
    const QVector3D n0 = s0 != 0.0f ? c0 / std::fabs(s0) : QVector3D(1, 0, 0);
    const QVector3D n1 = s1 != 0.0f ? c1 / s1 : QVector3D(0, 1, 0);
    const QVector3D n2 = s2 != 0.0f ? c2 / s2 : QVector3D(0, 0, 1);
    scale = QVector3D(s0, s1, s2);
    QMatrix3x3 basis;
    basis(0,0)=n0.x(); basis(1,0)=n0.y(); basis(2,0)=n0.z();
    basis(0,1)=n1.x(); basis(1,1)=n1.y(); basis(2,1)=n1.z();
    basis(0,2)=n2.x(); basis(1,2)=n2.y(); basis(2,2)=n2.z();
    rot = QQuaternion::fromRotationMatrix(basis).normalized();
    return std::max({ std::fabs(QVector3D::dotProduct(n0, n1)),
                      std::fabs(QVector3D::dotProduct(n0, n2)),
                      std::fabs(QVector3D::dotProduct(n1, n2)) });
}

/// The inverse of decomposeTRS: the local matrix SceneNode::getLocalTransform
/// builds, in the same order (translate, rotate, scale).
inline QMatrix4x4 composeTRS(const QVector3D &pos, const QQuaternion &rot, const QVector3D &scale)
{
    QMatrix4x4 m;
    m.setToIdentity();
    m.translate(pos);
    m.rotate(rot);
    m.scale(scale);
    return m;
}

} // namespace iris

#endif // IRIS_TRS_H
