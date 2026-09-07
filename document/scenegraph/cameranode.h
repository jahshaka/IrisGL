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
#include "document/scenegraph/cameralens.h"
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
    /// The angle of view is VERTICAL; WHICH sensor dimension it binds through is
    /// `sensorFit` below (CAMERA_LENS_SPEC §3 — the phase that finally made
    /// `sensorWidth` do something; it was stored and inert before).
    float sensorWidth;
    float sensorHeight;

    // ---- CAMERA_LENS_SPEC §3: the filmback block -------------------------

    /// Which sensor dimension the focal length binds through (see cameralens.h
    /// for the whole model). VERTICAL is the default because it is what this
    /// class always did: no existing scene's projection moves because these
    /// fields arrived.
    CameraSensorFit sensorFit;

    /// Anamorphic squeeze factor: the effective sensor WIDTH is
    /// `sensorWidth * anamorphicSqueeze`. 1.0 (spherical) is the default and is
    /// inert; a 2x anamorphic sees as wide as a half-length spherical lens.
    /// Only participates when the binding axis is horizontal.
    float anamorphicSqueeze;

    /// LENS SHIFT (rise/fall and cross), as a FRACTION OF THE FRAME: 0.5 slides
    /// the image half a frame width / height without tilting the camera — the
    /// architectural-photography move that keeps verticals parallel. The
    /// conversion into a projection offset is re-derived from fov/aspect/near
    /// wherever it is applied (never stored), because it depends on all three.
    float lensShiftX;
    float lensShiftY;

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

    // ---- CAMERA_LENS_SPEC §3 P2: the focus block -------------------------
    //
    // RENDER-INERT, all of it: no pass reads these (the DoF pass is deferred by
    // the owner). What they DO drive is the tracking arithmetic, the focus
    // numbers `camera.focusInfo` reports, and the debug plane — i.e. everything
    // needed to author a focus pull correctly before anything renders it.

    /// Added to the tracked distance in Track mode (metres, may be negative):
    /// "focus a little in front of his eyes" is a real instruction.
    float focusOffset;
    /// Ease the tracked distance instead of snapping it (a focus puller's hand,
    /// not a servo). Off by default so tracking stays exactly predictable.
    bool smoothFocus;
    /// How fast the eased distance converges, in e-folds per second
    /// (lens::smoothTowards). 8 is roughly "settles in half a second".
    float focusSmoothingSpeed;
    /// The closest the lens can focus, in metres. Tracking clamps to it.
    float minFocusDistance;
    /// Diaphragm blades — the shape of the bokeh. Stored for the deferred DoF
    /// pass and for export; nothing samples it yet. Clamped to 3..16.
    int bladeCount;
    /// Draw the focus plane as a wire rectangle inside the frustum helper
    /// (an editor helper like the body: hidden in play/game view, never in a
    /// render). Off by default.
    bool focusPlaneVisible;

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

    /// This camera's filmback as the lens math sees it — sensor pair, squeeze,
    /// fit and the authored aspect, in one struct (cameralens.h).
    iris::lens::Filmback filmback() const;

    /// The lens view of `angle`, in millimetres, through the filmback:
    ///
    ///     vertical fit:   angle = 2 * atan(sensorHeight / (2 * focalLength))
    ///     horizontal fit: hFov  = 2 * atan(sensorWidth * squeeze / (2 * f))
    ///                     angle = 2 * atan(tan(hFov / 2) / aspectRatio)
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
    /// Same contract as setSensorSize for the other two filmback rows: the
    /// authored view of the angle survives, the derived one moves.
    void setSensorFit(CameraSensorFit fit);
    void setAnamorphicSqueeze(float squeeze);

    /// The horizontal / diagonal angles of view (degrees) for the authored
    /// aspect. Derived, never stored — reported by the verbs and the panel.
    float horizontalFov() const;
    float diagonalFov() const;

    /// Depth of field, in numbers (CAMERA_LENS_SPEC §3, P2): hyperfocal
    /// distance and the near/far limits at the CURRENT focus distance, for this
    /// lens, f-stop and sensor. Render-inert — this is what makes the stored
    /// aperture honest before any DoF pass exists. `focusMode == Off` still
    /// reports, using focusDistance, because "what would be sharp" is a
    /// question about the lens, not about the tracking mode.
    iris::lens::FocusInfo focusInfo() const;

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
        // CAMERA_LENS_SPEC §3 defaults, chosen so a camera made today projects
        // EXACTLY what a camera made yesterday did: vertical fit is the old
        // hard-coded binding, a squeeze of 1 is spherical, and zero shift is a
        // centred frustum.
        sensorFit = CameraSensorFit::Vertical;
        anamorphicSqueeze = 1.0f;
        lensShiftX = 0.0f;
        lensShiftY = 0.0f;
        constrainAspect = false;
        dofEnabled = false;
        focusMode = CameraFocusMode::Manual;
        focusDistance = 10.0f;
        fStop = 2.8f;
        focusOffset = 0.0f;
        smoothFocus = false;
        focusSmoothingSpeed = 8.0f;
        minFocusDistance = 0.1f;
        bladeCount = 5;
        focusPlaneVisible = false;
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
