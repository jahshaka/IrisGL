#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "irisglfwd.h"
#include "import/modelloader.h"
#include "import/model.h"
#include "document/assets/mesh.h"
#include "document/assets/skeleton.h"

#include "assimp/postprocess.h"
#include "import/importflags.h"
#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/mesh.h"

#include <QFile>
#include <functional>

namespace iris
{
ModelLoader::ModelLoader()
{
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
		if (!file.open(QIODevice::ReadOnly))
		    qWarning("ModelLoader::load: failed to open %s", qUtf8Printable(filePath));
		auto data = file.readAll();
		scene = importer.ReadFileFromMemory((void*)data.data(),
			data.length(),
			iris::ImportFlags::Canonical);
	}
	else {
		// load mesh from file
		scene = importer.ReadFile(filePath.toStdString().c_str(),
			iris::ImportFlags::Canonical);
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
		bone->pos = iris::Vec3(pos.x, pos.y, pos.z);
		bone->scale = iris::Vec3(scale.x, scale.y, scale.z);
		bone->rot = iris::Quat(rot.w, rot.x, rot.y, rot.z);

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
		iris::Mat4 meshTransform;
		meshTransform.setToIdentity();
		meshTransform.translate(iris::Vec3(pos.x, pos.y, pos.z));
		meshTransform.rotate(iris::Quat(rot.w, rot.x, rot.y, rot.z));
		meshTransform.scale(iris::Vec3(scale.x, scale.y, scale.z));

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