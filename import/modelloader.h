#ifndef MODELLOADER_H
#define MODELLOADER_H

#include "irisglfwd.h"
#include "import/model.h"

struct aiScene;
namespace iris
{

class ModelLoader
{
public:
	ModelLoader();
	ModelPtr load(QString path);

private:
	static SkeletonPtr extractSkeletonFromScene(const aiScene* scene);
	static QVector<ModelMesh> extractMeshesFromScene(const aiScene* scene);
};

}
#endif