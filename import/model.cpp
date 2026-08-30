/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/model.h"

#include <QString>
#include <QFile>
#include <QtMath>

#include "irisglfwd.h"
#include "core/logger.h"
#include "document/materials/material.h"
#include "document/assets/mesh.h"

#include "assimp/postprocess.h"
#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/mesh.h"

#include "document/assets/vertexlayout.h"
#include "core/geometry/trimesh.h"
#include "document/assets/skeleton.h"
#include "document/animation/skeletalanimation.h"
#include "core/geometry/boundingsphere.h"
#include "core/geometry/aabb.h"

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

Model::~Model()
{
}

}
