/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#ifndef CAMERANODE_H
#define CAMERANODE_H

#include <QMatrix4x4>
#include <QtCore/QtMath>
#include "../irisglfwd.h"
#include "../scenegraph/scenenode.h"

namespace iris
{

enum class CameraProjection {
	Orthogonal,
	Perspective
};

class CameraNode : public SceneNode
{
public:
    float aspectRatio;
    float angle;
    float nearClip;
    float farClip;
    float vrViewScale;
	float orthoSize;
	bool isPerspective;

	CameraProjection projMode;

    QMatrix4x4 viewMatrix;
    QMatrix4x4 projMatrix;

	void setProjection(CameraProjection view);
    CameraProjection getProjection();
    float getVrViewScale();
    void setVrViewScale(float viewScale);
    void setAspectRatio(float aspect);
    void setFieldOfViewRadians(float fov);
    void setFieldOfViewDegrees(float fov);
    void lookAt(QVector3D target);
    void updateCameraMatrices();
	void setOrthagonalZoom(float size);
    void update(float dt) override;

    static CameraNodePtr create() {
        return QSharedPointer<CameraNode>(new CameraNode());
    }

    /**
     * Calculate picking ray given the screen position.
     * Assumes the ray's origin is the camera's position.
     * @param viewPortWidth
     * @param viewPortHeight
     * @param pos point in screen space
     * @return
     */
    QVector3D calculatePickingDirection(int viewPortWidth, int viewPortHeight, QPointF pos);

	SceneNodePtr createDuplicate() override;

private:
    CameraNode()
    {
        angle = 45;         // Degrees are always used internally
        nearClip = 0.1f;
        farClip = 500.0f;
        aspectRatio = 1.0f; // Assumes a square viewport by default
		orthoSize = 10.0f;
        exportable = false;
		projMode = CameraProjection::Perspective;
		vrViewScale = 2.0f; // good default
        updateCameraMatrices();
    }

};

}
#endif // CAMERANODE_H
