#ifndef SHAPEBUILDER_H
#define SHAPEBUILDER_H

#include "core/math/vec.h"
#include "irisglfwd.h"
#include "core/geometry/aabb.h"

namespace iris
{

class ShapeHelper
{
public:

    static MeshPtr createWireCube(float size = 1);
    static MeshPtr createWireSphere(float radius = 0.5);
    static MeshPtr createWireCone(float baseRadius = 0.5);

	static MeshPtr createWireCube(const iris::Vec3& min, const iris::Vec3& max);
	static MeshPtr createWireCube(const AABB& aabb);
};

}

#endif // SHAPEBUILDER_H
