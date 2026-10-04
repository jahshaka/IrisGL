/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "core/math/vec.h"
#include "core/geometry/trimesh.h"

#include <cmath>
#include <vector>

namespace iris
{

/**
 * Adds points for triangle. Assumes points are in a counter-clockwise rotation.
 * @param a
 * @param b
 * @param c
 */
void TriMesh::addTriangle(const iris::Vec3& a,const iris::Vec3& b,const iris::Vec3& c)
{
    Triangle tri = {a,b,c,iris::Vec3::crossProduct(b-a,c-a)};

    triangles.append(tri);
    if (index.isBuilt()) index.clear();
}

void TriMesh::buildIndex()
{
    index.build(triangles.constData(), int(triangles.size()));
}

//https://github.com/qt/qt3d/blob/5476bc6b4b6a12c921da502c24c4e078b04dd3b3/src/render/jobs/pickboundingvolumejob.cpp
//realtime rendering page 192
bool TriMesh::segmentHitsTriangle(const Triangle& tri, const iris::Vec3& segmentStart,
                                  const iris::Vec3& segmentEnd, float& tOut, iris::Vec3& hitPoint)
{
    auto ab = tri.b - tri.a;
    auto ac = tri.c - tri.a;
    auto qp = segmentStart-segmentEnd;

    //auto normal = tri.normal;
    auto normal = iris::Vec3::crossProduct(ab, ac);
    float d = iris::Vec3::dotProduct(qp, normal);

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
        return false;
    const float sign = d < 0.0f ? -1.0f : 1.0f;
    const float ad = std::fabs(d);

    auto ap = segmentStart - tri.a;
    auto t = iris::Vec3::dotProduct(ap, normal) * sign;

    if (t < 0 || t > ad)
        return false;

    auto e = iris::Vec3::crossProduct(qp, ap);
    auto v = iris::Vec3::dotProduct(ac, e) * sign;

    if (v < 0 || v > ad)
        return false;

    auto w = -iris::Vec3::dotProduct(ab, e) * sign;

    if (w < 0.0f || v + w > ad)
        return false;

    t /= ad;

    //all conditions have been met
    hitPoint = segmentStart + (segmentEnd-segmentStart)*t;
    tOut = t;
    return true;
}

/**
 * Does a segment-mesh intersection test
 * Returns number of intersections
 * @return
 */
int TriMesh::getSegmentIntersections(const iris::Vec3& segmentStart,const iris::Vec3& segmentEnd,QList<TriangleIntersectionResult>& results) const
{
    // (This counted every hit TWICE once — once in the loop and once after
    // appending — so callers reading the count saw 2, 4, 6... It is the number
    // of results appended.)
    int hits = 0;
    const Triangle *tris = triangles.constData();
    auto test = [&](int i) {
        TriangleIntersectionResult result;
        if (!segmentHitsTriangle(tris[i], segmentStart, segmentEnd, result.t, result.hitPoint)) return;
        result.triangleIndex = i;
        results.append(result);
        hits++;
    };
    // THE INDEX, when it is there and still describes this list: only the
    // triangles whose boxes the segment touches, ascending — the brute-force
    // loop's hits in the brute-force loop's order (picking.trimesh_index).
    // The candidate list is per thread: picking runs on the UI thread, and a
    // second caller elsewhere gets its own.
    if (hasIndex()) {
        thread_local std::vector<int> candidates;
        index.segmentCandidates(segmentStart, segmentEnd, candidates);
        for (int i : candidates) test(i);
        return hits;
    }
    for (int i = 0; i < int(triangles.size()); i++) test(i);
    return hits;
}

}
