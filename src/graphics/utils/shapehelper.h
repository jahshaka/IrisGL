#ifndef SHAPEBUILDER_H
#define SHAPEBUILDER_H

#include "../../irisglfwd.h"
#include "geometry/aabb.h"

namespace iris
{

class ShapeHelper
{
public:

    static MeshPtr createWireCube(float size = 1);
    static MeshPtr createWireSphere(float radius = 0.5);
    static MeshPtr createWireCone(float baseRadius = 0.5);

	static MeshPtr createWireCube(const QVector3D& min, const QVector3D& max);
	static MeshPtr createWireCube(const AABB& aabb);
};

}

#endif // SHAPEBUILDER_H
