#ifndef LINEMESHBUILDER_H
#define LINEMESHBUILDER_H

#include "core/math/vec.h"
#include <QVector>
#include "irisglfwd.h"

namespace iris {

class LineMeshBuilder
{
	struct LineData
	{
		iris::Vec3 pos;
		iris::Vec4 color;
	};

    QVector<LineData> lineData;

public:
    void addLine(iris::Vec3 a, iris::Vec3 b);
	void addLine(iris::Vec3 a, QColor aCol, iris::Vec3 b, QColor bCol);
    MeshPtr build();
};

}

#endif // LINEMESHBUILDER_H
