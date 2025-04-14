#include "../irisglfwd.h"
#include "modelloader.h"
#include "graphics/model.h"
#include "../graphics/mesh.h"
#include "../graphics/skeleton.h"

#include "assimp/postprocess.h"
#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/mesh.h"

#include <QFile>
#include <functional>

namespace iris
{
ModelLoader::ModelLoader(GraphicsDevicePtr device)
{
	this->device = device;
}

// Extracts meshes and skeleton from scene
ModelPtr ModelLoader::load(QString filePath)
{
	// legacy -- update TODO
	Assimp::Importer importer;
	const aiScene *scene;

	QFile file(filePath);
	if (!file.exists())
	{
		irisLog("model " + filePath + " does not exists");
		return ModelPtr();
	}

	if (filePath.startsWith(":") || filePath.startsWith("qrc:")) {
		// loads mesh from resource
		file.open(QIODevice::ReadOnly);
		auto data = file.readAll();
		scene = importer.ReadFileFromMemory((void*)data.data(),
			data.length(),
			aiProcessPreset_TargetRealtime_Fast);
	}
	else {
		// load mesh from file
		scene = importer.ReadFile(filePath.toStdString().c_str(),
			aiProcessPreset_TargetRealtime_Fast);
	}

	if (!scene) {
		irisLog("model " + filePath + ": error parsing file");
		return ModelPtr();
	}

	if (scene->mNumMeshes <= 0) {
		irisLog("model " + filePath + ": scene has no meshes");
		return ModelPtr();
	}

	/*
	QList<MeshPtr> meshes;
	for (int i = 0; i < scene->mNumMeshes; i++) {
		auto mesh = scene->mMeshes[0];
		auto meshObj = MeshPtr(new Mesh(scene->mMeshes[0]));
		auto skel = Mesh::extractSkeleton(mesh, scene);

		if (!!skel)
			meshObj->setSkeleton(skel);

		meshes.append(meshObj);
	}
	*/
	auto modelMeshes = extractMeshesFromScene(scene);

	auto skeleton = ModelLoader::extractSkeletonFromScene(scene);
	auto anims = Mesh::extractAnimations(scene);
	auto model = new Model(modelMeshes);
	model->setSkeleton(skeleton);
	for (auto animName : anims.keys())
	{
		model->addSkeletalAnimation(animName, anims[animName]);
	}
	
	return ModelPtr(model);
}

SkeletonPtr ModelLoader::extractSkeletonFromScene(const aiScene* scene)
{
	auto skel = Skeleton::create();

	std::function<void(aiNode*, BonePtr parentBone)> evalChildren;
	evalChildren = [skel, &evalChildren](aiNode* node, BonePtr parentBone) {
		auto bone = Bone::create(QString(node->mName.C_Str()));

		//extract transform
		aiVector3D pos, scale;
		aiQuaternion rot;

		//auto transform = node->mTransformation;
		node->mTransformation.Decompose(scale, rot, pos);
		bone->pos = QVector3D(pos.x, pos.y, pos.z);
		bone->scale = QVector3D(scale.x, scale.y, scale.z);
		bone->rot = QQuaternion(rot.w, rot.x, rot.y, rot.z);

		bone->bindingPos = bone->pos;
		bone->bindingScale = bone->scale;
		bone->bindingRot = bone->rot;

		skel->addBone(bone);
		if (!!parentBone)
			parentBone->addChild(bone);
		

		for (unsigned i = 0; i < node->mNumChildren; i++)
		{
			auto childNode = node->mChildren[i];
			evalChildren(childNode, bone);
		}
	};

	//auto bone = Bone::create(QString(meshBone->mName.C_Str()));
	evalChildren(scene->mRootNode, BonePtr());

	return skel;
}

QVector<ModelMesh> ModelLoader::extractMeshesFromScene(const aiScene * scene)
{
	QVector<ModelMesh> modelMeshes;
	std::function<void(aiNode*, const aiMatrix4x4&)> evalChildren;
	evalChildren = [&modelMeshes, &evalChildren, scene](aiNode* node, const aiMatrix4x4& parentTransform) {
		
		//extract transform
		aiVector3D pos, scale;
		aiQuaternion rot;

		// assimp's matrices are row major so the mult order is child * parent
		auto globalTransform = node->mTransformation * parentTransform;
		//auto globalTransform = parentTransform * node->mTransformation;
		//node->mTransformation.Decompose(scale, rot, pos);
		globalTransform.Decompose(scale, rot, pos);

		// all meshes under this node will inherit this transform
		QMatrix4x4 meshTransform;
		meshTransform.setToIdentity();
		meshTransform.translate(QVector3D(pos.x, pos.y, pos.z));
		meshTransform.rotate(QQuaternion(rot.w, rot.x, rot.y, rot.z));
		meshTransform.scale(QVector3D(scale.x, scale.y, scale.z));

		for (int i = 0; i < node->mNumMeshes; i++) {
			auto mesh = scene->mMeshes[node->mMeshes[i]];
			ModelMesh modelMesh;
			modelMesh.meshName = QString(node->mName.C_Str());
			
			modelMesh.transform = meshTransform;
			

			auto meshObj = MeshPtr(new Mesh(scene->mMeshes[0]));
			auto skel = Mesh::extractSkeleton(mesh, scene);

			if (!!skel)
				meshObj->setSkeleton(skel);

			modelMesh.mesh = meshObj;
			modelMeshes.append(modelMesh);
		}


		for (unsigned i = 0; i < node->mNumChildren; i++)
		{
			auto childNode = node->mChildren[i];
			evalChildren(childNode, globalTransform);
		}
	};
	evalChildren(scene->mRootNode, aiMatrix4x4());

	return modelMeshes;
}

}