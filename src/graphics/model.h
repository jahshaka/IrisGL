/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#ifndef MODEL_H
#define MODEL_H

#include <QString>
#include <qopengl.h>
#include <QColor>
#include "../irisglfwd.h"
#include "../animation/skeletalanimation.h"
#include "../geometry/boundingsphere.h"
#include "../geometry/aabb.h"

#include "assimp/scene.h"

namespace iris
{

class Model
{
	friend class ModelLoader;

    SkeletonPtr skeleton;
    QMap<QString, SkeletalAnimationPtr> skeletalAnimations;
	SkeletalAnimationPtr activeAnimation;
	float animTime;

	QList<MeshPtr> meshes;
	GraphicsDevicePtr device;

	BoundingSphere boundingSphere;
	AABB aabb;
public:
    bool hasSkeleton();
    SkeletonPtr getSkeleton();
	void setSkeleton(const SkeletonPtr &value);
    void addSkeletalAnimation(QString name, SkeletalAnimationPtr anim);
    QMap<QString, SkeletalAnimationPtr> getSkeletalAnimations();
    bool hasSkeletalAnimations();

	AABB getAABB() { return aabb; }
	BoundingSphere getBoundingSphere() { return boundingSphere; }

	void updateAnimation(float dt);
    void draw(GraphicsDevicePtr device);
	
    ~Model();
private:
	explicit Model(QList<MeshPtr> meshes);
	Model(QList<MeshPtr> meshes, QMap<QString, SkeletalAnimationPtr> skeletalAnimations);
};

}

#endif // MODEL_H
