/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "core/math/mat4.h"
#include "core/math/vec.h"
#include "document/scenegraph/cameranode.h"

#include <QPoint>
#include <QtMath>

#include <cmath>

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

    // ---- CAMERAS_SPEC §2 -------------------------------------------------
    // The physical-camera rows. Every one of them is keyable through
    // PropertyAnim and writable through node.setProperty for exactly this
    // reason: they are reflected here.

    prop = new FloatProperty();
    prop->displayName = "Focal Length (mm)";
    prop->name = "focalLength";
    prop->value = focalLength();
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Sensor Width (mm)";
    prop->name = "sensorWidth";
    prop->value = sensorWidth;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Sensor Height (mm)";
    prop->name = "sensorHeight";
    prop->value = sensorHeight;
    props.append(prop);

    intProp = new IntProperty();
    intProp->displayName = "Author Mode";
    intProp->name = "authorMode";
    intProp->value = static_cast<int>(authorMode);
    props.append(intProp);

    // ---- CAMERA_LENS_SPEC §3, the filmback rows --------------------------

    intProp = new IntProperty();
    intProp->displayName = "Sensor Fit";
    intProp->name = "sensorFit";
    intProp->value = static_cast<int>(sensorFit);
    props.append(intProp);

    prop = new FloatProperty();
    prop->displayName = "Anamorphic Squeeze";
    prop->name = "anamorphicSqueeze";
    prop->value = anamorphicSqueeze;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Lens Shift X";
    prop->name = "lensShiftX";
    prop->value = lensShiftX;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Lens Shift Y";
    prop->name = "lensShiftY";
    prop->value = lensShiftY;
    props.append(prop);

    auto boolProp = new BoolProperty();
    boolProp->displayName = "Constrain Aspect Ratio";
    boolProp->name = "constrainAspect";
    boolProp->value = constrainAspect;
    props.append(boolProp);

    boolProp = new BoolProperty();
    boolProp->displayName = "Depth of Field";
    boolProp->name = "dofEnabled";
    boolProp->value = dofEnabled;
    props.append(boolProp);

    intProp = new IntProperty();
    intProp->displayName = "Focus Mode";
    intProp->name = "focusMode";
    intProp->value = static_cast<int>(focusMode);
    props.append(intProp);

    prop = new FloatProperty();
    prop->displayName = "Focus Distance";
    prop->name = "focusDistance";
    prop->value = focusDistance;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "F-Stop";
    prop->name = "fStop";
    prop->value = fStop;
    props.append(prop);

    // ---- CAMERA_LENS_SPEC §3 P2, the focus rows --------------------------
    // Keyable like everything else here, which is the whole point: a focus pull
    // is a keyframed focusDistance and a stop ramp is a keyframed fStop, both
    // authorable before the DoF pass exists to render them.

    prop = new FloatProperty();
    prop->displayName = "Focus Offset";
    prop->name = "focusOffset";
    prop->value = focusOffset;
    props.append(prop);

    boolProp = new BoolProperty();
    boolProp->displayName = "Smooth Focus";
    boolProp->name = "smoothFocus";
    boolProp->value = smoothFocus;
    props.append(boolProp);

    prop = new FloatProperty();
    prop->displayName = "Focus Smoothing Speed";
    prop->name = "focusSmoothingSpeed";
    prop->value = focusSmoothingSpeed;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Min Focus Distance";
    prop->name = "minFocusDistance";
    prop->value = minFocusDistance;
    props.append(prop);

    intProp = new IntProperty();
    intProp->displayName = "Diaphragm Blades";
    intProp->name = "bladeCount";
    intProp->value = bladeCount;
    props.append(intProp);

    boolProp = new BoolProperty();
    boolProp->displayName = "Show Focus Plane";
    boolProp->name = "focusPlaneVisible";
    boolProp->value = focusPlaneVisible;
    props.append(boolProp);

    intProp = new IntProperty();
    intProp->displayName = "Output Height";
    intProp->name = "outputHeight";
    intProp->value = outputHeight;
    props.append(intProp);

    boolProp = new BoolProperty();
    boolProp->displayName = "Show Camera Body";
    boolProp->name = "bodyVisible";
    boolProp->value = bodyVisible;
    props.append(boolProp);

    // focusTarget is deliberately NOT a row: it is a node GUID, and the
    // property list is the keyable/panel surface — there is no widget type for
    // a node reference and nothing sensible to interpolate between two guids.
    // It is still readable and writable through get/setPropertyValue below (the
    // same shape DecalNode's decalNormal/decalEmissive guids have) and it has a
    // first-class home on camera.settings().

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

    // CAMERAS_SPEC §2.
    if (valueName == "focalLength")     return focalLength();
    if (valueName == "sensorWidth")     return sensorWidth;
    if (valueName == "sensorHeight")    return sensorHeight;
    if (valueName == "authorMode")      return static_cast<int>(authorMode);
    // CAMERA_LENS_SPEC §3.
    if (valueName == "sensorFit")         return static_cast<int>(sensorFit);
    if (valueName == "anamorphicSqueeze") return anamorphicSqueeze;
    if (valueName == "lensShiftX")        return lensShiftX;
    if (valueName == "lensShiftY")        return lensShiftY;
    if (valueName == "focusOffset")         return focusOffset;
    if (valueName == "smoothFocus")         return smoothFocus;
    if (valueName == "focusSmoothingSpeed") return focusSmoothingSpeed;
    if (valueName == "minFocusDistance")    return minFocusDistance;
    if (valueName == "bladeCount")          return bladeCount;
    if (valueName == "focusPlaneVisible")   return focusPlaneVisible;
    if (valueName == "constrainAspect") return constrainAspect;
    if (valueName == "dofEnabled")      return dofEnabled;
    if (valueName == "focusMode")       return static_cast<int>(focusMode);
    if (valueName == "focusDistance")   return focusDistance;
    if (valueName == "focusTarget")     return focusTarget;
    if (valueName == "fStop")           return fStop;
    if (valueName == "outputHeight")    return outputHeight;
    if (valueName == "bodyVisible")     return bodyVisible;

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

    // CAMERAS_SPEC §2. The two lens rows go through their setters, which is
    // what keeps `angle` and the focal length the SAME value seen two ways.
    if (valueName == "focalLength")  { setFocalLength(value.toFloat());              return true; }
    if (valueName == "sensorWidth")  { setSensorSize(value.toFloat(), sensorHeight); return true; }
    if (valueName == "sensorHeight") { setSensorSize(sensorWidth, value.toFloat());  return true; }
    if (valueName == "authorMode") {
        const int m = value.toInt();
        authorMode = (m == static_cast<int>(CameraAuthorMode::Millimeters))
                         ? CameraAuthorMode::Millimeters : CameraAuthorMode::Degrees;
        return true;
    }
    // CAMERA_LENS_SPEC §3. The three filmback rows go through their setters for
    // the same reason the sensor pair does: each of them changes what a focal
    // length MEANS, so the authored view of the angle has to be the one that
    // survives.
    if (valueName == "sensorFit") {
        const int f = value.toInt();
        setSensorFit(f == static_cast<int>(CameraSensorFit::Horizontal) ? CameraSensorFit::Horizontal
                   : f == static_cast<int>(CameraSensorFit::Auto)       ? CameraSensorFit::Auto
                                                                        : CameraSensorFit::Vertical);
        return true;
    }
    if (valueName == "anamorphicSqueeze") { setAnamorphicSqueeze(value.toFloat()); return true; }
    // Shift is clamped to a frame either way: past that the frustum's near rect
    // no longer contains the axis and the projection stops being useful.
    if (valueName == "lensShiftX") { lensShiftX = qBound(-1.0f, value.toFloat(), 1.0f); return true; }
    if (valueName == "lensShiftY") { lensShiftY = qBound(-1.0f, value.toFloat(), 1.0f); return true; }
    if (valueName == "focusOffset")         { focusOffset = value.toFloat();               return true; }
    if (valueName == "smoothFocus")         { smoothFocus = value.toBool();                return true; }
    if (valueName == "focusSmoothingSpeed") { focusSmoothingSpeed = qMax(0.0f, value.toFloat()); return true; }
    if (valueName == "minFocusDistance")    { minFocusDistance = qMax(0.0f, value.toFloat()); return true; }
    if (valueName == "bladeCount")          { bladeCount = qBound(3, value.toInt(), 16);   return true; }
    if (valueName == "focusPlaneVisible")   { focusPlaneVisible = value.toBool();          return true; }
    if (valueName == "constrainAspect") { constrainAspect = value.toBool(); return true; }
    if (valueName == "dofEnabled")      { dofEnabled = value.toBool();      return true; }
    if (valueName == "focusMode") {
        const int m = value.toInt();
        focusMode = (m == static_cast<int>(CameraFocusMode::Track))   ? CameraFocusMode::Track
                  : (m == static_cast<int>(CameraFocusMode::Off))     ? CameraFocusMode::Off
                                                                      : CameraFocusMode::Manual;
        return true;
    }
    // Non-negative: a negative focus distance or f-stop has no meaning and the
    // DOF pass would divide by it.
    if (valueName == "focusDistance") { focusDistance = qMax(0.0f, value.toFloat()); return true; }
    if (valueName == "focusTarget")   { focusTarget = value.toString();              return true; }
    if (valueName == "fStop")         { fStop = qMax(0.0f, value.toFloat());         return true; }
    // One pixel is the floor; the cap is the largest render anyone can ask for
    // without wedging the machine on the offscreen path.
    if (valueName == "outputHeight")  { outputHeight = qBound(1, value.toInt(), 16384); return true; }
    if (valueName == "bodyVisible")   { bodyVisible = value.toBool();                return true; }

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
    // THE ASPECT IS PART OF THE LENS BINDING when the fit is horizontal
    // (CAMERA_LENS_SPEC §3): a 35 mm lens covers a fixed HORIZONTAL angle, so
    // changing the frame's shape must change the VERTICAL angle and not the
    // lens. Only when the user authored millimetres, and only when the fit
    // actually resolves horizontally — which is why the default (Vertical) and
    // every camera authored in degrees behave exactly as they always did: this
    // branch is not taken and the aspect is a plain assignment.
    const bool rebind = authorMode == CameraAuthorMode::Millimeters &&
                        iris::lens::fitAxis(sensorFit, aspect) == iris::lens::FitAxis::Horizontal &&
                        iris::lens::fitAxis(sensorFit, aspectRatio) == iris::lens::FitAxis::Horizontal;
    const float keepMm = rebind ? focalLength() : 0.0f;
    aspectRatio = aspect;
    if (rebind && keepMm > 0.0f) {
        const float v = iris::lens::verticalFovDegFromFocal(filmback(), keepMm);
        if (v > 0.0f) angle = v;
    }
}

