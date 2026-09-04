/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "core/geometry/trimesh.h"

#include <cmath>

namespace iris
{

/**
 * Adds points for triangle. Assumes points are in a counter-clockwise rotation.
 * @param a
 * @param b
 * @param c
 */
void TriMesh::addTriangle(const QVector3D& a,const QVector3D& b,const QVector3D& c)
{
    Triangle tri = {a,b,c,QVector3D::crossProduct(b-a,c-a)};

    triangles.append(tri);
}

//https://github.com/qt/qt3d/blob/5476bc6b4b6a12c921da502c24c4e078b04dd3b3/src/render/jobs/pickboundingvolumejob.cpp
//realtime rendering page 192
//no need to get uvw, just return true at the first sign of a hit
bool TriMesh::isHitBySegment(const QVector3D& segmentStart,const QVector3D& segmentEnd,QVector3D& hitPoint)
{
    for(auto tri:triangles)
    {
        auto ab = tri.b - tri.a;
        auto ac = tri.c - tri.a;
        auto qp = segmentStart-segmentEnd;

        //auto normal = tri.normal;
        auto normal = QVector3D::crossProduct(ab, ac);
        float d = QVector3D::dotProduct(qp, normal);

        //if (d <= 0)
        //    continue;
        // Two-sided, the same fold getSegmentIntersections documents: this one
        // already refused only the parallel case, but then compared the
        // barycentric numerators against a possibly-negative d, so a back-facing
        // triangle was rejected anyway.
        if (d == 0) continue;
        const float sign = d < 0.0f ? -1.0f : 1.0f;
        const float ad = std::fabs(d);

        auto ap = segmentStart - tri.a;
        auto t = QVector3D::dotProduct(ap, normal) * sign;

        if (t < 0 || t > ad)
            continue;

        auto e = QVector3D::crossProduct(qp, ap);
        auto v = QVector3D::dotProduct(ac, e) * sign;

        if (v < 0.0f || v > ad)
            continue;

        auto w = -QVector3D::dotProduct(ab, e) * sign;

        if (w < 0.0f || v + w > ad)
            continue;

        t /= ad;

        //all conditions have been met
        //todo: fix please. return t instead
        hitPoint = segmentStart + (segmentEnd-segmentStart)*t;//t is in range 0 and 1 and denotes how far along the distance the hit is
        return true;
    }

    return false;
}

/**
 * Does a segment-mesh intersection test
 * Returns number of intersections
 * @return
 */
int TriMesh::getSegmentIntersections(const QVector3D& segmentStart,const QVector3D& segmentEnd,QList<TriangleIntersectionResult>& results)
{
    int hits = 0;
    for(auto i=0;i<triangles.size();i++)
    {
        //auto tri = triangles[i];
        const Triangle& tri = triangles[i];
        auto ab = tri.b - tri.a;
        auto ac = tri.c - tri.a;
        auto qp = segmentStart-segmentEnd;

        //auto normal = tri.normal;
        auto normal = QVector3D::crossProduct(ab, ac);
        float d = QVector3D::dotProduct(qp, normal);

        // TWO-SIDED. `d <= 0` here rejected every triangle the segment reaches
        // from behind, so picking BACK-FACE CULLED: a plane (or any open
        // surface, or a model whose winding the importer flipped) was
        // unselectable from one side, and the player's raycast — which comes
        // through this same function — reported no hit at all. Only a
        // degenerate triangle, or a segment exactly parallel to its plane, has
        // nothing to intersect (deep audit 2026-09, area 2).
        //
        // With d free to be negative, the barycentric tests below are done
        // against |d| with each numerator carrying d's sign: t/d, v/d and w/d
        // are the actual parameters, and each must land in [0,1] with
        // v/d + w/d <= 1. Multiplying through by d flips the inequalities when
        // d < 0, which is exactly what the sign fold undoes.
        if (d == 0.0f)
            continue;
        const float sign = d < 0.0f ? -1.0f : 1.0f;
        const float ad = std::fabs(d);

        auto ap = segmentStart - tri.a;
        auto t = QVector3D::dotProduct(ap, normal) * sign;

        if (t < 0 || t > ad)
            continue;

        auto e = QVector3D::crossProduct(qp, ap);
        auto v = QVector3D::dotProduct(ac, e) * sign;

        if (v < 0 || v > ad)
            continue;

        auto w = -QVector3D::dotProduct(ab, e) * sign;

        if (w < 0.0f || v + w > ad)
            continue;

        t /= ad;

        //all conditions have been met
        // (This counted every hit TWICE — once here and once after appending —
        // so callers reading the count saw 2, 4, 6... Nothing branched on the
        // parity, but `if (int n = getSegmentIntersections(...))` is used as a
        // "how many" as well as a "any", and it was lying.)
        const QVector3D hitPoint = segmentStart + (segmentEnd-segmentStart)*t;

        TriangleIntersectionResult result;
        result.triangleIndex = i;
        result.hitPoint = hitPoint;
        result.t = t;
        //result.hitPoint = tri.a*u + tri.b*v + tri.c*w;
        results.append(result);
        hits++;
    }

    return hits;
}

}
