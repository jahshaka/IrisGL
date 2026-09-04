#include "core/math/vec.h"
#include "core/geometry/plane.h"
#include "core/geometry/boundingsphere.h"


namespace iris {

SphereClassification Plane::classifySphere(BoundingSphere *sphere)
{
    auto& spherePos = sphere->pos;

    auto dist = normal.x() * spherePos.x() +
                normal.y() * spherePos.y() +
                normal.z() * spherePos.z() +
                d;

    if (dist < -sphere->radius)
        return SphereClassification::Behind;
    if (dist > sphere->radius)
        return SphereClassification::InFront;

    return SphereClassification::Intersects;

}

Plane::Plane()
{
    normal = iris::Vec3(0, 1, 0);
    d = 0;
}

Plane::Plane(iris::Vec3 planeNormal, float distance)
{
    normal = planeNormal;
    d = distance;
}


}
