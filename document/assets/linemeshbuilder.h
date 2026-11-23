#ifndef LINEMESHBUILDER_H
#define LINEMESHBUILDER_H

#include "document/export.h"

#include "core/math/vec.h"
#include <vector>
#include "irisglfwd.h"
#include "document/srgbcolor.h"

namespace iris {

class IRIS_DOCUMENT_API LineMeshBuilder
{
	struct LineData
	{
		iris::Vec3 pos;
		iris::Vec4 color;
	};

    std::vector<LineData> lineData;

public:
    void addLine(iris::Vec3 a, iris::Vec3 b);
	void addLine(iris::Vec3 a, SrgbColor aCol, iris::Vec3 b, SrgbColor bCol);
    MeshPtr build();
};

}

#endif // LINEMESHBUILDER_H
