#include "core/math/vec.h"
#include "core/geometry/aabb.h"
#include "core/geometry/boundingsphere.h"
#include <limits>

namespace iris
{

AABB::AABB()
{
	setNegativeInfinity();
}


void AABB::setNegativeInfinity()
{
	minPos = iris::Vec3(std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max());

	maxPos = iris::Vec3(-std::numeric_limits<float>::max(),
		-std::numeric_limits<float>::max(),
		-std::numeric_limits<float>::max());
}

iris::Vec3 AABB::getMin(const iris::Vec3& a, const iris::Vec3& b) const
{
	iris::Vec3 result;

	result.setX(a.x() < b.x() ? a.x() : b.x());
	result.setY(a.y() < b.y() ? a.y() : b.y());
	result.setZ(a.z() < b.z() ? a.z() : b.z());

	return result;
}

iris::Vec3 AABB::getMax(const iris::Vec3& a, const iris::Vec3& b) const
{
	iris::Vec3 result;

	result.setX(a.x() > b.x() ? a.x() : b.x());
	result.setY(a.y() > b.y() ? a.y() : b.y());
	result.setZ(a.z() > b.z() ? a.z() : b.z());

	return result;
}

iris::Vec3 AABB::getCenter() const
{
	return (minPos + maxPos) * 0.5f;
}

iris::Vec3 AABB::getSize() const
{
	return iris::Vec3(maxPos.x() - minPos.x(),
		maxPos.y() - minPos.y(),
		maxPos.z() - minPos.z());
}

iris::Vec3 AABB::getHalfSize() const
{
	return getSize() * 0.5f;
}

void AABB::offset(const iris::Vec3& offset)
{
	minPos += offset;
    maxPos += offset;
}

void AABB::scale(float scale)
{
    this->scale(scale, this->getCenter());
}

void AABB::scale(float scale, const iris::Vec3& pivot)
{
    auto diff = minPos - pivot;
    minPos += diff * scale;

    diff = maxPos - pivot;
    maxPos += diff * scale;
}

void AABB::merge(const iris::Vec3& point)
{
	minPos = getMin(minPos, point);
	maxPos = getMax(maxPos, point);
}

void AABB::merge(const QVector<iris::Vec3>& points)
{
	for (auto &p : points) {
		merge(p);
	}
}

void AABB::merge(const AABB& aabb)
{
	minPos = getMin(minPos, aabb.minPos);
	maxPos = getMax(maxPos, aabb.maxPos);
}

BoundingSphere AABB::getMinimalEnclosingSphere() const
{
	return { getCenter(), getSize().length() * 0.5f };
}

}
