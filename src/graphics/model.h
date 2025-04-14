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
#include <QMatrix4x4>
#include "../irisglfwd.h"
#include "../animation/skeletalanimation.h"
#include "../geometry/boundingsphere.h"
#include "../geometry/aabb.h"

#include "assimp/scene.h"

namespace iris
{

struct ModelMesh
{
	iris::MeshPtr mesh;
	iris::MaterialPtr material;

	// refers to the parent bone
	QString meshName;

	// from the root of the model's scene
	QMatrix4x4 transform;

	ModelMesh()
	{
		transform.setToIdentity();
	}
};

class Model
{
	friend class ModelLoader;

    SkeletonPtr skeleton;
    QMap<QString, SkeletalAnimationPtr> skeletalAnimations;
	SkeletalAnimationPtr activeAnimation;
	float animTime;

	
	GraphicsDevicePtr device;

	BoundingSphere boundingSphere;
	AABB aabb;
public:
	//QList<MeshPtr> meshes;
	QVector<ModelMesh> modelMeshes;
    bool hasSkeleton();
    SkeletonPtr getSkeleton();
	void setSkeleton(const SkeletonPtr &value);
	void setActiveAnimation(SkeletalAnimationPtr animation) { activeAnimation = animation; }
	void setActiveAnimation(const QString& animationName);
    void addSkeletalAnimation(QString name, SkeletalAnimationPtr anim);
    QMap<QString, SkeletalAnimationPtr> getSkeletalAnimations();
    bool hasSkeletalAnimations();

	AABB getAABB() { return aabb; }
	BoundingSphere getBoundingSphere() { return boundingSphere; }

	void applyAnimation(float time);
	void updateAnimation(float dt);
    void draw(GraphicsDevicePtr device);
	
    ~Model();
private:
	explicit Model(QVector<ModelMesh> modelMeshes);
	Model(QVector<ModelMesh> modelMeshes, QMap<QString, SkeletalAnimationPtr> skeletalAnimations);
};

}

#endif // MODEL_H
