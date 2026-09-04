#ifndef FRUSTUM_H
#define FRUSTUM_H

#include "core/math/mat4.h"
#include <QList>
#include "core/geometry/plane.h"

namespace iris {

class BoundingSphere;
class Frustum
{
public:
    QList<Plane> planes;

    // projection x view
    void build(iris::Mat4 viewProj);

    // checks if the sphere is inside or touches the bounding sphere
    bool isSphereInside(BoundingSphere* sphere);
};

}

#endif // FRUSTUM_H
