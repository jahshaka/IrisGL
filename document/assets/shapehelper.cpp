#include "core/math/vec.h"
#include "document/assets/shapehelper.h"
#include "document/assets/linemeshbuilder.h"
#include <qmath.h>

namespace iris
{

MeshPtr ShapeHelper::createWireCube(float size)
{
    auto halfSize = size / 2;
    LineMeshBuilder builder;

    //builder.addLine(iris::Vec3(halfSize, halfSize, halfSize), iris::Vec3(halfSize, halfSize, halfSize));
    // build 4 columns
    builder.addLine(iris::Vec3(-halfSize, halfSize, -halfSize), iris::Vec3(-halfSize, halfSize, halfSize));
    builder.addLine(iris::Vec3(-halfSize, -halfSize, -halfSize), iris::Vec3(-halfSize, -halfSize, halfSize));
    builder.addLine(iris::Vec3(halfSize, -halfSize, -halfSize), iris::Vec3(halfSize, -halfSize, halfSize));
    builder.addLine(iris::Vec3(halfSize, halfSize, -halfSize), iris::Vec3(halfSize, halfSize, halfSize));

    // top
    builder.addLine(iris::Vec3(-halfSize, halfSize, halfSize), iris::Vec3(-halfSize, -halfSize, halfSize));
    builder.addLine(iris::Vec3(-halfSize, -halfSize, halfSize), iris::Vec3(halfSize, -halfSize, halfSize));
    builder.addLine(iris::Vec3(halfSize, -halfSize, halfSize), iris::Vec3(halfSize, halfSize, halfSize));
    builder.addLine(iris::Vec3(halfSize, halfSize, halfSize), iris::Vec3(-halfSize, halfSize, halfSize));

    // bottom
    builder.addLine(iris::Vec3(-halfSize, halfSize, -halfSize), iris::Vec3(-halfSize, -halfSize, -halfSize));
    builder.addLine(iris::Vec3(-halfSize, -halfSize, -halfSize), iris::Vec3(halfSize, -halfSize, -halfSize));
    builder.addLine(iris::Vec3(halfSize, -halfSize, -halfSize), iris::Vec3(halfSize, halfSize, -halfSize));
    builder.addLine(iris::Vec3(halfSize, halfSize, -halfSize), iris::Vec3(-halfSize, halfSize, -halfSize));

    return builder.build();
}

MeshPtr ShapeHelper::createWireSphere(float radius)
{
    LineMeshBuilder builder;


    int divisions = 36;
    float arcWidth = 360.0f/divisions;

    // XY plane
    for(int i=0;i<divisions;i++)
    {
        float angle = i * arcWidth;
        iris::Vec3 a = iris::Vec3(qSin(qDegreesToRadians(angle)), qCos(qDegreesToRadians(angle)), 0) * radius;

        angle = (i+1) * arcWidth;
        iris::Vec3 b = iris::Vec3(qSin(qDegreesToRadians(angle)), qCos(qDegreesToRadians(angle)), 0) * radius;

        builder.addLine(a, b);
    }

    // XZ plane
    for(int i=0;i<divisions;i++)
    {
        float angle = i * arcWidth;
        iris::Vec3 a = iris::Vec3(qSin(qDegreesToRadians(angle)), 0, qCos(qDegreesToRadians(angle))) * radius;

        angle = (i+1) * arcWidth;
        iris::Vec3 b = iris::Vec3(qSin(qDegreesToRadians(angle)), 0, qCos(qDegreesToRadians(angle))) * radius;

        builder.addLine(a, b);
    }

    // YZ plane
    for(int i=0;i<divisions;i++)
    {
        float angle = i * arcWidth;
        iris::Vec3 a = iris::Vec3(0, qSin(qDegreesToRadians(angle)), qCos(qDegreesToRadians(angle))) * radius;

        angle = (i+1) * arcWidth;
        iris::Vec3 b = iris::Vec3(0, qSin(qDegreesToRadians(angle)), qCos(qDegreesToRadians(angle))) * radius;

        builder.addLine(a, b);
    }

    return builder.build();
}

MeshPtr ShapeHelper::createWireCone(float baseRadius)
{
    LineMeshBuilder builder;

    int divisions = 36;
    float arcWidth = 360.0f/divisions;

    // XZ plane
    for(int i=0;i<divisions;i++)
    {
        float angle = i * arcWidth;
        iris::Vec3 a = iris::Vec3(qSin(qDegreesToRadians(angle)), -1, qCos(qDegreesToRadians(angle))) * baseRadius;

        angle = (i+1) * arcWidth;
        iris::Vec3 b = iris::Vec3(qSin(qDegreesToRadians(angle)), -1, qCos(qDegreesToRadians(angle))) * baseRadius;

        builder.addLine(a, b);
    }

    // lines from base to center
    divisions = 4;
    arcWidth = 360.0f/divisions;
    for(int i=0;i<divisions;i++)
    {
        float angle = i * arcWidth;
        iris::Vec3 a = iris::Vec3(qSin(qDegreesToRadians(angle)), -1, qCos(qDegreesToRadians(angle))) * baseRadius;

        iris::Vec3 b = iris::Vec3(0, 0, 0);

        builder.addLine(a, b);
    }

    return builder.build();
}

MeshPtr ShapeHelper::createWireCube(const iris::Vec3& min, const iris::Vec3& max)
{
	LineMeshBuilder builder;

	auto halfSize = (max - min) * 0.5f;
	auto offset = (max + min) * 0.5f;

	// build 4 columns
	builder.addLine(offset + iris::Vec3(halfSize.x(), -halfSize.y(), -halfSize.z()), offset + iris::Vec3(halfSize.x(), halfSize.y(), -halfSize.z()));
	builder.addLine(offset + iris::Vec3(-halfSize.x(), -halfSize.y(), -halfSize.z()), offset + iris::Vec3(-halfSize.x(), halfSize.y(), -halfSize.z()));
	builder.addLine(offset + iris::Vec3(-halfSize.x(), -halfSize.y(), halfSize.z()), offset + iris::Vec3(-halfSize.x(), halfSize.y(), halfSize.z()));
	builder.addLine(offset + iris::Vec3(halfSize.x(), -halfSize.y(), halfSize.z()), offset + iris::Vec3(halfSize.x(), halfSize.y(), halfSize.z()));

	// top
	builder.addLine(offset + iris::Vec3(-halfSize.x(), halfSize.y(), halfSize.z()), offset + iris::Vec3(-halfSize.x(), halfSize.y(), -halfSize.z()));
	builder.addLine(offset + iris::Vec3(-halfSize.x(), halfSize.y(), -halfSize.z()), offset + iris::Vec3(halfSize.x(), halfSize.y(), -halfSize.z()));
	builder.addLine(offset + iris::Vec3(halfSize.x(), halfSize.y(), -halfSize.z()), offset + iris::Vec3(halfSize.x(), halfSize.y(), halfSize.z()));
	builder.addLine(offset + iris::Vec3(halfSize.x(), halfSize.y(), halfSize.z()), offset + iris::Vec3(-halfSize.x(), halfSize.y(), halfSize.z()));

	// bottom
	builder.addLine(offset + iris::Vec3(-halfSize.x(), -halfSize.y(), halfSize.z()), offset + iris::Vec3(-halfSize.x(), -halfSize.y(), -halfSize.z()));
	builder.addLine(offset + iris::Vec3(-halfSize.x(), -halfSize.y(), -halfSize.z()), offset + iris::Vec3(halfSize.x(), -halfSize.y(), -halfSize.z()));
	builder.addLine(offset + iris::Vec3(halfSize.x(), -halfSize.y(), -halfSize.z()), offset + iris::Vec3(halfSize.x(), -halfSize.y(), halfSize.z()));
	builder.addLine(offset + iris::Vec3(halfSize.x(), -halfSize.y(), halfSize.z()), offset + iris::Vec3(-halfSize.x(), -halfSize.y(), halfSize.z()));


	return builder.build();
}

MeshPtr ShapeHelper::createWireCube(const AABB& aabb)
{
	return createWireCube(aabb.getMin(), aabb.getMax());
}

}
