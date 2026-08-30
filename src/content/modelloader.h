#ifndef MODELLOADER_H
#define MODELLOADER_H

#include "../irisglfwd.h"
#include "../graphics/model.h"

class aiScene;
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