/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef INTERSECTIONHELPER_H
#define INTERSECTIONHELPER_H

#include "core/math/vec.h"
#include <qmath.h>
#include "core/geometry/plane.h"

namespace iris
{
/*
struct Plane {
    iris::Vec3 n; // Plane normal. Points x on the plane satisfy Dot(n,x) = d
    float d; // d = dot(n,p) for a given point p on the plane
};
*/

class IntersectionHelper
{
public:
    // Given three noncollinear points (ordered ccw), compute plane equation
    static Plane computePlaneND(iris::Vec3 a, iris::Vec3 b, iris::Vec3 c) {
        Plane p = { iris::Vec3::crossProduct(b - a, c - a).normalized(),
                    iris::Vec3::dotProduct(p.normal, a) };
        return p;
    }

    // realtime collision detection page 178
    // assumes dir is normalized
    // returns whether or not a collision was made
    static bool raySphereIntersects(iris::Vec3 p,
                                    iris::Vec3 d,
                                    iris::Vec3 spherePos,
                                    float radius,
                                    float& t,
                                    iris::Vec3& hitPoint)
    {
        iris::Vec3 m = p - spherePos;
        float b = iris::Vec3::dotProduct(m, d);
        float c = iris::Vec3::dotProduct(m, m) - radius * radius;

        if (c > 0 && b > 0) return false;

        float discr = b * b - c;

        if (discr < 0) return false;

        t = -b -qSqrt(discr);

        if (t < 0) t = 0;

        hitPoint = p + t * d;

        return true;
    }

    // realtime collision detection page 175
    static int intersectSegmentPlane(iris::Vec3 a, iris::Vec3 b, const Plane& p, float &t, iris::Vec3 &q) {
        // Compute the t value for the directed line ab intersecting the plane
        iris::Vec3 ab = b - a;
        t = (p.d - iris::Vec3::dotProduct(p.normal, a)) / iris::Vec3::dotProduct(p.normal, ab);
        // If t in [0..1] compute and return intersection point
        if (t >= 0.0f && t <= 1.0f) {
            q = a + t * ab;
            return 1;
        }
        // Else no intersection
        return 0;
    }
};

}

#endif // INTERSECTIONHELPER_H
