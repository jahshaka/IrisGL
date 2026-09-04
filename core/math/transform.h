#ifndef TRANSFORM_H
#define TRANSFORM_H

#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"


namespace iris
{

class Transform
{
public:
    iris::Vec3 pos;
    iris::Quat rot;
    iris::Vec3 scale;

    iris::Mat4 toMatrix()
    {
        iris::Mat4 matrix;
        matrix.setToIdentity();
        return matrix;
    }
};

}

#endif // TRANSFORM_H
