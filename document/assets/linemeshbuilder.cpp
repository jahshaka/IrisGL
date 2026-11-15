#include "core/math/vec.h"
#include "document/assets/linemeshbuilder.h"
#include "document/assets/vertexlayout.h"
#include "document/assets/mesh.h"


namespace iris {


void LineMeshBuilder::addLine(iris::Vec3 a, iris::Vec3 b)
{
	this->addLine(a, QColor(255, 255, 255), b, QColor(255, 255, 255));
}

void LineMeshBuilder::addLine(iris::Vec3 a, QColor aCol, iris::Vec3 b, QColor bCol)
{
	lineData.append({ a, iris::Vec4(aCol.redF(), aCol.greenF(), aCol.blueF(), aCol.alphaF()) });
	lineData.append({ b, iris::Vec4(bCol.redF(), bCol.greenF(), bCol.blueF(), bCol.alphaF()) });
}

MeshPtr LineMeshBuilder::build()
{
    auto layout = new VertexLayout();
    layout->addAttrib(VertexAttribUsage::Position, AttribTypeFloat, 3, sizeof(float) * 3);
	layout->addAttrib(VertexAttribUsage::Color, AttribTypeFloat, 4, sizeof(float) * 4);

    auto mesh = Mesh::create(lineData.data(), lineData.size() * sizeof(LineData), lineData.size(), layout);
    mesh->setPrimitiveMode(PrimitiveMode::Lines);
    return MeshPtr(mesh);
}

}
