#include "aabb.h"
#include "boundingsphere.h"
#include <limits>

namespace iris
{

AABB::AABB()
{
	setNegativeInfinity();
}


void AABB::setNegativeInfinity()
{
	minPos = QVector3D(std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max());

	maxPos = QVector3D(-std::numeric_limits<float>::max(),
		-std::numeric_limits<float>::max(),
		-std::numeric_limits<float>::max());
}

QVector3D AABB::getMin(const QVector3D& a, const QVector3D& b) const
{
	QVector3D result;

	result.setX(a.x() < b.x() ? a.x() : b.x());
	result.setY(a.y() < b.y() ? a.y() : b.y());
	result.setZ(a.z() < b.z() ? a.z() : b.z());

	return result;
}

QVector3D AABB::getMax(const QVector3D& a, const QVector3D& b) const
{
	QVector3D result;

	result.setX(a.x() > b.x() ? a.x() : b.x());
	result.setY(a.y() > b.y() ? a.y() : b.y());
	result.setZ(a.z() > b.z() ? a.z() : b.z());

	return result;
}

QVector3D AABB::getCenter() const
{
	return (minPos + maxPos) * 0.5f;
}

QVector3D AABB::getSize() const
{
	return QVector3D(maxPos.x() - minPos.x(),
		maxPos.y() - minPos.y(),
		maxPos.z() - minPos.z());
}

QVector3D AABB::getHalfSize() const
{
	return getSize() * 0.5f;
}

void AABB::offset(const QVector3D& offset)
{
	minPos += offset;
    maxPos += offset;
}

void AABB::scale(float scale)
{
    this->scale(scale, this->getCenter());
}

void AABB::scale(float scale, const QVector3D& pivot)
{
    auto diff = minPos - pivot;
    minPos += diff * scale;

    diff = maxPos - pivot;
    maxPos += diff * scale;
}

void AABB::merge(const QVector3D& point)
{
	minPos = getMin(minPos, point);
	maxPos = getMax(maxPos, point);
}

void AABB::merge(const QVector<QVector3D>& points)
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
	auto halfSize = getHalfSize();
	auto radius = qMax(halfSize.x(), qMax(halfSize.y(), halfSize.z()));
	return { getCenter(), radius };
}

bool AABB::intersects(const AABB &other)
{
	// 6 separating axes
	if (maxPos.x() < other.minPos.x()) return false;
	if (maxPos.y() < other.minPos.y()) return false;
	if (maxPos.z() < other.minPos.z()) return false;

	if (minPos.x() > other.maxPos.x()) return false;
	if (minPos.y() > other.maxPos.y()) return false;
	if (minPos.z() > other.maxPos.z()) return false;

	// must be intersecting at this point
	return true;
}

}