#ifndef PLANE_H
#define PLANE_H

#include "core/export.h"
#include "core/math/vec.h"


namespace iris
{
class BoundingSphere;

enum SphereClassification
{
    InFront,
    Intersects,
    Behind
};

class IRIS_CORE_API Plane
{
public:
    iris::Vec3 normal;
    float d = 0.0f;

    Plane();
    Plane(iris::Vec3 planeNormal, float distance);

    SphereClassification classifySphere(BoundingSphere* sphere);
};

}

#endif // PLANE_H
