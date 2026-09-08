#ifndef PLANE_H
#define PLANE_H

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

class Plane
{
public:
    iris::Vec3 normal;
    float d;

    Plane();
    Plane(iris::Vec3 planeNormal, float distance);

    SphereClassification classifySphere(BoundingSphere* sphere);
};

}

#endif // PLANE_H
