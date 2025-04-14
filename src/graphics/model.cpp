/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#include "../irisglfwd.h"
#include "model.h"
#include "material.h"

#include "assimp/postprocess.h"
#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/mesh.h"

#include <QString>
#include <QFile>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_2_Core>
#include <QOpenGLTexture>
#include <QtMath>

#include "graphicsdevice.h"
#include "vertexlayout.h"
#include "../geometry/trimesh.h"
#include "skeleton.h"
#include "../animation/skeletalanimation.h"
#include "../geometry/boundingsphere.h"
#include "../geometry/aabb.h"

#include <functional>

namespace iris
{

Model::Model(QVector<ModelMesh> modelMeshes)
{
	this->modelMeshes = modelMeshes;
	animTime = 0;
}

Model::Model(QVector<ModelMesh> modelMeshes, QMap<QString, SkeletalAnimationPtr> skeletalAnimations)
{
	this->modelMeshes = modelMeshes;
	this->skeletalAnimations = skeletalAnimations;
}

void Model::setSkeleton(const SkeletonPtr &value)
{
    skeleton = value;
}

bool Model::hasSkeleton()
{
    return !!skeleton;
}

SkeletonPtr Model::getSkeleton()
{
    return skeleton;
}

void Model::setActiveAnimation(const QString& animationName)
{
	if (skeletalAnimations.contains(animationName))
		this->setActiveAnimation(skeletalAnimations[animationName]);
	else
		irisLog("No animation named: " + animationName);
}

void Model::addSkeletalAnimation(QString name, SkeletalAnimationPtr anim)
{
    skeletalAnimations.insert(name, anim);
}

QMap<QString, SkeletalAnimationPtr> Model::getSkeletalAnimations()
{
    return skeletalAnimations;
}

bool Model::hasSkeletalAnimations()
{
    return skeletalAnimations.count() != 0;
}

void Model::applyAnimation(float time)
{
	if (!!activeAnimation && !!skeleton) {
		skeleton->applyAnimation(activeAnimation, time);

		QMap<QString, QMatrix4x4> skeletonSpaceMatrices;
		for (auto boneName : skeleton->boneMap.keys()) {
			skeletonSpaceMatrices[boneName] = skeleton->bones[skeleton->boneMap[boneName]]->transformMatrix;
		}
		for (auto& modelMesh : modelMeshes) {
			modelMesh.transform = skeletonSpaceMatrices[modelMesh.meshName];
			auto inverseMeshMatrix = modelMesh.transform.inverted();
			if (modelMesh.mesh->hasSkeleton())
				modelMesh.mesh->getSkeleton()->applyAnimation(inverseMeshMatrix, skeletonSpaceMatrices);
		}
	}
}

void Model::updateAnimation(float dt)
{
	animTime += dt;
	applyAnimation(animTime);
}

void Model::draw(GraphicsDevicePtr device)
{
	for (auto& modelMesh : modelMeshes)
		modelMesh.mesh->draw(device);
}

Model::~Model()
{
}

}
