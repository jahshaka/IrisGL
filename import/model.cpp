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

// Model::applyAnimation / updateAnimation are GONE with the document's clip
// evaluator (ANIMATION_ENGINE_MIGRATION_SPEC, full retirement). They were the
// ModelLoader-era twin of SceneNode::updateAnimation's skeletal branch and the
// only callers of Skeleton::applyAnimation's bone-local overload — the one that
// read Bone::bindingPos/Rot/Scale, which nothing on the editor's import route
// ever wrote (§1.5 F1). ModelLoader is not on that route and nothing called
// either function.

Model::~Model()
{
}

}
