/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef CAMERANODE_H
#define CAMERANODE_H


#include "core/math/mat4.h"
#include "core/math/vec.h"
#include "irisglfwd.h"
#include "document/scenegraph/scenenode.h"


namespace iris
{

enum class CameraProjection {
	Orthogonal,
	Perspective
};

/// Which of the two views of the SAME angle of view the user authored
/// (CAMERAS_SPEC §2, Blender's model). The document stores the angle; the focal
/// length is derived from it through the sensor. The flag decides which of the
/// pair survives a sensor-size change — nothing else.
enum class CameraAuthorMode {
	Degrees,
	Millimeters
};

/// How the focus distance is decided (CAMERAS_SPEC §2, D7). `Manual` uses
/// focusDistance as written; `Track` derives it from focusTarget's world
/// position every frame; `Off` means no focus is computed at all (DOF may still
/// be flagged on and simply does nothing).
enum class CameraFocusMode {
	Manual,
	Track,
	Off
};

class CameraNode : public SceneNode
{
public:
    float aspectRatio;
    /// VERTICAL field of view, in DEGREES, end to end (CAMERAS_SPEC §1:
    /// angle -> CameraDesc.fovDegrees -> Ogre setFOVy). Unreal's basic camera
    /// FOV is horizontal; converting an Unreal value means going through the
    /// aspect ratio first.
    ///
    /// This is THE stored angle of view. `focalLength()` is a second view of
    /// it, not a second field, so a direct write here (the reader, the
    /// controllers, the preview framing) can never leave the two disagreeing.
    float angle;
    float nearClip;
    float farClip;
    float vrViewScale;
	float orthoSize;
	bool isPerspective;

	CameraProjection projMode;

    // ---- CAMERAS_SPEC §2: the physical-camera settings -------------------
    // All of them are reflected through getProperties/get/setPropertyValue, so
    // they are keyable by PropertyAnim and reachable from scripts with no new
    // machinery. None of them change what the ENGINE renders in phase 1 except
    // `angle` (which always did) — the body, the frustum, the letterbox and the
    // DOF pass are later phases; the document is the deliverable here.

    /// Sensor size in millimetres. 36 x 24 = full frame — OURS, not a
    /// reproduction of any engine's published default (Epic publishes none).
    /// The angle of view is VERTICAL, so it binds through `sensorHeight`;
    /// `sensorWidth` is stored because a horizontal FOV (Unreal import, a
    /// future UI row) is derived from it and because a sensor is a pair.
    float sensorWidth;
    float sensorHeight;

    /// Which view of the angle the user authored. Only consulted when the
    /// SENSOR changes: in Degrees the angle is kept and the focal length
    /// follows, in Millimeters the focal length is kept and the angle follows.
    CameraAuthorMode authorMode;

    /// Letterbox the view to `aspectRatio` instead of filling it. Stored and
    /// serialized in phase 1; the bars are drawn in phase 3.
    bool constrainAspect;

    // Focus / depth of field. Stored, animated and exported in v1; the live
    // DOF pass is the program's second act (D7).
    bool dofEnabled;
    CameraFocusMode focusMode;
    float focusDistance;      // metres, used when focusMode == Manual
    QString focusTarget;      // node guid, used when focusMode == Track
    float fStop;

    /// The output height in pixels for RENDERS and EXPORTS; with aspectRatio it
    /// derives the whole output size. The main view and the PiP ignore it
    /// (CAMERAS_SPEC §2 — no engine puts resolution on the camera; this is a
    /// named Jahshaka concept, not a claim about anyone else's model).
    int outputHeight;

    /// Draw the editor-only camera body + frustum wires for this camera
    /// (phase 2). Like every editor helper it is hidden in play/game view.
    bool bodyVisible;

    iris::Mat4 viewMatrix;
    iris::Mat4 projMatrix;

	void setProjection(CameraProjection view);
    CameraProjection getProjection();
    float getVrViewScale();
    void setVrViewScale(float viewScale);
    void setAspectRatio(float aspect);
    void setFieldOfViewRadians(float fov);
    void setFieldOfViewDegrees(float fov);

    /// The lens view of `angle`, in millimetres, through the sensor HEIGHT
    /// (the angle is vertical):
    ///
    ///     angle       = 2 * atan(sensorHeight / (2 * focalLength))
    ///     focalLength = sensorHeight / (2 * tan(angle / 2))
    ///
    /// Exact inverses of each other, and derived rather than stored, so the
    /// dozen places that assign `angle` directly cannot desynchronise the pair.
    float focalLength() const;
    /// Sets the angle of view from a focal length and flips authorMode to
    /// Millimeters. Values <= 0 are ignored (a zero-length lens has no angle).
    void setFocalLength(float mm);
    /// Changes the sensor and keeps whichever view of the angle was authored:
    /// in Degrees the angle survives, in Millimeters the focal length does.
    /// Non-positive dimensions are ignored.
    void setSensorSize(float widthMm, float heightMm);

    void lookAt(iris::Vec3 target);
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
    iris::Vec3 calculatePickingDirection(int viewPortWidth, int viewPortHeight, QPointF pos);

    virtual QList<Property*> getProperties() override;
    virtual QVariant getPropertyValue(QString valueName) override;
    virtual bool setPropertyValue(QString valueName, const QVariant &value) override;

	SceneNodePtr createDuplicate() override;

private:
    CameraNode()
    {
        // THE line this class never had (CAMERAS_SPEC §1, the type-enum trap).
        // Until cameras became scene-graph citizens nothing assigned it, so
        // every switch on getSceneNodeType() read a camera as `Empty` and the
        // exporters had to dynamic_cast around it. Setting it re-routes every
        // one of those switches — the phase-1 sweep audited each.
        sceneNodeType = SceneNodeType::Camera;
        angle = 45;         // Degrees are always used internally
        nearClip = 0.1f;
        farClip = 500.0f;
        aspectRatio = 1.0f; // Assumes a square viewport by default
		orthoSize = 10.0f;
        exportable = false;
		projMode = CameraProjection::Perspective;
        // CAMERAS_SPEC §2 defaults. 36x24 is full frame; with the historical
        // 45 degree angle that is a ~28.97 mm lens.
        sensorWidth = 36.0f;
        sensorHeight = 24.0f;
        authorMode = CameraAuthorMode::Degrees;
        constrainAspect = false;
        dofEnabled = false;
        focusMode = CameraFocusMode::Manual;
        focusDistance = 10.0f;
        fStop = 2.8f;
        outputHeight = 1080;
        bodyVisible = true;
		// Was left indeterminate (only setProjection() wrote it): any consumer
		// of a camera that never called setProjection read garbage — the engine
		// mirror rendered preview cameras ORTHOGRAPHIC when the garbage came up
		// false. Keep it in lock-step with projMode.
		isPerspective = true;
		vrViewScale = 2.0f; // good default
        updateCameraMatrices();
    }

};

}
#endif // CAMERANODE_H
