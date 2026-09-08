#pragma once
#include "core/math/vec.h"
#include <QVector>
#include "core/geometry/boundingsphere.h"

namespace iris
{

class AABB
{
	iris::Vec3 minPos;
	iris::Vec3 maxPos;

public:
	AABB();

	void setNegativeInfinity();

	iris::Vec3 getMin() const { return minPos; }
	iris::Vec3 getMax() const { return maxPos; }

	iris::Vec3 getCenter() const;
	iris::Vec3 getSize() const;
	iris::Vec3 getHalfSize() const;

	void offset(const iris::Vec3& offset);

    // scale from center
    void scale(float scale);

    // scale from arbitrary pivot
    void scale(float scale, const iris::Vec3& pivot);

	void merge(const iris::Vec3& point);
	void merge(const QVector<iris::Vec3>& points);
	void merge(const AABB& aabb);

	BoundingSphere getMinimalEnclosingSphere() const;

	static AABB fromPoints(const QVector<iris::Vec3>& points);

private:
	iris::Vec3 getMin(const iris::Vec3& a, const iris::Vec3& b) const;
	iris::Vec3 getMax(const iris::Vec3& a, const iris::Vec3& b) const;
};

}
