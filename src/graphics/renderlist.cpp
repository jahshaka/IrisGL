#include "renderlist.h"
#include "renderitem.h"
#include "model.h"
#include <algorithm>

namespace iris {

RenderList::RenderList()
{
    used.reserve(1000);
    pool.reserve(1000);

    // todo: check how slow this is
    for(int i = 0;i<1000; i++)
        pool.append(new RenderItem());
}

void RenderList::add(RenderItem *item)
{
    renderList.append(item);
}

RenderItem *RenderList::submitMesh(MeshPtr mesh, MaterialPtr mat, QMatrix4x4 worldMatrix)
{
    RenderItem* item;
    if (pool.size()>0) {
        item = pool.takeFirst();
    } else {
        item = new RenderItem();
    }
    item->reset();
    used.push_back(item);

    item->type = RenderItemType::Mesh;
    item->mesh = mesh;
    item->material = mat;
    item->worldMatrix = worldMatrix;
	item->renderStates = mat->renderStates;
	item->renderLayer = mat->renderLayer;

    renderList.append(item);
	return item;
}

RenderItem *RenderList::submitMesh(MeshPtr mesh, QOpenGLShaderProgram *shader, QMatrix4x4 worldMatrix, int renderLayer)
{
    RenderItem* item;
    if (pool.size()>0) {
        item = pool.takeFirst();
    } else {
        item = new RenderItem();
    }
    item->reset();
    used.push_back(item);

    item->type = RenderItemType::Mesh;
    item->mesh = mesh;
    item->worldMatrix = worldMatrix;
    item->renderLayer = renderLayer;

    renderList.append(item);
	return item;
}

void RenderList::submitModel(ModelPtr model, MaterialPtr mat, QMatrix4x4 worldMatrix)
{
	for (auto modelMesh : model->modelMeshes) {
		submitMesh(modelMesh.mesh, mat, worldMatrix);
	}
}

void RenderList::submitModel(ModelPtr model, QMatrix4x4 worldMatrix)
{
	for (auto modelMesh : model->modelMeshes) {
		submitMesh(modelMesh.mesh, modelMesh.material, worldMatrix);
	}
}

void RenderList::clear()
{
    renderList.clear();

    for(auto item : used)
        pool.append(item);
    used.clear();
}

void RenderList::sort()
{
    /*
    qSort(renderList.begin(), renderList.end(), [](const RenderItem* a, const RenderItem* b) {
        return a->renderLayer < b->renderLayer;
    });
    */

    std::sort(renderList.begin(), renderList.end(), [](const RenderItem* a, const RenderItem* b) { return true; });
}

RenderList::~RenderList()
{

}



}
