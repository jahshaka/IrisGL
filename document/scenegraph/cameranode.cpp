/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/scenegraph/cameranode.h"

#include <QVector3D>
#include <QPoint>

#include "core/math/mathhelper.h"
#include "core/properties/property.h"

namespace iris
{

QList<Property*> CameraNode::getProperties()
{
    auto props = SceneNode::getProperties();

    auto prop = new FloatProperty();
    prop->displayName = "Aspect Ratio";
    prop->name = "aspectRatio";
    prop->value = aspectRatio;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Field of View";
    prop->name = "angle";
    prop->value = angle;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Near Clip";
    prop->name = "nearClip";
    prop->value = nearClip;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Far Clip";
    prop->name = "farClip";
    prop->value = farClip;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Ortho Size";
    prop->name = "orthoSize";
    prop->value = orthoSize;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "VR View Scale";
    prop->name = "vrViewScale";
    prop->value = vrViewScale;
    props.append(prop);

    auto intProp = new IntProperty();
    intProp->displayName = "Projection Mode";
    intProp->name = "projMode";
    intProp->value = static_cast<int>(projMode);
    props.append(intProp);

    return props;
}

QVariant CameraNode::getPropertyValue(QString valueName)
{
    if (valueName == "aspectRatio") return aspectRatio;
    if (valueName == "angle")       return angle;
    if (valueName == "nearClip")    return nearClip;
    if (valueName == "farClip")     return farClip;
    if (valueName == "orthoSize")   return orthoSize;
    if (valueName == "vrViewScale") return vrViewScale;
    if (valueName == "projMode")    return static_cast<int>(projMode);

    return SceneNode::getPropertyValue(valueName);
}

bool CameraNode::setPropertyValue(QString valueName, const QVariant &value)
{
    if (valueName == "aspectRatio") { setAspectRatio(value.toFloat());          return true; }
    if (valueName == "angle")       { setFieldOfViewDegrees(value.toFloat());   return true; }
    if (valueName == "nearClip")    { nearClip = value.toFloat();               return true; }
    if (valueName == "farClip")     { farClip = value.toFloat();                return true; }
    if (valueName == "orthoSize")   { setOrthagonalZoom(value.toFloat());       return true; }
    if (valueName == "vrViewScale") { setVrViewScale(value.toFloat());          return true; }
    // setProjection, not a raw assignment: it keeps isPerspective in lock-step
    // with projMode (an out-of-sync pair renders previews orthographic).
    if (valueName == "projMode")    { setProjection(static_cast<CameraProjection>(value.toInt())); return true; }

    return SceneNode::setPropertyValue(valueName, value);
}

void CameraNode::setProjection(CameraProjection projMode)
{
	this->projMode = projMode;
	isPerspective = projMode == CameraProjection::Perspective ? true : false;
}

CameraProjection CameraNode::getProjection()
{
    return projMode;
}

float CameraNode::getVrViewScale()
{
    return vrViewScale;
}

void CameraNode::setVrViewScale(float viewScale)
{
    vrViewScale = viewScale;
}

void CameraNode::setAspectRatio(float aspect)
{
    aspectRatio = aspect;
}

void CameraNode::updateCameraMatrices()
{
    viewMatrix.setToIdentity();

    QVector3D pos = globalTransform.column(3).toVector3D();
    QVector3D dir = (globalTransform * QVector4D(0, 0, -1, 1)).toVector3D();
    QVector3D up = (globalTransform * QVector4D(0, 1, 0, 0)).toVector3D();

    viewMatrix.lookAt(pos, dir, up);

    projMatrix.setToIdentity();

    if ((projMode == CameraProjection::Perspective)) {
        projMatrix.perspective(angle, aspectRatio, nearClip, farClip);
    }
    else {
        projMatrix.ortho(-orthoSize * aspectRatio, orthoSize * aspectRatio, -orthoSize, orthoSize, -farClip, farClip);
    }

    //vrViewScale = 5.0f;
}

void CameraNode::setFieldOfViewRadians(float fov)
{
    angle = qRadiansToDegrees(fov);
}

void CameraNode::setFieldOfViewDegrees(float fov)
{
    angle = fov;
}

void CameraNode::lookAt(QVector3D target)
{
    //todo: use global matrices
    QMatrix4x4 matrix;
    matrix.setToIdentity();
    matrix.lookAt(pos, target, QVector3D(0, 1, 0));
    matrix = matrix.inverted();
    MathHelper::decomposeMatrix(matrix, pos, rot, scale);
}

void CameraNode::setOrthagonalZoom(float size)
{
	orthoSize = size;
	updateCameraMatrices();
}

void CameraNode::update(float dt)
{
    SceneNode::update(dt);
    updateCameraMatrices();
}

QVector3D CameraNode::calculatePickingDirection(int viewPortWidth, int viewPortHeight, QPointF pos)
{
    float x = ((2.0f * pos.x()) / viewPortWidth) - 1.0f;
    float y = 1.0f - ((2.0f * pos.y()) / viewPortHeight);

    QVector4D ray = projMatrix.inverted() * QVector4D(x, y, -1.0f, 1.0f);
    ray.setZ(-1.0f);
    ray.setW(0.0f);
    ray = viewMatrix.inverted() * ray;
    return ray.toVector3D().normalized();
}

SceneNodePtr CameraNode::createDuplicate()
{
	auto camera = iris::CameraNode::create();

	camera->angle = this->angle;
	camera->nearClip = this->nearClip;
	camera->farClip = this->farClip;
	camera->aspectRatio = this->aspectRatio;
	camera->orthoSize = this->orthoSize;
	
	return camera;
}

}