void CameraNode::updateCameraMatrices()
{
    viewMatrix.setToIdentity();

    const iris::Mat4 world = getGlobalTransform();
    iris::Vec3 pos = world.column(3).toVector3D();
    iris::Vec3 dir = (world * iris::Vec4(0, 0, -1, 1)).toVector3D();
    iris::Vec3 up = (world * iris::Vec4(0, 1, 0, 0)).toVector3D();

    viewMatrix.lookAt(pos, dir, up);

    projMatrix.setToIdentity();

    if ((projMode == CameraProjection::Perspective)) {
        if (lensShiftX == 0.0f && lensShiftY == 0.0f) {
            projMatrix.perspective(angle, aspectRatio, nearClip, farClip);
        } else {
            // LENS SHIFT, document-side (CAMERA_LENS_SPEC §3). The engine offsets
            // its own frustum; this matrix is what PICKING and every document-side
            // projection use, so it has to be the same off-axis frustum or a click
            // would land where the shot is not. Derived here from fov/aspect/near
            // rather than stored, exactly like the engine's copy — see
            // cameralens.h's note about why this conversion is never cached.
            const float halfH = iris::lens::halfExtentAtNear(angle, nearClip);
            const float halfW = halfH * (aspectRatio > 0.0f ? aspectRatio : 1.0f);
            const float ox = iris::lens::nearOffsetFromShift(lensShiftX, halfW);
            const float oy = iris::lens::nearOffsetFromShift(lensShiftY, halfH);
            projMatrix.frustum(-halfW + ox, halfW + ox, -halfH + oy, halfH + oy,
                               nearClip, farClip);
        }
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
    // The user just spoke in degrees; a later sensor change keeps THIS number
    // and moves the focal length (CAMERAS_SPEC §2).
    authorMode = CameraAuthorMode::Degrees;
}

// ---- the lens <-> angle binding (CAMERAS_SPEC §2) -------------------------
//
// The angle of view is VERTICAL, so the binding dimension is the sensor
// HEIGHT. The two directions are exact inverses:
//
//     angle       = 2 * atan(sensorHeight / (2 * focalLength))
//     focalLength = sensorHeight / (2 * tan(angle / 2))
//
// Only `angle` is stored. Deriving the millimetres instead of storing them is
// what makes a direct `cam->angle = x` (the scene reader, the camera
// controllers, previewframing) impossible to get wrong.

iris::lens::Filmback CameraNode::filmback() const
{
    iris::lens::Filmback fb;
    fb.sensorWidth  = sensorWidth;
    fb.sensorHeight = sensorHeight;
    fb.squeeze      = anamorphicSqueeze;
    fb.fit          = sensorFit;
    // The AUTHORED aspect. A view that does not constrain the aspect renders at
    // its target's, so a horizontal-fit camera's horizontal angle is only
    // exactly this one in a frame of the authored shape (cameralens.h says so
    // at length). The VERTICAL angle — the value that is stored and pushed — is
    // always what the document says, whatever the frame.
    fb.aspect       = aspectRatio > 0.01f ? aspectRatio : 1.0f;
    return fb;
}

float CameraNode::focalLength() const
{
    return iris::lens::focalFromVerticalFovDeg(filmback(), angle);
}

void CameraNode::setFocalLength(float mm)
{
    if (mm <= 0.0f) return;   // a zero-length lens has no angle of view
    const float v = iris::lens::verticalFovDegFromFocal(filmback(), mm);
    if (v <= 0.0f) return;
    angle = v;
    authorMode = CameraAuthorMode::Millimeters;
}

void CameraNode::setSensorSize(float widthMm, float heightMm)
{
    if (widthMm <= 0.0f || heightMm <= 0.0f) return;
    // Which of the two views survives is the WHOLE job of authorMode: a
    // photographer who typed "35 mm" expects a bigger sensor to widen the shot;
    // someone who typed "45 degrees" expects the framing to stay put.
    const float keepMm = focalLength();
    sensorWidth = widthMm;
    sensorHeight = heightMm;
    if (authorMode == CameraAuthorMode::Millimeters && keepMm > 0.0f) {
        const float v = iris::lens::verticalFovDegFromFocal(filmback(), keepMm);
        if (v > 0.0f) angle = v;
    }
}

void CameraNode::setSensorFit(CameraSensorFit fit)
{
    // Same contract as setSensorSize, and for the same reason: changing the fit
    // changes which sensor dimension a focal length is measured against, so it
    // changes the angle a "35 mm" means. In Degrees the framing is what the
    // user authored and it stays; in Millimeters the lens is, and the framing
    // moves under it.
    const float keepMm = focalLength();
    sensorFit = fit;
    if (authorMode == CameraAuthorMode::Millimeters && keepMm > 0.0f) {
        const float v = iris::lens::verticalFovDegFromFocal(filmback(), keepMm);
        if (v > 0.0f) angle = v;
    }
}

void CameraNode::setAnamorphicSqueeze(float squeeze)
{
    if (!(squeeze > 0.0f)) return;   // a zero or negative squeeze is not a lens
    const float keepMm = focalLength();
    anamorphicSqueeze = squeeze;
    if (authorMode == CameraAuthorMode::Millimeters && keepMm > 0.0f) {
        const float v = iris::lens::verticalFovDegFromFocal(filmback(), keepMm);
        if (v > 0.0f) angle = v;
    }
}

float CameraNode::horizontalFov() const
{
    return iris::lens::horizontalFovDeg(angle, filmback().aspect);
}

float CameraNode::diagonalFov() const
{
    return iris::lens::diagonalFovDeg(angle, filmback().aspect);
}

iris::lens::FocusInfo CameraNode::focusInfo() const
{
    return iris::lens::focusInfo(focalLength(), fStop, focusDistance,
                                 iris::lens::circleOfConfusionMm(sensorWidth, sensorHeight));
}

void CameraNode::lookAt(iris::Vec3 target)
{
    //todo: use global matrices
    iris::Mat4 matrix;
    matrix.setToIdentity();
    matrix.lookAt(getLocalPos(), target, iris::Vec3(0, 1, 0));
    matrix = matrix.inverted();
    setLocalTransform(matrix);
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

iris::Vec3 CameraNode::calculatePickingDirection(int viewPortWidth, int viewPortHeight, QPointF pos)
{
    float x = ((2.0f * pos.x()) / viewPortWidth) - 1.0f;
    float y = 1.0f - ((2.0f * pos.y()) / viewPortHeight);

    iris::Vec4 ray = projMatrix.inverted() * iris::Vec4(x, y, -1.0f, 1.0f);
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
	// setProjection, not a raw assignment: isPerspective and projMode are a
	// pair and a copy that splits them renders orthographic. vrViewScale was
	// silently dropped by this method too — a duplicated VR camera came back
	// with the default scale.
	camera->setProjection(this->projMode);
	camera->vrViewScale = this->vrViewScale;

	// CAMERAS_SPEC §2: every setting the table names, copied by VALUE. The
	// order matters exactly once — the sensor is set before authorMode, and
	// `angle` is copied above rather than derived, so the duplicate's lens is
	// the same number and not a re-derivation of it.
	camera->sensorWidth = this->sensorWidth;
	camera->sensorHeight = this->sensorHeight;
	camera->authorMode = this->authorMode;
	// CAMERA_LENS_SPEC §3: the filmback and focus blocks, by VALUE for the same
	// reason as the rest — a duplicate must be the same camera, not a
	// re-derivation of one.
	camera->sensorFit = this->sensorFit;
	camera->anamorphicSqueeze = this->anamorphicSqueeze;
	camera->lensShiftX = this->lensShiftX;
	camera->lensShiftY = this->lensShiftY;
	camera->focusOffset = this->focusOffset;
	camera->smoothFocus = this->smoothFocus;
	camera->focusSmoothingSpeed = this->focusSmoothingSpeed;
	camera->minFocusDistance = this->minFocusDistance;
	camera->bladeCount = this->bladeCount;
	camera->focusPlaneVisible = this->focusPlaneVisible;
	camera->constrainAspect = this->constrainAspect;
	camera->dofEnabled = this->dofEnabled;
	camera->focusMode = this->focusMode;
	camera->focusDistance = this->focusDistance;
	camera->focusTarget = this->focusTarget;
	camera->fStop = this->fStop;
	camera->outputHeight = this->outputHeight;
	camera->bodyVisible = this->bodyVisible;

	return camera;
}

}
