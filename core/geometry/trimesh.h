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

#include "core/math/vec.h"
#include <QList>

namespace iris
{

struct TriangleIntersectionResult
{
    int triangleIndex;
    iris::Vec3 hitPoint;
    float t;//distance along length of the segment

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
     * @param a
     * @param b
     * @param c
     */
    void addTriangle(const iris::Vec3& a, const iris::Vec3& b, const iris::Vec3& c);

    //https://github.com/qt/qt3d/blob/5476bc6b4b6a12c921da502c24c4e078b04dd3b3/src/render/jobs/pickboundingvolumejob.cpp
    //realtime rendering page 192
    //no need to get uvw, just return true at the first sign of a hit
    bool isHitBySegment(const iris::Vec3& segmentStart, const iris::Vec3& segmentEnd, iris::Vec3& hitPoint);

    /**
     * Does a segment-mesh intersection test
     * Returns number of intersections
     * @return
     */
    int getSegmentIntersections(const iris::Vec3& segmentStart, const iris::Vec3& segmentEnd, QList<TriangleIntersectionResult>& results);


};


}

#endif // TRIMESH_H
