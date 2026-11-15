#ifndef BOUNDINGSPHERE_H
#define BOUNDINGSPHERE_H

#include <algorithm>

#include "core/math/vec.h"


namespace iris
{

class BoundingSphere
{
public:
    iris::Vec3 pos;
    /// INITIALISED, deliberately: Mesh's non-assimp constructors never run
    /// calculateBounds, so a default-constructed BoundingSphere is what
    /// getBoundingSphere() hands out for them. Indeterminate here means a
    /// ray-sphere broad phase (ScenePicker, Scene::rayCast) answers from
    /// uninitialised memory. Zero is the honest answer: "no bounds known".
    float radius = 0.0f;

    void expand(iris::Vec3 point)
    {
        auto dist = pos.distanceToPoint(point);
        if (dist > radius)
            radius =dist;
    }

    bool intersects(const BoundingSphere& other)
    {
        return pos.distanceToPoint(other.pos) < radius + other.radius;
    }

    static BoundingSphere merge(const BoundingSphere& a, const BoundingSphere& b)
    {
        iris::Vec3 center = b.pos - a.pos;
        auto dist = center.length();

        // handle the case where one sphere might be in the next
        if (dist < a.radius + b.radius) {
            if (dist <= a.radius - b.radius)
                return a;

            if (dist <= b.radius - a.radius)
                return b;
        }

        float aRad = std::max(a.radius - dist, b.radius);
        float bRad = std::max(a.radius + dist, b.radius);

        center = center + (((aRad - bRad) / (2 * center.length())) * center);

        BoundingSphere mergedSphere;
        mergedSphere.pos = a.pos + center;
        mergedSphere.radius = (aRad + bRad) / 2.0f;

        return mergedSphere;
    }
};

}

#endif // BOUNDINGSPHERE_H
