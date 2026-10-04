/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef TRIMESH_H
#define TRIMESH_H

#include "core/geometry/trianglebvh.h"
#include "core/math/vec.h"
#include <QList>

namespace iris
{

struct TriangleIntersectionResult
{
    int triangleIndex;
    iris::Vec3 hitPoint;
    float t = 0.0f;//distance along length of the segment

    TriangleIntersectionResult()
    {
        triangleIndex = -1;
    }
};

class Triangle
{
public:
    //triangle's points in counter-clockwise order
    iris::Vec3 a,b,c;
    iris::Vec3 normal;
};


/**
 * This class defines a mesh using triangles. It's used for ray-casting and intersection tests
 */
class TriMesh
{
public:
    QList<Triangle> triangles;


    /**
     * Adds points for triangle. Assumes points are in a counter-clockwise rotation.
     * Drops the spatial index (an index over a list that has changed is not
     * trusted; buildIndex() again once the list is complete).
     */
    void addTriangle(const iris::Vec3& a, const iris::Vec3& b, const iris::Vec3& c);

    /**
     * THE NARROW PHASE'S SPATIAL INDEX (SPEED-CPU, perf audit 2026-10-03 P1):
     * builds a TriangleBvh over `triangles`, after which getSegmentIntersections
     * tests only the triangles whose boxes the segment touches — the same
     * hits, in the same order, with the same numbers (picking.trimesh_index).
     * Called where a picking mesh is completed (the bake reader); a mesh that
     * never built one keeps the brute-force loop.
     */
    void buildIndex();
    bool hasIndex() const { return index.isBuilt() && index.triangleCount() == triangles.size(); }
    int indexNodeCount() const { return index.nodeCount(); }

    /**
     * THE ONE TRIANGLE TEST (realtime rendering p.192; two-sided): does the
     * segment a->b cross `tri`, and where. `t` is the hit's fraction of the
     * segment (0..1). Both narrow-phase paths — and the index suite's
     * brute-force reference — call this and nothing else.
     */
    static bool segmentHitsTriangle(const Triangle& tri, const iris::Vec3& segmentStart,
                                    const iris::Vec3& segmentEnd, float& t, iris::Vec3& hitPoint);

    /**
     * Does a segment-mesh intersection test: every triangle the segment
     * crosses, in ascending triangle order.
     * Returns number of intersections
     * @return
     */
    int getSegmentIntersections(const iris::Vec3& segmentStart, const iris::Vec3& segmentEnd, QList<TriangleIntersectionResult>& results) const;

private:
    TriangleBvh index;
};


}

#endif // TRIMESH_H
